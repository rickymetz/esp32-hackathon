/* esp32-sim: run launcher apps on the host and drive them with the same verbs
 * the on-board serial harness speaks (RUN / STOP / TAP / SWIPE / SHOT / sleep).
 *
 * Command line mirrors tools/drive.py: a ':'-separated pipeline, e.g.
 *
 *   sim run apps/counter.lua : sleep 1 : tap 184 224 : shot out.png
 *
 * Global options (before the first command): --sdroot DIR  (SD-card root that
 * font_load / file paths resolve against; default: current directory).
 */
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "cap_lua.h"
#include "lua_module_lvgl.h"
#include "app_timer.h"
#include "app_button.h"
#include "app_voice.h"
#include "app_audio.h"
#include "app_wifi.h"
#include "app_sensors.h"
#include "app_sandbox.h"
#include "lua_module_ui.h"
#include "launcher_home.h"

/* Sim-only reset hooks (the device wifi/sensors modules are event/register
 * driven and have no such entry point; the stubs add one for determinism). */
void sim_wifi_reset(void);
void sim_sensors_reset(void);

/* Sim-only state injectors, driven by the accel/gyro/battery/rtc/wifi verbs so
 * degraded and dynamic paths (tilt, low battery, rtc-not-set, wifi-failed) are
 * testable without a board. */
void sim_sensors_set_accel(double x, double y, double z);
void sim_sensors_set_gyro(double x, double y, double z);
void sim_sensors_set_battery(int pct, bool charging, bool external);
void sim_sensors_set_rtc_unset(void);
esp_err_t app_sensors_rtc_set_tm(int y, int mo, int d, int h, int mi, int s, int wday);
void sim_wifi_set_outcome(bool succeed);
#include "sim_display.h"
#include "sim_input.h"
#include "lv_font_lexend.h"

#include "lvgl.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#include <unistd.h>

#define EVENT_PUMP_MS 30

uint32_t sim_tick_ms(void);
void     sim_tick_init(void);

/* ---- Watchdog ----------------------------------------------------------
 * A runaway app (while-true) or a runaway callback must not wedge the sim --
 * the device has the task watchdog for exactly this. We arm a SIGALRM around
 * every stretch of Lua execution: on the first fire we set cap_lua's stop flag
 * so the sandbox's interrupt hook unwinds the app (the graceful path, like
 * BOOT/STOP on device); if the app is in an uninterruptible loop (a tight C
 * call or coroutine body -- see APP_CONTRACT.md) and still hasn't stopped after
 * a short grace period, the second fire hard-exits so CI reports the hang. */
static int s_watchdog_s = 10;               /* per-Lua-call budget, seconds */
static volatile sig_atomic_t s_wd_fired = 0;
#define WATCHDOG_HARD_GRACE_S 3

static void watchdog_handler(int sig)
{
    (void)sig;
    if (!s_wd_fired) {
        s_wd_fired = 1;
        launcher_lua_request_stop(true);    /* async-signal-safe: atomic store */
        alarm(WATCHDOG_HARD_GRACE_S);
    } else {
        static const char msg[] = "sim: watchdog timeout -- app did not stop, killing\n";
        ssize_t n = write(2, msg, sizeof(msg) - 1);
        (void)n;
        _exit(124);
    }
}

static void watchdog_arm(void)
{
    if (s_watchdog_s <= 0) return;
    s_wd_fired = 0;
    signal(SIGALRM, watchdog_handler);
    alarm((unsigned)s_watchdog_s);
}

static void watchdog_disarm(void)
{
    if (s_watchdog_s <= 0) return;
    alarm(0);
    s_wd_fired = 0;
}

/* ---- Lua state lifecycle (mirrors the launcher's lua_setup_state) -------- */

static int traceback_handler(lua_State *L)
{
    const char *msg = lua_tostring(L, -1);
    luaL_traceback(L, L, msg ? msg : "(non-string error)", 1);
    return 1;
}

