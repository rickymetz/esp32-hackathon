/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

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

#ifdef __cplusplus
}
#endif
