/* Slice 3: the real lua_module_lvgl bindings build a screen from Lua and it
 * renders on the host -- no device, no ESP-IDF. */
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "cap_lua.h"
#include "lua_module_lvgl.h"
#include "lvgl.h"
#include "sim_display.h"

#include <stdio.h>

void sim_tick_init(void);

/* A counter-style UI, straight from the app contract's shape. */
static const char *SCRIPT =
    "local lvgl = require('lvgl')\n"
    "lvgl.init({ buffer_lines = 40 })\n"
    "local scr = lvgl.create_screen()\n"
    "scr:set_style({ bg_color = '#101014' })\n"
    "lvgl.label(scr, { text = 'Counter', align = 'top_mid', y = 30, text_color = '#ffffff' })\n"
    "local btn = lvgl.button(scr, { text = 'Tap me', align = 'center',\n"
    "    w = 240, h = 120, bg_color = '#2f80ed', text_color = '#ffffff' })\n"
    "scr:load()\n";

int main(int argc, char **argv)
{
    const char *out = argc > 1 ? argv[1] : "slice3.png";

    lv_init();
    sim_tick_init();

    if (lua_module_lvgl_register_with_data_root(".") != ESP_OK) {
        fprintf(stderr, "module register failed\n");
        return 1;
    }

    lua_State *L = luaL_newstate();
    luaL_openlibs(L);
    if (launcher_lua_open_modules(L) != ESP_OK) {
        fprintf(stderr, "open modules failed\n");
        return 1;
    }

    if (luaL_dostring(L, SCRIPT) != LUA_OK) {
        fprintf(stderr, "lua error: %s\n", lua_tostring(L, -1));
        return 1;
    }

    /* Drain LVGL's timer/refresh once so the loaded screen is drawn. */
    lv_timer_handler();

    if (sim_display_capture_png(out) != 0) {
        fprintf(stderr, "capture failed\n");
        return 1;
    }
    printf("wrote %s via real bindings\n", out);
    return 0;
}
