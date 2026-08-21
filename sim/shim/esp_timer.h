/* Host shim for ESP-IDF's esp_timer.h -- monotonic microseconds. */
#ifndef SIM_ESP_TIMER_H
#define SIM_ESP_TIMER_H

#include <stdint.h>

typedef struct sim_esp_timer *esp_timer_handle_t;

/* Microseconds since an arbitrary fixed point (host monotonic clock). */
int64_t esp_timer_get_time(void);

#endif /* SIM_ESP_TIMER_H */
