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
    const char *icon;      /* icon-name key for the grid glyph, or NULL -> letter
                            * avatar. launcher_home_default_icon() derives one
                            * from the basename; both callers use it. */
    const char *icon_path; /* optional LVGL path to a card icon the app ships
                            * ("D:/apps/<id>/icon.bin"). Tried first when set and
                            * the file exists; otherwise the icon/letter fallback
                            * chain below runs. NULL for apps without one. */

    /* False for an app baked into the firmware: the info sheet then omits
     * Delete entirely rather than offering a control that must refuse. A
     * button you are not allowed to press is worse than no button. */
    bool deletable;
} launcher_home_app_t;

/* Curated default icon for a known app basename (case-insensitive substring
 * match, extension ignored). Returns an icon-name key launcher_home resolves to
 * a glyph, or NULL when no pictograph fits (the grid then shows a letter
 * avatar). Shared by the device and the simulator so both pick the same icon. */
const char *launcher_home_default_icon(const char *basename);

/* Fill *out for app `index` (0-based); return false to stop early (end of
 * list). Called once per visible row during the build. */
typedef bool (*launcher_home_get_app_t)(size_t index, launcher_home_app_t *out, void *ctx);

/* The home screen shows apps either as a scrollable vertical list (the
 * original) or as a swipeable 2x2 grid of icon tiles ("sheets"). A header
 * toggle switches between them. */
typedef enum {
    LAUNCHER_VIEW_LIST = 0,
    LAUNCHER_VIEW_GRID = 1,
} launcher_view_t;

/*
 * Build the home UI onto `screen` (which the caller has already created, styled
 * with a black background, and will load). Draws the "Apps" header, then either
 * the empty state (a message + a Refresh button) or the scrollable list of app
 * rows capped at `max_visible` (with a "N more not shown" note and a Refresh row
 * at the end).
 *
 * `view` selects the list or the 2x2 grid. When there are apps, a toggle in the
 * top-right corner switches between them via `on_toggle` (CLICKED).
 *
 * When `on_row_click` is non-NULL each app row/tile is wired for interaction: it
 * gets `on_row_click` (SHORT_CLICKED -- so a long-press does not also launch)
 * with user_data set to a strdup of the app's basename, and `on_row_delete`
 * (DELETE) to free that copy when LVGL deletes it. `on_row_long_press`
 * (LONG_PRESSED), when non-NULL, gets the same basename copy -- the launcher
 * uses it to open the app-info sheet. `on_refresh` (CLICKED) is attached to the
 * Refresh control, and `on_toggle` (CLICKED) to the view toggle, when non-NULL.
 * Pass NULL for the callbacks (the simulator) to render a non-interactive screen.
 *
 * Pure LVGL: the caller holds any display lock and owns the screen lifecycle.
 */
void launcher_home_build(lv_obj_t *screen, size_t count, bool sd_mounted,
                         size_t max_visible, launcher_view_t view,
                         launcher_home_get_app_t get_app, void *ctx,
                         lv_event_cb_t on_row_click,
                         lv_event_cb_t on_row_delete,
                         lv_event_cb_t on_row_long_press,
                         lv_event_cb_t on_refresh,
                         lv_event_cb_t on_toggle);

/*
 * Build the app-info sheet onto `screen` (caller-created, black, will load):
 * the app's icon, its name, a `detail` line (e.g. size), and an armed Delete
 * plus a Cancel. `on_delete` (CLICKED) fires only after a 400ms arm delay (the
 * sanctioned destructive-action pattern); `on_cancel` (CLICKED) dismisses. Both
 * may be NULL (the simulator renders it non-interactive). Pure LVGL.
 */
void launcher_home_app_sheet(lv_obj_t *screen, const launcher_home_app_t *app,
                             const char *detail,
                             lv_event_cb_t on_delete, lv_event_cb_t on_cancel);
