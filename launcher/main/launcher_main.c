/*
 * Launcher bring-up: BSP + LVGL + Lua.
 *
 * NOTE: bsp_display_start() alone leaves the panel dark. On this board
 * BSP_LCD_RST / BSP_LCD_TOUCH_RST / BSP_LCD_BACKLIGHT are all GPIO_NUM_NC --
 * the reset lines hang off the TCA9554 IO expander, and the BSP never
 * initialises it. The panel therefore stays held in reset and nothing renders,
 * with no error reported anywhere. We reproduce the vendor's own sequence
 * (EXIO1 + EXIO2 pulsed low then high) before starting the display.
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

static const char *TAG = "launcher";

/* Expander lines, from the vendor's reference sketch.
 * EXIO1 / EXIO2 = LCD and touch reset. EXIO4 = PWR button, EXIO5 = PMU IRQ. */
#define EXIO_LCD_RST    IO_EXPANDER_PIN_NUM_1
#define EXIO_TOUCH_RST  IO_EXPANDER_PIN_NUM_2
#define EXIO_PWR_BTN    IO_EXPANDER_PIN_NUM_4
#define EXIO_PMU_IRQ    IO_EXPANDER_PIN_NUM_5

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

/* Runs a script and returns the string it produced, or NULL. Caller must
 * copy the result before the next Lua call. */
static const char *lua_eval(lua_State *L, const char *src)
{
    if (luaL_dostring(L, src) != LUA_OK) {
        ESP_LOGE(TAG, "lua error: %s", lua_tostring(L, -1));
        lua_pop(L, 1);
        return NULL;
    }
    return lua_tostring(L, -1);
}

void app_main(void)
{
    printf("\n=== ESP32-S3-Touch-AMOLED-1.8 Launcher ===\n");
    printf("LVGL %d.%d.%d\n", LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH);

    ESP_ERROR_CHECK(release_panel_reset());

    lv_display_t *disp = bsp_display_start();
    if (disp == NULL) {
        ESP_LOGE(TAG, "bsp_display_start() failed");
        return;
    }
    bsp_display_backlight_on();

    /* Bring up Lua and prove it actually executes. */
    size_t psram_before = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    lua_State *L = lua_newstate(lua_psram_alloc, NULL, 0);  /* Lua 5.5 takes a seed */
    if (L == NULL) {
        ESP_LOGE(TAG, "lua_newstate failed");
        return;
    }
    luaL_openlibs(L);

    printf("%s\n", LUA_RELEASE);

    static char ver[32];
    const char *v = lua_eval(L, "return _VERSION");
    snprintf(ver, sizeof(ver), "%s", v ? v : "Lua unavailable");
    ESP_LOGI(TAG, "lua _VERSION = %s", ver);
    lua_settop(L, 0);

    /* A real computation, so we know the VM is not just loading. */
    static char result[64];
    const char *r = lua_eval(L,
        "local t = {}                      \n"
        "for i = 1, 10 do t[i] = i * i end  \n"
        "local sum = 0                      \n"
        "for _, v in ipairs(t) do sum = sum + v end \n"
        "return ('squares 1..10 sum = %d'):format(sum)");
    snprintf(result, sizeof(result), "%s", r ? r : "lua failed");
    ESP_LOGI(TAG, "%s", result);
    lua_settop(L, 0);

    size_t psram_after = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "lua VM psram cost = %u bytes", (unsigned)(psram_before - psram_after));

    /* Show it on the panel. */
    bsp_display_lock(0);

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101014), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "Launcher");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -40);

    lv_obj_t *lua_ver = lv_label_create(scr);
    lv_label_set_text(lua_ver, ver);
    lv_obj_set_style_text_color(lua_ver, lv_color_hex(0x6CE86C), LV_PART_MAIN);
    lv_obj_align(lua_ver, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *out = lv_label_create(scr);
    lv_label_set_text(out, result);
    lv_obj_set_style_text_color(out, lv_color_hex(0x8A8A99), LV_PART_MAIN);
    lv_obj_align(out, LV_ALIGN_CENTER, 0, 36);

    bsp_display_unlock();

    ESP_LOGI(TAG, "internal free=%u psram free=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}
