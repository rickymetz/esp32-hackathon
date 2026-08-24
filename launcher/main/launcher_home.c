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
#include "launcher_icons.h"    /* launcher_app_image -- custom icon bitmaps */

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

/* True if `path` names a file the LVGL filesystem can open (a card icon the
 * app ships). Uses only the LVGL FS API so launcher_home stays pure LVGL; the
 * D: driver is registered at startup on device and in the sim. Opening then
 * closing is the cheapest existence probe -- an lv_image with a missing source
 * would otherwise render as an empty box instead of falling back. */
static bool card_icon_exists(const char *path)
{
    if (!path) {
        return false;
    }
    lv_fs_file_t f;
    if (lv_fs_open(&f, path, LV_FS_MODE_RD) != LV_FS_RES_OK) {
        return false;
    }
    lv_fs_close(&f);
    return true;
}

/* The image source to use for an app's tile: its shipped card icon when
 * present, else a compiled-in bitmap for a known icon key, else NULL (the
 * caller then draws a glyph or letter avatar). lv_image_set_src() accepts both
 * a file-path string and an lv_image_dsc_t*, so one source drives both. */
static const void *app_image_src(const launcher_home_app_t *app)
{
    if (card_icon_exists(app->icon_path)) {
        return app->icon_path;
    }
    return launcher_app_image(app->icon);
}

/* ---- app icons: a glyph per app ---------------------------------------- */

/* Resolve an icon-name key to a UTF-8 glyph string. Mixes the LVGL built-in
 * symbols with the extended FontAwesome set baked into the Lexend faces (see
 * fonts_lexend/ICONS.md); both render through the theme font's icon fallback.
 * Returns NULL for an unknown key. */
static const char *glyph_for(const char *key)
{
    if (!key) return NULL;
    struct { const char *k; const char *g; } map[] = {
        { "clock",       "\xEF\x80\x97" },       /* U+F017 */
        { "stopwatch",   "\xEF\x8B\xB2" },       /* U+F2F2 */
        { "sun",         "\xEF\x86\x85" },       /* U+F185 */
        { "fire",        "\xEF\x81\xAD" },       /* U+F06D */
        { "heart",       "\xEF\x80\x84" },       /* U+F004 */
        { "thermometer", "\xEF\x8B\x89" },       /* U+F2C9 */
        { "microphone",  "\xEF\x84\xB0" },       /* U+F130 */
        { "audio",       LV_SYMBOL_AUDIO },
        { "tint",        LV_SYMBOL_TINT },
        { "list",        LV_SYMBOL_LIST },
        { "settings",    LV_SYMBOL_SETTINGS },
        { "wifi",        LV_SYMBOL_WIFI },
        { "plus",        LV_SYMBOL_PLUS },
        { "keyboard",    LV_SYMBOL_KEYBOARD },
        { "image",       LV_SYMBOL_IMAGE },
        { "dollar",      "$" },
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (!strcmp(key, map[i].k)) return map[i].g;
    }
    return NULL;
}

/* True if `hay` (lower-cased) contains `needle`. */
static bool name_has(const char *hay, const char *needle)
{
    size_t nl = strlen(needle);
    for (const char *p = hay; *p; p++) {
        size_t i = 0;
        while (i < nl && p[i]) {
            char c = p[i];
            if (c >= 'A' && c <= 'Z') c += 32;
            if (c != needle[i]) break;
            i++;
        }
        if (i == nl) return true;
    }
    return false;
}

