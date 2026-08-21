#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "serial_push.h"
#include "app_registry.h"
#include "launcher_main.h"
#include "esp_log.h"
#include "esp_crc.h"
#include "mbedtls/base64.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bsp/esp-bsp.h"

static const char *TAG = "serial_push";

#define LINE_MAX     128
#define NAME_MAX      64
#define PAYLOAD_MAX (64 * 1024)

/* Reject anything that is not a bare .lua basename. Not a security boundary --
 * anyone with USB can already reflash the board -- but it stops a stray path
 * from writing somewhere surprising and turning into a confusing afternoon.
 * Shared by PUSH (write target) and RUN (registry lookup key). */
static bool name_is_safe(const char *name)
{
    size_t len = strlen(name);
    if (len == 0 || len > NAME_MAX) {
        return false;
    }
    if (strchr(name, '/') || strstr(name, "..")) {
        return false;
    }
    return len > 4 && strcmp(name + len - 4, ".lua") == 0;
}

/* Whether `basename` matches an app currently known to the registry. Used by
 * RUN to tell "not_found" apart from "already_running" without either
 * duplicating launcher_main's task-state tracking or widening the
 * launcher_run_app_by_name() API beyond the bool it was specified as. */
static bool registry_has_basename(const char *basename)
{
    app_entry_t app;
    return app_registry_find_by_basename(basename, &app);
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
    if (!name_is_safe(name)) {
        printf("PUSH_ERR bad_name\n");
        return;
    }
    if (expect_len == 0 || expect_len > PAYLOAD_MAX) {
        printf("PUSH_ERR bad_length\n");
        return;
    }
    if (!app_registry_sd_mounted()) {
        printf("PUSH_ERR no_sdcard\n");
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

    /* Write to a temp file then rename, so a power loss mid-write cannot
     * leave a half-written app that the launcher would try to run. */
    char tmp_path[160], final_path[160];
    snprintf(tmp_path,   sizeof(tmp_path),   "%s/apps/.push.tmp",  BSP_SD_MOUNT_POINT);
    snprintf(final_path, sizeof(final_path), "%s/apps/%s",         BSP_SD_MOUNT_POINT, name);

    FILE *f = fopen(tmp_path, "wb");
    if (f == NULL) {
        free(buf);
        printf("PUSH_ERR open_failed\n");
        return;
    }
    size_t written = fwrite(buf, 1, got, f);
    fclose(f);
    free(buf);

    if (written != got) {
        remove(tmp_path);
        printf("PUSH_ERR write_failed\n");
        return;
    }
    remove(final_path);
    if (rename(tmp_path, final_path) != 0) {
        printf("PUSH_ERR rename_failed\n");
        return;
    }

    ESP_LOGI(TAG, "received %s (%u bytes)", name, (unsigned)got);

    /* A freshly-pushed app is not in the registry yet -- pick it up now,
     * before replying, so a RUN sent the instant the host sees PUSH_OK (the
     * whole point of this channel) cannot race the rescan and see a stale
     * registry. */
    app_registry_scan();
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
    if (!name_is_safe(name)) {
        printf("RUN_ERR bad_name\n");
        return;
    }
    if (!registry_has_basename(name)) {
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

/* STOP -- ask the running app to stop, exactly as pressing PWR does. */
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
        }
    }
}

void serial_push_start(void)
{
    xTaskCreate(serial_push_task, "serial_push", 6144, NULL, 4, NULL);
}
