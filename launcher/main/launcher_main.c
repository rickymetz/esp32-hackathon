/*
 * Launcher bring-up: BSP + LVGL.
 *
 * Stage 1 only -- confirms the display and touch come up through the BSP, and
 * reports which board revision the BSP bound. The Lua runtime and app loader
 * land on top of this.
 */

#include <stdio.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "bsp/esp-bsp.h"
#include "lvgl.h"

static const char *TAG = "launcher";

void app_main(void)
{
    printf("\n=== ESP32-S3-Touch-AMOLED-1.8 Launcher ===\n");
    printf("LVGL %d.%d.%d\n", LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH);

    /* Display + touch, both via the BSP so the V1/V2 revision split is handled for us. */
    lv_display_t *disp = bsp_display_start();
    if (disp == NULL) {
        ESP_LOGE(TAG, "bsp_display_start() failed");
        return;
    }
    bsp_display_backlight_on();

    ESP_LOGI(TAG, "display %" PRId32 "x%" PRId32,
             lv_display_get_horizontal_resolution(disp),
             lv_display_get_vertical_resolution(disp));

    /* Minimal UI so we can see on the panel that we got this far. */
    bsp_display_lock(0);

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101014), LV_PART_MAIN);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "Launcher");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -20);

    lv_obj_t *sub = lv_label_create(scr);
    lv_label_set_text(sub, "BSP + LVGL up");
    lv_obj_set_style_text_color(sub, lv_color_hex(0x8A8A99), LV_PART_MAIN);
    lv_obj_align(sub, LV_ALIGN_CENTER, 0, 20);

    bsp_display_unlock();

    ESP_LOGI(TAG, "internal free=%u psram free=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}
