#include "lua_module_store.h"
#include "cap_lua.h"
#include "lauxlib.h"

/* EMBED_TXTFILES null-terminates, so _end - _start - 1 is the length. */
extern const char store_lua_start[] asm("_binary_store_lua_start");
extern const char store_lua_end[]   asm("_binary_store_lua_end");

/* Runs once per app launch via luaL_requiref: loads the embedded source and
 * runs it; the chunk returns the module table. The app's state-file path is
 * read from the __APP_STORE__ global the launcher injects before running the
 * app, so nothing here needs the app identity. */
static int luaopen_store(lua_State *L)
{
    size_t len = (size_t)(store_lua_end - store_lua_start - 1);
    if (luaL_loadbuffer(L, store_lua_start, len, "store.lua") != LUA_OK ||
        lua_pcall(L, 0, 1, 0) != LUA_OK) {
        return lua_error(L);   /* re-raise with the loader's message */
    }
    return 1;
}

esp_err_t lua_module_store_register(void)
{
    /* No load-time require() of other modules, so registration order is free. */
    return cap_lua_register_module("store", luaopen_store);
}
