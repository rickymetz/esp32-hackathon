/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lua_lvgl_private.h"

static void lua_lvgl_apply_color_style_field(lua_State *L, int index, lv_obj_t *obj, const char *field)
{
    lv_color_t color;

    lua_getfield(L, index, field);
    if (!lua_isnil(L, -1)) {
        if (lua_lvgl_parse_color(L, -1, &color) != ESP_OK) {
            luaL_error(L, "lvgl style '%s' must be a 0xRRGGBB number or '#RRGGBB' string", field);
        }
        if (strcmp(field, "bg_color") == 0) {
            lv_obj_set_style_bg_color(obj, color, 0);
        } else if (strcmp(field, "text_color") == 0) {
            lv_obj_set_style_text_color(obj, color, 0);
        } else if (strcmp(field, "border_color") == 0) {
            lv_obj_set_style_border_color(obj, color, 0);
        } else if (strcmp(field, "line_color") == 0) {
            lv_obj_set_style_line_color(obj, color, 0);
            /* INDICATOR only. Setting the main part too painted an arc's
             * background track in the same colour as its value, so a
             * ring at 1%% looked identical to one at 100%% -- the whole
             * rings watch face rendered as three solid bands that
             * encoded nothing. Use track_color for the background. */
            lv_obj_set_style_arc_color(obj, color, LV_PART_INDICATOR);
            /* The knob is that arc's tip, so it takes the arc's colour;
             * left on the theme accent it read as a foreign dot sitting
             * on a white ring. */
            lv_obj_set_style_bg_color(obj, color, LV_PART_KNOB);
        } else if (strcmp(field, "track_color") == 0) {
            lv_obj_set_style_arc_color(obj, color, LV_PART_MAIN);
        } else if (strcmp(field, "knob_color") == 0) {
            /* A slider's/arc's handle -- style it apart from the track so a thin
             * track can carry a big, distinctly coloured knob. */
            lv_obj_set_style_bg_color(obj, color, LV_PART_KNOB);
        }
    }
    lua_pop(L, 1);
}

static void lua_lvgl_apply_style_int_field(lua_State *L, int index, lv_obj_t *obj, const char *field)
{
    int value;

    lua_getfield(L, index, field);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        return;
    }
    if (!lua_isinteger(L, -1)) {
        luaL_error(L, "lvgl style '%s' must be an integer", field);
    }
    value = (int)lua_tointeger(L, -1);
    lua_pop(L, 1);

    if (strcmp(field, "bg_opa") == 0) {
        lv_obj_set_style_bg_opa(obj, (lv_opa_t)value, 0);
    } else if (strcmp(field, "opa") == 0) {
        lv_obj_set_style_opa(obj, (lv_opa_t)value, 0);
    } else if (strcmp(field, "radius") == 0) {
        lv_obj_set_style_radius(obj, value, 0);
    } else if (strcmp(field, "border_width") == 0) {
        lv_obj_set_style_border_width(obj, value, 0);
    } else if (strcmp(field, "pad") == 0) {
        lv_obj_set_style_pad_all(obj, value, 0);
    } else if (strcmp(field, "pad_row") == 0) {
        lv_obj_set_style_pad_row(obj, value, 0);
    } else if (strcmp(field, "pad_column") == 0) {
        lv_obj_set_style_pad_column(obj, value, 0);
    } else if (strcmp(field, "line_width") == 0) {
        lv_obj_set_style_line_width(obj, value, 0);
    } else if (strcmp(field, "arc_width") == 0) {
        lv_obj_set_style_arc_width(obj, value, 0);
    } else if (strcmp(field, "knob_pad") == 0) {
        /* Grows the slider handle beyond the track height, for a thin-track,
         * big-knob look. */
        lv_obj_set_style_pad_all(obj, value, LV_PART_KNOB);
    } else if (strcmp(field, "knob_radius") == 0) {
        /* Round the handle independently of the track (a large value clamps to
         * a circle). */
        lv_obj_set_style_radius(obj, value, LV_PART_KNOB);
    }
}

void lua_lvgl_apply_style_opts_locked(lua_State *L, int index, lv_obj_t *obj)
{
    if (!lua_lvgl_opt_table(L, index)) {
        return;
    }

    lua_lvgl_apply_color_style_field(L, index, obj, "bg_color");
    lua_lvgl_apply_color_style_field(L, index, obj, "text_color");
    lua_lvgl_apply_color_style_field(L, index, obj, "border_color");
    lua_lvgl_apply_color_style_field(L, index, obj, "line_color");
    lua_lvgl_apply_color_style_field(L, index, obj, "track_color");
    lua_lvgl_apply_color_style_field(L, index, obj, "knob_color");
    lua_lvgl_apply_style_int_field(L, index, obj, "bg_opa");
    lua_lvgl_apply_style_int_field(L, index, obj, "opa");
    lua_lvgl_apply_style_int_field(L, index, obj, "radius");
    lua_lvgl_apply_style_int_field(L, index, obj, "border_width");
    lua_lvgl_apply_style_int_field(L, index, obj, "pad");
    lua_lvgl_apply_style_int_field(L, index, obj, "pad_row");
    lua_lvgl_apply_style_int_field(L, index, obj, "pad_column");
    lua_lvgl_apply_style_int_field(L, index, obj, "line_width");
    lua_lvgl_apply_style_int_field(L, index, obj, "arc_width");
    lua_lvgl_apply_style_int_field(L, index, obj, "knob_pad");
    lua_lvgl_apply_style_int_field(L, index, obj, "knob_radius");
    lua_lvgl_apply_font_style_field(L, index, obj);
}
int lua_lvgl_set_style(lua_State *L)
{
    lua_lvgl_obj_ud_t *ud = lua_lvgl_check_ud(L, 1);
    esp_err_t err = lua_lvgl_lock();
    lv_obj_t *obj;
    const char *obj_error = NULL;

    luaL_checktype(L, 2, LUA_TTABLE);
    if (err != ESP_OK) {
        return lua_lvgl_error_esp(L, "lock", err);
    }
    obj = lua_lvgl_validate_ud_locked(ud, NULL, &obj_error);
    if (!obj) {
        lua_lvgl_unlock();
        return luaL_error(L, "%s", obj_error);
    }
    lua_lvgl_apply_style_opts_locked(L, 2, obj);
    lua_lvgl_unlock();
    lua_pushboolean(L, 1);
    return 1;
}

/* set_style is exposed as a method on every widget metatable via the base
 * method table in lua_lvgl_methods.c. */
