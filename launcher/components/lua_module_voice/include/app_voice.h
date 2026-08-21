/*
 * The `voice` Lua module: offline speech commands via ESP-SR MultiNet.
 *
 * Same two-task split as app_button: a capture task feeds mic frames to
 * MultiNet and posts results into a spinlock-guarded slot; the app task
 * drains it into Lua callbacks from the pump. The capture task never
 * touches the Lua state.
 *
 * Open-vocabulary dictation does not fit this silicon (Whisper-class
 * models start ~40MB against 8MB PSRAM); command recognition over an
 * app-supplied vocabulary does. Push-to-talk, no wake word: MultiNet is
 * fed directly, skipping AFE/WakeNet entirely.
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "lua.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Drain a pending recognition result into Lua callbacks (app task). */
void app_voice_run_pending(lua_State *L);

/** Release subscriptions, stop any capture. App setup and exit. */
void app_voice_reset(lua_State *L);

/** One-time: load models, register the module. From app_main(). */
esp_err_t app_voice_register(void);

#ifdef __cplusplus
}
#endif
