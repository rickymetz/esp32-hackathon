/* The watch faces. See launcher_face.h. */
#include "launcher_face.h"

#include "lua_module_lvgl.h"   /* lua_module_lvgl_scaled_builtin_font */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Palette, shared with launcher_home.c so the shell reads as one surface.
 * The contrast ratios are from the accessibility pass on the original app. */
#define COL_TEXT   0xFFFFFF
#define COL_MIN    0xC8C8D4       /* minute hand: differs from hour in colour */
#define COL_ACCENT 0x2F80ED       /* reserved for SECONDS on every face */
#define COL_DIM    0xA0A0AE       /* 8.1:1 caption token */
#define COL_TICK   0x7A7A88       /* 5.0:1, clears the 3:1 non-text floor */
#define COL_TRACK  0x2A2A33       /* unlit arc / faint tick */
#define COL_WARN   0xEB5757

#define CX 184
#define CY 224

static const char *const DAYS[]   = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
static const char *const MONTHS[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                      "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

/* A hand is two segments: a narrow tail and the full-width body. lv_line draws
 * a constant-width slab, so a full-width tail reads as a phantom fourth hand,
 * and the three tails converging looked like a pivot 19px off true centre. */
typedef struct {
    lv_obj_t *body;
    lv_obj_t *tail;
    int tail_r;
    int tip_r;
} hand_t;

struct launcher_face {
    launcher_face_style_t style;

    /* complications, on every face */
    lv_obj_t *date;
    lv_obj_t *batt;

    /* digital / minimal / rings centre */
    lv_obj_t *big;
    lv_obj_t *small;

    /* analog */
    hand_t h, m, s;
    lv_obj_t *pin;          /* the pinion, hidden with the hands */

    /* rings */
    lv_obj_t *ring_h, *ring_m, *ring_s;

    /* words */
    lv_obj_t *words;
    lv_obj_t *ampm;

    /* the "clock not set" message, shown instead of the time */
    lv_obj_t *unset;

    /* Repaint gating: complications change once a minute, and the battery is
     * an I2C read. The original redrew all four faces every second and polled
     * the gauge four times a second, three quarters of it offscreen. */
    int last_min;
    int last_sec;
};

/* Points are {x=,y=} tables in the Lua binding; in C they are lv_point_precise_t.
 * Either way an array pair silently yields (0,0) -- a zero-length line that is
 * present, invisible, and raises nothing. */
static lv_point_precise_t polar(int r, float deg)
{
    float a = (deg - 90.0f) * (float)M_PI / 180.0f;
    lv_point_precise_t p = {
        .x = (lv_value_precise_t)(CX + r * cosf(a) + 0.5f),
        .y = (lv_value_precise_t)(CY + r * sinf(a) + 0.5f),
    };
    return p;
}

/* lv_line does not copy its point array, so each line needs storage that
 * outlives the call. Hands get theirs from the handle. */
typedef struct { lv_point_precise_t p[2]; } seg_t;

/* LV_EVENT_DELETE handlers are lv_event_cb_t -- void(lv_event_t *). Casting
 * lv_free to that type and letting LVGL call it passes the EVENT as the
 * pointer to free, not the user data, which corrupts the heap. Unwrap it. */
static void free_user_data_cb(lv_event_t *e)
{
    lv_free(lv_event_get_user_data(e));
}

static lv_obj_t *make_line(lv_obj_t *parent, lv_point_precise_t a, lv_point_precise_t b,
                           uint32_t colour, int width)
{
    seg_t *pts = lv_malloc(sizeof(seg_t));
    if (pts == NULL) return NULL;
    pts->p[0] = a;
    pts->p[1] = b;

    lv_obj_t *ln = lv_line_create(parent);
    /* An explicit size, or the line shrinks to its points' bounding box and
     * absolute coordinates collapse. */
    lv_obj_set_size(ln, 368, 448);
    lv_obj_set_pos(ln, 0, 0);
    lv_line_set_points(ln, pts->p, 2);
    lv_obj_set_style_line_color(ln, lv_color_hex(colour), LV_PART_MAIN);
    lv_obj_set_style_line_width(ln, width, LV_PART_MAIN);
    lv_obj_remove_flag(ln, LV_OBJ_FLAG_CLICKABLE);
    /* Freed with the line: LVGL owns the user data pointer we attach. */
    lv_obj_set_user_data(ln, pts);
    lv_obj_add_event_cb(ln, free_user_data_cb, LV_EVENT_DELETE, pts);
    return ln;
}

static void move_line(lv_obj_t *ln, lv_point_precise_t a, lv_point_precise_t b)
{
    if (ln == NULL) return;
    seg_t *pts = lv_obj_get_user_data(ln);
    if (pts == NULL) return;
    pts->p[0] = a;
    pts->p[1] = b;
    lv_line_set_points(ln, pts->p, 2);
}

/* ---- complications ------------------------------------------------------
 * In the panel's top and bottom bands, OUTSIDE the dial. They used to sit
 * inside it, where the hands are z-ordered above them and crossed the date
 * every hour. Those bands were dead black anyway, so this also stops the
 * layout pretending the panel is round. */
static void add_complications(struct launcher_face *f, lv_obj_t *parent,
                              int y_date, int y_batt)
{
    f->date = lv_label_create(parent);
    lv_label_set_text(f->date, "");
    lv_obj_set_style_text_color(f->date, lv_color_hex(COL_DIM), LV_PART_MAIN);
    lv_obj_set_style_text_font(f->date, lua_module_lvgl_scaled_builtin_font(26), LV_PART_MAIN);
    lv_obj_align(f->date, LV_ALIGN_CENTER, 0, y_date);

    f->batt = lv_label_create(parent);
    lv_label_set_text(f->batt, "");
    lv_obj_set_style_text_color(f->batt, lv_color_hex(COL_DIM), LV_PART_MAIN);
    lv_obj_set_style_text_font(f->batt, lua_module_lvgl_scaled_builtin_font(26), LV_PART_MAIN);
    lv_obj_align(f->batt, LV_ALIGN_CENTER, 0, y_batt);
}

static const char *battery_glyph(int pct, bool charging)
{
    if (charging)  return LV_SYMBOL_CHARGE;
    if (pct >= 90) return LV_SYMBOL_BATTERY_FULL;
    if (pct >= 65) return LV_SYMBOL_BATTERY_3;
    if (pct >= 40) return LV_SYMBOL_BATTERY_2;
    if (pct >= 15) return LV_SYMBOL_BATTERY_1;
    return LV_SYMBOL_BATTERY_EMPTY;
}

static void update_complications(struct launcher_face *f, const launcher_face_data_t *d)
{
    char buf[40];

    const char *day_name   = (d->wday  >= 0 && d->wday  < 7)   ? DAYS[d->wday]         : "?";
    const char *month_name = (d->month >= 1 && d->month <= 12) ? MONTHS[d->month - 1]  : "?";
    snprintf(buf, sizeof(buf), "%s %d %s", day_name, d->day, month_name);
    lv_label_set_text(f->date, buf);

    if (d->batt_valid) {
        snprintf(buf, sizeof(buf), "%s %d%%",
                 battery_glyph(d->batt_percent, d->charging), d->batt_percent);
        lv_label_set_text(f->batt, buf);
        lv_obj_set_style_text_color(f->batt,
            lv_color_hex(d->charging ? COL_ACCENT
                         : (d->batt_percent <= 15 ? COL_WARN : COL_DIM)),
            LV_PART_MAIN);
    } else {
        lv_label_set_text(f->batt, "");
    }
}

/* ---- 1: analog ---------------------------------------------------------- */

static hand_t make_hand(lv_obj_t *parent, int tail_r, int tip_r, int w, uint32_t colour)
{
    lv_point_precise_t c = polar(0, 0);
    hand_t h = { .tail_r = tail_r, .tip_r = tip_r };
    h.body = make_line(parent, c, polar(tip_r, 0), colour, w);
    h.tail = make_line(parent, c, polar(-tail_r, 0), colour, w / 3 < 2 ? 2 : w / 3);
    return h;
}

static void point_hand(const hand_t *h, float deg)
{
    move_line(h->body, polar(0, deg), polar(h->tip_r, deg));
    move_line(h->tail, polar(0, deg), polar(-h->tail_r, deg));
}

static void build_analog(struct launcher_face *f, lv_obj_t *p)
{
    /* Three tick tiers. There were only 12 marks and no minute track, so the
     * minute hand had nothing to be read against: a precision hand on an
     * approximate dial. */
    for (int i = 0; i < 60; i++) {
        if (i == 0) continue;   /* 12 is doubled below; without this it is three bars */
        bool is_hour    = (i % 5 == 0);
        bool is_quarter = (i % 15 == 0);
        int r1 = is_quarter ? 134 : (is_hour ? 140 : 148);
        make_line(p, polar(r1, i * 6.0f), polar(155, i * 6.0f),
                  is_hour ? (is_quarter ? COL_TEXT : COL_TICK) : COL_TRACK,
                  is_quarter ? 6 : (is_hour ? 4 : 2));
    }
    /* The 12 is doubled: 12 and 6 were identical, so a half-glimpsed dial had
     * no orientation anchor. */
    for (int k = 0; k < 2; k++) {
        int dx = k ? 7 : -7;
        lv_point_precise_t a = { CX + dx, CY - 134 };
        lv_point_precise_t b = { CX + dx, CY - 155 };
        make_line(p, a, b, COL_TEXT, 6);
    }

    add_complications(f, p, -186, 176);

    /* Hour and minute differ in colour as well as width and length. Two white
     * bars 21 degrees apart were genuinely ambiguous, and 4px of width
     * difference is at the limit of acuity at this viewing distance. */
    f->h = make_hand(p, 14, 96,  14, COL_TEXT);
    f->m = make_hand(p, 16, 142, 6,  COL_MIN);
    f->s = make_hand(p, 18, 150, 3,  COL_ACCENT);

    /* The pinion, created last so it draws above every hand. Without it the
     * three tails merge into a wedge and the eye reads that as the centre. */
    lv_obj_t *pin = lv_obj_create(p);
    f->pin = pin;
    lv_obj_set_size(pin, 22, 22);
    lv_obj_align(pin, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(pin, lv_color_hex(COL_TEXT), LV_PART_MAIN);
    lv_obj_set_style_radius(pin, 11, LV_PART_MAIN);
    lv_obj_set_style_border_width(pin, 0, LV_PART_MAIN);
    lv_obj_remove_flag(pin, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *dot = lv_obj_create(p);
    lv_obj_set_size(dot, 8, 8);
    lv_obj_align(dot, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_radius(dot, 4, LV_PART_MAIN);
    lv_obj_set_style_border_width(dot, 0, LV_PART_MAIN);
    lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
}

/* ---- 2: rings ----------------------------------------------------------- */

/* A true 360 dial starting at 12 o'clock. LVGL's default arc range is 135..45
 * -- a 270 degree sweep with the gap at the bottom, the visual signature of a
 * car tachometer -- so ring position did not correspond to hand position at
 * all. track_color makes the unlit remainder visible; without it the value arc
 * and its background are the same colour and 1% looks identical to 100%. */
static lv_obj_t *make_ring(lv_obj_t *parent, int r, int w, uint32_t colour)
{
    lv_obj_t *a = lv_arc_create(parent);
    lv_obj_set_size(a, r * 2, r * 2);
    lv_obj_align(a, LV_ALIGN_CENTER, 0, 0);
    lv_arc_set_range(a, 0, 1000);
    lv_arc_set_value(a, 0);
    /* Angles must be within 0..360: an earlier 270..630 computed a zero-width
     * span and collapsed every ring to its knob dot. */
    lv_arc_set_bg_angles(a, 0, 360);
    lv_arc_set_rotation(a, 270);
    lv_obj_set_style_arc_width(a, w, LV_PART_MAIN);
    lv_obj_set_style_arc_width(a, w, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(a, lv_color_hex(COL_TRACK), LV_PART_MAIN);
    lv_obj_set_style_arc_color(a, lv_color_hex(colour), LV_PART_INDICATOR);
    lv_obj_remove_style(a, NULL, LV_PART_KNOB);       /* no drag handle on a dial */
    lv_obj_remove_flag(a, LV_OBJ_FLAG_CLICKABLE);
    return a;
}

static void build_rings(struct launcher_face *f, lv_obj_t *p)
{
    f->ring_h = make_ring(p, 152, 16, COL_TEXT);
    f->ring_m = make_ring(p, 124, 14, COL_MIN);
    f->ring_s = make_ring(p, 98,  6,  COL_ACCENT);

    f->big = lv_label_create(p);
    lv_label_set_text(f->big, "--:--");
    lv_obj_set_style_text_color(f->big, lv_color_hex(COL_TEXT), LV_PART_MAIN);
    lv_obj_set_style_text_font(f->big, lua_module_lvgl_scaled_builtin_font(60), LV_PART_MAIN);
    lv_obj_align(f->big, LV_ALIGN_CENTER, 0, 0);

    add_complications(f, p, -186, 176);
}

/* ---- 3: words ----------------------------------------------------------- */

static const char *const ONES[] = { "twelve", "one", "two", "three", "four", "five",
                                    "six", "seven", "eight", "nine", "ten", "eleven" };
/* "25" rather than "twenty-five": spelled out at this size it needed ~400px
 * against a 320px box and silently reflowed to a third line for ten minutes of
 * every hour, so the face changed shape as you watched. */
static const char *const MINS[] = { "o'clock", "five past", "ten past", "quarter past",
                                    "twenty past", "25 past", "half past",
                                    "25 to", "twenty to", "quarter to", "ten to", "five to" };

static void in_words(int h, int m, char *out, size_t cap)
{
    /* The rollover is tested on the UNWRAPPED slot. A `% 12` that collapsed
     * slot 12 to 0 first meant the "...to the next hour" branch never ran at
     * :58 or :59 and the face read a whole hour early. */
    int raw  = (m + 2) / 5;
    int slot = raw % 12;
    int hour = h % 12;
    if (raw >= 7) hour = (hour + 1) % 12;

    if (slot == 0) snprintf(out, cap, "%s\n%s", ONES[hour], MINS[0]);
    else           snprintf(out, cap, "%s\n%s", MINS[slot], ONES[hour]);
}

static void build_words(struct launcher_face *f, lv_obj_t *p)
{
    f->words = lv_label_create(p);
    lv_label_set_text(f->words, "");
    lv_obj_set_width(f->words, 320);
    lv_obj_set_style_text_color(f->words, lv_color_hex(COL_TEXT), LV_PART_MAIN);
    lv_obj_set_style_text_font(f->words, lua_module_lvgl_scaled_builtin_font(40), LV_PART_MAIN);
    lv_obj_align(f->words, LV_ALIGN_LEFT_MID, 24, -20);

    f->ampm = lv_label_create(p);
    lv_label_set_text(f->ampm, "");
    lv_obj_set_style_text_color(f->ampm, lv_color_hex(COL_DIM), LV_PART_MAIN);
    lv_obj_set_style_text_font(f->ampm, lua_module_lvgl_scaled_builtin_font(26), LV_PART_MAIN);
    lv_obj_align(f->ampm, LV_ALIGN_LEFT_MID, 24, 96);

    add_complications(f, p, -186, 176);
}

/* ---- 4: minimal, 5: digital --------------------------------------------- */

/* Minimal: 120px hero. The minute is WHITE and close to the hour -- it was
 * grey at nearly the date's size, so half the time read as a caption, and
 * equal gaps above and below grouped it with the date rather than the hour. */
static void build_minimal(struct launcher_face *f, lv_obj_t *p)
{
    f->big = lv_label_create(p);
    lv_label_set_text(f->big, "--");
    lv_obj_set_style_text_color(f->big, lv_color_hex(COL_TEXT), LV_PART_MAIN);
    lv_obj_set_style_text_font(f->big, lua_module_lvgl_scaled_builtin_font(120), LV_PART_MAIN);
    lv_obj_align(f->big, LV_ALIGN_CENTER, 0, -66);

    f->small = lv_label_create(p);
    lv_label_set_text(f->small, "--");
    lv_obj_set_style_text_color(f->small, lv_color_hex(COL_TEXT), LV_PART_MAIN);
    lv_obj_set_style_text_font(f->small, lua_module_lvgl_scaled_builtin_font(60), LV_PART_MAIN);
    lv_obj_align(f->small, LV_ALIGN_CENTER, 0, 30);

    add_complications(f, p, -186, 176);
}

static void build_digital(struct launcher_face *f, lv_obj_t *p)
{
    f->big = lv_label_create(p);
    lv_label_set_text(f->big, "--:--");
    lv_obj_set_style_text_color(f->big, lv_color_hex(COL_TEXT), LV_PART_MAIN);
    lv_obj_set_style_text_font(f->big, lua_module_lvgl_scaled_builtin_font(120), LV_PART_MAIN);
    lv_obj_align(f->big, LV_ALIGN_CENTER, 0, -20);

    add_complications(f, p, 70, 176);
}

/* ---- public API --------------------------------------------------------- */

const char *launcher_face_style_name(launcher_face_style_t style)
{
    switch (style) {
        case LAUNCHER_FACE_ANALOG:  return "Analog";
        case LAUNCHER_FACE_RINGS:   return "Rings";
        case LAUNCHER_FACE_WORDS:   return "Words";
        case LAUNCHER_FACE_MINIMAL: return "Minimal";
        case LAUNCHER_FACE_DIGITAL: default: return "Digital";
    }
}

bool launcher_face_wants_seconds(launcher_face_style_t style)
{
    return style == LAUNCHER_FACE_ANALOG || style == LAUNCHER_FACE_RINGS;
}

launcher_face_t *launcher_face_create(lv_obj_t *screen, launcher_face_style_t style)
{
    if (screen == NULL) return NULL;
    if (style < 0 || style >= LAUNCHER_FACE_COUNT) style = LAUNCHER_FACE_DIGITAL;

    struct launcher_face *f = lv_malloc(sizeof(*f));
    if (f == NULL) return NULL;
    memset(f, 0, sizeof(*f));
    f->style = style;
    f->last_min = -1;
    f->last_sec = -1;

    switch (style) {
        case LAUNCHER_FACE_ANALOG:  build_analog(f, screen);  break;
        case LAUNCHER_FACE_RINGS:   build_rings(f, screen);   break;
        case LAUNCHER_FACE_WORDS:   build_words(f, screen);   break;
        case LAUNCHER_FACE_MINIMAL: build_minimal(f, screen); break;
        case LAUNCHER_FACE_DIGITAL: default: build_digital(f, screen); break;
    }
    return f;
}

void launcher_face_destroy(launcher_face_t *face)
{
    lv_free(face);   /* widgets belong to the screen */
}

/* Every widget that shows a time, across all five styles. Hidden together
 * when the clock has no trustworthy value. Each is NULL on the styles that
 * do not use it, which lv_obj_* would not tolerate, hence the guard. */
static void show_time_widgets(struct launcher_face *f, bool show)
{
    lv_obj_t *const w[] = {
        f->big, f->small, f->words, f->ampm,
        f->ring_h, f->ring_m, f->ring_s,
        f->h.body, f->h.tail, f->m.body, f->m.tail, f->s.body, f->s.tail,
        f->pin,
    };
    for (size_t i = 0; i < sizeof(w) / sizeof(w[0]); i++) {
        if (w[i] == NULL) continue;
        if (show) {
            lv_obj_remove_flag(w[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(w[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void launcher_face_update(launcher_face_t *f, const launcher_face_data_t *d)
{
    if (f == NULL || d == NULL) return;

    if (!d->time_valid) {
        /* No trustworthy time. Say so rather than painting 00:00, which reads
         * as a working clock that is merely wrong.
         *
         * Deliberately NOT in the 120px face: that one is digits and ".:" only
         * (a full charset there costs ~2 MB -- see APP_CONTRACT), so a "--:--"
         * placeholder renders as empty boxes. Caught in the simulator. */
        if (f->unset == NULL) {
            f->unset = lv_label_create(lv_obj_get_parent(f->date ? f->date : f->big));
            lv_label_set_text(f->unset, "Clock not set");
            lv_obj_set_style_text_color(f->unset, lv_color_hex(COL_TEXT), LV_PART_MAIN);
            lv_obj_set_style_text_font(f->unset, lua_module_lvgl_scaled_builtin_font(48), LV_PART_MAIN);
            lv_obj_align(f->unset, LV_ALIGN_CENTER, 0, 0);
        }
        lv_obj_remove_flag(f->unset, LV_OBJ_FLAG_HIDDEN);
        /* Hide the time itself, or the message lands ON TOP of it. Adding the
         * label was not enough: the builders seed their readouts with "--:--"
         * / "--", and in the 120px face -- digits and ".:" only, a full charset
         * there costs ~2 MB -- those dashes are missing glyphs, so a fresh
         * board drew tofu boxes through "Clock not set". The other faces left
         * a live-looking placeholder or a set of hands behind it, which is the
         * same mistake in a politer form: on an unset clock nothing that looks
         * like a time may be on screen. */
        show_time_widgets(f, false);
        return;
    }
    if (f->unset != NULL) {
        lv_obj_add_flag(f->unset, LV_OBJ_FLAG_HIDDEN);
    }
    show_time_widgets(f, true);

    char buf[64];

    switch (f->style) {
        case LAUNCHER_FACE_ANALOG:
            point_hand(&f->h, (d->hour % 12) * 30.0f + d->min * 0.5f);
            point_hand(&f->m, d->min * 6.0f + d->sec * 0.1f);
            point_hand(&f->s, d->sec * 6.0f);
            break;

        case LAUNCHER_FACE_RINGS:
            lv_arc_set_value(f->ring_h, (int)(((d->hour % 12) * 60 + d->min) / 720.0f * 1000));
            lv_arc_set_value(f->ring_m, (int)(d->min / 60.0f * 1000));
            lv_arc_set_value(f->ring_s, (int)(d->sec / 60.0f * 1000));
            snprintf(buf, sizeof(buf), "%02d:%02d", d->hour, d->min);
            lv_label_set_text(f->big, buf);
            break;

        case LAUNCHER_FACE_WORDS:
            in_words(d->hour, d->min, buf, sizeof(buf));
            lv_label_set_text(f->words, buf);
            lv_label_set_text(f->ampm, d->hour < 12 ? "AM" : "PM");
            break;

        case LAUNCHER_FACE_MINIMAL:
            snprintf(buf, sizeof(buf), "%02d", d->hour);
            lv_label_set_text(f->big, buf);
            snprintf(buf, sizeof(buf), "%02d", d->min);
            lv_label_set_text(f->small, buf);
            break;

        case LAUNCHER_FACE_DIGITAL:
        default:
            snprintf(buf, sizeof(buf), "%02d:%02d", d->hour, d->min);
            lv_label_set_text(f->big, buf);
            break;
    }

    /* Complications change once a minute and the battery is an I2C read, so
     * gate them. The original redrew all four faces every second and polled
     * the gauge four times a second, three quarters of it offscreen. */
    if (d->min != f->last_min) {
        f->last_min = d->min;
        update_complications(f, d);
    }
    f->last_sec = d->sec;
}
