/*
 * The launcher home screen (app list / app grid). See launcher_home.h.
 *
 * Factored out of launcher_main.c's build_launcher_ui() and parameterised so
 * the board and the headless simulator share one builder: the app data arrives
 * through get_app(), the row/tile/refresh/toggle behaviour through event-callback
 * pointers. Two layouts -- a scrollable list and a swipeable 2x2 grid of icon
 * tiles -- selectable with `view` and switchable at runtime via the header
 * toggle.
 */
#include "launcher_home.h"

#include "lua_module_lvgl.h"   /* lua_module_lvgl_scaled_builtin_font */
#include "lv_font_lexend.h"    /* lv_font_lexend_26 */

#include <stdlib.h>
#include <string.h>

#define GRID_COLS 2
#define GRID_ROWS 2
#define GRID_PER_PAGE (GRID_COLS * GRID_ROWS)

/* A small palette for the letter-avatar tiles, picked by a hash of the app
 * name so each app gets a stable colour without needing a real icon file. */
static uint32_t letter_color(const char *name)
{
    static const uint32_t palette[] = {
        0x2F80ED, 0x9B51E0, 0x27AE60, 0xEB5757, 0xF2994A, 0x2D9CDB,
    };
    uint32_t h = 2166136261u;
    for (const char *p = name; p && *p; p++) {
        h = (h ^ (uint32_t)(unsigned char)*p) * 16777619u;
    }
    return palette[h % (sizeof(palette) / sizeof(palette[0]))];
}

/* Attach a strdup'd basename + the click/delete callbacks to a row or tile,
 * exactly as the list rows always did. No-op when not interactive (the sim). */
static void wire_launch(lv_obj_t *obj, const char *basename,
                        lv_event_cb_t on_click, lv_event_cb_t on_delete)
{
    if (!on_click) return;
    char *copy = strdup(basename);
    lv_obj_add_event_cb(obj, on_click, LV_EVENT_CLICKED, copy);
    if (on_delete) {
        lv_obj_add_event_cb(obj, on_delete, LV_EVENT_DELETE, copy);
    }
}

/* ---- the header view toggle -------------------------------------------- */

/* Draw the target-view glyph inside the toggle from plain rectangles, so it
 * needs no icon font: a 2x2 of squares means "switch to grid", three bars mean
 * "switch to list". */
