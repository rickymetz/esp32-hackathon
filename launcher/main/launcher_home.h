/*
 * The launcher's home screen (the app list), factored out of launcher_main.c
 * so it can be built anywhere LVGL runs -- on the board, and in the headless
 * simulator, which renders it with a fake app list for regression coverage.
 *
 * This file is deliberately pure LVGL: no BSP, no app registry, no FreeRTOS.
 * The caller supplies the app data through a callback and the row/refresh
 * behaviour through LVGL event callbacks, so the same builder produces the real
 * interactive launcher on device and a non-interactive render in the sim.
 */
#pragma once

#include "lvgl.h"
#include <stddef.h>
#include <stdbool.h>

/* Full-width row height. >=200x104 is the tap floor measured on this
 * digitizer; smaller rows drop taps (see docs/APP_CONTRACT.md). */
#define LAUNCHER_ROW_HEIGHT 104

typedef struct {
    const char *name;      /* row label -- LVGL copies it, so it need not persist */
    const char *basename;  /* app filename -- strdup'd into on_row_click's user_data */
} launcher_home_app_t;

/* Fill *out for app `index` (0-based); return false to stop early (end of
 * list). Called once per visible row during the build. */
typedef bool (*launcher_home_get_app_t)(size_t index, launcher_home_app_t *out, void *ctx);

/*
 * Build the home UI onto `screen` (which the caller has already created, styled
 * with a black background, and will load). Draws the "Apps" header, then either
 * the empty state (a message + a Refresh button) or the scrollable list of app
 * rows capped at `max_visible` (with a "N more not shown" note and a Refresh row
 * at the end).
 *
 * When `on_row_click` is non-NULL each app row is wired for interaction: it gets
 * `on_row_click` (CLICKED) with user_data set to a strdup of the app's basename,
 * and `on_row_delete` (DELETE) to free that copy when LVGL deletes the row.
 * `on_refresh` (CLICKED) is attached to the Refresh control when non-NULL.
 * Pass NULL for all three (the simulator) to render non-interactive rows.
 *
 * Pure LVGL: the caller holds any display lock and owns the screen lifecycle.
 */
void launcher_home_build(lv_obj_t *screen, size_t count, bool sd_mounted,
                         size_t max_visible,
                         launcher_home_get_app_t get_app, void *ctx,
                         lv_event_cb_t on_row_click,
                         lv_event_cb_t on_row_delete,
                         lv_event_cb_t on_refresh);
