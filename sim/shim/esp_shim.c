/* Implementations for the ESP-IDF / FreeRTOS host shims. */
#include "esp_err.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_lcd_touch.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include <stdlib.h>
#include <stdint.h>
#include <time.h>

/* --- esp_err ------------------------------------------------------------ */

const char *esp_err_to_name(esp_err_t code)
{
    switch (code) {
    case ESP_OK:                  return "ESP_OK";
    case ESP_FAIL:                return "ESP_FAIL";
    case ESP_ERR_NO_MEM:          return "ESP_ERR_NO_MEM";
    case ESP_ERR_INVALID_ARG:     return "ESP_ERR_INVALID_ARG";
    case ESP_ERR_INVALID_STATE:   return "ESP_ERR_INVALID_STATE";
    case ESP_ERR_INVALID_SIZE:    return "ESP_ERR_INVALID_SIZE";
    case ESP_ERR_NOT_FOUND:       return "ESP_ERR_NOT_FOUND";
    case ESP_ERR_NOT_SUPPORTED:   return "ESP_ERR_NOT_SUPPORTED";
    case ESP_ERR_TIMEOUT:         return "ESP_ERR_TIMEOUT";
    default:                      return "ESP_ERR_UNKNOWN";
    }
}

/* --- esp_timer ---------------------------------------------------------- */

int64_t esp_timer_get_time(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/* --- heap_caps ---------------------------------------------------------- */

void *heap_caps_malloc(size_t size, unsigned caps)      { (void)caps; return malloc(size); }
void *heap_caps_calloc(size_t n, size_t sz, unsigned c) { (void)c; return calloc(n, sz); }
void  heap_caps_free(void *ptr)                         { free(ptr); }

/* --- FreeRTOS task ------------------------------------------------------ */

void vTaskDelay(TickType_t ticks)
{
    struct timespec ts;
    ts.tv_sec  = ticks / 1000;
    ts.tv_nsec = (long)(ticks % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

TaskHandle_t xTaskGetCurrentTaskHandle(void)
{
    static int the_task;
    return (TaskHandle_t)&the_task;
}

/* --- FreeRTOS mutex (single-threaded: ownership is a plain counter) ------ */

struct sim_semaphore { int count; };

SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    struct sim_semaphore *s = calloc(1, sizeof(*s));
    return (SemaphoreHandle_t)s;
}

void vSemaphoreDelete(SemaphoreHandle_t sem) { free(sem); }

BaseType_t xSemaphoreTake(SemaphoreHandle_t sem, TickType_t ticks)
{
    (void)ticks;
    if (sem) sem->count++;
    return pdTRUE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t sem)
{
    if (sem && sem->count > 0) sem->count--;
    return pdTRUE;
}

/* --- esp_lcd_touch (always "no touch"; sim uses a synthetic indev) ------- */

esp_err_t esp_lcd_touch_read_data(esp_lcd_touch_handle_t tp) { (void)tp; return ESP_OK; }

esp_err_t esp_lcd_touch_get_data(esp_lcd_touch_handle_t tp,
                                 esp_lcd_touch_point_data_t *points,
                                 uint8_t *point_num, uint8_t max_points)
{
    (void)tp; (void)points; (void)max_points;
    if (point_num) *point_num = 0;
    return ESP_OK;
}
