/* Slice 1: prove LVGL renders our panel geometry into a PNG, no hardware. */
#include "lvgl.h"
#include "sim_display.h"

#include <stdio.h>

void sim_tick_init(void);

int main(int argc, char **argv)
{
    const char *out = argc > 1 ? argv[1] : "slice1.png";

    lv_init();
    sim_tick_init();
    sim_display_init();

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101014), 0);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "Sim OK");
    lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 30);

    lv_obj_t *btn = lv_button_create(scr);
    lv_obj_set_size(btn, 240, 120);
    lv_obj_align(btn, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x2f80ed), 0);
    lv_obj_t *btn_lbl = lv_label_create(btn);
    lv_label_set_text(btn_lbl, "Tap me");
    lv_obj_center(btn_lbl);

    if (sim_display_capture_png(out) != 0) {
        fprintf(stderr, "capture failed\n");
        return 1;
    }
    printf("wrote %s (%dx%d)\n", out, SIM_HOR_RES, SIM_VER_RES);
    return 0;
}
