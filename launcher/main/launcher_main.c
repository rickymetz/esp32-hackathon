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
#include "esp_random.h"
#include "nvs.h"
#include "nvs_flash.h"
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
#include "launcher_home.h"
#include "launcher_face.h"
#include "app_sandbox.h"
#include "app_timer.h"
#include "app_button.h"
#include "lv_font_lexend.h"
#include "lua_module_ui.h"
#include "lua_module_store.h"
#include "lua_module_prefs.h"
#include <sys/stat.h>
#include "app_voice.h"
#include "app_sensors.h"
#include "app_audio.h"
#include "app_wifi.h"
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

/* ---- Screen timeout ---------------------------------------------------
 *
 * The panel is the dominant load on this board. A device left lit and idle for
 * ~32 minutes measured ~68 C actual silicon (hot to the touch), and it is what
 * drains a 200 mAh cell. On OLED, brightness IS emission -- a black pixel is an
 * off pixel -- so dimming is a real power lever rather than a cosmetic one.
 *
 * 50% was chosen by eye against real UI: clearly dimmer than full, still very
 * legible. Do not substitute a guessed number.
 *
 * SCREEN_ASLEEP_PCT is 0, which stops emission but is NOT device-off: the panel
 * controller stays clocked and LVGL keeps flushing dirty regions. A true
 * panel-off needs esp_lcd_panel_disp_on_off(), and the BSP calls that once
 * internally and never exposes the panel handle -- so 0 is the deepest sleep
 * reachable without patching a managed component. */
#define SCREEN_DIM_MS     30000
#define SCREEN_SLEEP_MS   120000
#define SCREEN_AWAKE_PCT  100
#define SCREEN_DIM_PCT    50
#define SCREEN_ASLEEP_PCT 0

typedef enum { SCREEN_AWAKE, SCREEN_DIMMED, SCREEN_ASLEEP } screen_state_t;

/* Written by button_poll_task and by launcher_screen_wake() (which the serial
 * task calls). Deliberately unlocked: it is a word-sized enum so it cannot
 * tear, and the two writers cannot disagree for long -- the poll loop
 * recomputes the correct state from LVGL's inactivity clock every 20 ms, and
 * a wake resets that clock, so a lost update self-heals within one tick.
 *
 * A mutex here would be the wrong trade: this is reached from the serial task
 * while screen_set() touches the display, which is exactly the shape of the
 * AB-BA inversion this codebase already shipped once. A 20 ms transient in
 * panel brightness is not worth that risk. */
static screen_state_t s_screen = SCREEN_AWAKE;

static lv_obj_t *s_launcher_screen;

/* ---- Shell navigation ---------------------------------------------------
 * Home is the watch face, not the app list: a watch shows you the time and
 * makes apps somewhere you navigate to. BOOT is the only navigation control
 * and it is a three-way toggle:
 *
 *     running an app  ->  home      (the app is stopped first)
 *     home (face)     ->  app list
 *     app list        ->  home
 *
 * That keeps the escape guarantee BOOT has always carried -- it is hardware,
 * no app can consume it, so a misbehaving app is always escapable -- while
 * making the face, not the launcher, the place you land.
 *
 * s_shell_view tracks which of the two shell surfaces is up. It is only
 * meaningful when no app is running; the app-exit path always returns to the
 * face, so it is set there rather than inferred. */
typedef enum {
    SHELL_VIEW_FACE = 0,
    SHELL_VIEW_APPS = 1,
} shell_view_t;

static shell_view_t s_shell_view = SHELL_VIEW_FACE;
/* Defined with the other shell surfaces below; declared here because
 * lua_app_task's exit path (above them) returns to the face. */
static void show_face_screen(void);
static void show_apps_screen(void);
static lv_obj_t    *s_face_screen;
static lv_timer_t  *s_face_tick;
static lv_display_t *s_disp;   /* for re-applying the theme after app exit */
static launcher_view_t s_view_mode = LAUNCHER_VIEW_LIST;   /* list <-> grid */
static void apply_persisted_font_scale(lv_display_t *disp);

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
    /* A serial TAP/SWIPE wakes the screen, so tools/drive.py keeps working
     * across a chain that idles past the sleep timeout. Explicit, rather than
     * inferred from the inactivity counter going small. */
    launcher_screen_wake();

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
 * clearing it on exit. Created in app_main() before anything can launch.
 *
 * It also guards s_sheet_screen and s_launcher_screen -- see below.
 *
 * ==== LOCK ORDER: the display lock is ALWAYS outermost. ====
 *
 * esp_lvgl_port holds the display lock across the whole of lv_timer_handler(),
 * so every LVGL event callback already runs inside it and then takes this
 * mutex: display -> s_app_mutex. Any other task that needs both MUST use the
 * same order, or take neither and post the work to the LVGL task with
 * lv_async_call (see launcher_refresh_ui).
 *
 * Taking s_app_mutex first and then blocking on the display lock is an AB-BA
 * deadlock with any concurrent tap, and neither task is late enough for the
 * watchdog to notice -- they both block cleanly, so the board simply stops.
 * That shipped once. Do not reintroduce it. */
static SemaphoreHandle_t s_app_mutex;

/* True when the app set on the card changed but the home screen could not be
 * rebuilt at the time (an app was running, or the info sheet was open). The
 * next moment the launcher becomes visible, it is rebuilt rather than merely
 * re-loaded. Written only under s_app_mutex. */
static bool s_home_stale;

/* One rebuild request in flight at a time; see launcher_refresh_ui(). */
static volatile bool s_refresh_pending;

