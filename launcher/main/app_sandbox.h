/*
 * App containment.
 *
 * Two jobs, both about protecting the launcher from an app rather than
 * protecting the user from a malicious author. Apps are NOT sandboxed --
 * they keep io, os and coroutine, and can read and overwrite anything on the
 * SD card. We only remove what would break the launcher itself.
 */

#pragma once

#include "lua.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Remove stdlib entries that break the launcher.
 *
 * debug        -- debug.sethook would disable our interrupt hook
 * os.exit      -- calls exit(), taking the firmware down
 * os.execute   -- inert on ESP-IDF, removed for clarity
 * package      -- loadlib is an escape hatch to C
 *
 * Nilling the *global* debug/package is NOT enough on its own: luaL_openlibs
 * (via luaL_requiref) also stashes each module table in the registry's
 * LUA_LOADED_TABLE, and require() reads from that cache rather than from
 * _G. An app that does require("debug") would get the live table back --
 * sethook and all -- even with the global nilled out from under it. What
 * actually makes this stick is nilling the dangerous fields (sethook,
 * gethook) on the debug table *object* before dropping any reference to it,
 * so the change is visible through every alias of that table, plus clearing
 * the registry cache entries for debug/package so require() cannot hand
 * back a live table at all. Do not "simplify" this back down to a global
 * nil -- see app_sandbox.c for the full mechanism, and do not nil the
 * global `require` itself: apps depend on it for lvgl, timer, and other
 * capability modules.
 */
void app_sandbox_apply(lua_State *L);

/**
 * @brief Install the latching interrupt hook on this state.
 *
 * MUST be called on the app task before luaL_dofile. Never cross-task:
 * lua_sethook writes L->hook and walks the CallInfo chain, which races a
 * running app.
 */
void app_sandbox_install_hook(lua_State *L);

#ifdef __cplusplus
}
#endif
