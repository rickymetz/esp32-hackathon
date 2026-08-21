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
#include "app_sandbox.h"
#include "lua_module_ui.h"
#include "sim_display.h"
#include "sim_input.h"

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

/* ---- Running app ------------------------------------------------------- */

static lua_State *s_app;        /* the currently running app VM, or NULL */
static const char *s_sdroot = ".";

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

static void app_stop(void)
{
    if (!s_app) return;
    launcher_lua_request_stop(true);
    app_timer_reset(s_app);
    app_button_reset(s_app);
    app_voice_reset(s_app);
    launcher_lua_run_exit_cleanup(s_app);
    lua_close(s_app);
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
        lua_close(L);
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
            fprintf(stderr, "RUN_ERR timeout %s (watchdog stopped a runaway app)\n", path);
            lua_close(L);
            return 0;
        }
        fprintf(stderr, "app '%s' failed: %s\n", path, lua_tostring(L, -1));
        lua_close(L);
        return -1;
    }
    lua_remove(L, errfunc);
    s_app = L;
    printf("RUN_OK %s\n", path);
    /* Settle the first frame (initial timers, layout). */
    pump_for(L, 60);
    return 0;
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
        app_run(argv[1]);
    } else if (!strcmp(cmd, "stop")) {
        app_stop();
        printf("STOP_OK\n");
    } else if (!strcmp(cmd, "sleep")) {
        if (!need(argc, 2, "sleep")) return;
        double s = atof(argv[1]);
        if (s_app) pump_for(s_app, (int)(s * 1000));
    } else if (!strcmp(cmd, "tap")) {
        if (!need(argc, 3, "tap")) return;
        int x = atoi(argv[1]), y = atoi(argv[2]);
        sim_input_inject(x, y, x, y, 80);
        if (s_app) pump_until_idle(s_app);
    } else if (!strcmp(cmd, "swipe")) {
        if (!need(argc, 5, "swipe")) return;
        int x0 = atoi(argv[1]), y0 = atoi(argv[2]);
        int x1 = atoi(argv[3]), y1 = atoi(argv[4]);
        int ms = argc >= 6 ? atoi(argv[5]) : 250;
        sim_input_inject(x0, y0, x1, y1, ms);
        if (s_app) pump_until_idle(s_app);
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
        else fprintf(stderr, "SHOT_ERR %s\n", argv[1]);
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
    return 0;
}
