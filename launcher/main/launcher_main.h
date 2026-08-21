/*
 * Small launcher-side API for other modules (e.g. serial_push) to drive app
 * lifecycle the same way a screen tap or the BOOT button would, without
 * reaching into launcher_main.c's statics directly.
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Launch the app named `basename` (e.g. "counter.lua"), exactly as
 * tapping its row in the launcher list does.
 *
 * @return true if the app was started, false if no app matches `basename`
 * or one is already running.
 */
bool launcher_run_app_by_name(const char *basename);

/**
 * @brief Ask the currently running app to stop, exactly as pressing BOOT
 * does.
 *
 * @return true if a stop was requested, false if no app is running.
 */
bool launcher_stop_app(void);

/**
 * @brief Inject a synthetic touch gesture, as if a finger did it.
 *
 * A second LVGL pointer indev replays it through the normal event
 * pipeline, so widgets cannot tell it from a real tap. A tap is a swipe
 * with x0==x1, y0==y1. Duration is clamped to [60, 2000] ms.
 *
 * Exists for the serial TAP/SWIPE commands: together with SHOT they let
 * an agent drive the UI and see the result without a human at the panel.
 */
void launcher_input_inject(int x0, int y0, int x1, int y1, int duration_ms);

#ifdef __cplusplus
}
#endif
