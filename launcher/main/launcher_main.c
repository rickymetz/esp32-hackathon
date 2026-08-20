/*
 * Launcher: BSP + LVGL + Lua.
 *
 * Two things here are non-obvious and were established by testing on hardware:
 *
 * 1. bsp_display_start() alone leaves the panel dark. BSP_LCD_RST /
 *    BSP_LCD_TOUCH_RST / BSP_LCD_BACKLIGHT are all GPIO_NUM_NC on this board --
 *    the reset lines hang off the TCA9554 IO expander, which the BSP never
 *    initialises, so the panel sits held in reset with no error reported.
 *    release_panel_reset() reproduces the vendor's own EXIO1/EXIO2 pulse.
 *
 * 2. Lua event callbacks do not fire on their own. The LVGL event trampoline
 *    in lua_module_lvgl only *enqueues* callbacks; something must call
 *    lvgl.process_events() to drain the queue. We pump it from the app task
 *    rather than making every app write its own loop -- that keeps apps
 *    declarative and leaves the launcher able to stop them.
 */

#include <stdio.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_io_expander.h"
#include "bsp/esp-bsp.h"
#include "lvgl.h"

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "cap_lua.h"
#include "display_service.h"
#include "lua_module_lvgl.h"

static const char *TAG = "launcher";

/* Expander lines, from the vendor's reference sketch.
 * EXIO1 / EXIO2 = LCD and touch reset. EXIO4 = PWR button, EXIO5 = PMU IRQ. */
#define EXIO_LCD_RST    IO_EXPANDER_PIN_NUM_1
#define EXIO_TOUCH_RST  IO_EXPANDER_PIN_NUM_2
#define EXIO_PWR_BTN    IO_EXPANDER_PIN_NUM_4
#define EXIO_PMU_IRQ    IO_EXPANDER_PIN_NUM_5

#define APP_TASK_STACK  (32 * 1024)
#define EVENT_PUMP_MS   100

/* First app, embedded until loading from SD lands. Note it has no loop of its
 * own: it builds the UI, wires callbacks, and returns. */
static const char APP_HELLO[] =
    "local lvgl = require('lvgl')                                  \n"
    "lvgl.init({ buffer_lines = 40 })                              \n"
    "local scr = lvgl.create_screen()                              \n"
    "scr:set_style({ bg_color = '#101014' })                       \n"
    "local title = lvgl.label(scr, {                               \n"
    "    text = 'Hello from Lua',                                  \n"
    "    align = 'top_mid', y = 30, text_color = '#ffffff' })      \n"
    "local n = 0                                                   \n"
    "for i = 1, 10 do n = n + i * i end                            \n"
    "lvgl.label(scr, {                                             \n"
    "    text = 'sum of squares = ' .. n,                          \n"
    "    align = 'bottom_mid', y = -30, text_color = '#6ce86c' })      \n"
    "local btn = lvgl.button(scr, {                                \n"
    "    text = 'Tap me', align = 'center', y = 0,                 \n"
    "    w = 240, h = 120,                                         \n"
    "    bg_color = '#2f80ed', text_color = '#ffffff' })           \n"
    "local taps = 0                                                \n"
    "btn:on('clicked', function()                                  \n"
    "    taps = taps + 1                                           \n"
    "    title:set_text('tapped ' .. taps)                         \n"
    "end)                                                          \n"
    "scr:load()                                                    \n"
    "return 'app started'                                          \n";

static esp_err_t release_panel_reset(void)
{
    esp_io_expander_handle_t exp = bsp_io_expander_init();
    if (exp == NULL) {
        ESP_LOGE(TAG, "io expander init failed");
        return ESP_FAIL;
    }

    const uint32_t rst = EXIO_LCD_RST | EXIO_TOUCH_RST;

    ESP_ERROR_CHECK(esp_io_expander_set_dir(exp, rst, IO_EXPANDER_OUTPUT));
    ESP_ERROR_CHECK(esp_io_expander_set_dir(exp, EXIO_PWR_BTN | EXIO_PMU_IRQ, IO_EXPANDER_INPUT));

    ESP_ERROR_CHECK(esp_io_expander_set_level(exp, rst, 0));
    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_ERROR_CHECK(esp_io_expander_set_level(exp, rst, 1));
    vTaskDelay(pdMS_TO_TICKS(50));

    ESP_LOGI(TAG, "panel reset released via IO expander");
    return ESP_OK;
}

