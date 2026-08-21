/*
 * The `button` Lua module: the PWR button as an app-assignable input.
 *
 * Split across two tasks by design. The launcher's 20 ms button poller
 * calls app_button_record_edge() with debounced PWR transitions; the app
 * task calls app_button_run_pending() from its pump loop to turn recorded
 * edges into Lua callbacks. The two sides share only a spinlock-guarded
 * edge ring -- the poller never touches the Lua state.
 *
 * BOOT is deliberately absent from this API: it is the hardware Home
 * button, and no app can see or consume it (button.on("boot", ...) raises).
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "lua.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Record a debounced PWR edge. Called from the button poller task
 * only. Safe to call when no app is running; edges are cleared by
 * app_button_reset() before the next app starts.
 */
void app_button_record_edge(bool pressed);

/**
 * @brief Drain recorded edges into the subscribed Lua callbacks, and fire
 * long_pressed once per press held >= 2000 ms. Called from the app task's
 * pump loop only. Callback errors are logged and dispatch continues.
 */
void app_button_run_pending(lua_State *L);

/**
 * @brief Release every subscription and clear all recorded state. Called
 * from the app task at app setup and exit, exactly like app_timer_reset():
 * a new app must not inherit edges (or a latched press) from before it
 * launched.
 */
void app_button_reset(lua_State *L);

/**
 * @brief Register the `button` module with cap_lua. One-time, from
 * app_main().
 */
esp_err_t app_button_register(void);

#ifdef __cplusplus
}
#endif
