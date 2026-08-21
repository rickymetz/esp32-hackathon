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
#include "app_button.h"
#include "lv_font_lexend.h"
#include "lua_module_ui.h"
#include "app_voice.h"
#include "driver/gpio.h"
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
 * one dropped roughly half. Both watchOS (44pt) and Wear OS (48dp) land on
 * 88-96px at this panel's 2x scale; 104px is Wear OS's standard Chip height
 * and comfortable here -- see docs/DESIGN_GUIDE.md. Keep launcher rows at
 * least this tall. */
#define ROW_HEIGHT      104

/* I3: caps rows actually rendered, independent of APP_MAX_COUNT (32). Only
 * Bounds refresh_clicked()'s peak widget count (it builds the new screen
 * before deleting the old one, so peak is ~2x one screen's rows). The old
 * cap of 16 guarded a fixed internal LVGL pool that no longer exists --
 * ff9acc2 moved LVGL's heap to PSRAM, where 2x64 rows is noise -- and it
 * ended up hiding real apps behind a "more not shown" label on a card with
 * 23. The list scrolls; 64 is a pathological-card backstop, not a UI
 * limit. A truncated list still says so rather than silently hiding apps. */
#define MAX_VISIBLE_ROWS 64

static lv_obj_t *s_launcher_screen;

/* ---- Synthetic touch injection (serial TAP/SWIPE; see launcher_main.h).
 * Single-writer (serial task) / single-reader (LVGL task via read_cb),
 * guarded by a spinlock because the fields must change atomically. ---- */
typedef struct {
    bool    active;
    int     x0, y0, x1, y1;
    int64_t start_us;
    int64_t dur_us;
} synth_touch_t;

/* Queued, not overwritten: back-to-back serial TAPs used to clobber the
 * in-flight gesture before its release was read, merging three taps into
 * one long press (caught driving the stepper: +3 registered as +1). A
 * small ring holds pending gestures; the read_cb starts the next only
 * after the current one released AND an enforced released-state gap. */
#define SYNTH_QUEUE  8
#define SYNTH_GAP_US 90000

static portMUX_TYPE s_synth_lock = portMUX_INITIALIZER_UNLOCKED;
static synth_touch_t s_synth_q[SYNTH_QUEUE];
static int s_synth_head, s_synth_count;   /* head = next to run */
static synth_touch_t s_synth_cur;         /* .active = running now */
static int64_t s_synth_idle_since;

void launcher_input_inject(int x0, int y0, int x1, int y1, int duration_ms)
{
    if (duration_ms < 60) duration_ms = 60;
    if (duration_ms > 2000) duration_ms = 2000;

    portENTER_CRITICAL(&s_synth_lock);
    if (s_synth_count < SYNTH_QUEUE) {
        int slot = (s_synth_head + s_synth_count) % SYNTH_QUEUE;
        s_synth_q[slot] = (synth_touch_t){
            .active = true,
            .x0 = x0, .y0 = y0, .x1 = x1, .y1 = y1,
            .start_us = 0,
            .dur_us = (int64_t)duration_ms * 1000,
        };
        s_synth_count++;
    }
    portEXIT_CRITICAL(&s_synth_lock);
}

/* Runs on the LVGL task. Plays the current gesture (PRESSED along the
 * interpolated path, one RELEASED at the end), then waits SYNTH_GAP_US
 * in the released state before dequeuing the next. */
