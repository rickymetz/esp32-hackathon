/*
 * Launcher: BSP + LVGL + Lua.
 *
 * Lists the Lua apps found on the SD card and runs the one you tap. Each app
 * gets its own Lua VM on its own task; the launcher keeps its own screen and
 * restores it when the app stops.
 *
 * Three things here are non-obvious, all established by testing on hardware:
 *
 * 1. bsp_display_start() alone leaves the panel dark. BSP_LCD_RST /
 *    BSP_LCD_TOUCH_RST / BSP_LCD_BACKLIGHT are all GPIO_NUM_NC on this board --
 *    the reset lines hang off the TCA9554 IO expander, which the BSP never
 *    initialises, so the panel sits held in reset with no error reported.
 *
 * 2. Lua event callbacks only queue. The LVGL event trampoline in
 *    lua_module_lvgl enqueues; something must call lvgl.process_events() to
 *    drain it. The launcher pumps it so apps stay declarative.
 *
 * 3. That pump needs a positive timeout AND a yield. A zero timeout returns
 *    immediately and starves the idle task into a watchdog reset.
 */

#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
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
#include "app_registry.h"
#include "launcher_main.h"
#include "serial_push.h"

static const char *TAG = "launcher";

/* Expander lines, from the vendor's reference sketch.
 * EXIO1 / EXIO2 = LCD and touch reset. EXIO4 = PWR button, EXIO5 = PMU IRQ. */
#define EXIO_LCD_RST    IO_EXPANDER_PIN_NUM_1
#define EXIO_TOUCH_RST  IO_EXPANDER_PIN_NUM_2
#define EXIO_PWR_BTN    IO_EXPANDER_PIN_NUM_4
#define EXIO_PMU_IRQ    IO_EXPANDER_PIN_NUM_5

#define APP_TASK_STACK  (32 * 1024)
#define EVENT_PUMP_MS   100

/* Touch on this panel is not pixel-accurate, so small targets get missed.
 * Verified on hardware: a 240x120 button catches every tap where a 180x56
 * one dropped roughly half. Keep launcher rows at least this tall. */
#define ROW_HEIGHT      72

static lv_obj_t *s_launcher_screen;
static TaskHandle_t s_app_task;
static esp_io_expander_handle_t s_expander;

/* Guards s_app_task's check-then-act (launch/stop) across the three tasks
 * that touch it: the LVGL/UI task (app_row_clicked), the serial task
 * (launcher_run_app_by_name / launcher_stop_app), and lua_app_task itself
 * clearing it on exit. Created in app_main() before anything can launch. */
static SemaphoreHandle_t s_app_mutex;

/* Single-app-at-a-time means a single static copy is enough: the launch
 * path fills this in (under s_app_mutex) from a live app_registry_get()
 * pointer just before starting the task, and lua_app_task reads only from
 * this copy for its whole run. That avoids holding a pointer into s_apps[]
 * across a run, which app_registry_scan() can rewrite at any time now that
 * a serial PUSH rescans at runtime. */
static app_entry_t s_current_app;

static esp_err_t release_panel_reset(void)
{
    esp_io_expander_handle_t exp = bsp_io_expander_init();
    if (exp == NULL) {
        ESP_LOGE(TAG, "io expander init failed");
        return ESP_FAIL;
    }
    s_expander = exp;

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
        lua_lvgl_force_unlock_if_held();
        return false;
    }
    lua_pop(L, 2);   /* result + lvgl table */
    return true;
}

static void show_launcher_screen(void)
{
    if (s_launcher_screen == NULL) {
        return;
    }
    bsp_display_lock(0);
    lv_screen_load(s_launcher_screen);
    bsp_display_unlock();
}

/* Runs one app in its own VM. A failing app is reported and torn down; it
 * must never take the launcher with it. */
