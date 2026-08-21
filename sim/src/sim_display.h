/* Headless LVGL display for the simulator.
 *
 * Creates a 368x448 display (matching the Waveshare panel) that renders into an
 * owned RGB565 framebuffer instead of a real screen. Capture converts that
 * framebuffer to a PNG -- the host-side equivalent of the board's SHOT command.
 */
#ifndef SIM_DISPLAY_H
#define SIM_DISPLAY_H

#include "lvgl.h"

#define SIM_HOR_RES 368
#define SIM_VER_RES 448

/* Create the headless display and register it with LVGL. Call once after
 * lv_init(). Returns the display handle (also becomes LVGL's default). */
lv_display_t *sim_display_init(void);

/* Boot the sim's display service: create the display + synthetic touch indev
 * and apply the launcher's Lexend-32 theme, once, mirroring the launcher's
 * app_main(). Must run before any app calls lvgl.init(). */
void sim_display_service_boot(void);

/* Force a synchronous render of any pending screen changes into the
 * framebuffer, then write it to `path` as a PNG. Returns 0 on success. */
int sim_display_capture_png(const char *path);

#endif /* SIM_DISPLAY_H */
