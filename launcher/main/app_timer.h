/*
 * The `timer` Lua module.
 *
 * Apps must not write their own loops -- a non-yielding Lua loop freezes the
 * device. Timers are therefore the only way for an app to run code
 * periodically, which makes this module the difference between a static
 * poster and a clock.
 *
 * Timers are driven from the app task's pump loop, so callbacks run on the
 * same task and lua_State as everything else the app does.
 */

#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "lua.h"

#ifdef __cplusplus
extern "C" {
#endif

#define APP_TIMER_MAX 16

/** Register the module with cap_lua. Call ONCE at boot, not per launch. */
esp_err_t app_timer_register(void);

/** Free every timer and its Lua reference. Call at app start and app exit. */
void app_timer_reset(lua_State *L);

/**
 * @brief Fire every timer whose deadline has passed.
 * @return absolute time (esp_timer_get_time units) of the next due timer,
 *         or INT64_MAX when none are pending.
 */
int64_t app_timer_run_due(lua_State *L);

#ifdef __cplusplus
}
#endif
