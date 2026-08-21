/* Simulator loader for the embedded-Lua modules `ui` and `keyboard`.
 *
 * On device these ship as binary blobs baked into the firmware by
 * lua_module_ui.c (EMBED_TXTFILES). The host build instead loads the very same
 * ui.lua / keyboard.lua from the launcher component directory, so the sim
 * always runs the current shared-UI source with no copy to keep in sync.
 *
 * UI_LUA_DIR is set at configure time to that component's absolute path.
 */
#include "lua_module_ui.h"
#include "cap_lua.h"
#include "lauxlib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef UI_LUA_DIR
#error "UI_LUA_DIR must be defined (path to components/lua_module_ui)"
#endif

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

static int open_from_disk(lua_State *L, const char *filename, const char *chunkname)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", UI_LUA_DIR, filename);
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

static int luaopen_ui(lua_State *L)       { return open_from_disk(L, "ui.lua", "ui.lua"); }
static int luaopen_keyboard(lua_State *L) { return open_from_disk(L, "keyboard.lua", "keyboard.lua"); }

esp_err_t lua_module_ui_register(void)
{
    /* Registration order matters: ui/keyboard require() lvgl, timer and voice
     * at load, so those must already be registered (see the runner). */
    esp_err_t err = cap_lua_register_module("ui", luaopen_ui);
    if (err != ESP_OK) return err;
    return cap_lua_register_module("keyboard", luaopen_keyboard);
}
