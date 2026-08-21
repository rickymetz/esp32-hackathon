/* Synthetic touch injection for the simulator.
 *
 * Ported from the launcher's serial TAP/SWIPE path (launcher_main.c): gestures
 * are queued (never overwritten) and each plays as PRESSED along an
 * interpolated path then one RELEASED, with an enforced released-state gap
 * before the next -- so back-to-back taps stay distinct, exactly as on device.
 */
#ifndef SIM_INPUT_H
#define SIM_INPUT_H

#include <stdbool.h>
#include "lvgl.h"

/* Queue a tap (x0,y0 == x1,y1) or swipe/drag over `duration_ms`. */
void sim_input_inject(int x0, int y0, int x1, int y1, int duration_ms);

/* LVGL indev read callback (LV_INDEV_TYPE_POINTER). */
void sim_input_read_cb(lv_indev_t *indev, lv_indev_data_t *data);

/* True when nothing is queued and no gesture is mid-flight -- the runner uses
 * this to know a script step's input has fully drained. */
bool sim_input_idle(void);

#endif /* SIM_INPUT_H */