static void synth_indev_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    int64_t now = esp_timer_get_time();

    portENTER_CRITICAL(&s_synth_lock);
    if (!s_synth_cur.active && s_synth_count > 0 &&
        now - s_synth_idle_since >= SYNTH_GAP_US) {
        s_synth_cur = s_synth_q[s_synth_head];
        s_synth_cur.start_us = now;
        s_synth_head = (s_synth_head + 1) % SYNTH_QUEUE;
        s_synth_count--;
    }
    synth_touch_t t = s_synth_cur;
    portEXIT_CRITICAL(&s_synth_lock);

    if (!t.active) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    int64_t el = now - t.start_us;
    if (el >= t.dur_us) {
        data->point.x = t.x1;
        data->point.y = t.y1;
        data->state = LV_INDEV_STATE_RELEASED;
        portENTER_CRITICAL(&s_synth_lock);
        s_synth_cur.active = false;
        s_synth_idle_since = now;
        portEXIT_CRITICAL(&s_synth_lock);
        return;
    }

    data->point.x = t.x0 + (int)((int64_t)(t.x1 - t.x0) * el / t.dur_us);
    data->point.y = t.y0 + (int)((int64_t)(t.y1 - t.y0) * el / t.dur_us);
    data->state = LV_INDEV_STATE_PRESSED;
}
static TaskHandle_t s_app_task;
static esp_io_expander_handle_t s_expander;

/* Guards s_app_task's check-then-act (launch/stop) across the three tasks
 * that touch it: the LVGL/UI task (app_row_clicked), the serial task
 * (launcher_run_app_by_name / launcher_stop_app), and lua_app_task itself
 * clearing it on exit. Created in app_main() before anything can launch. */
static SemaphoreHandle_t s_app_mutex;

/* Single-app-at-a-time means a single static copy is enough: the launch
 * path fills this in (under s_app_mutex) from app_registry_find_by_basename()
 * just before starting the task, and lua_app_task reads only from this copy
 * for its whole run. That avoids holding a pointer into s_apps[] across a
 * run, which app_registry_scan() can rewrite at any time now that a serial
 * PUSH rescans at runtime. */
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

/* I4: raised by lua_module_lvgl's process_events whenever the app has never
 * called lvgl.init() -- nothing in the app contract requires it, so a
 * timer-only, sensor-only, or print-only app must not be treated as failed
 * just because there is no LVGL runtime to drain. */
#define LVGL_NOT_INITIALIZED_ERRMSG "lvgl runtime is not initialized"

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
        const char *msg = lua_tostring(L, -1);

        if (msg != NULL && strstr(msg, LVGL_NOT_INITIALIZED_ERRMSG) != NULL) {
            /* Not a crash -- there is simply nothing queued to drain. Sleep
             * for the requested wait ourselves (process_events never got to)
             * so app_timer_run_due()'s timers keep firing on schedule and
             * the app stays alive instead of being torn down after its first
             * pump. */
            lua_pop(L, 2);
            if (timeout_ms > 0) {
                vTaskDelay(pdMS_TO_TICKS(timeout_ms));
            }
            return true;
        }

        ESP_LOGE(TAG, "process_events failed: %s", msg);
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

/* Body label sits between the title (Montserrat 40, line_height 44, top ~8px)
 * and the "press the top button" hint (default font is Montserrat 32, line_height
 * 35, bottom ~8px) on the 368x448 panel. Capped and scrollable so a deep
 * traceback stays reachable instead of overlapping the hint or being
 * silently clipped. */
#define ERROR_BODY_TOP     60
#define ERROR_BODY_HEIGHT  300

/* Show a failure on the panel. Five people debugging through one USB cable is
 * miserable; the error belongs where they are already looking. Returns the
 * screen object so the caller can delete it once it is no longer displayed --
 * lv_screen_load() does not free the screen it replaces, and nothing else
 * can see this one to clean it up. */