static int setup_state(lua_State *L)
{
    luaL_openlibs(L);
    esp_err_t err = launcher_lua_open_modules(L);
    if (err != ESP_OK) {
        return luaL_error(L, "launcher_lua_open_modules failed: %s", esp_err_to_name(err));
    }
    app_timer_reset(L);
    app_button_reset(L);
    app_voice_reset(L);
    app_audio_reset(L);
    /* NB: sensor/wifi state is NOT reset here -- only in teardown_state (below)
     * and at process start (static defaults). That lets an injection verb run
     * BEFORE `run` (e.g. `battery 5 : run clock`) survive into the app's
     * load-time read; post-run injection still works for apps that poll on a
     * timer. Each sim invocation is a fresh process, so nothing leaks across
     * separate test/scenario runs. */
    app_sandbox_apply(L);
    app_sandbox_install_hook(L);
    return 0;
}

/* ---- Event pump (mirrors lua_app_task's loop body) ---------------------- */

/* Drain the Lua event queue that LVGL's dispatch just enqueued. On device a
 * separate LVGL task calls lv_timer_handler; here the runner calls it, then
 * asks the binding to drain (process_events(0) == one non-blocking pass). */
static void drain_lua_events(lua_State *L)
{
    lua_getglobal(L, "lvgl");
    if (!lua_istable(L, -1)) { lua_pop(L, 1); return; }
    lua_getfield(L, -1, "process_events");
    if (!lua_isfunction(L, -1)) { lua_pop(L, 2); return; }
    lua_pushinteger(L, 0);
    if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
        fprintf(stderr, "process_events: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
    }
    lua_pop(L, 1); /* lvgl table */
}

static void pump_once(lua_State *L)
{
    watchdog_arm();         /* guard this stretch of Lua/callback execution */
    app_button_run_pending(L);
    app_voice_run_pending(L);
    (void)app_timer_run_due(L);
    lv_timer_handler();     /* indev read, animations, refresh, event dispatch */
    drain_lua_events(L);
    watchdog_disarm();
}

/* Pump for approximately `ms` of real time so timers fire and gestures play. */
static void pump_for(lua_State *L, int ms)
{
    uint32_t start = sim_tick_ms();
    do {
        pump_once(L);
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 5 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    } while ((int)(sim_tick_ms() - start) < ms);
}

/* Pump until injected input has fully drained (press->release->gap), plus a
 * short tail so the resulting click/gesture callbacks dispatch. */
static void pump_until_idle(lua_State *L)
{
    for (int guard = 0; guard < 1000; guard++) {
        pump_once(L);
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 5 * 1000 * 1000 };
        nanosleep(&ts, NULL);
        if (sim_input_idle()) break;
    }
    pump_for(L, 120); /* let the release-edge click event run */
}

/* Same, but with no running app -- drives the bare LVGL handler so injected
 * taps/swipes reach the launcher home screen (the `home` command), whose
 * toggle/tiles are LVGL objects with C callbacks, not a Lua app. */
static void pump_home_input(void)
{
    for (int guard = 0; guard < 1000; guard++) {
        lv_timer_handler();
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 5 * 1000 * 1000 };
        nanosleep(&ts, NULL);
        if (sim_input_idle()) break;
    }
    for (int i = 0; i < 30; i++) {   /* let the release-edge click dispatch */
        lv_timer_handler();
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 5 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
}

/* ---- Running app ------------------------------------------------------- */

static lua_State *s_app;        /* the currently running app VM, or NULL */
static const char *s_sdroot = ".";
static int s_exit_code = 0;     /* nonzero if any command failed (for CI) */
#define BLANK_COLOR_THRESHOLD 4 /* < this many distinct colors == "didn't draw" */

/* Render the launcher's error screen so a SHOT after a failed RUN shows the
 * failure, not a stale frame. Mirrors show_error_screen() in launcher_main.c:
 * black field, red title, scrollable wrapped traceback. Built in C on the sim's
 * display, independent of any Lua state (the app VM is already gone). */
static void render_error_screen(const char *app_name, const char *msg)
{
    lv_obj_t *scr = lv_obj_create(NULL);
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
    lv_obj_set_height(body, 320);
    lv_label_set_text(body, msg ? msg : "(no message)");
    lv_obj_set_style_text_color(body, lv_color_hex(0xE0E0E0), LV_PART_MAIN);
    lv_obj_set_style_text_font(body, &lv_font_lexend_26, LV_PART_MAIN);
    lv_obj_align(body, LV_ALIGN_TOP_LEFT, 0, 64);

    lv_obj_t *hint = lv_label_create(scr);
    lv_label_set_text(hint, "press the top button to go back");
    lv_obj_set_style_text_color(hint, lv_color_hex(0x9A9AA5), LV_PART_MAIN);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -8);

    lv_screen_load(scr);
}

