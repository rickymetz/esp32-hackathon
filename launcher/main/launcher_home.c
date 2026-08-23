/*
 * The launcher home screen (app list). See launcher_home.h.
 *
 * Moved verbatim from launcher_main.c's build_launcher_ui() and parameterised:
 * the app data arrives through get_app(), and the row/refresh behaviour through
 * event-callback pointers, so the board and the simulator share one builder.
 */
#include "launcher_home.h"

#include "lua_module_lvgl.h"   /* lua_module_lvgl_scaled_builtin_font */
#include "lv_font_lexend.h"    /* lv_font_lexend_26 */

#include <string.h>

void launcher_home_build(lv_obj_t *screen, size_t count, bool sd_mounted,
                         size_t max_visible,
                         launcher_home_get_app_t get_app, void *ctx,
                         lv_event_cb_t on_row_click,
                         lv_event_cb_t on_row_delete,
                         lv_event_cb_t on_refresh)
{
    lv_obj_t *header = lv_label_create(screen);
    lv_label_set_text(header, "Apps");
    lv_obj_set_style_text_color(header, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(header, lua_module_lvgl_scaled_builtin_font(40), LV_PART_MAIN);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 24);

    if (count == 0) {
        /* Nothing else on screen, so a centered standalone button is safe
         * here -- and necessary: after inserting a card this is the only
         * way to rescan without rebooting. */
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
    } else {
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

            if (on_row_click) {
                /* Own copy of the basename, not a pointer into the caller's
                 * (possibly transient) storage: a rescan rewrites it in place,
                 * and this row can outlive the scan that built it. Freed in
                 * on_row_delete when LVGL deletes the row. */
                char *basename = strdup(app.basename);
                lv_obj_add_event_cb(row, on_row_click, LV_EVENT_CLICKED, basename);
                if (on_row_delete) {
                    lv_obj_add_event_cb(row, on_row_delete, LV_EVENT_DELETE, basename);
                }
            }

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
}
