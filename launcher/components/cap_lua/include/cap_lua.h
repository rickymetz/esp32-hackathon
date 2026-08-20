/*
 * Minimal stand-in for esp-claw's cap_lua capability.
 *
 * lua_module_lvgl (vendored from espressif/esp-claw) touches exactly three
 * functions from its parent framework. Rather than pull in all of esp-claw we
 * implement just those here, against our own launcher.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "lua.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *name;
    lua_CFunction open_fn;
} cap_lua_module_t;

typedef void (*cap_lua_exit_cleanup_fn_t)(lua_State *L);

/**
 * @brief Register a Lua module opener under `name`.
 *
 * Registered modules are installed into a lua_State by
 * launcher_lua_open_modules().
 */
esp_err_t cap_lua_register_module(const char *name, lua_CFunction open_fn);

/**
 * @brief Register a cleanup callback run when an app's lua_State is torn down.
 */
esp_err_t cap_lua_register_exit_cleanup(cap_lua_exit_cleanup_fn_t cleanup_fn);

/**
 * @brief Whether the launcher has asked the running app to stop.
 *
 * This is the cooperative "kill the running app" primitive: long-running Lua
 * loops poll it and bail out.
 */
bool cap_lua_runtime_stop_requested(lua_State *L);

/* ---- Launcher-side API (not part of esp-claw) ---- */

/** Install every registered module into `L`. */
esp_err_t launcher_lua_open_modules(lua_State *L);

/** Run every registered exit-cleanup callback for `L`. */
void launcher_lua_run_exit_cleanup(lua_State *L);

/** Ask the currently running app to stop. */
void launcher_lua_request_stop(bool stop);

#ifdef __cplusplus
}
#endif
