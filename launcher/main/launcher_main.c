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
#include "esp_timer.h"
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
#include "app_sandbox.h"
#include "app_timer.h"
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

/* Message handler for lua_pcall: converts the error into a traceback. Without
 * this you get a bare one-line message with no call stack -- exactly what
 * happened before this was added: one untraceable line on serial and a
 * silent return to the launcher. */
static int traceback_handler(lua_State *L)
{
    const char *msg = lua_tostring(L, 1);
    luaL_traceback(L, L, msg ? msg : "(non-string error)", 1);
    return 1;
}

/* Body label sits between the title (top, ~44px) and the "press PWR" hint
 * (bottom, ~40px) on the 368x448 panel. Capped and scrollable so a deep
 * traceback stays reachable instead of overlapping the hint or being
 * silently clipped. */
#define ERROR_BODY_TOP     44
#define ERROR_BODY_HEIGHT  330

/* Show a failure on the panel. Five people debugging through one USB cable is
 * miserable; the error belongs where they are already looking. Returns the
 * screen object so the caller can delete it once it is no longer displayed --
 * lv_screen_load() does not free the screen it replaces, and nothing else
 * can see this one to clean it up. */
static lv_obj_t *show_error_screen(const char *app_name, const char *msg)
{
    bsp_display_lock(0);

    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x2A0E0E), LV_PART_MAIN);
    lv_obj_set_style_pad_all(scr, 12, LV_PART_MAIN);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text_fmt(title, "%s failed", app_name);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFF6B6B), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    lv_obj_t *body = lv_label_create(scr);
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(body, LV_PCT(96));
    lv_obj_set_height(body, ERROR_BODY_HEIGHT);
    lv_obj_add_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);
    lv_label_set_text(body, msg ? msg : "(no message)");
    lv_obj_set_style_text_color(body, lv_color_hex(0xE0E0E0), LV_PART_MAIN);
    lv_obj_align(body, LV_ALIGN_TOP_LEFT, 0, ERROR_BODY_TOP);

    lv_obj_t *hint = lv_label_create(scr);
    lv_label_set_text(hint, "press PWR to go back");
    lv_obj_set_style_text_color(hint, lv_color_hex(0x9A9AA5), LV_PART_MAIN);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -8);

    lv_screen_load(scr);
    bsp_display_unlock();
    return scr;
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
    app_timer_reset(L);   /* no timers leak in from a previous app */
    app_sandbox_apply(L);
    app_sandbox_install_hook(L);

    lua_pushcfunction(L, traceback_handler);
    int errfunc = lua_gettop(L);

    if (luaL_loadfile(L, app->path) != LUA_OK ||
        lua_pcall(L, 0, 0, errfunc) != LUA_OK) {
        const char *msg = lua_tostring(L, -1);

        if (cap_lua_runtime_stop_requested(L)) {
            /* The interrupt hook raises a Lua error to unwind a runaway app
             * when PWR/STOP was pressed deliberately -- that is not a crash
             * and must not flash the red error screen. Check the atomic
             * stop flag, never the message string: an app can raise this
             * exact text itself (deliberately, or by re-raising a caught
             * error), and comparing strings would let the launcher silently
             * swallow a genuine crash. */
            ESP_LOGI(TAG, "app '%s' stopped: %s", app->name, msg ? msg : "(nil)");
            lua_lvgl_force_unlock_if_held();
            lua_settop(L, 0);
            goto close;
        }

        ESP_LOGE(TAG, "app '%s' failed: %s", app->name, msg ? msg : "(nil)");
        /* The error may have unwound out of an LVGL binding that was holding
         * the display lock. Without this the LVGL task blocks forever. The
         * error screen's own bsp_display_lock() below must come after this. */
        lua_lvgl_force_unlock_if_held();
        lv_obj_t *err_scr = show_error_screen(app->name, msg);
        /* msg has now been copied into the label; safe to drop the Lua
         * stack values (message handler + error string) it pointed into. */
        lua_settop(L, 0);

        /* Leave the error on screen; PWR (via STOP / cap_lua_runtime_stop_requested)
         * returns to the launcher. */
        while (!cap_lua_runtime_stop_requested(L)) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }

        /* Load the launcher screen before deleting the error screen so the
         * currently-active screen is never the one being freed. Nothing
         * else can see err_scr to clean it up -- lv_screen_load() does not
         * free the screen it replaces, so without this every crash leaked
         * a screen plus its three labels. */
        show_launcher_screen();
        bsp_display_lock(0);
        lv_obj_delete(err_scr);
        bsp_display_unlock();
        goto close;
    }
    lua_remove(L, errfunc);
    lua_settop(L, 0);

    ESP_LOGI(TAG, "app '%s' running, vm psram cost = %d bytes", app->name,
             (int)(psram_before - heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
    /* Widget callback errors are swallowed by design (dispatch must keep
     * going so one dead button doesn't take the whole app down), and logged
     * by the vendored binding under tag 'lua_lvgl_evt' -- point people at it
     * once per run so a dead button isn't a total dead end. */
    ESP_LOGI(TAG, "app '%s' running (callback errors appear under tag 'lua_lvgl_evt')",
             app->name);

    /* Event pump: this is what makes Lua callbacks actually fire. Timers run
     * first each iteration, and the wait is shortened to the next timer
     * deadline so a short timer is not stuck behind a long event wait. */
    while (!cap_lua_runtime_stop_requested(L)) {
        int64_t next_due = app_timer_run_due(L);

        int wait_ms = EVENT_PUMP_MS;
        if (next_due != INT64_MAX) {
            int64_t delta_ms = (next_due - esp_timer_get_time()) / 1000;
            if (delta_ms < 0) {
                delta_ms = 0;
            }
            if (delta_ms < wait_ms) {
                wait_ms = (int)delta_ms;
            }
        }
        if (!pump_events(L, wait_ms)) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }

close:
    app_timer_reset(L);
    launcher_lua_run_exit_cleanup(L);
    lua_close(L);
    {
        /* internal_free is ESP heap; lv_mem free_size is LVGL's own fixed
         * 64 KB pool (CONFIG_LV_MEM_SIZE_KILOBYTES) that the error screen
         * is carved out of -- the pool a leaked error screen would exhaust
         * over repeated crashes. Logged here so the leak (and its fix) is
         * visible on every app close, not just crashes. */
        lv_mem_monitor_t mon;
        lv_mem_monitor(&mon);
        ESP_LOGI(TAG, "app '%s' closed, psram free=%u, internal free=%u, "
                 "lv_mem free=%u/%u",
                 app->name,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)mon.free_size, (unsigned)mon.total_size);
    }

out:
    show_launcher_screen();
    xSemaphoreTake(s_app_mutex, portMAX_DELAY);
    s_app_task = NULL;
    xSemaphoreGive(s_app_mutex);
    vTaskDelete(NULL);
}

