#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "serial_push.h"
#include "app_registry.h"
#include "launcher_main.h"
#include "app_button.h"
#include "esp_heap_caps.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "esp_crc.h"
#include "mbedtls/base64.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "serial_push";

#define LINE_MAX     128
#define NAME_MAX      64
#define PAYLOAD_MAX (64 * 1024)

/* None of these are a security boundary -- anyone with USB can already reflash
 * the board -- but they stop a stray path from writing somewhere surprising and
 * turning into a confusing afternoon. `..` and a leading '/' are always out. */
static bool has_dotdot_or_absolute(const char *name)
{
    return name[0] == '/' || strstr(name, "..") != NULL;
}

/* A RUN/DELETE target: an app id. A flat app's id is its "counter.lua" file
 * basename; a folder app's is its bare "mygame" directory name. Either way it
 * names one entry directly under apps/, so no '/' and no '..'. */
static bool id_is_safe(const char *name)
{
    size_t len = strlen(name);
    if (len == 0 || len > NAME_MAX) {
        return false;
    }
    return !has_dotdot_or_absolute(name) && strchr(name, '/') == NULL;
}

/* A PUSH target: a path under apps/. Either a flat "counter.lua", or one
 * directory deep for a folder app ("mygame/main.lua", "mygame/icon.bin").
 * At most one '/', and it must have a non-empty folder and file on each side. */
static bool push_path_is_safe(const char *name)
{
    size_t len = strlen(name);
    if (len == 0 || len > NAME_MAX || has_dotdot_or_absolute(name)) {
        return false;
    }
    const char *slash = strchr(name, '/');
    if (slash == NULL) {
        return true;   /* flat file directly under apps/ */
    }
    if (slash == name || slash[1] == '\0') {
        return false;  /* "/foo" or "foo/" -- need dir and file both */
    }
    return strchr(slash + 1, '/') == NULL;   /* only one level deep */
}

/* Whether `id` matches an app currently known to the registry. Used by
 * RUN to tell "not_found" apart from "already_running" without either
 * duplicating launcher_main's task-state tracking or widening the
 * launcher_run_app_by_name() API beyond the bool it was specified as. */
static bool registry_has_id(const char *id)
{
    app_entry_t app;
    return app_registry_find_by_id(id, &app);
}

static bool read_line(char *out, size_t cap)
{
    size_t n = 0;
    while (n + 1 < cap) {
        int c = fgetc(stdin);
        if (c == EOF) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (c == '\r') {
            continue;
        }
        if (c == '\n') {
            out[n] = '\0';
            return true;
        }
        out[n++] = (char)c;
    }
    out[cap - 1] = '\0';

    /* Oversized line: cap-1 bytes were consumed but the rest of this
     * physical line is still sitting in the stream. Drain and discard up
     * to the next '\n' (or EOF) so every call to read_line consumes exactly
     * one whole line and the next call starts at the next line, not mid-line. */
    for (;;) {
        int c = fgetc(stdin);
        if (c == EOF) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (c == '\n') {
            break;
        }
    }
    return false;
}

static void handle_push(const char *header)
{
    char name[NAME_MAX + 1];
    unsigned expect_len = 0, expect_crc = 0;

    if (sscanf(header, "PUSH %64s %u %x", name, &expect_len, &expect_crc) != 3) {
        printf("PUSH_ERR bad_header\n");
        return;
    }
    if (!push_path_is_safe(name)) {
        printf("PUSH_ERR bad_name\n");
        return;
    }
    if (expect_len == 0 || expect_len > PAYLOAD_MAX) {
        printf("PUSH_ERR bad_length\n");
        return;
    }

    uint8_t *buf = malloc(expect_len);
    if (buf == NULL) {
        printf("PUSH_ERR no_memory\n");
        return;
    }

    size_t got = 0;
    char line[LINE_MAX];
    bool ok = true;

    while (read_line(line, sizeof(line))) {
        if (strcmp(line, "ENDPUSH") == 0) {
            break;
        }
        size_t produced = 0;
        if (mbedtls_base64_decode(buf + got, expect_len - got, &produced,
                                  (const unsigned char *)line, strlen(line)) != 0) {
            ok = false;
            break;
        }
        got += produced;
    }

    if (!ok || got != expect_len) {
        free(buf);
        printf("PUSH_ERR truncated\n");
        return;
    }
    if (esp_crc32_le(0, buf, got) != expect_crc) {
        free(buf);
        printf("PUSH_ERR crc_mismatch\n");
        return;
    }

    /* The payload is fully received and CRC-verified -- now, and only now,
     * touch the SD card. app_registry_write_app() does the write (temp file
     * + rename) and the registry rescan under the same lock that
     * app_registry_invalidate() takes to unmount the card, so a concurrent
     * Refresh cannot pull the filesystem out from under this write. The
     * lock is held only for the write/rescan, never across the multi-second
     * base64 receive above. */
    bool wrote = app_registry_write_app(name, buf, got);
    free(buf);

    if (!wrote) {
        printf("PUSH_ERR write_failed\n");
        return;
    }

    ESP_LOGI(TAG, "received %s (%u bytes)", name, (unsigned)got);
    printf("PUSH_OK %s\n", name);
}

