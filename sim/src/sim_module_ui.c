/* Simulator loader for the embedded-Lua modules `ui` and `keyboard`.
 *
 * On device these ship as binary blobs baked into the firmware by
 * lua_module_ui.c (EMBED_TXTFILES). The host build instead loads the very same
 * ui.lua / keyboard.lua from the launcher component directory via the shared
 * sim loader, so the sim always runs the current shared-UI source with no copy
 * to keep in sync.
 *
 * UI_LUA_DIR is set at configure time to that component's absolute path.
 */
#include "lua_module_ui.h"
#include "cap_lua.h"
#include "sim_embedded_lua.h"

#ifndef UI_LUA_DIR
#error "UI_LUA_DIR must be defined (path to components/lua_module_ui)"
#endif

static int luaopen_ui(lua_State *L)       { return sim_open_lua_module(L, UI_LUA_DIR, "ui.lua", "ui.lua"); }
static int luaopen_keyboard(lua_State *L) { return sim_open_lua_module(L, UI_LUA_DIR, "keyboard.lua", "keyboard.lua"); }

esp_err_t lua_module_ui_register(void)
{
    /* Registration order matters: ui/keyboard require() lvgl, timer and voice
     * at load, so those must already be registered (see the runner). */
    esp_err_t err = cap_lua_register_module("ui", luaopen_ui);
    if (err != ESP_OK) return err;
    return cap_lua_register_module("keyboard", luaopen_keyboard);
}