static void draw_toggle_glyph(lv_obj_t *btn, launcher_view_t current)
{
    if (current == LAUNCHER_VIEW_LIST) {          /* show a grid glyph */
        for (int i = 0; i < 4; i++) {
            lv_obj_t *sq = lv_obj_create(btn);
            lv_obj_set_size(sq, 14, 14);
            lv_obj_set_style_radius(sq, 3, LV_PART_MAIN);
            lv_obj_set_style_bg_color(sq, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
            lv_obj_set_style_border_width(sq, 0, LV_PART_MAIN);
            lv_obj_clear_flag(sq, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_align(sq, LV_ALIGN_CENTER, (i % 2) ? 9 : -9, (i / 2) ? 9 : -9);
        }
    } else {                                       /* show a list glyph */
        for (int i = 0; i < 3; i++) {
            lv_obj_t *bar = lv_obj_create(btn);
            lv_obj_set_size(bar, 34, 6);
            lv_obj_set_style_radius(bar, 3, LV_PART_MAIN);
            lv_obj_set_style_bg_color(bar, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
            lv_obj_set_style_border_width(bar, 0, LV_PART_MAIN);
            lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_align(bar, LV_ALIGN_CENTER, 0, (i - 1) * 12);
        }
    }
}

static void add_toggle(lv_obj_t *screen, launcher_view_t view, lv_event_cb_t on_toggle)
{
    lv_obj_t *btn = lv_button_create(screen);
    lv_obj_set_size(btn, 72, 72);
    lv_obj_align(btn, LV_ALIGN_TOP_RIGHT, -8, 12);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x24303C), LV_PART_MAIN);
    lv_obj_set_style_radius(btn, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_all(btn, 0, LV_PART_MAIN);
    if (on_toggle) {
        lv_obj_add_event_cb(btn, on_toggle, LV_EVENT_CLICKED, NULL);
    }
    draw_toggle_glyph(btn, view);
}

/* ---- the list layout (the original) ------------------------------------ */

static void build_list(lv_obj_t *screen, size_t count, size_t max_visible,
                       launcher_home_get_app_t get_app, void *ctx,
                       lv_event_cb_t on_row_click, lv_event_cb_t on_row_delete,
                       lv_event_cb_t on_refresh)
{
    lv_obj_t *list = lv_obj_create(screen);
    lv_obj_set_size(list, LV_PCT(100), LV_PCT(84));
    lv_obj_align(list, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(list, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(list, 12, LV_PART_MAIN);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 16, LV_PART_MAIN);
    /* Fade the scrollbar when idle -- the always-on hairline reads as
     * a rendering defect (persona review; the ui module already does
     * this, the launcher list was the holdout). */
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_ACTIVE);

    size_t visible = (count < max_visible) ? count : max_visible;
    for (size_t i = 0; i < visible; i++) {
        launcher_home_app_t app;
        if (!get_app(i, &app, ctx)) {
            break;   /* end of list, or the array shrank under us */
        }

        lv_obj_t *row = lv_button_create(list);
        lv_obj_set_size(row, LV_PCT(100), LAUNCHER_ROW_HEIGHT);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x1E1E28), LV_PART_MAIN);
        lv_obj_set_style_radius(row, 12, LV_PART_MAIN);
        wire_launch(row, app.basename, on_row_click, on_row_delete);

        lv_obj_t *label = lv_label_create(row);
        lv_label_set_text(label, app.name);
        lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
        lv_obj_set_style_text_font(label, lua_module_lvgl_scaled_builtin_font(32), LV_PART_MAIN);
        /* Leading-aligned like every ui.row: the review flagged the
         * centered launcher rows as a second list grammar. */
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 16, 0);
    }

    if (count > max_visible) {
        /* Silent truncation would be worse than the bug this caps --
         * say what's missing instead of just hiding it. */
        lv_obj_t *more = lv_label_create(list);
        lv_label_set_text_fmt(more, "%u more not shown",
                               (unsigned)(count - max_visible));
        lv_obj_set_style_text_color(more, lv_color_hex(0x8A8A99), LV_PART_MAIN);
        lv_obj_set_style_text_font(more, &lv_font_lexend_26, LV_PART_MAIN);
        lv_obj_set_style_text_align(more, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_set_width(more, LV_PCT(100));
    }

    /* Refresh lives IN the list, as its last full-width row: the list
     * container spans the screen bottom and its rows paint over anything
     * behind it, which is exactly how the old standalone bottom button
     * ended up invisible behind 3+ rows (found by Rick on device). A row
     * scrolls into reach no matter how many apps precede it. */
    lv_obj_t *refresh = lv_button_create(list);
    lv_obj_set_size(refresh, LV_PCT(100), LAUNCHER_ROW_HEIGHT);
    lv_obj_set_style_bg_color(refresh, lv_color_hex(0x24303C), LV_PART_MAIN);
    lv_obj_set_style_radius(refresh, 12, LV_PART_MAIN);
    if (on_refresh) {
        lv_obj_add_event_cb(refresh, on_refresh, LV_EVENT_CLICKED, NULL);
    }

    lv_obj_t *rlabel = lv_label_create(refresh);
    lv_label_set_text(rlabel, "Refresh");
    lv_obj_set_style_text_color(rlabel, lv_color_hex(0x9FB4C7), LV_PART_MAIN);
    lv_obj_center(rlabel);
}

/* ---- the grid layout (2x2 icon tiles, swipeable pages) ----------------- */

/* Page dots need to follow the active page; store them so the tileview's
 * value_changed handler can recolour them, and free the store on delete. */
typedef struct { lv_obj_t *dots[16]; int n; lv_obj_t *tv; } grid_dots_t;

