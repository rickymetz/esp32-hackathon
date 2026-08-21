/*
 * The `ui` and `keyboard` Lua modules: shared UI primitives and text
 * entry, written in Lua and embedded in the firmware. Apps reach them
 * with require("ui") / require("keyboard") -- they cannot live on the SD
 * card because the sandbox removes `package`, so apps cannot require()
 * files at all. Source: ui.lua / keyboard.lua alongside.
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Register both modules with cap_lua. One-time, from app_main(). */
esp_err_t lua_module_ui_register(void);

#ifdef __cplusplus
}
#endif
