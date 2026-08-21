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