static void grid_page_changed(lv_event_t *e)
{
    grid_dots_t *d = (grid_dots_t *)lv_event_get_user_data(e);
    lv_obj_t *active = lv_tileview_get_tile_act(d->tv);
    int idx = active ? (int)lv_obj_get_x(active) / 368 : 0;   /* col == page */
    for (int i = 0; i < d->n; i++) {
        lv_obj_set_style_bg_color(d->dots[i],
            lv_color_hex(i == idx ? 0x2F80ED : 0x3A3A44), LV_PART_MAIN);
    }
}

static void grid_dots_free(lv_event_t *e)
{
    free(lv_event_get_user_data(e));
}

/* One app tile: a rounded panel with a letter-avatar icon and the name below. */
static void app_tile(lv_obj_t *page, const launcher_home_app_t *app,
                     int x, int y, int w, int h,
                     lv_event_cb_t on_click, lv_event_cb_t on_delete)
{
    lv_obj_t *tile = lv_button_create(page);
    lv_obj_set_size(tile, w, h);
    lv_obj_set_pos(tile, x, y);
    lv_obj_set_style_bg_color(tile, lv_color_hex(0x1E1E28), LV_PART_MAIN);
    lv_obj_set_style_radius(tile, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_all(tile, 0, LV_PART_MAIN);
    wire_launch(tile, app->basename, on_click, on_delete);

    lv_obj_t *icon = lv_obj_create(tile);
    lv_obj_set_size(icon, 68, 68);
    lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 16);
    lv_obj_set_style_bg_color(icon, lv_color_hex(letter_color(app->name)), LV_PART_MAIN);
    lv_obj_set_style_radius(icon, 18, LV_PART_MAIN);
    lv_obj_set_style_border_width(icon, 0, LV_PART_MAIN);
    lv_obj_clear_flag(icon, LV_OBJ_FLAG_SCROLLABLE);

    char initial[2] = { app->name && app->name[0] ? app->name[0] : '?', '\0' };
    if (initial[0] >= 'a' && initial[0] <= 'z') initial[0] -= 32;   /* upper */
    lv_obj_t *ilabel = lv_label_create(icon);
    lv_label_set_text(ilabel, initial);
    lv_obj_set_style_text_color(ilabel, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(ilabel, lua_module_lvgl_scaled_builtin_font(32), LV_PART_MAIN);
    lv_obj_center(ilabel);

    lv_obj_t *name = lv_label_create(tile);
    lv_label_set_text(name, app->name);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_width(name, w - 12);
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_color(name, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(name, &lv_font_lexend_26, LV_PART_MAIN);
    lv_obj_align(name, LV_ALIGN_TOP_MID, 0, 94);
}

static void build_grid(lv_obj_t *screen, size_t count,
                       launcher_home_get_app_t get_app, void *ctx,
                       lv_event_cb_t on_row_click, lv_event_cb_t on_row_delete)
{
    int pages = (int)((count + GRID_PER_PAGE - 1) / GRID_PER_PAGE);
    if (pages < 1) pages = 1;
    if (pages > 16) pages = 16;   /* dots array bound; 64 apps */

    lv_obj_t *tv = lv_tileview_create(screen);
    lv_obj_set_size(tv, 368, 330);
    lv_obj_align(tv, LV_ALIGN_TOP_MID, 0, 72);
    lv_obj_set_style_bg_opa(tv, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(tv, LV_SCROLLBAR_MODE_OFF);

    const int TW = 160, TH = 150, GX = 16, GY = 14;
    const int X0 = 16, Y0 = 8;
    size_t i = 0;
    for (int p = 0; p < pages; p++) {
        lv_obj_t *page = lv_tileview_add_tile(tv, p, 0, LV_DIR_HOR);
        lv_obj_set_style_pad_all(page, 0, LV_PART_MAIN);
        for (int cell = 0; cell < GRID_PER_PAGE; cell++) {
            launcher_home_app_t app;
            if (i >= count || !get_app(i, &app, ctx)) break;
            i++;
            int c = cell % GRID_COLS, r = cell / GRID_COLS;
            app_tile(page, &app, X0 + c * (TW + GX), Y0 + r * (TH + GY),
                     TW, TH, on_row_click, on_row_delete);
        }
    }

    /* Page dots, bottom centre, tracking the active page. */
    if (pages > 1) {
        grid_dots_t *d = (grid_dots_t *)calloc(1, sizeof(grid_dots_t));
        if (d) {
            d->tv = tv;
            d->n = pages;
            lv_obj_t *row = lv_obj_create(screen);
            lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
            lv_obj_align(row, LV_ALIGN_BOTTOM_MID, 0, -14);
            lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
            lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
            lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
            lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
            lv_obj_set_style_pad_column(row, 10, LV_PART_MAIN);
            lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
            for (int p = 0; p < pages; p++) {
                lv_obj_t *dot = lv_obj_create(row);
                lv_obj_set_size(dot, 10, 10);
                lv_obj_set_style_radius(dot, 5, LV_PART_MAIN);
                lv_obj_set_style_border_width(dot, 0, LV_PART_MAIN);
                lv_obj_set_style_bg_color(dot,
                    lv_color_hex(p == 0 ? 0x2F80ED : 0x3A3A44), LV_PART_MAIN);
                lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
                d->dots[p] = dot;
            }
            lv_obj_add_event_cb(tv, grid_page_changed, LV_EVENT_VALUE_CHANGED, d);
            lv_obj_add_event_cb(tv, grid_dots_free, LV_EVENT_DELETE, d);
        }
    }
}

/* ---- entry point ------------------------------------------------------- */

void launcher_home_build(lv_obj_t *screen, size_t count, bool sd_mounted,
                         size_t max_visible, launcher_view_t view,
                         launcher_home_get_app_t get_app, void *ctx,
                         lv_event_cb_t on_row_click, lv_event_cb_t on_row_delete,
                         lv_event_cb_t on_refresh, lv_event_cb_t on_toggle)
{
    lv_obj_t *header = lv_label_create(screen);
    lv_label_set_text(header, "Apps");
    lv_obj_set_style_text_color(header, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(header, lua_module_lvgl_scaled_builtin_font(40), LV_PART_MAIN);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 24);

    if (count == 0) {
        /* Nothing else on screen, so a centered standalone button is safe
         * here -- and necessary: after inserting a card this is the only
         * way to rescan without rebooting. No view toggle: nothing to switch. */
        lv_obj_t *refresh = lv_button_create(screen);
        lv_obj_set_size(refresh, 200, 104);   /* >= 200x104; smaller drops taps */
        lv_obj_align(refresh, LV_ALIGN_BOTTOM_MID, 0, -16);
        lv_obj_set_style_bg_color(refresh, lv_color_hex(0x24303C), LV_PART_MAIN);
        if (on_refresh) {
            lv_obj_add_event_cb(refresh, on_refresh, LV_EVENT_CLICKED, NULL);
        }

        lv_obj_t *rlabel = lv_label_create(refresh);
        lv_label_set_text(rlabel, "Refresh");
        lv_obj_set_style_text_color(rlabel, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
        lv_obj_center(rlabel);

        lv_obj_t *empty = lv_label_create(screen);
        lv_label_set_text(empty, sd_mounted
                                     ? "No apps yet.\nCopy .lua files to\n/apps on the SD card."
                                     : "No SD card.\nInsert one with an\n/apps directory.");
        lv_obj_set_style_text_color(empty, lv_color_hex(0x8A8A99), LV_PART_MAIN);
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_set_style_text_font(empty, lua_module_lvgl_scaled_builtin_font(26), LV_PART_MAIN);
        lv_obj_center(empty);
        return;
    }

    /* With apps present, the top-right toggle switches list <-> grid. */
    add_toggle(screen, view, on_toggle);

    if (view == LAUNCHER_VIEW_GRID) {
        build_grid(screen, count, get_app, ctx, on_row_click, on_row_delete);
    } else {
        build_list(screen, count, max_visible, get_app, ctx,
                   on_row_click, on_row_delete, on_refresh);
    }
}
