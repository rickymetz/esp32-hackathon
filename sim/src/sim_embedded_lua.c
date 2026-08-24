/* See sim_embedded_lua.h. */
#include "sim_embedded_lua.h"
#include "lauxlib.h"

#include <stdio.h>
#include <stdlib.h>

/* Read a whole file into a malloc'd, NUL-terminated buffer. */
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

int sim_open_lua_module(lua_State *L, const char *dir, const char *filename,
                        const char *chunkname)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", dir, filename);
    size_t len = 0;
    char *src = read_file(path, &len);
    if (!src) {
        return luaL_error(L, "sim: cannot read embedded module '%s' at %s", chunkname, path);
    }
    int rc = luaL_loadbuffer(L, src, len, chunkname);
    free(src);
    if (rc != LUA_OK || lua_pcall(L, 0, 1, 0) != LUA_OK) {
        return lua_error(L);
    }
    return 1;
}
