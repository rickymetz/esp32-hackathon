/* Shared by the device build and the simulator.
 *
 * It lives in its own file because the two register the `store` module
 * differently -- the device embeds store.lua in flash with EMBED_TXTFILES,
 * the simulator loads it from disk -- and the exit behaviour must not be
 * allowed to drift between them. The simulator not having this at all is
 * exactly how it would go unnoticed.
 */
#include "lua_module_store.h"
#include "cap_lua.h"
#include "lauxlib.h"
#include "lua.h"
#include "esp_log.h"

static const char *TAG = "lua_store";

/* Write an app's unsaved store on the way out.
 *
 * There is no on_exit hook an app can register, and adding one would mean
 * running arbitrary app Lua during teardown -- after a stop that may have been
 * delivered by interrupting the interpreter mid-statement. This solves the
 * problem that hook would have been used for, without that: BOOT lands
 * whenever the user presses it, so "call save() after every mutation" was a
 * rule that could only be followed perfectly or not at all, and getting it
 * wrong lost data silently.
 *
 * What runs here is the launcher's own three-line function, not the app's, and
 * it is a no-op for an app that already saved.
 *
 * Two things have to be handled or this cannot call Lua at all:
 *
 *  - The interrupt hook LATCHES. Once a stop is requested it re-arms itself at
 *    every single instruction so a runaway app cannot make progress even
 *    inside a pcall -- which means our own call would raise immediately too.
 *    So: clear the flag, clear the hook, flush, put the flag back. The VM is
 *    closed moments later, so nothing else observes the gap.
 *  - The app may have died mid-error. Hence lua_pcall and a restored stack:
 *    a failure here must not stop the rest of teardown.
 */
static void store_exit_flush(lua_State *L)
{
    if (L == NULL) {
        return;
    }

    bool was_stopping = cap_lua_runtime_stop_requested(L);
    lua_Hook hook = lua_gethook(L);
    int hook_mask = lua_gethookmask(L);
    int hook_count = lua_gethookcount(L);

    if (was_stopping) {
        launcher_lua_request_stop(false);
    }
    lua_sethook(L, NULL, 0, 0);

    int top = lua_gettop(L);
    if (lua_getfield(L, LUA_REGISTRYINDEX, LUA_LOADED_TABLE) == LUA_TTABLE &&
            lua_getfield(L, -1, "store") == LUA_TTABLE &&
            lua_getfield(L, -1, "__flush_if_dirty") == LUA_TFUNCTION) {
        if (lua_pcall(L, 0, 1, 0) != LUA_OK) {
            ESP_LOGW(TAG, "exit flush failed: %s", lua_tostring(L, -1));
        } else if (lua_toboolean(L, -1)) {
            ESP_LOGI(TAG, "wrote unsaved store changes on exit");
        }
    }
    lua_settop(L, top);

    lua_sethook(L, hook, hook_mask, hook_count);
    if (was_stopping) {
        launcher_lua_request_stop(true);
    }
}

esp_err_t lua_module_store_register_exit_flush(void)
{
    return cap_lua_register_exit_cleanup(store_exit_flush);
}
