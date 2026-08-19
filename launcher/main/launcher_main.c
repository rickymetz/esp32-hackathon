/*
 * Launcher bring-up: BSP + LVGL.
 *
 * Stage 1 -- display and touch up through the BSP.
 *
 * NOTE: bsp_display_start() alone leaves the panel dark. On this board
 * BSP_LCD_RST / BSP_LCD_TOUCH_RST / BSP_LCD_BACKLIGHT are all GPIO_NUM_NC --
 * the reset lines hang off the TCA9554 IO expander, and the BSP never
 * initialises it. The panel therefore stays held in reset and nothing renders,
 * with no error reported anywhere. We reproduce the vendor's own sequence
 * (EXIO1 + EXIO2 pulsed low then high) before starting the display.
 */

#include <stdio.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_io_expander.h"
#include "bsp/esp-bsp.h"
#include "lvgl.h"

static const char *TAG = "launcher";

/* Expander lines, from the vendor's reference sketch.
 * EXIO1 / EXIO2 = LCD and touch reset. EXIO4 = PWR button, EXIO5 = PMU IRQ. */
#define EXIO_LCD_RST    IO_EXPANDER_PIN_NUM_1
#define EXIO_TOUCH_RST  IO_EXPANDER_PIN_NUM_2
#define EXIO_PWR_BTN    IO_EXPANDER_PIN_NUM_4
#define EXIO_PMU_IRQ    IO_EXPANDER_PIN_NUM_5

static esp_err_t release_panel_reset(void)
{
    esp_io_expander_handle_t exp = bsp_io_expander_init();
    if (exp == NULL) {
        ESP_LOGE(TAG, "io expander init failed");
        return ESP_FAIL;
    }

    const uint32_t rst = EXIO_LCD_RST | EXIO_TOUCH_RST;

    ESP_ERROR_CHECK(esp_io_expander_set_dir(exp, rst, IO_EXPANDER_OUTPUT));
    ESP_ERROR_CHECK(esp_io_expander_set_dir(exp, EXIO_PWR_BTN | EXIO_PMU_IRQ, IO_EXPANDER_INPUT));

    ESP_ERROR_CHECK(esp_io_expander_set_level(exp, rst, 0));
    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_ERROR_CHECK(esp_io_expander_set_level(exp, rst, 1));
    vTaskDelay(pdMS_TO_TICKS(50));

    ESP_LOGI(TAG, "panel reset released via IO expander");
    return ESP_OK;
}

void app_main(void)
{
    printf("\n=== ESP32-S3-Touch-AMOLED-1.8 Launcher ===\n");
    printf("LVGL %d.%d.%d\n", LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH);

    ESP_ERROR_CHECK(release_panel_reset());

    lv_display_t *disp = bsp_display_start();
    if (disp == NULL) {
        ESP_LOGE(TAG, "bsp_display_start() failed");
        return;
    }
    bsp_display_backlight_on();

    ESP_LOGI(TAG, "display %" PRId32 "x%" PRId32,
             lv_display_get_horizontal_resolution(disp),
             lv_display_get_vertical_resolution(disp));

    bsp_display_lock(0);

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101014), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

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