/* RUN <basename.lua> -- launch an app exactly as tapping its row does.
 * Useful when nobody is at the board to touch the screen. */
static void handle_run(const char *header)
{
    char name[NAME_MAX + 1];

    if (sscanf(header, "RUN %64s", name) != 1) {
        printf("RUN_ERR bad_name\n");
        return;
    }
    if (!id_is_safe(name)) {
        printf("RUN_ERR bad_name\n");
        return;
    }
    if (!registry_has_id(name)) {
        printf("RUN_ERR not_found\n");
        return;
    }
    if (!launcher_run_app_by_name(name)) {
        printf("RUN_ERR already_running\n");
        return;
    }

    ESP_LOGI(TAG, "RUN %s", name);
    printf("RUN_OK %s\n", name);
}

/* LIST -- print every app the registry knows, one basename per line,
 * then a LIST_OK trailer with the count. Copies each entry out of the
 * registry (never holds the lock across printf). */
static void handle_list(void)
{
    size_t n = 0;

    for (size_t i = 0; i < APP_MAX_COUNT; i++) {
        app_entry_t app;
        if (!app_registry_get_copy(i, &app)) {
            break;
        }
        printf("APP %s\n", app.id);   /* the id RUN/DELETE accept, folder or flat */
        n++;
    }
    printf("LIST_OK %u\n", (unsigned)n);
}

/* DELETE <basename.lua> -- remove an app file from the card. Goes through
 * app_registry_delete_app(), which holds the registry lock across the
 * unlink and rescans before returning -- never a bare unlink, so a
 * concurrent Refresh cannot unmount the card mid-delete. */
static void handle_delete(const char *header)
{
    char name[NAME_MAX + 1];

    if (sscanf(header, "DELETE %64s", name) != 1 || !id_is_safe(name)) {
        printf("DELETE_ERR bad_name\n");
        return;
    }
    if (!registry_has_id(name)) {
        printf("DELETE_ERR not_found\n");
        return;
    }
    if (!app_registry_delete_app(name)) {
        printf("DELETE_ERR delete_failed\n");
        return;
    }

    ESP_LOGI(TAG, "DELETE %s", name);
    printf("DELETE_OK %s\n", name);
}

/* SHOT -- re-render the active screen into a PSRAM buffer with
 * lv_snapshot and stream it out base64, so an agent can SEE the UI
 * without a human photographing the panel. Rick's request after a day
 * of design tuning over photos.
 *
 * Format: "SHOT <w> <h> <stride>" then base64 lines then "ENDSHOT".
 * Pixel data is RGB565 little-endian, h*stride bytes. The snapshot runs
 * under the display lock; streaming happens after it is released, from
 * our own copy of nothing -- the draw buf stays valid because only this
 * task frees it, and LVGL never touches snapshot bufs it did not make. */