/* Single-app-at-a-time means a single static copy is enough: the launch
 * path fills this in (under s_app_mutex) from app_registry_find_by_id()
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

/* The body sits between the title and the hint, and its box is now measured
 * from those two at build time rather than assumed -- see show_error_screen().
 * It stays scrollable so a deep traceback is reachable rather than clipped. */

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

    /* Width-limited and wrapping, like the body below. A centred label with no
     * width shrinks to its content and then loses characters off BOTH ends, so
     * a long app name turned "weather_clock.lua failed" into "ther_clock.lua
     * fail" -- least readable exactly when something has gone wrong. */
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text_fmt(title, "%s failed", app_name);
    lv_label_set_long_mode(title, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(title, LV_PCT(96));
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFF6B6B), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, lua_module_lvgl_scaled_builtin_font(40), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    lv_obj_t *body = lv_label_create(scr);
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(body, LV_PCT(96));
    lv_obj_add_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);
    lv_label_set_text(body, msg ? msg : "(no message)");
    lv_obj_set_style_text_color(body, lv_color_hex(0xE0E0E0), LV_PART_MAIN);
    lv_obj_set_style_text_font(body, lua_module_lvgl_scaled_builtin_font(26), LV_PART_MAIN);

    /* Same treatment, and it mattered more here: on the theme font this string
     * is about 500px wide on a 368px panel, so the one line telling you how to
     * get out of the error screen was clipped at BOTH ends at every font scale.
     * Dropped to 26 as well, so it wraps to two lines rather than three. */
    lv_obj_t *hint = lv_label_create(scr);
    lv_label_set_text(hint, "press the top button to go back");
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(hint, LV_PCT(96));
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x9A9AA5), LV_PART_MAIN);
    lv_obj_set_style_text_font(hint, lua_module_lvgl_scaled_builtin_font(26), LV_PART_MAIN);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -8);

    /* The body is placed between the title and the hint by MEASURING them
     * rather than assuming each is one line. Both wrap now, and a long app
     * name or a large font scale makes the title two lines -- with a fixed
     * top the traceback was drawn straight over it. lv_obj_update_layout()
     * forces the sizes to be computed before they are read. */
    lv_obj_update_layout(scr);
    int32_t top = 8 + lv_obj_get_height(title) + 8;
    int32_t avail = 448 - top - lv_obj_get_height(hint) - 24;
    if (avail < 80) avail = 80;
    lv_obj_set_height(body, avail);
    lv_obj_align(body, LV_ALIGN_TOP_LEFT, 0, top);

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

    /* Load the face before deleting the error screen so the currently-active
     * screen is never the one being freed. Nothing else can see err_scr to
     * clean it up -- lv_screen_load() does not free the screen it replaces,
     * so without this every crash leaked a screen plus its labels.
     *
     * The face, not the app list: since the shell boots to the face, the app
     * list may never have been built, and the old show_launcher_screen() bailed
     * out on a NULL screen -- which would leave err_scr active and then delete
     * it, exactly the crash this ordering exists to avoid. build_face_ui()
     * always produces a screen. */
    show_face_screen();
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
/* Lua seeds math.random from time(NULL) plus a stack address. Neither is
 * random here: nothing sets the system clock from the RTC, so without Wi-Fi
 * time(NULL) is just seconds-since-boot, and an embedded stack has no ASLR.
 * Two launches at the same uptime therefore deal identical "random" numbers.
 * esp_random() is the hardware RNG, so reseed from it. */
static void seed_random(lua_State *L)
{
    if (lua_getglobal(L, "math") != LUA_TTABLE) {
        lua_pop(L, 1);
        return;
    }
    if (lua_getfield(L, -1, "randomseed") != LUA_TFUNCTION) {
        lua_pop(L, 2);
        return;
    }
    lua_pushinteger(L, (lua_Integer)esp_random());
    lua_pushinteger(L, (lua_Integer)esp_random());
    lua_call(L, 2, 0);   /* inside the pcall around lua_setup_state */
    lua_pop(L, 1);       /* math */
}

