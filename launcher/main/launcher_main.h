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
 * @brief Request a rebuild of the launcher home screen from the current
 * registry contents.
 *
 * For callers that changed the app set without going through the Refresh
 * button: a serial PUSH or DELETE rescans the registry, but until this runs
 * the screen still shows the list as it was when it was last built, so a
 * pushed app is invisible until someone taps Refresh. Does NOT rescan or
 * remount the card -- Refresh still owns that.
 *
 * ASYNCHRONOUS. It does not rebuild on the calling task; it posts the work to
 * the LVGL task (lv_async_call) and returns. Callers must not assume the
 * screen has changed by the time this returns.
 *
 * That is not an implementation detail, it is the point: rebuilding inline
 * meant taking s_app_mutex and then the display lock, which is the inverse of
 * the order every LVGL event callback uses, and a tap concurrent with a PUSH
 * deadlocked the board with no watchdog to recover it. See the lock-order note
 * at s_app_mutex in launcher_main.c.
 *
 * Safe from any task. Repeated calls before the pending rebuild runs are
 * coalesced into one. If an app is running or the app-info sheet is open, the
 * rebuild is deferred (not dropped) and happens when the launcher next becomes
 * visible.
 *
 * @return true if a rebuild was requested or one was already pending; false
 *         only if the request could not be queued at all.
 */
bool launcher_refresh_ui(void);

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

/**
 * @brief One BOOT press, exactly as the physical button delivers it.
 *
 * BOOT is the only navigation control and the only escape hatch, and until now
 * it was the one input the harness could not produce -- RUN/STOP go through
 * launcher_stop_app(), not through this. That left the three-way toggle, the
 * app list, and the wake-from-dark path unverifiable without a finger.
 *
 * This does NOT weaken the guarantee that BOOT belongs to the hardware: the
 * serial link is a development channel, the same one that already injects
 * touch (TAP/SWIPE) and PWR edges. No app can reach it.
 *
 * Call from a task holding neither the display lock nor s_app_mutex.
 */
void launcher_boot_press(void);

/**
 * @brief Wake the panel and restart the inactivity ladder from the top.
 *
 * Safe from any task. Sets brightness to 100%, re-enables the touch indev,
 * and resets LVGL's inactivity clock. Takes no launcher mutex.
 */
void launcher_screen_wake(void);

#ifdef __cplusplus
}
#endif
