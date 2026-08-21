#include "sim_display.h"
#include "png_write.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Full-frame render buffer LVGL draws into (RGB565, LV_COLOR_DEPTH 16). */
static uint16_t s_render_buf[SIM_HOR_RES * SIM_VER_RES];
/* Our owned copy, updated on every flush -- the source for capture. */
static uint16_t s_framebuf[SIM_HOR_RES * SIM_VER_RES];
static lv_display_t *s_disp;

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    const int32_t stride = SIM_HOR_RES;
    const uint16_t *src = (const uint16_t *)px_map;
    /* FULL render mode hands us the whole frame, but copy by area so this is
     * also correct if the render mode ever changes. */
    for (int32_t y = area->y1; y <= area->y2; y++) {
        for (int32_t x = area->x1; x <= area->x2; x++) {
            s_framebuf[y * stride + x] = *src++;
        }
    }
    lv_display_flush_ready(disp);
}

lv_display_t *sim_display_init(void)
{
    s_disp = lv_display_create(SIM_HOR_RES, SIM_VER_RES);
    lv_display_set_flush_cb(s_disp, flush_cb);
    lv_display_set_buffers(s_disp, s_render_buf, NULL, sizeof(s_render_buf),
                           LV_DISPLAY_RENDER_MODE_FULL);
    return s_disp;
}

int sim_display_distinct_colors(int cap)
{
    /* One bit per possible RGB565 value (64K bits = 8 KiB). */
    static uint8_t seen[65536 / 8];
    memset(seen, 0, sizeof(seen));

    lv_refr_now(s_disp);

    int count = 0;
    for (int i = 0; i < SIM_HOR_RES * SIM_VER_RES; i++) {
        uint16_t px = s_framebuf[i];
        if (!(seen[px >> 3] & (1u << (px & 7)))) {
            seen[px >> 3] |= (uint8_t)(1u << (px & 7));
            if (++count >= cap) break;
        }
    }
    return count;
}

int sim_display_capture_png(const char *path)
{
    /* Render pending changes now so the framebuffer is current. */
    lv_refr_now(s_disp);

    static uint8_t rgb[SIM_HOR_RES * SIM_VER_RES * 3];
    for (int i = 0; i < SIM_HOR_RES * SIM_VER_RES; i++) {
        uint16_t px = s_framebuf[i];
        uint8_t r5 = (px >> 11) & 0x1F;
        uint8_t g6 = (px >> 5) & 0x3F;
        uint8_t b5 = px & 0x1F;
        /* Expand to 8 bits with rounding. */
        rgb[i * 3 + 0] = (uint8_t)((r5 * 255 + 15) / 31);
        rgb[i * 3 + 1] = (uint8_t)((g6 * 255 + 31) / 63);
        rgb[i * 3 + 2] = (uint8_t)((b5 * 255 + 15) / 31);
    }
    return png_write_rgb(path, rgb, SIM_HOR_RES, SIM_VER_RES);
}