static int lua_setup_state(lua_State *L)
{
    luaL_openlibs(L);
    seed_random(L);

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
    app_audio_reset(L);   /* nor a queued tone */
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

    /* Per-app persistent store: point require("store") at this app's state
     * file (<sd>/state/<key>.json). get/set work in memory; store.save()
     * writes here. mkdir is harmless if the dir already exists.
     *
     * Key on the app id (a flat app's basename minus ".lua", a folder app's
     * folder name), NOT the pretty display name -- that is what the simulator
     * (sim_main.c app_store_key) and the apps' own docs use, so store data
     * lines up across device, sim, and card. */
    {
        char key[APP_ID_MAX];
        /* Explicit precision, not a bare "%s": app->id is a char[APP_ID_MAX]
         * buffer, so GCC 14 cannot prove the copy fits and -Werror=
         * format-truncation rejects it. Bounding it to sizeof(key)-1 states
         * the invariant the id already satisfies. */
        snprintf(key, sizeof(key), "%.*s", (int)(sizeof(key) - 1), app->id);
        size_t kl = strlen(key);
        if (kl > 4 && strcmp(key + kl - 4, ".lua") == 0) {
            key[kl - 4] = '\0';
        }
        /* Sized for what it actually holds -- BSP_SD_MOUNT_POINT "/state",
         * 13 bytes -- rather than APP_PATH_MAX. At APP_PATH_MAX the compiler
         * had to assume dir could be 319 bytes and so could not prove
         * store_path below fits either. */
        char dir[64];
        snprintf(dir, sizeof(dir), "%s/state", BSP_SD_MOUNT_POINT);
        mkdir(dir, 0777);
        char store_path[APP_PATH_MAX];
        snprintf(store_path, sizeof(store_path), "%s/%s.json", dir, key);
        lua_pushstring(L, store_path);
        lua_setglobal(L, "__APP_STORE__");
    }

    /* A built-in's source is a NUL-terminated blob in flash, not a file, so
     * app->path is a label and cannot be opened. strlen() rather than the
     * _end symbol on purpose: whether _end sits before or after the
     * terminator that EMBED_TXTFILES appends is exactly the off-by-one that
     * shows up as "unexpected symbol near '<eof>'". Treat it as a C string
     * and the question does not arise. */
    int loaded = (app->builtin_src != NULL)
        ? luaL_loadbuffer(L, app->builtin_src, strlen(app->builtin_src), app->id)
        : luaL_loadfile(L, app->path);

    if (loaded != LUA_OK ||
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
        /* Only yield when pump_events did not already sleep.
         *
         * The yield is REQUIRED when wait_ms is 0: pump_events returns
         * immediately in that case, so without this the loop spins and
         * starves the idle task into a watchdog reset (file header, note 3).
         *
         * When wait_ms > 0, pump_events USUALLY blocked for it -- but not
         * always, and the earlier version of this comment stated the guarantee
         * as fact, which it is not. The drain loop only sleeps when the queue
         * goes empty; while events keep arriving it dispatches back-to-back
         * until the deadline and returns without ever blocking. What actually
         * bounds that is task priority: the LVGL task (producer, prio 4) runs
         * BELOW lua_app (consumer, prio 5), so it cannot outrun the drain and
         * the queue must reach empty. The residual risk is an app whose own
         * callbacks synchronously re-enqueue events on this task; that is a
         * pathological app, and it is the one case where this loop can now go
         * a whole 100 ms window without yielding. */
        if (wait_ms <= 0) {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }

close:
    /* Unconditional: a task can never be deleted below while still owning
     * the LVGL mutex, whatever path got us here. */
    lua_lvgl_force_unlock_if_held();
    app_timer_reset(L);
    app_button_reset(L);
    app_voice_reset(L);
    app_audio_reset(L);
    lua_lvgl_keep_awake_reset();   /* a crashed app must not pin the backlight on */
    /* Re-read the persisted font scale: an app that called
     * lvgl.font_scale() without persisting must not restyle every later
     * app (Settings persists first, so its change survives). Also
     * re-applies the theme so the next screens build at the right scale. */
    apply_persisted_font_scale(s_disp);
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
    /* Clear s_app_task BEFORE asking for any refresh: refresh_ui_async_cb
     * bails (and re-flags stale) while an app is still registered as running,
     * so requesting first would just re-defer the rebuild we are here to do. */
    xSemaphoreTake(s_app_mutex, portMAX_DELAY);
    s_app_task = NULL;
    bool stale = s_home_stale;
    xSemaphoreGive(s_app_mutex);

    /* Home is the face: leaving an app lands you on the watch, the way a
     * watch behaves, not back in the app list you launched from. */
    show_face_screen();

    /* A PUSH or DELETE that arrived while this app was running could not touch
     * the screen at the time, and we have just landed on the FACE, not the app
     * list -- so nothing has consulted the registry yet. Without this the
     * change stayed invisible until someone tapped Refresh, which is the exact
     * bug the refresh feature exists to remove. In the documented
     * `drive.py push : run` loop every push after the first lands while an app
     * is running, so this is the common path, not the corner case. */
    if (stale) {
        launcher_refresh_ui();
    }
    vTaskDelete(NULL);
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
 * tapped. Resolving by name at click time via app_registry_find_by_id()
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
    if (!app_registry_find_by_id(basename, &match)) {
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
    if (!app_registry_find_by_id(basename, &match)) {
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

    app_registry_invalidate();
    app_registry_scan();
    build_launcher_ui();   /* frees the screen it replaces */
    s_home_stale = false;   /* an explicit Refresh satisfies any deferred one */

    xSemaphoreGive(s_app_mutex);
}

/* The header view toggle: flip list <-> grid and rebuild in place. Unlike
 * Refresh this does not rescan the card -- the app set is unchanged, only the
 * layout -- but it uses the same rebuild-and-swap-screens dance so the same
 * "never touch a running app's screen" invariant holds. */
static void view_toggle_clicked(lv_event_t *e)
{
    (void)e;

    xSemaphoreTake(s_app_mutex, portMAX_DELAY);
    if (s_app_task != NULL) {
        xSemaphoreGive(s_app_mutex);
        return;   /* never rebuild under a running app */
    }

    s_view_mode = (s_view_mode == LAUNCHER_VIEW_LIST) ? LAUNCHER_VIEW_GRID
                                                      : LAUNCHER_VIEW_LIST;

    build_launcher_ui();   /* frees the screen it replaces */
    s_home_stale = false;   /* the rebuild picks up any deferred change too */

    xSemaphoreGive(s_app_mutex);
}

/* Fill a launcher_home_app_t view from a registry entry. `icon_buf` backs the
 * folder-app icon path (a folder app may ship apps/<id>/icon.bin; launcher_home
 * only uses it if the file exists, so a folder app without one falls back to a
 * glyph). Shared by the home rows and the app-info sheet. */
static void fill_home_app(const app_entry_t *app, launcher_home_app_t *out,
                          char *icon_buf, size_t icon_buf_sz)
{
    out->name = app->name;
    out->basename = app->id;   /* the stable RUN/DELETE identity, folder or flat */
    out->deletable = (app->builtin_src == NULL);
    out->icon = launcher_home_default_icon(app->id);
    if (app->in_folder && icon_buf &&
        snprintf(icon_buf, icon_buf_sz, "D:/apps/%s/icon.bin", app->id) < (int)icon_buf_sz) {
        out->icon_path = icon_buf;
    } else {
        out->icon_path = NULL;
    }
}

/* Adapts app_registry to launcher_home_build's get_app callback. The statics
 * are consumed by the builder (label text copied, basename strdup'd) before the
 * next call, so reusing them across rows is safe on the UI task. */
static bool launcher_home_get_app(size_t index, launcher_home_app_t *out, void *ctx)
{
    (void)ctx;
    static app_entry_t app;
    static char icon_path[APP_PATH_MAX];
    if (!app_registry_get_copy(index, &app)) {
        return false;
    }
    fill_home_app(&app, out, icon_path, sizeof(icon_path));
    return true;
}

/* ---- app-info sheet (long-press a home app) ---------------------------- */

static void build_launcher_ui(void);
static lv_obj_t *s_sheet_screen;             /* the sheet, over the home screen */
static char s_sheet_id[APP_ID_MAX];          /* which app the open sheet acts on */

/* Both sheet callbacks run on the LVGL task with the display lock already
 * held, so they take s_app_mutex in the sanctioned order (display first) --
 * and they MUST take it. They capture s_launcher_screen, publish a new one and
 * delete the old; refresh_ui_async_cb does the same. Unsynchronised, the two
 * could both capture the same `old`, both build, and both lv_obj_delete() it:
 * a double free of an lv_obj_t and its whole subtree, plus row_data_delete_cb
 * free()ing each row's strdup'd basename twice. Deleting a folder app from the
 * sheet while a push was in flight could hit it. */

/* Cancel: drop the sheet, return to the home screen -- rebuilding it first if
 * a push/delete landed while the sheet was up. */
static void sheet_cancel_cb(lv_event_t *e)
{
    (void)e;
    xSemaphoreTake(s_app_mutex, portMAX_DELAY);

    lv_obj_t *sheet = s_sheet_screen;
    s_sheet_screen = NULL;

    if (s_home_stale) {
        /* Same reasoning as the app-exit path: re-loading the old screen would
         * silently discard a registry change made while the sheet was open. */
        build_launcher_ui();   /* frees the screen it replaces */
        s_home_stale = false;
    } else {
        bsp_display_lock(0);
        if (s_launcher_screen) lv_screen_load(s_launcher_screen);
        bsp_display_unlock();
    }

    bsp_display_lock(0);
    if (sheet) lv_obj_delete(sheet);
    bsp_display_unlock();

    xSemaphoreGive(s_app_mutex);
}

/* Delete: remove the app from the card, then rebuild the home list fresh. */
static void sheet_delete_cb(lv_event_t *e)
{
    (void)e;
    app_registry_delete_app(s_sheet_id);     /* unlink + rescan under the lock */

    xSemaphoreTake(s_app_mutex, portMAX_DELAY);

    lv_obj_t *sheet = s_sheet_screen;
    s_sheet_screen = NULL;
    build_launcher_ui();   /* loads a new home screen and frees the old */
    s_home_stale = false;  /* this rebuild IS the refresh */

    bsp_display_lock(0);
    if (sheet) lv_obj_delete(sheet);
    bsp_display_unlock();

    xSemaphoreGive(s_app_mutex);
}

/* Long-press a home row/tile: open the app-info sheet for it. */
static void app_row_long_pressed(lv_event_t *e)
{
    const char *id = (const char *)lv_event_get_user_data(e);
    if (id == NULL) return;

    /* Held across the WHOLE function, not just the running check. The first
     * version released here, then did a registry lookup and an SD stat() --
     * milliseconds -- before assigning s_sheet_screen. A refresh landing in
     * that gap saw s_sheet_screen == NULL, rebuilt, and loaded the new home
     * screen over the sheet being created. The sheet then survived off-screen
     * with s_sheet_screen still set: its buttons unreachable, no new sheet
     * openable, and every later refresh bailing forever while pushes kept
     * reporting OK. A silent, permanent wedge until reboot.
     *
     * We already hold the display lock here (LVGL callback), so the SD I/O
     * below blocks the UI either way; adding s_app_mutex costs nothing extra
     * and the order is the sanctioned display -> s_app_mutex. */
    xSemaphoreTake(s_app_mutex, portMAX_DELAY);
    if (s_app_task != NULL || s_sheet_screen != NULL) {
        xSemaphoreGive(s_app_mutex);
        return;   /* an app is running, or a sheet is already open */
    }

    app_entry_t match;
    if (!app_registry_find_by_id(id, &match)) {
        xSemaphoreGive(s_app_mutex);
        return;
    }

    /* Detail line: the code file's size, and "Folder" for a folder app. A
     * built-in has no file -- stat() on its label path fails -- so say what it
     * is rather than leaving the line blank and looking like a failed read. */
    char detail[64] = "";
    struct stat st;
    if (match.builtin_src != NULL) {
        snprintf(detail, sizeof(detail), "Built-in  -  %.1f KB",
                 (double)strlen(match.builtin_src) / 1024.0);
    } else if (stat(match.path, &st) == 0) {
        double kb = (double)st.st_size / 1024.0;
        snprintf(detail, sizeof(detail), "%s%.1f KB",
                 match.in_folder ? "Folder app  -  " : "", kb);
    }

    snprintf(s_sheet_id, sizeof(s_sheet_id), "%s", match.id);
    static char icon_path[APP_PATH_MAX];
    launcher_home_app_t view;
    fill_home_app(&match, &view, icon_path, sizeof(icon_path));

    bsp_display_lock(0);
    s_sheet_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_sheet_screen, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_sheet_screen, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_sheet_screen, 0, LV_PART_MAIN);
    launcher_home_app_sheet(s_sheet_screen, &view, detail, sheet_delete_cb, sheet_cancel_cb);
    lv_screen_load(s_sheet_screen);
    bsp_display_unlock();

    xSemaphoreGive(s_app_mutex);
}

/* ---- The face surface ---------------------------------------------------
 * Built once and then mutated on each tick -- never rebuilt. The analog dial
 * alone is 60+ tick lines plus three hands; recreating that four times a
 * second to move a second hand would be absurd. The screen is only rebuilt
 * when the STYLE changes (a swipe), which is a user action, not a tick.
 *
 * Style and timezone both live in NVS rather than on the card: home has to
 * work with no card in it, and NVS is already initialised (the Wi-Fi stack
 * does it) and cheaper than mounting a filesystem. */
#define SHELL_NVS_NS      "shell"
#define SHELL_NVS_FACE    "face"      /* launcher_face_style_t */
#define SHELL_NVS_TZ_MIN  "tz_min"    /* minutes east of UTC */

static launcher_face_t *s_face;
static launcher_face_style_t s_face_style = LAUNCHER_FACE_DIGITAL;

static int32_t shell_nvs_get_i32(const char *key, int32_t fallback)
{
    nvs_handle_t h;
    if (nvs_open(SHELL_NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return fallback;
    }
    int32_t v = fallback;
    if (nvs_get_i32(h, key, &v) != ESP_OK) {
        v = fallback;
    }
    nvs_close(h);
    return v;
}

static void shell_nvs_set_i32(const char *key, int32_t value)
{
    nvs_handle_t h;
    if (nvs_open(SHELL_NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    if (nvs_set_i32(h, key, value) == ESP_OK) {
        nvs_commit(h);
    }
    nvs_close(h);
}

/* Days in `mon` (1-12) of `year`, for the timezone shift's date rollover. */
static int days_in_month(int year, int mon)
{
    static const int len[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (mon < 1 || mon > 12) return 31;
    if (mon == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) return 29;
    return len[mon - 1];
}

/* Shift a UTC reading by `minutes`, rolling the date properly.
 *
 * This matters and is easy to get wrong: NTP sets the RTC in UTC, so a naive
 * face shows the right time nowhere except Greenwich -- and a shift that moves
 * the clock but not the date shows tomorrow's time beside today's date across
 * midnight. The former apps/faces.lua carried the same warning after exactly
 * that bug; this is now the only copy on the device. */
static void shift_local(launcher_face_data_t *d, int minutes)
{
    if (!d->time_valid || minutes == 0) return;

    int total = d->hour * 60 + d->min + minutes;
    int day_delta = 0;
    while (total < 0)     { total += 24 * 60; day_delta--; }
    while (total >= 1440) { total -= 24 * 60; day_delta++; }
    d->hour = total / 60;
    d->min  = total % 60;

    if (day_delta == 0) return;

    d->wday = ((d->wday + day_delta) % 7 + 7) % 7;
    d->day += day_delta;
    if (d->day < 1) {
        d->month = (d->month == 1) ? 12 : d->month - 1;
        if (d->month == 12) d->year--;
        d->day = days_in_month(d->year, d->month);
    } else if (d->day > days_in_month(d->year, d->month)) {
        d->day = 1;
        d->month = (d->month == 12) ? 1 : d->month + 1;
        if (d->month == 1) d->year++;
    }
}

/* Sample the clock and gauge, in local time. Never fails: an unreachable
 * sensor lands as invalid and the face draws its honest degraded state
 * rather than a fabricated reading. */
static void read_face_data(launcher_face_data_t *d)
{
    memset(d, 0, sizeof(*d));

    int year, mon, mday, hour, min, sec, wday;
    if (app_sensors_rtc_get_tm(&year, &mon, &mday, &hour, &min, &sec, &wday) == ESP_OK) {
        d->time_valid = true;
        d->hour = hour; d->min = min; d->sec = sec;
        d->year = year; d->month = mon; d->day = mday; d->wday = wday;
        shift_local(d, (int)shell_nvs_get_i32(SHELL_NVS_TZ_MIN, 0));
    }

    int pct; bool charging;
    if (app_sensors_battery_get(&pct, &charging) == ESP_OK) {
        d->batt_valid = true;
        d->batt_percent = pct;
        d->charging = charging;
    }
}

static void face_gesture_cb(lv_event_t *e);

/* Build (or rebuild) the face screen and leave it loaded. Only called on boot
 * and on a style change. Caller must NOT hold the display lock. */
static void build_face_ui(void)
{
    launcher_face_data_t d;
    read_face_data(&d);

    bsp_display_lock(0);

    lv_obj_t *old = s_face_screen;
    launcher_face_t *old_face = s_face;

    s_face_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_face_screen, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_face_screen, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_face_screen, 0, LV_PART_MAIN);

    s_face = launcher_face_create(s_face_screen, s_face_style);
    launcher_face_update(s_face, &d);

    /* Swipe to change face, the way the old faces app paged between them.
     * Gestures are delivered to the screen, not to widgets. */
    lv_obj_add_event_cb(s_face_screen, face_gesture_cb, LV_EVENT_GESTURE, NULL);

    lv_screen_load(s_face_screen);

    /* Only now that the new screen is active is the old one safe to free --
     * lv_screen_load() does not free what it replaces, and deleting the
     * screen that is still on-screen is exactly the crash refresh_clicked()
     * documents avoiding. */
    if (old != NULL && old != s_face_screen) {
        lv_obj_delete(old);
    }
    launcher_face_destroy(old_face);

    bsp_display_unlock();
}

/* Repaint the face in place while it is the visible surface.
 *
 * Sampling faster than the thing being sampled is deliberate: a periodic timer
 * re-arms AFTER its callback, so a 1000 ms tick against the RTC's own 1 Hz
 * edge is always a little over a second and some seconds are never observed --
 * the second hand visibly jumps two ticks. 250 ms cannot skip one. Faces
 * without a second hand only need to catch the minute, so they tick at 1 s and
 * the update is gated on the value actually changing. */
static void face_tick_cb(lv_timer_t *t)
{
    (void)t;
    if (s_shell_view != SHELL_VIEW_FACE || s_app_task != NULL || s_face == NULL) {
        return;
    }
    launcher_face_data_t d;
    read_face_data(&d);
    launcher_face_update(s_face, &d);
}

/* Re-read the saved style from NVS. apps/settings.lua writes "face" through
 * the prefs module while the shell is suspended behind it, so s_face_style is
 * stale by the time we land back here -- reading it only at boot made the
 * Settings picker look like it did nothing until the next reboot. (tz_min
 * never had this bug: read_face_data() reads it fresh on every tick.) */
static void reload_face_style(void)
{
    int32_t saved = shell_nvs_get_i32(SHELL_NVS_FACE, LAUNCHER_FACE_DIGITAL);
    if (saved < 0 || saved >= LAUNCHER_FACE_COUNT) {
        saved = LAUNCHER_FACE_DIGITAL;
    }
    s_face_style = (launcher_face_style_t)saved;
}

static void show_face_screen(void)
{
    s_shell_view = SHELL_VIEW_FACE;
    reload_face_style();
    build_face_ui();

    bsp_display_lock(0);
    uint32_t period = launcher_face_wants_seconds(s_face_style) ? 250 : 1000;
    if (s_face_tick == NULL) {
        s_face_tick = lv_timer_create(face_tick_cb, period, NULL);
    } else {
        lv_timer_set_period(s_face_tick, period);
    }
    lv_timer_resume(s_face_tick);
    bsp_display_unlock();
}

/* Swipe left/right cycles the face and remembers the choice, the way a watch
 * does. Runs on the LVGL task, so it must not take the display lock that
 * build_face_ui() takes -- LVGL's lock is recursive on this BSP, but the
 * screen swap is done here rather than inline to keep one code path. */
static void face_gesture_cb(lv_event_t *e)
{
    (void)e;
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
    if (dir != LV_DIR_LEFT && dir != LV_DIR_RIGHT) {
        return;
    }

    int next = (int)s_face_style + (dir == LV_DIR_LEFT ? 1 : -1);
    if (next < 0) next = LAUNCHER_FACE_COUNT - 1;
    if (next >= LAUNCHER_FACE_COUNT) next = 0;
    s_face_style = (launcher_face_style_t)next;

    shell_nvs_set_i32(SHELL_NVS_FACE, next);
    ESP_LOGI(TAG, "face -> %s", launcher_face_style_name(s_face_style));
    show_face_screen();
}

static void show_apps_screen(void)
{
    s_shell_view = SHELL_VIEW_APPS;
    /* Stop repainting the face while it is not visible -- the tick would
     * otherwise redraw a screen nobody is looking at, and on the analog face
     * that is an I2C read four times a second. */
    bsp_display_lock(0);
    if (s_face_tick != NULL) {
        lv_timer_pause(s_face_tick);
    }
    bsp_display_unlock();
    build_launcher_ui();
}

/* BOOT's three-way toggle, run off the button task. Called only when no app
 * is running -- the app case is handled by the stop request, whose exit path
 * lands on the face. */
static void shell_toggle_view(void)
{
    if (s_shell_view == SHELL_VIEW_FACE) {
        show_apps_screen();
        return;
    }

    /* Leaving the app list. A long-press info sheet may be open over it, and
     * it is a screen of its own -- neither show_face_screen() nor anything it
     * calls knows about it. Left behind it leaked, AND app_row_long_pressed()
     * bails on `s_sheet_screen != NULL`, so long-press-to-delete stayed dead
     * for the rest of the boot. Dispose of it before the face loads. */
    if (s_sheet_screen != NULL) {
        lv_obj_t *sheet = s_sheet_screen;
        s_sheet_screen = NULL;
        bsp_display_lock(0);
        lv_obj_delete(sheet);
        bsp_display_unlock();
    }
    show_face_screen();
}

/* Builds the app list and leaves it loaded, freeing whatever screen it
 * replaced -- the same contract build_face_ui() has. It used to be the
 * CALLER's job, which three of the four callers did and show_apps_screen()
 * did not, so every BOOT toggle from the face to the app list leaked a whole
 * screen tree: rows, their strdup'd basenames and their decoded card icons.
 * Owning it here means a new caller cannot get it wrong. */
static void build_launcher_ui(void)
{
    size_t count = app_registry_count();

    bsp_display_lock(0);

    lv_obj_t *old = s_launcher_screen;

    s_launcher_screen = lv_obj_create(NULL);
    /* True black, not near-black: watchOS/Wear OS are dark-theme only, and on
     * OLED a black pixel is an off pixel -- see docs/DESIGN_GUIDE.md. */
    lv_obj_set_style_bg_color(s_launcher_screen, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_launcher_screen, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_launcher_screen, 0, LV_PART_MAIN);

    /* The whole app-list UI now lives in launcher_home.c so the headless
     * simulator can render it too (with a fake app list). This passes the real
     * app-registry accessor and the real row/refresh callbacks. */
    launcher_home_build(s_launcher_screen, count, app_registry_sd_mounted(),
                        MAX_VISIBLE_ROWS, s_view_mode, launcher_home_get_app, NULL,
                        app_row_clicked, row_data_delete_cb, app_row_long_pressed,
                        refresh_clicked, view_toggle_clicked);

    lv_screen_load(s_launcher_screen);

    /* Only now that the new screen is active is the old one safe to free:
     * lv_screen_load() does not free what it replaces, and deleting the screen
     * that is still on-screen is the crash this ordering exists to prevent. */
    if (old != NULL && old != s_launcher_screen) {
        lv_obj_delete(old);
    }
    bsp_display_unlock();
}

/* Rebuild the home screen for a caller that changed the app set without
 * pressing Refresh -- today that is a serial PUSH or DELETE, which rescans the
 * registry but had no way to say so to the UI, so a pushed app stayed invisible
 * until someone tapped Refresh on the panel. See launcher_main.h.
 *
 * THE LOCK ORDER IS THE WHOLE DESIGN HERE. The first version of this ran the
 * rebuild inline on the serial task, taking s_app_mutex and then the display
 * lock -- the exact inverse of the order every LVGL event callback uses, since
 * esp_lvgl_port holds the display lock across the whole of lv_timer_handler()
 * and app_row_clicked/refresh_clicked/view_toggle_clicked/app_row_long_pressed
 * all take s_app_mutex from inside it. A finger on a row during a PUSH
 * deadlocked both tasks on portMAX_DELAY waits, with no watchdog to break it
 * (both block cleanly, so the idle task keeps running) -- a dead board needing
 * the physical PWR/BOOT recovery dance.
 *
 * So: never rebuild from the calling task. Post the work to the LVGL task with
 * lv_async_call and let it run there, where the display lock is already held
 * and taking s_app_mutex is the same order as every other callback. The one
 * global rule is now "display lock is outermost, always"; see s_app_mutex's
 * declaration.
 *
 * Deliberately NOT refresh_clicked(): that also invalidates and remounts the
 * card, which a PUSH has no reason to do (it just wrote through the same
 * mount) and which would unmount the filesystem out from under a second
 * queued push. */
static void refresh_ui_async_cb(void *arg)
{
    (void)arg;
    /* Runs on the LVGL task from lv_timer_handler(), so the display lock is
     * already held (recursively re-entered by build_launcher_ui below) and
     * this take is display -> s_app_mutex, matching every event callback. */
    xSemaphoreTake(s_app_mutex, portMAX_DELAY);
    s_refresh_pending = false;

    /* s_shell_view is the post-#7 condition #8 could not have known about: it
     * was written when the launcher list WAS home, so rebuilding and loading
     * it was always the right answer. Home is the watch face now, and
     * build_launcher_ui() ends in lv_screen_load() -- so without this a serial
     * PUSH would yank the screen off the face and onto the app list while
     * someone was looking at the time. Defer instead; show_apps_screen()
     * rebuilds from the registry every time it runs, so nothing is lost. */
    if (s_app_task != NULL || s_sheet_screen != NULL ||
        s_shell_view != SHELL_VIEW_APPS) {
        /* Not a good moment: an app owns the screen, the info sheet is up, or
         * the user is looking at the face.
         * Remember that the list on screen no longer matches the card, and
         * rebuild when the launcher next becomes visible. Without this flag the
         * update was simply LOST: returning home lands on the face, which never
         * consults the registry, so a push during a run stayed invisible even
         * after the app exited. */
        s_home_stale = true;
        xSemaphoreGive(s_app_mutex);
        return;
    }

    build_launcher_ui();   /* frees the screen it replaces */
    s_home_stale = false;
    xSemaphoreGive(s_app_mutex);
}

bool launcher_refresh_ui(void)
{
    /* Coalesce a burst. tools/push.py sends one PUSH per FILE, so installing a
     * folder app (main.lua + icon.bin + assets) used to run a full
     * build-load-delete of the whole screen tree once per file. One pending
     * request is enough -- the rebuild reads the registry when it runs, so it
     * always reflects the final state. */
    if (s_refresh_pending) {
        return true;
    }
    s_refresh_pending = true;

    /* lv_async_call touches LVGL's own list, so it needs the display lock --
     * and taking ONLY the display lock here (never s_app_mutex) is what keeps
     * the caller out of the inversion described above. */
    bsp_display_lock(0);
    lv_res_t res = lv_async_call(refresh_ui_async_cb, NULL);
    bsp_display_unlock();

    if (res != LV_RESULT_OK) {
        s_refresh_pending = false;
        ESP_LOGW(TAG, "lv_async_call failed -- home screen not refreshed");
        return false;
    }
    return true;
}

/* One BOOT press. Factored out so the serial BOOT command drives exactly the
 * same path as the physical button -- the alternative is a second copy of this
 * logic that drifts from the real one, which would make the harness prove
 * something the button does not do.
 *
 * Callable from any task that holds NEITHER lock. The s_app_mutex take below
 * is released BEFORE shell_toggle_view(), which is what keeps this off the
 * s_app_mutex -> display order that the lock-order note warns about.
 *
 * Two presses racing (a finger and a serial command at once) can interleave
 * into one stop plus one toggle. That is the same outcome as pressing twice,
 * and no worse than the existing race between a press and an app that is
 * already exiting. */
void launcher_boot_press(void)
{
    bool was_dark = (s_screen != SCREEN_AWAKE);
    launcher_screen_wake();

    if (was_dark) {
        ESP_LOGI(TAG, "BOOT -- waking the screen");
        return;
    }

    launcher_lua_request_stop(true);

    xSemaphoreTake(s_app_mutex, portMAX_DELAY);
    bool app_running = (s_app_task != NULL);
    xSemaphoreGive(s_app_mutex);

    if (app_running) {
        ESP_LOGI(TAG, "BOOT -- stopping app, returning home");
    } else {
        ESP_LOGI(TAG, "BOOT -- %s",
                 s_shell_view == SHELL_VIEW_FACE ? "face -> apps" : "apps -> face");
        shell_toggle_view();
    }
}

/* BOOT (GPIO0, top right) is Home: the universal way back to the launcher.
 * It is deliberately hardware: no app can consume it or paint over it, so a
 * misbehaving app can always be escaped. PWR (EXIO4, bottom right) belongs
 * to apps via the `button` module -- but holding PWR >=6s still powers the
 * board off, which is the AXP2101 acting below us, not something we handle.
 *
 * Runs whether or not an app is up: pressing BOOT with no app running is a
 * harmless no-op. */

/* The one place the panel's brightness and the touch indev change together.
 *
 * Disabling the touch indev on sleep does two jobs at once: a finger on a dark
 * screen cannot press a button it cannot see, and a disabled indev generates no
 * activity, so it cannot reset LVGL's inactivity timer and wake the screen it
 * just failed to touch. */
static void screen_set(screen_state_t next)
{
    if (next == s_screen) {
        return;               /* idempotent: this runs every 20 ms */
    }
    lv_indev_t *touch = bsp_display_get_input_dev();

    switch (next) {
    case SCREEN_AWAKE:
        bsp_display_brightness_set(SCREEN_AWAKE_PCT);
        if (touch) lv_indev_enable(touch, true);
        break;
    case SCREEN_DIMMED:
        bsp_display_brightness_set(SCREEN_DIM_PCT);
        if (touch) lv_indev_enable(touch, true);
        break;
    case SCREEN_ASLEEP:
        bsp_display_brightness_set(SCREEN_ASLEEP_PCT);
        if (touch) lv_indev_enable(touch, false);
        break;
    }
    s_screen = next;
    ESP_LOGI(TAG, "screen -> %s",
             next == SCREEN_AWAKE ? "awake" :
             next == SCREEN_DIMMED ? "dimmed" : "asleep");
}

/* Wake and restart the ladder from the top.
 *
 * Safe from any task: it takes the display lock briefly for LVGL's activity
 * reset and takes NO launcher mutex, so it cannot participate in a lock-order
 * inversion (see the note at s_app_mutex). */
void launcher_screen_wake(void)
{
    bsp_display_lock(0);
    lv_display_trigger_activity(NULL);   /* reset LVGL's inactivity clock */
    bsp_display_unlock();
    screen_set(SCREEN_AWAKE);
}

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
        /* BOOT: active low. Two jobs, in this order.
         *
         * WAKE FIRST. A press on a dark screen lights it and does nothing
         * else -- it must not silently stop an app the user cannot see. Only
         * a press on an already-lit screen is navigation.
         *
         * Then the three-way toggle (see the shell navigation notes above).
         * Within that branch the stop request stays UNCONDITIONAL: it must
         * never gate on s_app_task being assigned, which is what stopped the
         * very first version from ever returning. s_app_task is read only
         * afterwards, to decide whether the press ALSO toggles the surface.
         *
         * The wake gate is a different condition and a deliberate one, but it
         * has the same failure shape if it ever sticks: a screen state wedged
         * at not-AWAKE would make BOOT stop nothing. It self-heals because
         * launcher_screen_wake() sets AWAKE unconditionally, so the second
         * press is always navigation -- worst case you press BOOT twice, and
         * the escape hatch cannot be lost. */
        if (debounce_step(&boot_db, gpio_get_level(GPIO_NUM_0) == 0, &level) && level) {
            launcher_boot_press();
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

        /* Screen timeout. Reuses this task rather than adding a fourth one: it
         * already ticks at 20 ms and already owns BOOT, and this repo has
         * already shipped an AB-BA deadlock by giving a new task the display. */
        bsp_display_lock(0);
        uint32_t idle = lv_display_get_inactive_time(NULL);
        bsp_display_unlock();

        /* keep_awake suppresses the BLANK but not the DIM. A watch face still
         * drops to 50% after 30s, which is readable at a glance and roughly
         * half the power; holding 100% indefinitely would recreate exactly the
         * always-lit idle state this feature exists to remove -- reachable by
         * launching the most obvious app on the device. */
        /* `dim_only` (Settings -> Display & sound) suppresses the blank exactly
         * the way an app's keep_awake does. Task #40 asked for the timeout to
         * be "user-configurable to dim-only", and the argument for wanting it
         * is the watch face itself: a watch you must press a button to read is
         * a worse watch, even though blanking is the better default for
         * battery. Read fresh each tick rather than cached, so the choice
         * applies the moment it is made -- an NVS read is cheap next to the SD
         * and I2C work this same task already does every 20 ms. */
        if (idle >= SCREEN_SLEEP_MS && !lua_lvgl_keep_awake() &&
            shell_nvs_get_i32("dim_only", 0) != 1) {
            screen_set(SCREEN_ASLEEP);
        } else if (idle >= SCREEN_DIM_MS) {
            screen_set(SCREEN_DIMMED);
        } else {
            screen_set(SCREEN_AWAKE);
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

/* Restore the user's saved UI font scale (apps/settings.lua writes it) and
 * re-apply the theme so plain-label text picks it up. Called before the
 * launcher UI is built. Reads NVS, so it no longer depends on the SD being
 * mounted; an absent or out-of-range value leaves the default in place. */
static void apply_persisted_font_scale(lv_display_t *disp)
{
    /* NVS, not the card: the UI scale is a device setting and must apply with
     * no card in the slot -- the same reason the face style and timezone live
     * there. Settings writes "font_pct" through the `prefs` Lua module, which
     * shares this NVS namespace. Absent or out-of-range leaves the default. */
    int32_t pct = shell_nvs_get_i32("font_pct", 0);
    if (pct >= 70 && pct <= 130) {
        lua_module_lvgl_set_font_scale((float)pct / 100.0f);
    }

    /* Same story for the speaker level: Settings wrote "volume" to NVS and
     * nothing read it back, so the choice never survived a reboot even though
     * APP_CONTRACT lists it among the keys the shell must know about. -1 is
     * "never set", which app_audio_set_volume() ignores. */
    app_audio_set_volume((int)shell_nvs_get_i32("volume", -1));

    /* The FPS overlay is compiled in but starts hidden, so a release boots
     * clean; Settings turns it on and remembers the choice here. LVGL creates
     * it during lv_init when LV_USE_PERF_MONITOR is set, hence the explicit
     * hide rather than relying on a default. It lives on the display's system
     * layer, so it is unaffected by every screen swap below. */
#if LV_USE_SYSMON && LV_USE_PERF_MONITOR
    if (shell_nvs_get_i32("fps", 0) == 1) {
        lv_sysmon_show_performance(disp);
    } else {
        lv_sysmon_hide_performance(disp);
    }
#endif

    bsp_display_lock(0);
    lv_display_set_theme(disp,
        lv_theme_default_init(disp,
                              lv_palette_main(LV_PALETTE_BLUE),
                              lv_palette_main(LV_PALETTE_RED),
                              true /* dark */,
                              lua_module_lvgl_scaled_builtin_font(32)));
    bsp_display_unlock();
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

    /* NVS holds every device setting the shell reads without a card -- the
     * face style, the timezone, the font scale -- so it must be up before the
     * first of those reads, which is the face build a few lines below.
     *
     * It used to be initialised only as a side effect of Wi-Fi coming up
     * (app_wifi.c's stack_start), which made the boot face read a race the
     * shell usually lost: NVS was still closed, shell_nvs_get_i32 returned its
     * fallback, and the device booted to Digital no matter what the user had
     * chosen. tz_min escaped it only because read_face_data() re-reads it on
     * every tick, long after Wi-Fi has run. On a board with no network
     * configured, stack_start never runs at all and NVS never opened.
     *
     * stack_start still calls this; a second nvs_flash_init() is a no-op. */
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    if (nvs_err != ESP_OK) {
        /* Not fatal: settings fall back to their defaults and the device
         * still boots, which is the same rule networking follows. */
        ESP_LOGE(TAG, "nvs init failed: %s -- settings will use defaults",
                 esp_err_to_name(nvs_err));
    }

    ESP_ERROR_CHECK(release_panel_reset());

    lv_display_t *disp = bsp_display_start();
    s_disp = disp;
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
                              /* Theme default follows the global font scale
                               * (default 1.0); apply_persisted_font_scale()
                               * re-themes once the SD's saved value is read. */
                              lua_module_lvgl_scaled_builtin_font(32)));
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
    /* Register the D: card filesystem now so the home screen can load app
     * icons (D:/apps/<id>/icon.bin) before any app runs its own lvgl.init(). */
    ESP_ERROR_CHECK(lua_module_lvgl_register_fs());
    ESP_ERROR_CHECK(app_timer_register());
    ESP_ERROR_CHECK(app_button_register());
    /* Order matters: cap_lua opens modules in registration order, and
     * ui.lua/keyboard.lua require() lvgl, timer, and voice at load --
     * so voice must register BEFORE the embedded-Lua modules. */
    ESP_ERROR_CHECK(app_audio_register());   /* touches no hardware */
    ESP_ERROR_CHECK(app_voice_register());
    ESP_ERROR_CHECK(app_sensors_register());
    ESP_ERROR_CHECK(app_wifi_register());
    ESP_ERROR_CHECK(lua_module_store_register());
    ESP_ERROR_CHECK(lua_module_prefs_register());   /* no deps; order is free */
    ESP_ERROR_CHECK(lua_module_ui_register());

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
    apply_persisted_font_scale(disp);   /* after SD mount, before UI build */
    /* Boot to the watch face, not the app list: home is the watch. The app
     * list is one BOOT press away (see the shell navigation notes above).
     * The style is whatever was last swiped to; an out-of-range or absent
     * value falls back to the digital face, which is also the fallback for a
     * failed home app. */
    show_face_screen();   /* reloads the saved style itself */
    xTaskCreate(button_poll_task, "buttons", 3072, NULL, 6, NULL);
    serial_push_start();

    /* Last, and deliberately after the UI is up: a network that is slow,
     * absent or misconfigured must never delay the launcher appearing.
     * Non-blocking -- it returns immediately whatever the radio does. */
    app_wifi_autostart();

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
        /* Expectation tracks the font scale (the PR review caught the
         * probe reporting a mismatch on every healthy boot after the
         * scale landed). */
        const lv_font_t *want = lua_module_lvgl_scaled_builtin_font(32);
        ESP_LOGI(TAG, "theme probe: resolved=%p want=%p line_height=%d (want %d)",
                 (const void *)resolved, (const void *)want,
                 (int)lv_font_get_line_height(resolved),
                 (int)lv_font_get_line_height(want));
        lv_obj_delete(probe);
    }
    bsp_display_unlock();

    ESP_LOGI(TAG, "ready: %u app(s), internal free=%u psram free=%u",
             (unsigned)app_registry_count(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}
