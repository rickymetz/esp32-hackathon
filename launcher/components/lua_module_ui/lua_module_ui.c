#include "lua_module_ui.h"
#include "cap_lua.h"
#include "lauxlib.h"

/* EMBED_TXTFILES null-terminates, so _end - _start - 1 is the length. */
extern const char ui_lua_start[]       asm("_binary_ui_lua_start");
extern const char ui_lua_end[]         asm("_binary_ui_lua_end");
extern const char keyboard_lua_start[] asm("_binary_keyboard_lua_start");
extern const char keyboard_lua_end[]   asm("_binary_keyboard_lua_end");

/* Runs once per app launch via luaL_requiref: loads the embedded source
 * and runs it; the chunk returns the module table. Definitions only --
 * nothing here touches LVGL until the app calls into the module. */
static int open_embedded(lua_State *L, const char *src, size_t len, const char *name)
{
    if (luaL_loadbuffer(L, src, len, name) != LUA_OK ||
        lua_pcall(L, 0, 1, 0) != LUA_OK) {
        return lua_error(L);   /* re-raise with the loader's message */
    }
    return 1;
}

static int luaopen_ui(lua_State *L)
{
    return open_embedded(L, ui_lua_start,
                         (size_t)(ui_lua_end - ui_lua_start - 1), "ui.lua");
}

static int luaopen_keyboard(lua_State *L)
{
    return open_embedded(L, keyboard_lua_start,
                         (size_t)(keyboard_lua_end - keyboard_lua_start - 1),
                         "keyboard.lua");
}

esp_err_t lua_module_ui_register(void)
{
    /* Registration order matters: cap_lua opens modules in this order, and
     * both sources require("lvgl")/require("timer") at load time -- so this
     * must be registered AFTER lua_module_lvgl and app_timer (see
     * app_main). */
    esp_err_t err = cap_lua_register_module("ui", luaopen_ui);
    if (err != ESP_OK) {
        return err;
    }
    return cap_lua_register_module("keyboard", luaopen_keyboard);
}