static lv_obj_t *show_error_screen(const char *app_name, const char *msg)
{
    bsp_display_lock(0);

    lv_obj_t *scr = lv_obj_create(NULL);
    /* True black, not a red field: the guide warns against full-screen
     * colour on a long-lived view (this can sit up until BOOT is pressed).
     * The red title alone carries the "something failed" signal. */
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_pad_all(scr, 12, LV_PART_MAIN);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text_fmt(title, "%s failed", app_name);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFF6B6B), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &lv_font_lexend_40, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    lv_obj_t *body = lv_label_create(scr);
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(body, LV_PCT(96));
    lv_obj_set_height(body, ERROR_BODY_HEIGHT);
    lv_obj_add_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);
    lv_label_set_text(body, msg ? msg : "(no message)");
    lv_obj_set_style_text_color(body, lv_color_hex(0xE0E0E0), LV_PART_MAIN);
    lv_obj_set_style_text_font(body, &lv_font_lexend_26, LV_PART_MAIN);
    lv_obj_align(body, LV_ALIGN_TOP_LEFT, 0, ERROR_BODY_TOP);

    lv_obj_t *hint = lv_label_create(scr);
    lv_label_set_text(hint, "press the top button to go back");
    lv_obj_set_style_text_color(hint, lv_color_hex(0x9A9AA5), LV_PART_MAIN);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -8);

    lv_screen_load(scr);
    bsp_display_unlock();
    return scr;
}

/* Shared by every path that ends an app run with an error on screen: shows
 * `msg`, blocks until BOOT/STOP asks to go back, then restores the launcher
 * screen and frees the error screen. Never call this while still holding an
 * LVGL lock the error path itself needs (see lua_lvgl_force_unlock_if_held()
 * at each call site below). */
static void show_error_and_wait_for_stop(lua_State *L, const char *app_name, const char *msg)
{
    lv_obj_t *err_scr = show_error_screen(app_name, msg);
    /* msg may point into the Lua stack; it has been copied into the label,
     * so it is safe to drop now. */
    lua_settop(L, 0);

    while (!cap_lua_runtime_stop_requested(L)) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    /* Load the launcher screen before deleting the error screen so the
     * currently-active screen is never the one being freed. Nothing else can
     * see err_scr to clean it up -- lv_screen_load() does not free the
     * screen it replaces, so without this every crash leaked a screen plus
     * its labels. */
    show_launcher_screen();
    bsp_display_lock(0);
    lv_obj_delete(err_scr);
    bsp_display_unlock();
}

/* I6: everything a fresh lua_State needs before any app code loads --
 * opening the standard libs, installing every cap_lua module (this is what
 * runs luaopen_lvgl, building ~45 LVGL metatables, and the timer module's
 * opener), resetting timer state, and applying the sandbox. Pushed as a
 * C function and run via lua_pcall (see lua_app_task) rather than called
 * directly: with no protected call yet on this lua_State, an error anywhere
 * in this chain -- an allocation failure while building those metatables,
 * a future module that can fail to open -- has no error jump buffer to
 * unwind through, and Lua's default panic function calls abort(), taking
 * the whole board down with it. */
