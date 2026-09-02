/* Simulator loader for the embedded-Lua `store` module.
 *
 * On device store.lua is baked into the firmware (lua_module_store.c,
 * EMBED_TXTFILES). The host build loads the same store.lua from the launcher
 * component directory via the shared sim loader, so the sim always runs the
 * current source with no copy to keep in sync. STORE_LUA_DIR is set at
 * configure time to that path.
 */
#include "lua_module_store.h"
#include "cap_lua.h"
#include "sim_embedded_lua.h"

#ifndef STORE_LUA_DIR
#error "STORE_LUA_DIR must be defined (path to components/lua_module_store)"
#endif

static int luaopen_store(lua_State *L)
{
    return sim_open_lua_module(L, STORE_LUA_DIR, "store.lua", "store.lua");
}

esp_err_t lua_module_store_register(void)
{
    esp_err_t err = cap_lua_register_module("store", luaopen_store);
    if (err != ESP_OK) {
        return err;
    }
    /* Same on-exit flush the device installs, from the same source file --
     * the simulator silently lacking it is how a device-only behaviour goes
     * untested. */
    return lua_module_store_register_exit_flush();
}
