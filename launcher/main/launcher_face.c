/*
 * The built-in watch face. See launcher_face.h.
 *
 * Layout follows docs/DESIGN_GUIDE.md: true black (an off pixel on OLED), the
 * time as the hero, everything else quiet and secondary. The time uses the
 * 120px face, which is digits and ".:" only -- exactly a clock's charset, and
 * the reason that face exists.
 */
#include "launcher_face.h"

#include "lua_module_lvgl.h"   /* lua_module_lvgl_scaled_builtin_font */

#include <stdio.h>

/* Palette, matching launcher_home.c so the shell reads as one surface. */
#define COL_TEXT   0xFFFFFF
#define COL_DIM    0x8A8A99
#define COL_FAINT  0x6E6E7A
#define COL_ACCENT 0x2F80ED
#define COL_WARN   0xEB5757

static const char *const DAYS[]   = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
static const char *const MONTHS[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                      "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

/* A battery glyph that steps with charge, so the icon carries the reading
 * even before you read the number. Mirrors the symbols the app contract
 * exposes to Lua apps, so the face and an app agree on what 40% looks like. */
static const char *battery_glyph(int pct, bool charging)
{
    if (charging)  return LV_SYMBOL_CHARGE;
    if (pct >= 90) return LV_SYMBOL_BATTERY_FULL;
    if (pct >= 65) return LV_SYMBOL_BATTERY_3;
    if (pct >= 40) return LV_SYMBOL_BATTERY_2;
    if (pct >= 15) return LV_SYMBOL_BATTERY_1;
    return LV_SYMBOL_BATTERY_EMPTY;
}

void launcher_face_build(lv_obj_t *screen, const launcher_face_data_t *data)
{
    if (screen == NULL || data == NULL) {
        return;
    }

    if (!data->time_valid) {
        /* No trustworthy time. Say so plainly rather than painting 00:00,
         * which reads as a working clock that is merely wrong.
         *
         * Deliberately NOT the 120px face here: that one is digits and ".:"
         * only (a full charset at that size costs ~2 MB, see APP_CONTRACT),
         * so a "--:--" placeholder renders as empty boxes -- caught in the
         * simulator. The message itself becomes the hero instead, at a size
         * whose charset actually exists.
         */
        lv_obj_t *hint = lv_label_create(screen);
        lv_label_set_text(hint, "Clock not set");
        lv_obj_set_style_text_color(hint, lv_color_hex(COL_TEXT), LV_PART_MAIN);
        lv_obj_set_style_text_font(hint, lua_module_lvgl_scaled_builtin_font(48), LV_PART_MAIN);
        lv_obj_align(hint, LV_ALIGN_CENTER, 0, -16);

        lv_obj_t *how = lv_label_create(screen);
        lv_label_set_text(how, "Connect Wi-Fi to sync,\nor set it in Settings");
        lv_obj_set_style_text_color(how, lv_color_hex(COL_DIM), LV_PART_MAIN);
        lv_obj_set_style_text_font(how, lua_module_lvgl_scaled_builtin_font(26), LV_PART_MAIN);
        lv_obj_set_style_text_align(how, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_align(how, LV_ALIGN_CENTER, 0, 56);
        return;
    }

    /* Hero: HH:MM. 24-hour for now -- a 12/24h preference belongs with the
     * Settings app, and inventing one here would be a setting with no UI. */
    char hhmm[8];
    snprintf(hhmm, sizeof(hhmm), "%02d:%02d", data->hour, data->min);

    lv_obj_t *time_lbl = lv_label_create(screen);
    lv_label_set_text(time_lbl, hhmm);
    lv_obj_set_style_text_color(time_lbl, lv_color_hex(COL_TEXT), LV_PART_MAIN);
    lv_obj_set_style_text_font(time_lbl, lua_module_lvgl_scaled_builtin_font(120), LV_PART_MAIN);
    lv_obj_align(time_lbl, LV_ALIGN_CENTER, 0, -20);

    /* Date, directly under the time. */
    const char *day_name   = (data->wday  >= 0 && data->wday  < 7)  ? DAYS[data->wday]        : "";
    const char *month_name = (data->month >= 1 && data->month <= 12) ? MONTHS[data->month - 1] : "";
    char date[32];
    snprintf(date, sizeof(date), "%s %d %s", day_name, data->day, month_name);

    lv_obj_t *date_lbl = lv_label_create(screen);
    lv_label_set_text(date_lbl, date);
    lv_obj_set_style_text_color(date_lbl, lv_color_hex(COL_DIM), LV_PART_MAIN);
    lv_obj_set_style_text_font(date_lbl, lua_module_lvgl_scaled_builtin_font(32), LV_PART_MAIN);
    lv_obj_align(date_lbl, LV_ALIGN_CENTER, 0, 70);

    /* Battery, bottom centre. Omitted entirely when the gauge has nothing
     * trustworthy to say -- an empty slot beats a wrong number. */
    if (data->batt_valid) {
        char batt[24];
        snprintf(batt, sizeof(batt), "%s %d%%",
                 battery_glyph(data->batt_percent, data->charging), data->batt_percent);

        lv_obj_t *batt_lbl = lv_label_create(screen);
        lv_label_set_text(batt_lbl, batt);
        lv_obj_set_style_text_color(batt_lbl,
            lv_color_hex(data->charging ? COL_ACCENT
                         : (data->batt_percent <= 15 ? COL_WARN : COL_FAINT)),
            LV_PART_MAIN);
        lv_obj_set_style_text_font(batt_lbl, lua_module_lvgl_scaled_builtin_font(26), LV_PART_MAIN);
        lv_obj_align(batt_lbl, LV_ALIGN_BOTTOM_MID, 0, -18);
    }
}