static int lua_setup_state(lua_State *L)
{
    luaL_openlibs(L);

    esp_err_t err = launcher_lua_open_modules(L);
    if (err != ESP_OK) {
        /* Was ESP_ERROR_CHECK(): that aborts on failure. Raising a Lua error
         * instead lets the lua_pcall around this function catch it, so the
         * caller can report it on screen rather than panicking. */
        return luaL_error(L, "launcher_lua_open_modules failed: %s", esp_err_to_name(err));
    }

    app_timer_reset(L);   /* no timers leak in from a previous app */
    app_button_reset(L);  /* nor edges recorded before this app launched */
    app_voice_reset(L);   /* nor a capture someone left running */
    app_sandbox_apply(L);
    app_sandbox_install_hook(L);
    return 0;
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
    lua_pushcfunction(L, lua_setup_state);
    if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
        const char *msg = lua_tostring(L, -1);
        ESP_LOGE(TAG, "lua state setup failed for '%s': %s", app->name, msg ? msg : "(nil)");
        /* Nothing here can be holding the LVGL lock yet, but every other
         * error exit calls this first -- keep it uniform in case that ever
         * changes. */
        lua_lvgl_force_unlock_if_held();
        show_error_and_wait_for_stop(L, app->name, msg ? msg : "lua state setup failed");
        goto close;
    }

    lua_pushcfunction(L, traceback_handler);
    int errfunc = lua_gettop(L);

    if (luaL_loadfile(L, app->path) != LUA_OK ||
        lua_pcall(L, 0, 0, errfunc) != LUA_OK) {
        const char *msg = lua_tostring(L, -1);

        if (cap_lua_runtime_stop_requested(L)) {
            /* The interrupt hook raises a Lua error to unwind a runaway app
             * when BOOT/STOP was pressed deliberately -- that is not a crash
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
        show_error_and_wait_for_stop(L, app->name, msg);
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
        app_button_run_pending(L);
        app_voice_run_pending(L);
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
    /* Unconditional: a task can never be deleted below while still owning
     * the LVGL mutex, whatever path got us here. */
    lua_lvgl_force_unlock_if_held();
    app_timer_reset(L);
    app_button_reset(L);
    app_voice_reset(L);
    launcher_lua_run_exit_cleanup(L);
    lua_close(L);
    {
        /* internal_free is ESP heap. LVGL now uses the C stdlib allocator
         * (CONFIG_LV_USE_CLIB_MALLOC) instead of a fixed internal pool, so
         * lv_mem_monitor()'s core is a no-op here and free/total report as
         * 0/0 -- that is expected, not a bug. A leaked error screen would
         * now show up as a drop in internal_free/psram free above instead
         * of exhausting a fixed LVGL pool, which is why those two numbers
         * are logged on every app close, not just crashes. */
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
 * tapped. Resolving by name at click time via app_registry_find_by_basename()
 * makes a stale row a no-op instead of a wrong-app launch or a crash: the
 * lookup and the copy-out happen under the registry's own lock, so a PUSH
 * rescanning concurrently on the serial task cannot be observed half-done. */
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

    app_entry_t match;
    if (!app_registry_find_by_basename(basename, &match)) {
        xSemaphoreGive(s_app_mutex);
        ESP_LOGW(TAG, "tapped row '%s' is no longer in the registry", basename);
        return;
    }

    s_current_app = match;
    xTaskCreate(lua_app_task, "lua_app", APP_TASK_STACK,
                NULL, 5, &s_app_task);
    xSemaphoreGive(s_app_mutex);
}

/* Launcher-side API for other modules (serial_push) to drive app lifecycle
 * the same way a screen tap or the BOOT button would. Reuses the exact same
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

    app_entry_t match;
    if (!app_registry_find_by_basename(basename, &match)) {
        xSemaphoreGive(s_app_mutex);
        ESP_LOGW(TAG, "RUN '%s': not found", basename);
        return false;
    }

    s_current_app = match;
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
 * copy of its app's basename (see build_launcher_ui / app_registry_find_by_basename),
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
     * the new screen has already replaced it.
     *
     * Explicitly locked even though refresh_clicked() only ever runs inside
     * the LVGL task's own (recursive) lock today: that is an implicit
     * invariant, not something lv_obj_delete() enforces, and it would break
     * silently if Refresh is ever triggered from elsewhere -- e.g. a future
     * serial REFRESH command, alongside the existing RUN/STOP. */
    if (old != NULL && old != s_launcher_screen) {
        bsp_display_lock(0);
        lv_obj_delete(old);
        bsp_display_unlock();
    }

    xSemaphoreGive(s_app_mutex);
}

static void build_launcher_ui(void)
{
    size_t count = app_registry_count();

    bsp_display_lock(0);

    s_launcher_screen = lv_obj_create(NULL);
    /* True black, not near-black: watchOS/Wear OS are dark-theme only, and on
     * OLED a black pixel is an off pixel -- see docs/DESIGN_GUIDE.md. */
    lv_obj_set_style_bg_color(s_launcher_screen, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_launcher_screen, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_launcher_screen, 0, LV_PART_MAIN);

    lv_obj_t *header = lv_label_create(s_launcher_screen);
    lv_label_set_text(header, "Apps");
    lv_obj_set_style_text_color(header, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(header, &lv_font_lexend_40, LV_PART_MAIN);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 24);

    if (count == 0) {
        /* Nothing else on screen, so a centered standalone button is safe
         * here -- and necessary: after inserting a card this is the only
         * way to rescan without rebooting. */
        lv_obj_t *refresh = lv_button_create(s_launcher_screen);
        lv_obj_set_size(refresh, 200, 104);   /* >= 200x104; smaller drops taps */
        lv_obj_align(refresh, LV_ALIGN_BOTTOM_MID, 0, -16);
        lv_obj_set_style_bg_color(refresh, lv_color_hex(0x24303C), LV_PART_MAIN);
        lv_obj_add_event_cb(refresh, refresh_clicked, LV_EVENT_CLICKED, NULL);

        lv_obj_t *rlabel = lv_label_create(refresh);
        lv_label_set_text(rlabel, "Refresh");
        lv_obj_set_style_text_color(rlabel, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
        lv_obj_center(rlabel);

        lv_obj_t *empty = lv_label_create(s_launcher_screen);
        lv_label_set_text(empty, app_registry_sd_mounted()
                                     ? "No apps yet.\nCopy .lua files to\n/apps on the SD card."
                                     : "No SD card.\nInsert one with an\n/apps directory.");
        lv_obj_set_style_text_color(empty, lv_color_hex(0x8A8A99), LV_PART_MAIN);
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_set_style_text_font(empty, &lv_font_lexend_26, LV_PART_MAIN);
        lv_obj_center(empty);
    } else {
        lv_obj_t *list = lv_obj_create(s_launcher_screen);
        lv_obj_set_size(list, LV_PCT(100), LV_PCT(84));
        lv_obj_align(list, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(list, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(list, 12, LV_PART_MAIN);
        lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(list, 16, LV_PART_MAIN);

        size_t visible = (count < MAX_VISIBLE_ROWS) ? count : MAX_VISIBLE_ROWS;
        for (size_t i = 0; i < visible; i++) {
            app_entry_t app;
            if (!app_registry_get_copy(i, &app)) {
                break;   /* end of list, or the array shrank under us */
            }

            /* Own copy of the basename, not a pointer into app_registry's
             * static array: a rescan (Refresh, or a serial PUSH) rewrites
             * that array in place, and this row can outlive the scan that
             * built it. Freed in row_data_delete_cb when LVGL deletes the
             * row. */
            char *basename = strdup(path_basename(app.path));

            lv_obj_t *row = lv_button_create(list);
            lv_obj_set_size(row, LV_PCT(100), ROW_HEIGHT);
            lv_obj_set_style_bg_color(row, lv_color_hex(0x1E1E28), LV_PART_MAIN);
            lv_obj_set_style_radius(row, 12, LV_PART_MAIN);
            lv_obj_add_event_cb(row, app_row_clicked, LV_EVENT_CLICKED, basename);
            lv_obj_add_event_cb(row, row_data_delete_cb, LV_EVENT_DELETE, basename);

            lv_obj_t *label = lv_label_create(row);
            lv_label_set_text(label, app.name);
            lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
            lv_obj_set_style_text_font(label, &lv_font_lexend_32, LV_PART_MAIN);
            /* Leading-aligned like every ui.row: the review flagged the
             * centered launcher rows as a second list grammar. */
            lv_obj_align(label, LV_ALIGN_LEFT_MID, 16, 0);

            if (i == 0) {
                /* Structural proof (no board display to look at) that the
                 * 32px font is actually resolved on a row label, not just
                 * requested: log the font object LVGL resolved for this
                 * label and its line_height. Montserrat 14's line_height is
                 * 16px; Lexend 32's is 36px. */
                const lv_font_t *resolved =
                    lv_obj_get_style_text_font(label, LV_PART_MAIN);
                ESP_LOGI(TAG, "row label font check: resolved=%p want=%p "
                         "(lexend_32), line_height=%d",
                         (const void *)resolved,
                         (const void *)&lv_font_lexend_32,
                         lv_font_get_line_height(resolved));
            }
        }

        if (count > MAX_VISIBLE_ROWS) {
            /* Silent truncation would be worse than the bug this caps --
             * say what's missing instead of just hiding it. */
            lv_obj_t *more = lv_label_create(list);
            lv_label_set_text_fmt(more, "%u more not shown",
                                   (unsigned)(count - MAX_VISIBLE_ROWS));
            lv_obj_set_style_text_color(more, lv_color_hex(0x8A8A99), LV_PART_MAIN);
            lv_obj_set_style_text_font(more, &lv_font_lexend_26, LV_PART_MAIN);
            lv_obj_set_style_text_align(more, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
            lv_obj_set_width(more, LV_PCT(100));
        }

        /* Refresh lives IN the list, as its last full-width row: the list
         * container spans the screen bottom and its rows paint over anything
         * behind it, which is exactly how the old standalone bottom button
         * ended up invisible behind 3+ rows (found by Rick on device). A row
         * scrolls into reach no matter how many apps precede it. */
        lv_obj_t *refresh = lv_button_create(list);
        lv_obj_set_size(refresh, LV_PCT(100), ROW_HEIGHT);
        lv_obj_set_style_bg_color(refresh, lv_color_hex(0x24303C), LV_PART_MAIN);
        lv_obj_set_style_radius(refresh, 12, LV_PART_MAIN);
        lv_obj_add_event_cb(refresh, refresh_clicked, LV_EVENT_CLICKED, NULL);

        lv_obj_t *rlabel = lv_label_create(refresh);
        lv_label_set_text(rlabel, "Refresh");
        lv_obj_set_style_text_color(rlabel, lv_color_hex(0x9FB4C7), LV_PART_MAIN);
        lv_obj_center(rlabel);
    }

    lv_screen_load(s_launcher_screen);
    bsp_display_unlock();
}

/* BOOT (GPIO0, top right) is Home: the universal way back to the launcher.
 * It is deliberately hardware: no app can consume it or paint over it, so a
 * misbehaving app can always be escaped. PWR (EXIO4, bottom right) belongs
 * to apps via the `button` module -- but holding PWR >=6s still powers the
 * board off, which is the AXP2101 acting below us, not something we handle.
 *
 * Runs whether or not an app is up: pressing BOOT with no app running is a
 * harmless no-op. */

/* Two-consecutive-sample debounce on the 20 ms poll tick. Returns true
 * exactly when the stable level changes, with the new level in *stable. */
typedef struct {
    bool raw_prev;
    bool stable;
} debounce_t;

static bool debounce_step(debounce_t *d, bool raw, bool *stable)
{
    bool edge = false;

    if (raw == d->raw_prev && raw != d->stable) {
        d->stable = raw;
        edge = true;
    }
    d->raw_prev = raw;
    *stable = d->stable;
    return edge;
}

/* Polls both physical buttons every 20 ms.
 *
 * BOOT (GPIO0, top right) is Home: a press requests app stop,
 * unconditionally -- harmless when nothing is running, and it must not gate
 * on s_app_task being assigned (gating was why the first version never
 * returned). A direct GPIO read with no bus dependency: unlike the old
 * PWR-based back button this survives an I2C wedge, so the escape hatch is
 * strictly stronger than before.
 *
 * PWR (EXIO4, bottom right, via the I2C expander) belongs to apps: edges go
 * to the button module, which the app task drains. This task never touches
 * the Lua state. A failed expander read skips the sample -- never treat a
 * failed read as an edge. */
static void button_poll_task(void *arg)
{
    (void)arg;
    debounce_t boot_db = {0};
    debounce_t pwr_db = {0};
    bool level;

    for (;;) {
        /* BOOT: active low. */
        if (debounce_step(&boot_db, gpio_get_level(GPIO_NUM_0) == 0, &level) && level) {
            ESP_LOGI(TAG, "BOOT pressed -- returning to launcher");
            launcher_lua_request_stop(true);
        }

        /* PWR: active high, behind the expander. */
        if (s_expander != NULL) {
            uint32_t raw = 0;
            if (esp_io_expander_get_level(s_expander, EXIO_PWR_BTN, &raw) == ESP_OK) {
                if (debounce_step(&pwr_db, raw != 0, &level)) {
                    app_button_record_edge(level);
                }
            }
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

    /* Lexend as the theme default font. LVGL's Kconfig LV_FONT_DEFAULT
     * choice only offers its bundled faces, so the swap happens here via
     * the theme instead: every widget on this display -- launcher and app
     * screens alike -- resolves Lexend 32 unless a style overrides it.
     * LV_FONT_DEFAULT stays Montserrat 14 (sdkconfig) purely so the macro
     * still resolves; nothing renders it. Locked: the LVGL task is already
     * live once bsp_display_start() returns. */
    bsp_display_lock(0);
    lv_display_set_theme(disp,
        lv_theme_default_init(disp,
                              lv_palette_main(LV_PALETTE_BLUE),
                              lv_palette_main(LV_PALETTE_RED),
                              true /* dark */,
                              &lv_font_lexend_32));
    bsp_display_unlock();

    /* Synthetic touch indev for serial TAP/SWIPE -- same event pipeline
     * as the real digitizer, so widgets cannot tell the difference. */
    bsp_display_lock(0);
    {
        lv_indev_t *synth = lv_indev_create();
        lv_indev_set_type(synth, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(synth, synth_indev_read);
    }
    bsp_display_unlock();

    /* Hand the BSP display to the service the Lua LVGL binding talks to. */
    display_service_attach(disp);
    ESP_ERROR_CHECK(lua_module_lvgl_register_with_data_root(BSP_SD_MOUNT_POINT));
    ESP_ERROR_CHECK(app_timer_register());
    ESP_ERROR_CHECK(app_button_register());
    /* After lvgl and timer: ui.lua/keyboard.lua require() both at load. */
    ESP_ERROR_CHECK(lua_module_ui_register());
    ESP_ERROR_CHECK(app_voice_register());

    /* BOOT (GPIO0) is the Home button. Input + pull-up matches its idle
     * state; it is only special during reset, where the ROM samples it as a
     * strapping pin -- configuring it as an input afterwards is free. */
    {
        const gpio_config_t boot_cfg = {
            .pin_bit_mask = 1ULL << GPIO_NUM_0,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
        };
        ESP_ERROR_CHECK(gpio_config(&boot_cfg));
    }

    app_registry_scan();
    build_launcher_ui();
    xTaskCreate(button_poll_task, "buttons", 3072, NULL, 6, NULL);
    serial_push_start();

    /* Structural proof the theme font actually took (no board display to
     * look at from here). Advisor review caught that logging
     * lv_font_lexend_32's own line_height proves nothing about the THEME --
     * and the row-label check sets its font explicitly, so it doesn't
     * either. This probes an UNSTYLED label: whatever font it resolves is
     * what the theme hands every widget. Want lexend_32 (line_height 36);
     * montserrat_14 (16) here means the theme init did not take. */
    bsp_display_lock(0);
    {
        lv_obj_t *probe = lv_label_create(lv_screen_active());
        const lv_font_t *resolved = lv_obj_get_style_text_font(probe, LV_PART_MAIN);
        ESP_LOGI(TAG, "theme probe: resolved=%p lexend_32=%p line_height=%d (want 36)",
                 (const void *)resolved, (const void *)&lv_font_lexend_32,
                 (int)lv_font_get_line_height(resolved));
        lv_obj_delete(probe);
    }
    bsp_display_unlock();

    ESP_LOGI(TAG, "ready: %u app(s), internal free=%u psram free=%u",
             (unsigned)app_registry_count(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}
