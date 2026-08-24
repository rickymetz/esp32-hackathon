/*
 * The built-in watch face -- the shell's home screen.
 *
 * This is the face that is always available: it needs no SD card, no Lua VM
 * and no app, so it is what the watch shows at boot and what the shell falls
 * back to when a configured Lua home app is missing or fails to load. A user
 * may select a Lua face app instead; this one is the floor beneath that.
 *
 * Deliberately pure LVGL -- no BSP, no registry, no FreeRTOS, no sensor
 * access. All data arrives in launcher_face_data_t, exactly as
 * launcher_home_build() takes its app list through a callback. That is what
 * lets the headless simulator render the real face with injected values and
 * golden-test it without a board.
 */
#pragma once

#include "lvgl.h"
#include <stdbool.h>

/* Everything the face draws. The caller reads the hardware (or the simulator
 * injects values) and fills this in; the face itself touches nothing. */
typedef struct {
    /* false when the RTC has never been set (fresh board, dead backup cell).
     * The face then shows a "set the clock" prompt rather than a plausible
     * but wrong time -- the same honesty rule rtc.now() follows for apps. */
    bool time_valid;
    int  hour;      /* 0-23 */
    int  min;       /* 0-59 */
    int  year;      /* e.g. 2026 */
    int  month;     /* 1-12 */
    int  day;       /* 1-31 */
    int  wday;      /* 0 = Sunday */

    /* false when the fuel gauge has not settled or the PMU is unreachable;
     * the battery line is then omitted rather than showing a fake number. */
    bool batt_valid;
    int  batt_percent;   /* 0-100 */
    bool charging;
} launcher_face_data_t;

/**
 * @brief Build the built-in face onto `screen`.
 *
 * The caller has already created the screen, styled its background and will
 * load it; this only adds children. Safe to call repeatedly on a fresh screen.
 * Pure LVGL: the caller holds any display lock and owns the screen lifecycle.
 */
void launcher_face_build(lv_obj_t *screen, const launcher_face_data_t *data);
