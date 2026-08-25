/*
 * The `audio` Lua module: synthesised tones through the ES8311 speaker.
 *
 * Tones rather than files, deliberately. A metronome, a countdown alarm
 * and game feedback all need a beep, not a sample -- and beeps need no
 * assets on the card, which matters because the serial link can only
 * carry .lua files today.
 *
 * Nothing blocks: tone() queues and returns, a playback task
 * synthesises and writes. Same shape as `voice`, and for the same
 * reason -- an app builds its UI and returns.
 *
 * The speaker and the microphone share one I2S bus, so playback and
 * voice capture are mutually exclusive; each refuses while the other
 * holds it rather than producing garbage.
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "lua.h"

#ifdef __cplusplus
extern "C" {
#endif

/** True while the speaker is open. Checked by the voice module. */
bool app_audio_is_playing(void);

/** Stop playback and drop anything queued. From app setup and exit. */
void app_audio_reset(lua_State *L);

/** Register the module. One-time, from app_main(). */
esp_err_t app_audio_register(void);

/**
 * @brief Set the output volume, 0-100, from C.
 *
 * The shell restores the user's saved level at boot. Without this the Settings
 * volume was write-only: it went to NVS and nothing ever read it back, so every
 * reboot silently returned to the built-in default.
 *
 * Out-of-range values are ignored rather than clamped, so a missing or corrupt
 * setting leaves the default in place instead of forcing it to an edge.
 */
void app_audio_set_volume(int volume);

#ifdef __cplusplus
}
#endif