/* Resolve an app path: use it as given if it exists, else try it under the
 * SD-card root (so `run apps/foo.lua` works from any directory, and bare
 * `run foo.lua` finds apps/foo.lua the way the device resolves app names). */
static const char *resolve_app_path(const char *path, char *buf, size_t bufsz)
{
    if (access(path, R_OK) == 0) return path;
    snprintf(buf, bufsz, "%s/%s", s_sdroot, path);
    if (access(buf, R_OK) == 0) return buf;
    snprintf(buf, bufsz, "%s/apps/%s", s_sdroot, path);
    if (access(buf, R_OK) == 0) return buf;
    return path;   /* let luaL_loadfile report the original path */
}

/* Tear a Lua app state down the way the launcher's lua_app_task does on exit:
 * reset the per-app module state (timers, button edges, any pending voice
 * capture -- each holds registry refs into L), run cap_lua exit cleanup (which
 * deinits the lvgl runtime the app may own), then close. Used by every path
 * that disposes of an app state, so a failed run tears down as thoroughly as a
 * clean stop -- otherwise the module statics keep refs into the freed state. */
static void teardown_state(lua_State *L)
{
    launcher_lua_request_stop(true);
    app_timer_reset(L);
    app_button_reset(L);
    app_voice_reset(L);
    app_audio_reset(L);
    sim_wifi_reset();
    sim_sensors_reset();
    launcher_lua_run_exit_cleanup(L);
    lua_close(L);
}

static void app_stop(void)
{
    if (!s_app) return;
    teardown_state(s_app);
    s_app = NULL;
}

static int app_run(const char *path)
{
    if (s_app) app_stop();
    launcher_lua_request_stop(false);

    lua_State *L = luaL_newstate();
    if (!L) { fprintf(stderr, "RUN_ERR out of memory\n"); return -1; }

    lua_pushcfunction(L, setup_state);
    if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
        fprintf(stderr, "RUN_ERR setup: %s\n", lua_tostring(L, -1));
        teardown_state(L);
        return -1;
    }

    char pathbuf[1024];
    const char *rpath = resolve_app_path(path, pathbuf, sizeof(pathbuf));

    lua_pushcfunction(L, traceback_handler);
    int errfunc = lua_gettop(L);
    watchdog_arm();     /* the app's top-level chunk is the main runaway risk */
    int load_rc = (luaL_loadfile(L, rpath) != LUA_OK) ||
                  (lua_pcall(L, 0, 0, errfunc) != LUA_OK);
    watchdog_disarm();
    if (load_rc) {
        if (cap_lua_runtime_stop_requested(L)) {
            /* A runaway app is a FAILURE: on hardware this freezes the
             * device until the watchdog reboots it. Returning 0 here let
             * CI pass exactly the apps the sim exists to catch (review
             * finding). */
            fprintf(stderr, "RUN_ERR timeout %s (watchdog stopped a runaway app)\n", path);
            teardown_state(L);
            return -1;
        }
        const char *msg = lua_tostring(L, -1);
        fprintf(stderr, "app '%s' failed: %s\n", path, msg ? msg : "(nil)");
        /* Copy the message before teardown frees it; render the error screen
         * after teardown so the app's exit cleanup (which may reset the
         * display) runs first and can't clobber it. */
        static char saved[4096];
        snprintf(saved, sizeof(saved), "%s", msg ? msg : "(nil)");
        teardown_state(L);
        render_error_screen(path, saved);   /* so a following SHOT shows it */
        return -1;
    }
    lua_remove(L, errfunc);
    s_app = L;
    printf("RUN_OK %s\n", path);
    /* Settle the first frame (initial timers, layout). */
    pump_for(L, 60);
    return 0;
}

/* ---- Launcher home render (the `home` command) ------------------------- */

/* A fixed, deterministic app list so the launcher home screen -- the launcher's
 * own UI, built by the shared launcher_home_build() -- can be rendered and
 * golden-tested without a board or a real SD scan. */
