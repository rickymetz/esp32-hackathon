/* Simulator loader for the embedded-Lua `store` module.
 *
 * On device store.lua is baked into the firmware (lua_module_store.c,
 * EMBED_TXTFILES). The host build loads the same store.lua from the launcher
 * component directory, so the sim always runs the current source with no copy
 * to keep in sync. STORE_LUA_DIR is set at configure time to that path.
 */
#include "lua_module_store.h"
#include "cap_lua.h"
#include "lauxlib.h"

#include <stdio.h>
#include <stdlib.h>

#ifndef STORE_LUA_DIR
#error "STORE_LUA_DIR must be defined (path to components/lua_module_store)"
#endif

static char *read_file(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return NULL; }
    char *buf = malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = '\0';
    if (out_len) *out_len = got;
    return buf;
}

static int luaopen_store(lua_State *L)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/store.lua", STORE_LUA_DIR);
    size_t len = 0;
    char *src = read_file(path, &len);
    if (!src) return luaL_error(L, "sim: cannot read store.lua at %s", path);
    int rc = luaL_loadbuffer(L, src, len, "store.lua");
    free(src);
    if (rc != LUA_OK || lua_pcall(L, 0, 1, 0) != LUA_OK) return lua_error(L);
    return 1;
}

esp_err_t lua_module_store_register(void)
{
    return cap_lua_register_module("store", luaopen_store);
}
