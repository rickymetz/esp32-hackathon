/*
 * `prefs` -- device preferences that survive a missing SD card.
 *
 * Distinct from `store`, deliberately:
 *
 *   store   per-APP state, JSON on the SD card. Big, structured, app-scoped,
 *           and gone if the card is out.
 *   prefs   DEVICE settings, small scalars in NVS. Shared with the C shell,
 *           and readable/writable with no card in the slot.
 *
 * The shell reads some of these keys directly in C (the watch face style and
 * the timezone offset), which is the whole point: Settings is a Lua app, the
 * face is C, and NVS is the one place both can meet. Keys are shared with
 * launcher_main.c's SHELL_NVS_* namespace.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t lua_module_prefs_register(void);

#ifdef __cplusplus
}
#endif