static void lua_app_task(void *arg)
{
    (void)arg;
    /* Read only from the static copy the launch path filled in -- never a
     * pointer into app_registry's s_apps[], which a concurrent PUSH-driven
     * rescan can rewrite mid-run. */
    const app_entry_t *app = &s_current_app;
    size_t psram_before = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    lua_State *L;

    ESP_LOGI(TAG, "launching '%s' (%s)", app->name, app->path);
    launcher_lua_request_stop(false);

    L = lua_newstate(lua_psram_alloc, NULL, 0);  /* Lua 5.5 takes a seed */
    if (L == NULL) {
        ESP_LOGE(TAG, "lua_newstate failed for '%s'", app->name);
        goto out;
    }
    luaL_openlibs(L);
    ESP_ERROR_CHECK(launcher_lua_open_modules(L));

    if (luaL_dofile(L, app->path) != LUA_OK) {
        ESP_LOGE(TAG, "app '%s' failed: %s", app->name, lua_tostring(L, -1));
        /* The error may have unwound out of an LVGL binding that was holding
         * the display lock. Without this the LVGL task blocks forever. */
        lua_lvgl_force_unlock_if_held();
        goto close;
    }
    lua_settop(L, 0);

    ESP_LOGI(TAG, "app '%s' running, vm psram cost = %d bytes", app->name,
             (int)(psram_before - heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));

    /* Event pump: this is what makes Lua callbacks actually fire. */
    while (!cap_lua_runtime_stop_requested(L)) {
        if (!pump_events(L, EVENT_PUMP_MS)) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }

close:
    launcher_lua_run_exit_cleanup(L);
    lua_close(L);
    ESP_LOGI(TAG, "app '%s' closed, psram free=%u", app->name,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

out:
    show_launcher_screen();
    xSemaphoreTake(s_app_mutex, portMAX_DELAY);
    s_app_task = NULL;
    xSemaphoreGive(s_app_mutex);
    vTaskDelete(NULL);
}

static void app_row_clicked(lv_event_t *e)
{
    const app_entry_t *app = (const app_entry_t *)lv_event_get_user_data(e);

    xSemaphoreTake(s_app_mutex, portMAX_DELAY);
    if (s_app_task != NULL) {
        xSemaphoreGive(s_app_mutex);
        ESP_LOGW(TAG, "an app is already running");
        return;
    }
    s_current_app = *app;
    xTaskCreate(lua_app_task, "lua_app", APP_TASK_STACK,
                NULL, 5, &s_app_task);
    xSemaphoreGive(s_app_mutex);
}

/* "/sdcard/apps/counter.lua" -> "counter.lua" */
static const char *path_basename(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

/* Launcher-side API for other modules (serial_push) to drive app lifecycle
 * the same way a screen tap or the PWR button would. Reuses the exact same
 * task-creation and stop-request machinery as app_row_clicked() and
 * back_button_task() so RUN/STOP behave identically to a physical launch. */
bool launcher_run_app_by_name(const char *basename)
{
    if (basename == NULL || basename[0] == '\0') {
        return false;
    }

    xSemaphoreTake(s_app_mutex, portMAX_DELAY);

    if (s_app_task != NULL) {
        xSemaphoreGive(s_app_mutex);
        ESP_LOGW(TAG, "RUN '%s': an app is already running", basename);
        return false;
    }

    size_t count = app_registry_count();
    const app_entry_t *match = NULL;
    for (size_t i = 0; i < count; i++) {
        const app_entry_t *app = app_registry_get(i);
        if (strcmp(path_basename(app->path), basename) == 0) {
            match = app;
            break;
        }
    }
    if (match == NULL) {
        xSemaphoreGive(s_app_mutex);
        ESP_LOGW(TAG, "RUN '%s': not found", basename);
        return false;
    }

    s_current_app = *match;
    xTaskCreate(lua_app_task, "lua_app", APP_TASK_STACK,
                NULL, 5, &s_app_task);
    xSemaphoreGive(s_app_mutex);
    return true;
}

bool launcher_stop_app(void)
{
    xSemaphoreTake(s_app_mutex, portMAX_DELAY);
    bool running = (s_app_task != NULL);
    xSemaphoreGive(s_app_mutex);

    if (!running) {
        return false;
    }
    launcher_lua_request_stop(true);
    return true;
}

static void build_launcher_ui(void)
{
    size_t count = app_registry_count();

    bsp_display_lock(0);

    s_launcher_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_launcher_screen, lv_color_hex(0x0B0B0F), LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_launcher_screen, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_launcher_screen, 0, LV_PART_MAIN);

    lv_obj_t *header = lv_label_create(s_launcher_screen);
    lv_label_set_text(header, "Apps");
    lv_obj_set_style_text_color(header, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 24);

    if (count == 0) {
        lv_obj_t *empty = lv_label_create(s_launcher_screen);
        lv_label_set_text(empty, app_registry_sd_mounted()
                                     ? "No apps yet.\nCopy .lua files to\n/apps on the SD card."
                                     : "No SD card.\nInsert one with an\n/apps directory.");
        lv_obj_set_style_text_color(empty, lv_color_hex(0x8A8A99), LV_PART_MAIN);
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_center(empty);
    } else {
        lv_obj_t *list = lv_obj_create(s_launcher_screen);
        lv_obj_set_size(list, LV_PCT(100), LV_PCT(84));
        lv_obj_align(list, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(list, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(list, 12, LV_PART_MAIN);
        lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(list, 10, LV_PART_MAIN);

        for (size_t i = 0; i < count; i++) {
            const app_entry_t *app = app_registry_get(i);

            lv_obj_t *row = lv_button_create(list);
            lv_obj_set_size(row, LV_PCT(100), ROW_HEIGHT);
            lv_obj_set_style_bg_color(row, lv_color_hex(0x1E1E28), LV_PART_MAIN);
            lv_obj_set_style_radius(row, 12, LV_PART_MAIN);
            lv_obj_add_event_cb(row, app_row_clicked, LV_EVENT_CLICKED, (void *)app);

            lv_obj_t *label = lv_label_create(row);
            lv_label_set_text(label, app->name);
            lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
            lv_obj_center(label);
        }
    }

    lv_screen_load(s_launcher_screen);
    bsp_display_unlock();
}

/* PWR button (EXIO4, active high) is the universal way back to the launcher.
 * It is deliberately hardware: no app can consume it or paint over it, so a
 * misbehaving app can always be escaped. Holding it >=6s still powers the
 * board off -- that is the AXP2101 acting below us, not something we handle.
 *
 * Runs whether or not an app is up: pressing it with no app running is a
 * harmless no-op. */
static void back_button_task(void *arg)
{
    (void)arg;
    bool was_pressed = false;

    for (;;) {
        uint32_t level = 0;
        esp_err_t err = ESP_FAIL;
        if (s_expander != NULL) {
            err = esp_io_expander_get_level(s_expander, EXIO_PWR_BTN, &level);
        }
        if (err == ESP_OK) {
            bool pressed = (level != 0);

            /* Request stop unconditionally. Harmless when nothing is running,
             * and it avoids depending on s_app_task being assigned yet --
             * gating on that was why the first version never returned. */
            if (pressed && !was_pressed) {
                ESP_LOGI(TAG, "PWR pressed -- returning to launcher");
                launcher_lua_request_stop(true);
            }
            was_pressed = pressed;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void app_main(void)
{
    printf("\n=== ESP32-S3-Touch-AMOLED-1.8 Launcher ===\n");
    printf("LVGL %d.%d.%d / %s\n",
           LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH, LUA_RELEASE);

    /* Created before anything that can launch an app (UI build, serial
     * task): app_row_clicked, launcher_run_app_by_name, launcher_stop_app,
     * and lua_app_task's own exit path all take this. */
    s_app_mutex = xSemaphoreCreateMutex();
    if (s_app_mutex == NULL) {
        ESP_LOGE(TAG, "failed to create app mutex");
        return;
    }

    ESP_ERROR_CHECK(release_panel_reset());

    lv_display_t *disp = bsp_display_start();
    if (disp == NULL) {
        ESP_LOGE(TAG, "bsp_display_start() failed");
        return;
    }
    bsp_display_backlight_on();

    /* Hand the BSP display to the service the Lua LVGL binding talks to. */
    display_service_attach(disp);
    ESP_ERROR_CHECK(lua_module_lvgl_register_with_data_root(BSP_SD_MOUNT_POINT));

    app_registry_scan();
    build_launcher_ui();
    xTaskCreate(back_button_task, "back_btn", 3072, NULL, 6, NULL);
    serial_push_start();

    ESP_LOGI(TAG, "ready: %u app(s), internal free=%u psram free=%u",
             (unsigned)app_registry_count(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}
