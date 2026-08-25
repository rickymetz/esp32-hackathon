/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "lua.h"

#ifdef __cplusplus
extern "C" {
#endif

int luaopen_lvgl(lua_State *L);

/* LAUNCHER PATCH: release the LVGL locks if a Lua error unwound out of a
 * binding while they were held. Call after a failed pcall/dofile. */
void lua_lvgl_force_unlock_if_held(void);
esp_err_t lua_module_lvgl_register(void);
esp_err_t lua_module_lvgl_register_with_data_root(const char *data_root);

/* Register the D: card filesystem driver now, so the launcher's own home
 * screen can load card-resident app icons before any app has called
 * lvgl.init(). Idempotent; safe to call once at startup. */
esp_err_t lua_module_lvgl_register_fs(void);

/* Global UI font scale (default 0.8). lvgl.font(size) and the theme default
 * resolve to the built-in face nearest `size * scale`. Set/get from C so the
 * launcher can apply the user's persisted choice and scale the theme;
 * lua_module_lvgl_scaled_builtin_font() returns the face to hand a theme. */
struct _lv_font_t;
void lua_module_lvgl_set_font_scale(float scale);
float lua_module_lvgl_get_font_scale(void);
const struct _lv_font_t *lua_module_lvgl_scaled_builtin_font(int base_size);

/* Screen-timeout opt-out, set by an app via lvgl.keep_awake(). Lives in the
 * binding rather than the launcher so the simulator gets it from the same
 * definition -- see the note at s_keep_awake in lua_lvgl_runtime.c. */
bool lua_lvgl_keep_awake(void);

/* Clear the keep-awake flag. Called on app teardown so a crashed app cannot
 * pin the backlight on until reboot. */
void lua_lvgl_keep_awake_reset(void);

#ifdef __cplusplus
}
#endif
