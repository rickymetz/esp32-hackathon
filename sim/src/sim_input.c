#include "sim_input.h"
#include "esp_timer.h"

#include <stdint.h>
#include <string.h>

#define SYNTH_QUEUE  8
#define SYNTH_GAP_US 90000

typedef struct {
    bool    active;
    int     x0, y0, x1, y1;
    int64_t start_us;
    int64_t dur_us;
} synth_touch_t;

static synth_touch_t s_q[SYNTH_QUEUE];
static int s_head, s_count;
static synth_touch_t s_cur;
static int64_t s_idle_since;

void sim_input_inject(int x0, int y0, int x1, int y1, int duration_ms)
{
    if (duration_ms < 60) duration_ms = 60;
    if (duration_ms > 2000) duration_ms = 2000;

    if (s_count < SYNTH_QUEUE) {
        int slot = (s_head + s_count) % SYNTH_QUEUE;
        s_q[slot] = (synth_touch_t){
            .active = true,
            .x0 = x0, .y0 = y0, .x1 = x1, .y1 = y1,
            .start_us = 0,
            .dur_us = (int64_t)duration_ms * 1000,
        };
        s_count++;
    }
}

void sim_input_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    int64_t now = esp_timer_get_time();

    if (!s_cur.active && s_count > 0 && now - s_idle_since >= SYNTH_GAP_US) {
        s_cur = s_q[s_head];
        s_cur.start_us = now;
        s_head = (s_head + 1) % SYNTH_QUEUE;
        s_count--;
    }
    synth_touch_t t = s_cur;

    if (!t.active) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    int64_t el = now - t.start_us;
    if (el >= t.dur_us) {
        data->point.x = t.x1;
        data->point.y = t.y1;
        data->state = LV_INDEV_STATE_RELEASED;
        s_cur.active = false;
        s_idle_since = now;
        return;
    }

    data->point.x = t.x0 + (int)((int64_t)(t.x1 - t.x0) * el / t.dur_us);
    data->point.y = t.y0 + (int)((int64_t)(t.y1 - t.y0) * el / t.dur_us);
    data->state = LV_INDEV_STATE_PRESSED;
}

bool sim_input_idle(void)
{
    return !s_cur.active && s_count == 0;
}