/* Lua's allocator, pointed at PSRAM so apps cannot starve internal DRAM. */
static void *lua_psram_alloc(void *ud, void *ptr, size_t osize, size_t nsize)
{
    (void)ud; (void)osize;
    if (nsize == 0) {
        free(ptr);
        return NULL;
    }
    return heap_caps_realloc(ptr, nsize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

/* Drain queued LVGL events into their Lua callbacks.
 * Returns false if the call errored, which ends the app. */
static bool pump_events(lua_State *L, int timeout_ms)
{
    lua_getglobal(L, "lvgl");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return false;
    }
    lua_getfield(L, -1, "process_events");
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 2);
        return false;
    }
    lua_pushinteger(L, timeout_ms);
    if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
        ESP_LOGE(TAG, "process_events failed: %s", lua_tostring(L, -1));
        lua_pop(L, 2);
        return false;
    }
    lua_pop(L, 2);   /* result + lvgl table */
    return true;
}

typedef struct {
    const char *name;
    const char *src;
} app_desc_t;

/* Runs one app in its own VM on its own task. A failing app is reported and
 * torn down; it must never take the launcher with it. */
static void lua_app_task(void *arg)
{
    const app_desc_t *app = (const app_desc_t *)arg;
    size_t psram_before = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    lua_State *L = lua_newstate(lua_psram_alloc, NULL, 0);  /* Lua 5.5 takes a seed */
    if (L == NULL) {
        ESP_LOGE(TAG, "lua_newstate failed for '%s'", app->name);
        vTaskDelete(NULL);
        return;
    }
    luaL_openlibs(L);
    ESP_ERROR_CHECK(launcher_lua_open_modules(L));

    if (luaL_dostring(L, app->src) != LUA_OK) {
        ESP_LOGE(TAG, "app '%s' failed: %s", app->name, lua_tostring(L, -1));
        goto done;
    }

    ESP_LOGI(TAG, "app '%s': %s", app->name,
             lua_tostring(L, -1) ? lua_tostring(L, -1) : "(no result)");
    lua_settop(L, 0);

    ESP_LOGI(TAG, "app '%s' vm psram cost = %d bytes", app->name,
             (int)(psram_before - heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));

    /* Event pump: this is what makes Lua callbacks actually fire.
     * A positive timeout is the designed usage: process_events() then sleeps
     * ~20ms internally between polls, giving good responsiveness. The small
     * extra yield keeps the idle task fed. */
    while (!cap_lua_runtime_stop_requested(L)) {
        if (!pump_events(L, EVENT_PUMP_MS)) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    ESP_LOGI(TAG, "app '%s' stopping", app->name);

done:
    launcher_lua_run_exit_cleanup(L);
    lua_close(L);
    ESP_LOGI(TAG, "app '%s' closed, psram free=%u", app->name,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    vTaskDelete(NULL);
}

static void launch_app(const app_desc_t *app)
{
    xTaskCreate(lua_app_task, "lua_app", APP_TASK_STACK, (void *)app, 5, NULL);
}

static const app_desc_t s_hello = { .name = "hello", .src = APP_HELLO };

void app_main(void)
{
    printf("\n=== ESP32-S3-Touch-AMOLED-1.8 Launcher ===\n");
    printf("LVGL %d.%d.%d / %s\n",
           LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH, LUA_RELEASE);

    ESP_ERROR_CHECK(release_panel_reset());

    lv_display_t *disp = bsp_display_start();
    if (disp == NULL) {
        ESP_LOGE(TAG, "bsp_display_start() failed");
        return;
    }
    bsp_display_backlight_on();

    /* Hand the BSP display to the service the Lua LVGL binding talks to. */
    display_service_attach(disp);

    /* Data root is where apps and their assets live. The FS driver registers
     * even without a card; font loads then fall back to LVGL's built-in font. */
    ESP_ERROR_CHECK(lua_module_lvgl_register_with_data_root("/sdcard"));

    launch_app(&s_hello);

    ESP_LOGI(TAG, "internal free=%u psram free=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}
