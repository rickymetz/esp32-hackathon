/* LVGL tick source backed by the host monotonic clock. */
#include "lvgl.h"

#include <stdint.h>
#include <time.h>

uint32_t sim_tick_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000ULL);
}

void sim_tick_init(void)
{
    lv_tick_set_cb(sim_tick_ms);
}
