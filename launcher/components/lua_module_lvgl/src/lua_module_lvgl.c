/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lua_lvgl_private.h"

static void lua_lvgl_register_funcs(lua_State *L, const luaL_Reg *funcs)
{
    for (; funcs && funcs->name; funcs++) {
        lua_pushcfunction(L, funcs->func);
        lua_setfield(L, -2, funcs->name);
    }
}

int luaopen_lvgl(lua_State *L)
{
    /* Build all per-type metatables ("lvgl.obj.<type>") with their
     * inherited base methods. After this, lua_lvgl_push_obj() can find
     * the right metatable for any widget type via lua_lvgl_metatable_for_type().
     */
    lua_lvgl_register_metatables(L);
    lua_lvgl_register_font_metatable(L);

    /* The `lvgl` module table now hosts only runtime + factory entries.
     * All object operations (set_xxx / get_xxx / delete / clean / load /
     * add_text / set_cell / ...) are invoked as methods on widget userdata. */
    lua_newtable(L);

    lua_lvgl_register_funcs(L, lua_lvgl_runtime_funcs);
    lua_lvgl_register_funcs(L, lua_lvgl_core_widget_funcs);
    lua_lvgl_register_funcs(L, lua_lvgl_extra_widget_funcs);
    lua_lvgl_register_funcs(L, lua_lvgl_complex_widget_funcs);
    lua_lvgl_register_funcs(L, lua_lvgl_eaf_module_funcs);
    lua_lvgl_register_funcs(L, lua_lvgl_event_module_funcs);
    lua_lvgl_register_funcs(L, lua_lvgl_indev_module_funcs);
    lua_lvgl_register_funcs(L, lua_lvgl_demo_module_funcs);
    lua_lvgl_register_funcs(L, lua_lvgl_font_module_funcs);

    /* lvgl.symbol.* -- the LV_SYMBOL_ glyph strings (FontAwesome codepoints
     * baked into the compiled-in fonts). Apps concatenate these into label
     * text (lvgl.symbol.play .. " Start") instead of hard-coding UTF-8 byte
     * sequences. Keep every name lowercase-snake to match the event/dir
     * vocabulary. */
    lua_newtable(L);
#define LUA_LVGL_SYMBOL(name, value)      \
    do {                                  \
        lua_pushliteral(L, value);        \
        lua_setfield(L, -2, name);        \
    } while (0)
    LUA_LVGL_SYMBOL("bullet", LV_SYMBOL_BULLET);
    LUA_LVGL_SYMBOL("audio", LV_SYMBOL_AUDIO);
    LUA_LVGL_SYMBOL("video", LV_SYMBOL_VIDEO);
    LUA_LVGL_SYMBOL("list", LV_SYMBOL_LIST);
    LUA_LVGL_SYMBOL("ok", LV_SYMBOL_OK);
    LUA_LVGL_SYMBOL("close", LV_SYMBOL_CLOSE);
    LUA_LVGL_SYMBOL("power", LV_SYMBOL_POWER);
    LUA_LVGL_SYMBOL("settings", LV_SYMBOL_SETTINGS);
    LUA_LVGL_SYMBOL("home", LV_SYMBOL_HOME);
    LUA_LVGL_SYMBOL("download", LV_SYMBOL_DOWNLOAD);
    LUA_LVGL_SYMBOL("drive", LV_SYMBOL_DRIVE);
    LUA_LVGL_SYMBOL("refresh", LV_SYMBOL_REFRESH);
    LUA_LVGL_SYMBOL("mute", LV_SYMBOL_MUTE);
    LUA_LVGL_SYMBOL("volume_mid", LV_SYMBOL_VOLUME_MID);
    LUA_LVGL_SYMBOL("volume_max", LV_SYMBOL_VOLUME_MAX);
    LUA_LVGL_SYMBOL("image", LV_SYMBOL_IMAGE);
    LUA_LVGL_SYMBOL("tint", LV_SYMBOL_TINT);
    LUA_LVGL_SYMBOL("prev", LV_SYMBOL_PREV);
    LUA_LVGL_SYMBOL("play", LV_SYMBOL_PLAY);
    LUA_LVGL_SYMBOL("pause", LV_SYMBOL_PAUSE);
    LUA_LVGL_SYMBOL("stop", LV_SYMBOL_STOP);
    LUA_LVGL_SYMBOL("next", LV_SYMBOL_NEXT);
    LUA_LVGL_SYMBOL("eject", LV_SYMBOL_EJECT);
    LUA_LVGL_SYMBOL("left", LV_SYMBOL_LEFT);
    LUA_LVGL_SYMBOL("right", LV_SYMBOL_RIGHT);
    LUA_LVGL_SYMBOL("plus", LV_SYMBOL_PLUS);
    LUA_LVGL_SYMBOL("minus", LV_SYMBOL_MINUS);
    LUA_LVGL_SYMBOL("eye_open", LV_SYMBOL_EYE_OPEN);
    LUA_LVGL_SYMBOL("eye_close", LV_SYMBOL_EYE_CLOSE);
    LUA_LVGL_SYMBOL("warning", LV_SYMBOL_WARNING);
    LUA_LVGL_SYMBOL("shuffle", LV_SYMBOL_SHUFFLE);
    LUA_LVGL_SYMBOL("up", LV_SYMBOL_UP);
    LUA_LVGL_SYMBOL("down", LV_SYMBOL_DOWN);
    LUA_LVGL_SYMBOL("loop", LV_SYMBOL_LOOP);
    LUA_LVGL_SYMBOL("directory", LV_SYMBOL_DIRECTORY);
    LUA_LVGL_SYMBOL("upload", LV_SYMBOL_UPLOAD);
    LUA_LVGL_SYMBOL("call", LV_SYMBOL_CALL);
    LUA_LVGL_SYMBOL("cut", LV_SYMBOL_CUT);
    LUA_LVGL_SYMBOL("copy", LV_SYMBOL_COPY);
    LUA_LVGL_SYMBOL("save", LV_SYMBOL_SAVE);
    LUA_LVGL_SYMBOL("bars", LV_SYMBOL_BARS);
    LUA_LVGL_SYMBOL("envelope", LV_SYMBOL_ENVELOPE);
    LUA_LVGL_SYMBOL("charge", LV_SYMBOL_CHARGE);
    LUA_LVGL_SYMBOL("paste", LV_SYMBOL_PASTE);
    LUA_LVGL_SYMBOL("bell", LV_SYMBOL_BELL);
    LUA_LVGL_SYMBOL("keyboard", LV_SYMBOL_KEYBOARD);
    LUA_LVGL_SYMBOL("gps", LV_SYMBOL_GPS);
    LUA_LVGL_SYMBOL("file", LV_SYMBOL_FILE);
    LUA_LVGL_SYMBOL("wifi", LV_SYMBOL_WIFI);
    LUA_LVGL_SYMBOL("battery_full", LV_SYMBOL_BATTERY_FULL);
    LUA_LVGL_SYMBOL("battery_3", LV_SYMBOL_BATTERY_3);
    LUA_LVGL_SYMBOL("battery_2", LV_SYMBOL_BATTERY_2);
    LUA_LVGL_SYMBOL("battery_1", LV_SYMBOL_BATTERY_1);
    LUA_LVGL_SYMBOL("battery_empty", LV_SYMBOL_BATTERY_EMPTY);
    LUA_LVGL_SYMBOL("usb", LV_SYMBOL_USB);
    LUA_LVGL_SYMBOL("bluetooth", LV_SYMBOL_BLUETOOTH);
    LUA_LVGL_SYMBOL("trash", LV_SYMBOL_TRASH);
    LUA_LVGL_SYMBOL("edit", LV_SYMBOL_EDIT);
    LUA_LVGL_SYMBOL("backspace", LV_SYMBOL_BACKSPACE);
    LUA_LVGL_SYMBOL("sd_card", LV_SYMBOL_SD_CARD);
    LUA_LVGL_SYMBOL("new_line", LV_SYMBOL_NEW_LINE);
#undef LUA_LVGL_SYMBOL
    lua_setfield(L, -2, "symbol");

    lua_pushinteger(L, LUA_MODULE_LVGL_PANEL_IF_IO);
    lua_setfield(L, -2, "PANEL_IF_IO");
    lua_pushinteger(L, LUA_MODULE_LVGL_PANEL_IF_RGB);
    lua_setfield(L, -2, "PANEL_IF_RGB");
    lua_pushinteger(L, LUA_MODULE_LVGL_PANEL_IF_MIPI_DSI);
    lua_setfield(L, -2, "PANEL_IF_MIPI_DSI");

    return 1;
}

esp_err_t lua_module_lvgl_register(void)
{
    return lua_module_lvgl_register_with_data_root(NULL);
}

esp_err_t lua_module_lvgl_register_with_data_root(const char *data_root)
{
    esp_err_t err = cap_lua_register_module(LUA_MODULE_LVGL_NAME, luaopen_lvgl);

    if (err != ESP_OK) {
        return err;
    }
    err = lua_lvgl_set_data_root(data_root);
    if (err != ESP_OK) {
        return err;
    }
    return cap_lua_register_exit_cleanup(lua_lvgl_exit_cleanup);
}