static const char *const s_fake_apps[] = {
    "Counter", "Clock", "Faces", "Level", "Tone", "Stopwatch",
    "Tally", "Dice", "Countdown", "Reaction", "Tip", "Flashlight",
    /* Metronome/Calculator/Color exercise custom images; Settings hits the
     * glyph-avatar fallback; Zebra maps to nothing and hits the letter-avatar
     * fallback -- so the home views cover all three icon paths. */
    "Metronome", "Settings", "Color", "Calculator", "Zebra",
};
#define SIM_FAKE_APP_COUNT ((int)(sizeof(s_fake_apps) / sizeof(s_fake_apps[0])))

static bool sim_home_get_app(size_t index, launcher_home_app_t *out, void *ctx)
{
    (void)ctx;
    if (index >= (size_t)SIM_FAKE_APP_COUNT) return false;
    out->name = s_fake_apps[index];
    out->basename = s_fake_apps[index];   /* callbacks are NULL in the sim */
    out->icon = launcher_home_default_icon(s_fake_apps[index]);
    return true;
}

/* Home render state, so the view toggle can rebuild in the other layout the
 * way the device's view_toggle_clicked does. */
static int             s_home_count = -1;
static bool            s_home_sd = true;
static launcher_view_t s_home_view = LAUNCHER_VIEW_LIST;

static void sim_home_render(void);

/* The sim's view toggle: flip list <-> grid and rebuild, so `home : tap <x> <y>`
 * on the toggle actually switches the layout (the interaction is testable, not
 * just the two static renders). */
static void sim_home_toggle_cb(lv_event_t *e)
{
    (void)e;
    s_home_view = (s_home_view == LAUNCHER_VIEW_LIST) ? LAUNCHER_VIEW_GRID
                                                      : LAUNCHER_VIEW_LIST;
    sim_home_render();
}