/* "/sdcard/apps/counter.lua" -> "counter.lua" */
static const char *path_basename(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

/* Find the currently-registered app matching `basename` (e.g. "counter.lua").
 * Looked up fresh every time rather than cached, because app_registry's
 * static array can be rewritten in place by a rescan (Refresh, or a serial
 * PUSH) between when a row was built and when it is tapped. */
static const app_entry_t *find_app_by_basename(const char *basename)
{
    size_t count = app_registry_count();
    for (size_t i = 0; i < count; i++) {
        const app_entry_t *app = app_registry_get(i);
        if (strcmp(path_basename(app->path), basename) == 0) {
            return app;
        }
    }
    return NULL;
}

/* Frees the heap copy of a row's basename (see build_launcher_ui) when LVGL
 * deletes the row -- on Refresh's full rebuild and on ordinary screen
 * teardown alike. */
static void row_data_delete_cb(lv_event_t *e)
{
    free(lv_event_get_user_data(e));
}

/* Rows store a heap-allocated copy of the app's basename, not a raw
 * app_entry_t* into app_registry's static array. That array is rewritten in
 * place by every rescan -- Refresh, and (since PUSH also rescans at runtime)
 * an ordinary file push too -- so a raw pointer captured when the row was
 * built could point at a different app, or garbage, by the time it is
 * tapped. Resolving by name at click time makes a stale row a no-op instead
 * of a wrong-app launch. */
static void app_row_clicked(lv_event_t *e)
{
    const char *basename = (const char *)lv_event_get_user_data(e);
    if (basename == NULL) {
        return;   /* strdup() failed when the row was built; nothing to launch */
    }

    xSemaphoreTake(s_app_mutex, portMAX_DELAY);
    if (s_app_task != NULL) {
        xSemaphoreGive(s_app_mutex);
        ESP_LOGW(TAG, "an app is already running");
        return;
    }

    const app_entry_t *match = find_app_by_basename(basename);
    if (match == NULL) {
        xSemaphoreGive(s_app_mutex);
        ESP_LOGW(TAG, "tapped row '%s' is no longer in the registry", basename);
        return;
    }

    s_current_app = *match;
    xTaskCreate(lua_app_task, "lua_app", APP_TASK_STACK,
                NULL, 5, &s_app_task);
    xSemaphoreGive(s_app_mutex);
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

    const app_entry_t *match = find_app_by_basename(basename);
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

static void build_launcher_ui(void);

/* Rebuilds the whole UI rather than reusing rows: every row holds a heap
 * copy of its app's basename (see build_launcher_ui / find_app_by_basename),
 * resolved fresh at tap time, so a stale row is merely a no-op rather than a
 * wrong-app launch -- but the row list itself still needs to be rebuilt to
 * reflect apps added or removed since the last scan. */
static void refresh_clicked(lv_event_t *e)
{
    (void)e;

    xSemaphoreTake(s_app_mutex, portMAX_DELAY);
    if (s_app_task != NULL) {
        xSemaphoreGive(s_app_mutex);
        return;   /* never rescan or rebuild under a running app */
    }

    lv_obj_t *old = s_launcher_screen;

    app_registry_invalidate();
    app_registry_scan();
    build_launcher_ui();

    /* build_launcher_ui() already loaded the new screen; delete the old one
     * now that it is no longer the active screen. Never delete the screen
     * that is currently on-screen -- that path is only reached here because
     * the new screen has already replaced it. */
    if (old != NULL && old != s_launcher_screen) {
        lv_obj_delete(old);
    }

    xSemaphoreGive(s_app_mutex);
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

    lv_obj_t *refresh = lv_button_create(s_launcher_screen);
    lv_obj_set_size(refresh, 200, 100);   /* >= 200x100; smaller drops taps */
    lv_obj_align(refresh, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_style_bg_color(refresh, lv_color_hex(0x24303C), LV_PART_MAIN);
    lv_obj_add_event_cb(refresh, refresh_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *rlabel = lv_label_create(refresh);
    lv_label_set_text(rlabel, "Refresh");
    lv_obj_set_style_text_color(rlabel, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_center(rlabel);

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

            /* Own copy of the basename, not a pointer into app_registry's
             * static array: a rescan (Refresh, or a serial PUSH) rewrites
             * that array in place, and this row can outlive the scan that
             * built it. Freed in row_data_delete_cb when LVGL deletes the
             * row. */
            char *basename = strdup(path_basename(app->path));

            lv_obj_t *row = lv_button_create(list);
            lv_obj_set_size(row, LV_PCT(100), ROW_HEIGHT);
            lv_obj_set_style_bg_color(row, lv_color_hex(0x1E1E28), LV_PART_MAIN);
            lv_obj_set_style_radius(row, 12, LV_PART_MAIN);
            lv_obj_add_event_cb(row, app_row_clicked, LV_EVENT_CLICKED, basename);
            lv_obj_add_event_cb(row, row_data_delete_cb, LV_EVENT_DELETE, basename);

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
    ESP_ERROR_CHECK(app_timer_register());

    app_registry_scan();
    build_launcher_ui();
    xTaskCreate(back_button_task, "back_btn", 3072, NULL, 6, NULL);
    serial_push_start();

    ESP_LOGI(TAG, "ready: %u app(s), internal free=%u psram free=%u",
             (unsigned)app_registry_count(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}
