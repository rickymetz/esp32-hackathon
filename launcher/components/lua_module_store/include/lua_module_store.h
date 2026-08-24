/*
 * The `store` Lua module: per-app persistent key/value, saved as JSON on the
 * SD card. Apps reach it with require("store"). The launcher injects the
 * app's state-file path as the __APP_STORE__ global before running the app;
 * get/set work in memory and save() writes the file. Source: store.lua.
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Register the module with cap_lua. One-time, from app_main(). */
esp_err_t lua_module_store_register(void);

#ifdef __cplusplus
}
#endif