static void sim_home_render(void)
{
    int count = s_home_count;
    if (count < 0) count = SIM_FAKE_APP_COUNT;
    if (count > SIM_FAKE_APP_COUNT) count = SIM_FAKE_APP_COUNT;

    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_pad_all(scr, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(scr, 0, LV_PART_MAIN);

    launcher_home_build(scr, (size_t)count, s_home_sd, 64, s_home_view,
                        sim_home_get_app, NULL, NULL, NULL, NULL, sim_home_toggle_cb);
    lv_screen_load(scr);
    lv_timer_handler();   /* flush the render into the framebuffer */
    lv_timer_handler();
}

/* Set up the home state and render it. `view`/`count`/`sd_mounted` come from
 * the `home` command; 0 apps shows the empty state. */
static void render_home(launcher_view_t view, int count, bool sd_mounted)
{
    s_home_view = view;
    s_home_count = count;
    s_home_sd = sd_mounted;
    sim_home_render();
}

/* ---- Command interpreter ----------------------------------------------- */

static int need(int have, int want, const char *cmd)
{
    if (have < want) { fprintf(stderr, "%s: expected %d args\n", cmd, want); return 0; }
    return 1;
}

/* Execute one ':'-delimited command given its argv slice. */
static void exec_cmd(int argc, char **argv)
{
    if (argc == 0) return;
    const char *cmd = argv[0];

    if (!strcmp(cmd, "run")) {
        if (!need(argc, 2, "run")) return;
        if (app_run(argv[1]) < 0) s_exit_code = 1;
    } else if (!strcmp(cmd, "stop")) {
        app_stop();
        printf("STOP_OK\n");
    } else if (!strcmp(cmd, "sleep")) {
        if (!need(argc, 2, "sleep")) return;
        double s = atof(argv[1]);
        if (s_app) pump_for(s_app, (int)(s * 1000));
        else {                       /* settle a home-screen animation/scroll */
            uint32_t start = sim_tick_ms();
            do {
                lv_timer_handler();
                struct timespec ts = { .tv_sec = 0, .tv_nsec = 5 * 1000 * 1000 };
                nanosleep(&ts, NULL);
            } while ((int)(sim_tick_ms() - start) < (int)(s * 1000));
        }
    } else if (!strcmp(cmd, "tap")) {
        if (!need(argc, 3, "tap")) return;
        int x = atoi(argv[1]), y = atoi(argv[2]);
        sim_input_inject(x, y, x, y, 80);
        if (s_app) pump_until_idle(s_app);
        else pump_home_input();   /* launcher home has no app task */
    } else if (!strcmp(cmd, "swipe")) {
        if (!need(argc, 5, "swipe")) return;
        int x0 = atoi(argv[1]), y0 = atoi(argv[2]);
        int x1 = atoi(argv[3]), y1 = atoi(argv[4]);
        int ms = argc >= 6 ? atoi(argv[5]) : 250;
        sim_input_inject(x0, y0, x1, y1, ms);
        if (s_app) pump_until_idle(s_app);
        else pump_home_input();   /* launcher home has no app task */
    } else if (!strcmp(cmd, "pwr")) {
        /* PWR (bottom-right button) belongs to apps via require("button").
         *   pwr            quick press+release  -> pressed, released
         *   pwr down|up    a single edge
         *   pwr long       hold >=2s            -> pressed, long_pressed, released
         */
        const char *mode = argc >= 2 ? argv[1] : "click";
        if (!s_app) { fprintf(stderr, "pwr: no app running\n"); return; }
        if (!strcmp(mode, "down")) {
            app_button_record_edge(true);  pump_for(s_app, 120);
        } else if (!strcmp(mode, "up")) {
            app_button_record_edge(false); pump_for(s_app, 120);
        } else if (!strcmp(mode, "long")) {
            app_button_record_edge(true);  pump_for(s_app, 2300);
            app_button_record_edge(false); pump_for(s_app, 120);
        } else { /* click */
            app_button_record_edge(true);  pump_for(s_app, 150);
            app_button_record_edge(false); pump_for(s_app, 150);
        }
        printf("PWR_OK %s\n", mode);
    } else if (!strcmp(cmd, "shot")) {
        if (!need(argc, 2, "shot")) return;
        if (s_app) pump_once(s_app);
        if (sim_display_capture_png(argv[1]) == 0) printf("SHOT_OK %s\n", argv[1]);
        else { fprintf(stderr, "SHOT_ERR %s\n", argv[1]); s_exit_code = 1; }
    } else if (!strcmp(cmd, "check")) {
        /* Capture and assert the frame is non-blank -- "the app drew
         * something". Sets the process exit code for CI. */
        if (!need(argc, 2, "check")) return;
        if (s_app) pump_once(s_app);
        int rc = sim_display_capture_png(argv[1]);
        int colors = sim_display_distinct_colors(BLANK_COLOR_THRESHOLD);
        if (rc != 0) { fprintf(stderr, "CHECK_ERR %s (capture failed)\n", argv[1]); s_exit_code = 1; }
        else if (colors < BLANK_COLOR_THRESHOLD) {
            fprintf(stderr, "CHECK_FAIL %s (blank: %d colors)\n", argv[1], colors);
            s_exit_code = 1;
        } else {
            printf("CHECK_OK %s (%d+ colors)\n", argv[1], colors);
        }
    } else if (!strcmp(cmd, "accel")) {
        /* accel <x> <y> <z> in g -- set the fake IMU reading (tilt tests). */
        if (!need(argc, 4, "accel")) return;
        sim_sensors_set_accel(atof(argv[1]), atof(argv[2]), atof(argv[3]));
        printf("ACCEL_OK %s %s %s\n", argv[1], argv[2], argv[3]);
    } else if (!strcmp(cmd, "gyro")) {
        if (!need(argc, 4, "gyro")) return;
        sim_sensors_set_gyro(atof(argv[1]), atof(argv[2]), atof(argv[3]));
        printf("GYRO_OK %s %s %s\n", argv[1], argv[2], argv[3]);
    } else if (!strcmp(cmd, "battery")) {
        /* battery <pct|-1> [charging] [external] -- -1 => "gauge not ready". */
        if (!need(argc, 2, "battery")) return;
        int pct = atoi(argv[1]);
        bool charging = argc >= 3 ? atoi(argv[2]) != 0 : false;
        bool external = argc >= 4 ? atoi(argv[3]) != 0 : (pct >= 0 && charging);
        sim_sensors_set_battery(pct, charging, external);
        printf("BATTERY_OK %d\n", pct);
    } else if (!strcmp(cmd, "rtc")) {
        /* rtc unset | rtc set <y> <mo> <d> <h> <mi> <s> [wday] */
        if (!need(argc, 2, "rtc")) return;
        if (!strcmp(argv[1], "unset")) {
            sim_sensors_set_rtc_unset();
            printf("RTC_OK unset\n");
        } else if (!strcmp(argv[1], "set") && need(argc, 8, "rtc set")) {
            int wday = argc >= 9 ? atoi(argv[8]) : 0;
            app_sensors_rtc_set_tm(atoi(argv[2]), atoi(argv[3]), atoi(argv[4]),
                                   atoi(argv[5]), atoi(argv[6]), atoi(argv[7]), wday);
            printf("RTC_OK set\n");
        } else {
            fprintf(stderr, "rtc: expected 'unset' or 'set <y> <mo> <d> <h> <mi> <s> [wday]'\n");
        }
    } else if (!strcmp(cmd, "wifi")) {
        /* wifi ok | wifi fail -- how the next connect() resolves. */
        if (!need(argc, 2, "wifi")) return;
        if (!strcmp(argv[1], "ok")) {
            sim_wifi_set_outcome(true);  printf("WIFI_OK ok\n");
        } else if (!strcmp(argv[1], "fail")) {
            sim_wifi_set_outcome(false); printf("WIFI_OK fail\n");
        } else {
            fprintf(stderr, "wifi: expected 'ok' or 'fail'\n");
        }
    } else if (!strcmp(cmd, "home")) {
        /* Render the launcher's own home screen (the shared launcher_home_build)
         * with a fake app list.
         *   home                list view, full fake list
         *   home grid | list    that view, full list
         *   home [grid|list] N  N apps (0 = the empty/no-apps state)
         * Stops any running app first; the toggle is live (tap it to switch). */
        if (s_app) app_stop();
        launcher_view_t view = LAUNCHER_VIEW_LIST;
        int argi = 1;
        if (argc >= 2 && !strcmp(argv[1], "grid")) { view = LAUNCHER_VIEW_GRID; argi = 2; }
        else if (argc >= 2 && !strcmp(argv[1], "list")) { view = LAUNCHER_VIEW_LIST; argi = 2; }
        int n = argc > argi ? atoi(argv[argi]) : -1;   /* -1 => full fake list */
        render_home(view, n, /*sd_mounted=*/true);
        printf("HOME_OK\n");
    } else {
        fprintf(stderr, "unknown command: %s\n", cmd);
    }
}

int main(int argc, char **argv)
{
    const char *sdroot = ".";

    /* Leading global options. */
    int i = 1;
    while (i < argc && !strncmp(argv[i], "--", 2)) {
        if (!strcmp(argv[i], "--sdroot") && i + 1 < argc) { sdroot = argv[i + 1]; i += 2; }
        else if (!strcmp(argv[i], "--timeout") && i + 1 < argc) { s_watchdog_s = atoi(argv[i + 1]); i += 2; }
        else { fprintf(stderr, "unknown option %s\n", argv[i]); i++; }
    }

    s_sdroot = sdroot;

    lv_init();
    sim_tick_init();
    sim_display_service_boot();

    /* Registration order matters: ui/keyboard require() lvgl+timer+voice at
     * load, so those come first -- exactly the launcher's app_main() order. */
    if (lua_module_lvgl_register_with_data_root(sdroot) != ESP_OK ||
        app_timer_register() != ESP_OK ||
        app_button_register() != ESP_OK ||
        app_voice_register() != ESP_OK ||
        app_audio_register() != ESP_OK ||
        app_wifi_register() != ESP_OK ||
        app_sensors_register() != ESP_OK ||
        lua_module_ui_register() != ESP_OK) {
        fprintf(stderr, "module registration failed\n");
        return 1;
    }

    /* Walk the ':'-separated command pipeline. */
    int start = i;
    for (int j = i; j <= argc; j++) {
        if (j == argc || !strcmp(argv[j], ":")) {
            if (j > start) exec_cmd(j - start, &argv[start]);
            start = j + 1;
        }
    }

    app_stop();
    return s_exit_code;
}
