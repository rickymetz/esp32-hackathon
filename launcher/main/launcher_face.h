/*
 * The watch faces -- the shell's home screen.
 *
 * These live in C, not as a Lua app, for three reasons: home must work with
 * no SD card, it must survive a Lua app failing (it is the fallback when a
 * user-configured home app does not load), and keeping a VM resident just to
 * draw the time would tax the idle-power budget the always-on story needs.
 *
 * Ported from the former apps/faces.lua and apps/clock.lua, which between
 * them had three competing implementations of "the watch face". The comments
 * marking traps below came from that app's three-persona review (horology,
 * Wear OS, accessibility) and cost real debugging -- they are kept.
 *
 * Deliberately pure LVGL: no BSP, no registry, no FreeRTOS, no sensor access.
 * Time arrives already shifted to local in launcher_face_data_t; the caller
 * owns the timezone, so there is exactly one copy of that logic on the device.
 * That is also what lets the simulator render the real faces with injected
 * values and golden-test them without a board.
 *
 * Faces are created once and then mutated -- never rebuilt per tick. The
 * analog dial alone is 60+ tick lines plus three hands; recreating that four
 * times a second to move a second hand would be absurd.
 */
#pragma once

#include "lvgl.h"
#include <stdbool.h>

typedef enum {
    LAUNCHER_FACE_DIGITAL = 0,  /* big HH:MM -- the default, and the fallback */
    LAUNCHER_FACE_ANALOG,       /* hands on a real 60-minute track */
    LAUNCHER_FACE_RINGS,        /* hour/minute/second as concentric arcs */
    LAUNCHER_FACE_WORDS,        /* the time spelled out */
    LAUNCHER_FACE_MINIMAL,      /* one enormous hour over its minute */
    LAUNCHER_FACE_COUNT
} launcher_face_style_t;

/* Everything a face draws. The caller reads the hardware (or the simulator
 * injects values), applies the timezone, and fills this in. */
typedef struct {
    /* false when the RTC has never been set (fresh board, dead backup cell).
     * Faces then say so rather than painting a plausible but wrong time --
     * the same honesty rule rtc.now() follows for apps. */
    bool time_valid;
    int  hour;      /* 0-23, LOCAL */
    int  min;       /* 0-59 */
    int  sec;       /* 0-59 */
    int  year;
    int  month;     /* 1-12 */
    int  day;       /* 1-31 */
    int  wday;      /* 0 = Sunday */

    /* false while the fuel gauge is settling or the PMU is unreachable; the
     * battery complication is then omitted rather than showing a fake number. */
    bool batt_valid;
    int  batt_percent;
    bool charging;
} launcher_face_data_t;

/* Opaque: holds the handful of widgets a tick actually mutates. */
typedef struct launcher_face launcher_face_t;

/**
 * @brief Build `style` onto `screen`.
 *
 * The caller has already created and styled the screen and will load it; this
 * adds the face's widgets and returns a handle for updating them. The screen
 * owns the widgets -- deleting it deletes them.
 *
 * @return NULL on allocation failure, in which case nothing was drawn.
 */
launcher_face_t *launcher_face_create(lv_obj_t *screen, launcher_face_style_t style);

/**
 * @brief Repaint a face from fresh data. Cheap: moves hands and sets label
 *        text, allocating nothing. Safe with a NULL handle.
 */
void launcher_face_update(launcher_face_t *face, const launcher_face_data_t *data);

/**
 * @brief Free the handle. Does NOT delete widgets -- the screen owns those,
 *        so delete the screen (or let the shell's screen swap do it) as well.
 */
void launcher_face_destroy(launcher_face_t *face);

/** Human-readable style name, for Settings and the simulator. */
const char *launcher_face_style_name(launcher_face_style_t style);

/** Whether this style shows seconds, so the shell can pick its tick rate:
 *  4 Hz for a moving second hand, once every few seconds otherwise. */
bool launcher_face_wants_seconds(launcher_face_style_t style);