static void handle_shot(void)
{
    bsp_display_lock(0);
    lv_obj_t *act = lv_screen_active();
    lv_draw_buf_t *buf = act ? lv_snapshot_take(act, LV_COLOR_FORMAT_RGB565) : NULL;
    bsp_display_unlock();

    if (buf == NULL) {
        printf("SHOT_ERR failed\n");
        return;
    }

    uint32_t w = buf->header.w, h = buf->header.h, stride = buf->header.stride;
    size_t total = (size_t)h * stride;
    printf("SHOT %u %u %u\n", (unsigned)w, (unsigned)h, (unsigned)stride);

    const unsigned char *d = buf->data;
    unsigned char line[132];   /* 90 bytes -> 120 b64 chars + NUL + slack */
    for (size_t off = 0; off < total; off += 90) {
        size_t chunk = total - off < 90 ? total - off : 90;
        size_t olen = 0;
        if (mbedtls_base64_encode(line, sizeof(line), &olen, d + off, chunk) != 0) {
            break;
        }
        line[olen] = '\0';
        fputs((const char *)line, stdout);
        fputc('\n', stdout);
    }
    printf("ENDSHOT\n");

    bsp_display_lock(0);
    lv_draw_buf_destroy(buf);
    bsp_display_unlock();
}

/* MEM -- report free heap without disturbing the running app, so a
 * long soak can watch for slow leaks. Largest-contiguous matters as
 * much as the total: fragmentation fails allocations while plenty is
 * nominally free. */
static void handle_mem(void)
{
    printf("MEM %u %u %u\n",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

/* PWR -- inject a synthetic PWR press+release through the same edge
 * recorder the poller uses, so app button flows are drivable from the
 * harness (the physical button obviously is not). */
static void handle_pwr(void)
{
    app_button_record_edge(true);
    vTaskDelay(pdMS_TO_TICKS(60));
    app_button_record_edge(false);
    printf("PWR_OK\n");
}

/* TAP <x> <y> / SWIPE <x0> <y0> <x1> <y1> [ms] -- synthetic touch through
 * launcher_input_inject(). With SHOT this closes the agent's loop: look,
 * tap, look again, no human at the panel. */
static void handle_tap(const char *header)
{
    int x = 0, y = 0;

    if (sscanf(header, "TAP %d %d", &x, &y) != 2) {
        printf("TAP_ERR bad_args\n");
        return;
    }
    launcher_input_inject(x, y, x, y, 80);
    printf("TAP_OK\n");
}

static void handle_swipe(const char *header)
{
    int x0, y0, x1, y1, ms = 250;

    if (sscanf(header, "SWIPE %d %d %d %d %d", &x0, &y0, &x1, &y1, &ms) < 4) {
        printf("SWIPE_ERR bad_args\n");
        return;
    }
    launcher_input_inject(x0, y0, x1, y1, ms);
    printf("SWIPE_OK\n");
}

/* STOP -- ask the running app to stop, exactly as pressing BOOT does. */
static void handle_stop(void)
{
    if (!launcher_stop_app()) {
        printf("STOP_ERR not_running\n");
        return;
    }

    ESP_LOGI(TAG, "STOP");
    printf("STOP_OK\n");
}

static void serial_push_task(void *arg)
{
    (void)arg;
    char line[LINE_MAX];

    for (;;) {
        if (!read_line(line, sizeof(line))) {
            continue;
        }
        if (strncmp(line, "PUSH ", 5) == 0) {
            handle_push(line);
        } else if (strncmp(line, "RUN ", 4) == 0) {
            handle_run(line);
        } else if (strcmp(line, "STOP") == 0) {
            handle_stop();
        } else if (strcmp(line, "LIST") == 0) {
            handle_list();
        } else if (strncmp(line, "DELETE ", 7) == 0) {
            handle_delete(line);
        } else if (strcmp(line, "SHOT") == 0) {
            handle_shot();
        } else if (strncmp(line, "TAP ", 4) == 0) {
            handle_tap(line);
        } else if (strncmp(line, "SWIPE ", 6) == 0) {
            handle_swipe(line);
        } else if (strcmp(line, "PWR") == 0) {
            handle_pwr();
        } else if (strcmp(line, "MEM") == 0) {
            handle_mem();
        }
    }
}

void serial_push_start(void)
{
    xTaskCreate(serial_push_task, "serial_push", 6144, NULL, 4, NULL);
}
