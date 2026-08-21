/* Host shim for esp_lcd_touch.h.
 *
 * The sim injects touch through its own synthetic LVGL indev (mirroring the
 * launcher's serial TAP/SWIPE path), so the real-touch read path used by
 * lua_lvgl_indev.c is present only to compile and always reports "no touch".
 */
#ifndef SIM_ESP_LCD_TOUCH_H
#define SIM_ESP_LCD_TOUCH_H

#include <stdint.h>
#include "esp_err.h"

typedef struct sim_esp_lcd_touch *esp_lcd_touch_handle_t;

typedef struct {
    uint16_t x;
    uint16_t y;
    uint16_t strength;
} esp_lcd_touch_point_data_t;

esp_err_t esp_lcd_touch_read_data(esp_lcd_touch_handle_t tp);
esp_err_t esp_lcd_touch_get_data(esp_lcd_touch_handle_t tp,
                                 esp_lcd_touch_point_data_t *points,
                                 uint8_t *point_num, uint8_t max_points);

#endif /* SIM_ESP_LCD_TOUCH_H */