const char *launcher_home_default_icon(const char *basename)
{
    if (!basename) return NULL;
    struct { const char *sub; const char *icon; } map[] = {
        { "calculator", "calculator" },  /* before "counter" -- shares "c…" */
        { "stopwatch",  "stopwatch" },   /* before "watch"/others */
        { "countdown",  "hourglass" },
        { "clock",      "clock" },
        { "face",       "faces" },        /* watch faces */
        { "counter",    "plus" },
        { "tally",      "list" },
        { "tone",       "audio" },
        { "metronome",  "metronome" },
        { "flashlight", "flashlight" },
        { "reaction",   "fire" },
        { "color",      "color" },
        { "settings",   "settings" },
        { "wifi",       "wifi" },
        { "sensor",     "thermometer" },
        { "sign",       "keyboard" },
        { "breathe",    "heart" },
        { "tip",        "dollar" },
        { "voice",      "microphone" },
        { "dice",       "dice" },
        { "level",      "level" },
        { "simon",      "simon" },
        /* hello_world and any unmapped app fall through to the letter avatar */
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (name_has(basename, map[i].sub)) return map[i].icon;
    }
    return NULL;
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

/* The header view switcher follows the corner-button standard (ui.corner_button):
 * a 72px circular visual centred in an 88px invisible hit area, so it reads at
 * watch scale but still catches taps. Icon-only -> a circle. */
#define CORNER_TARGET 88
#define CORNER_VIS    72

static void add_toggle(lv_obj_t *screen, launcher_view_t view, lv_event_cb_t on_toggle)
{
    const int inset = (CORNER_TARGET - CORNER_VIS) / 2;

    lv_obj_t *visual = lv_button_create(screen);
    lv_obj_set_size(visual, CORNER_VIS, CORNER_VIS);
    lv_obj_align(visual, LV_ALIGN_TOP_RIGHT, -4 - inset, 8 + inset);
    lv_obj_set_style_bg_color(visual, lv_color_hex(0x1E1E28), LV_PART_MAIN);
    lv_obj_set_style_radius(visual, CORNER_VIS / 2, LV_PART_MAIN);   /* circle */
    lv_obj_set_style_pad_all(visual, 0, LV_PART_MAIN);
    lv_obj_clear_flag(visual, LV_OBJ_FLAG_CLICKABLE);   /* the hit area takes taps */
    draw_toggle_glyph(visual, view);

    lv_obj_t *hit = lv_button_create(screen);
    lv_obj_set_size(hit, CORNER_TARGET, CORNER_TARGET);
    lv_obj_align(hit, LV_ALIGN_TOP_RIGHT, -4, 8);
    lv_obj_set_style_bg_opa(hit, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(hit, 0, LV_PART_MAIN);
    if (on_toggle) {
        lv_obj_add_event_cb(hit, on_toggle, LV_EVENT_CLICKED, NULL);
    }
}

/* ---- the list layout (the original) ------------------------------------ */

/* A small leading icon for a list row, following the same image->glyph->letter
 * fallback chain as the grid tiles, so both views read as the same launcher.
 * Decorative (non-clickable) so taps still fall through to the row button. */
#define LIST_ICON 64

static void row_icon(lv_obj_t *row, const launcher_home_app_t *app)
{
    const void *img = app_image_src(app);
    if (img) {
        /* The icon bitmap is a disc baked over black; on the true-black grid the
         * corners vanish, but on the navy rows they'd read as a dark square. A
         * corner-clipping circular parent hides them so the disc floats clean. */
        lv_obj_t *clip = lv_obj_create(row);
        lv_obj_set_size(clip, LIST_ICON, LIST_ICON);
        lv_obj_align(clip, LV_ALIGN_LEFT_MID, 14, 0);
        lv_obj_set_style_radius(clip, LIST_ICON / 2, LV_PART_MAIN);
        lv_obj_set_style_clip_corner(clip, true, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(clip, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(clip, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(clip, 0, LV_PART_MAIN);
        lv_obj_remove_flag(clip, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(clip, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *im = lv_image_create(clip);
        lv_image_set_src(im, img);
        lv_obj_set_size(im, LIST_ICON, LIST_ICON);
        lv_image_set_inner_align(im, LV_IMAGE_ALIGN_STRETCH);  /* scale 120->64 */
        lv_obj_center(im);
        lv_obj_remove_flag(im, LV_OBJ_FLAG_CLICKABLE);
        return;
    }

    lv_obj_t *av = lv_obj_create(row);
    lv_obj_set_size(av, LIST_ICON, LIST_ICON);
    lv_obj_align(av, LV_ALIGN_LEFT_MID, 14, 0);
    lv_obj_set_style_radius(av, LIST_ICON / 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(av, lv_color_hex(letter_color(app->name)), LV_PART_MAIN);
    lv_obj_set_style_border_width(av, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(av, 0, LV_PART_MAIN);
    lv_obj_remove_flag(av, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(av, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *lbl = lv_label_create(av);
    const char *glyph = glyph_for(app->icon);
    if (glyph) {
        lv_label_set_text(lbl, glyph);
    } else {
        char initial[2] = { app->name && app->name[0] ? app->name[0] : '?', '\0' };
        if (initial[0] >= 'a' && initial[0] <= 'z') initial[0] -= 32;
        lv_label_set_text(lbl, initial);
    }
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, lua_module_lvgl_scaled_builtin_font(26), LV_PART_MAIN);
    lv_obj_center(lbl);
}

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

        row_icon(row, &app);   /* leftmost inline icon */

        lv_obj_t *label = lv_label_create(row);
        lv_label_set_text(label, app.name);
        lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
        lv_obj_set_style_text_font(label, lua_module_lvgl_scaled_builtin_font(32), LV_PART_MAIN);
        /* Leading-aligned like every ui.row, cleared past the icon: the review
         * flagged the centered launcher rows as a second list grammar. */
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 14 + LIST_ICON + 16, 0);
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
    lv_obj_t *active = lv_tileview_get_tile_active(d->tv);
    /* Tiles sit at col * content_width; derive the page from that rather than a
     * hardcoded 368, which would misindex if the tileview ever gained padding. */
    int cw = lv_obj_get_content_width(d->tv);
    int idx = (active && cw > 0) ? (int)lv_obj_get_x(active) / cw : 0;
    for (int i = 0; i < d->n; i++) {
        lv_obj_set_style_bg_color(d->dots[i],
            lv_color_hex(i == idx ? 0x2F80ED : 0x3A3A44), LV_PART_MAIN);
    }
}

static void grid_dots_free(lv_event_t *e)
{
    free(lv_event_get_user_data(e));
}

/* One app icon: a circular avatar (the app's initial on a colour hashed from
 * its name) that IS the launch button -- no card well, no name label, so the
 * grid reads as an app launcher, not a list with pictures. Its circle matches
 * the custom image tiles, so both icon paths read as one family. */
static void app_icon(lv_obj_t *page, const launcher_home_app_t *app,
                     int cx, int cy, int size,
                     lv_event_cb_t on_click, lv_event_cb_t on_delete)
{
    lv_obj_t *icon = lv_button_create(page);
    lv_obj_set_size(icon, size, size);
    lv_obj_set_pos(icon, cx - size / 2, cy - size / 2);
    lv_obj_set_style_radius(icon, LV_RADIUS_CIRCLE, LV_PART_MAIN);  /* circle, to match the image tiles */
    lv_obj_set_style_pad_all(icon, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(icon, 0, LV_PART_MAIN);
    wire_launch(icon, app->basename, on_click, on_delete);

    /* Icon fallback chain: a card icon the app ships or a full-colour bitmap
     * (which is the whole tile art, so the button sits transparent behind it),
     * else a FontAwesome glyph on a colour-hashed tile, else a letter avatar. */
    const void *img = app_image_src(app);
    if (img) {
        lv_obj_set_style_bg_opa(icon, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_t *im = lv_image_create(icon);
        lv_image_set_src(im, img);
        /* Fill the whole tile (120px art stretched to `size`) so image discs
         * and the letter-avatar circles below are the same diameter. */
        lv_obj_set_size(im, size, size);
        lv_image_set_inner_align(im, LV_IMAGE_ALIGN_STRETCH);
        lv_obj_center(im);
        return;
    }

    lv_obj_set_style_bg_color(icon, lv_color_hex(letter_color(app->name)), LV_PART_MAIN);
    lv_obj_t *ilabel = lv_label_create(icon);
    const char *glyph = glyph_for(app->icon);
    if (glyph) {
        lv_label_set_text(ilabel, glyph);   /* a real pictograph */
    } else {
        /* Letter avatar: the app's initial, for apps with no mapped glyph. */
        char initial[2] = { app->name && app->name[0] ? app->name[0] : '?', '\0' };
        if (initial[0] >= 'a' && initial[0] <= 'z') initial[0] -= 32;
        lv_label_set_text(ilabel, initial);
    }
    lv_obj_set_style_text_color(ilabel, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(ilabel, lua_module_lvgl_scaled_builtin_font(48), LV_PART_MAIN);
    lv_obj_center(ilabel);
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

    /* Icon-only cells, evenly spaced in the 368x330 page. */
    const int ICON = 128;
    const int col_cx[GRID_COLS] = { 101, 267 };
    const int row_cy[GRID_ROWS] = { 89, 242 };
    size_t i = 0;
    for (int p = 0; p < pages; p++) {
        lv_obj_t *page = lv_tileview_add_tile(tv, p, 0, LV_DIR_HOR);
        lv_obj_set_style_pad_all(page, 0, LV_PART_MAIN);
        for (int cell = 0; cell < GRID_PER_PAGE; cell++) {
            launcher_home_app_t app;
            if (i >= count || !get_app(i, &app, ctx)) break;
            i++;
            int c = cell % GRID_COLS, r = cell / GRID_COLS;
            app_icon(page, &app, col_cx[c], row_cy[r], ICON,
                     on_row_click, on_row_delete);
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
        lv_obj_set_style_radius(refresh, 12, LV_PART_MAIN);   /* match the in-list Refresh */
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
