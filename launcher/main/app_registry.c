#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>

#include "app_registry.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "bsp/esp-bsp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "app_registry";

#define APPS_DIR  BSP_SD_MOUNT_POINT "/apps"

static app_entry_t s_apps[APP_MAX_COUNT];
static size_t s_count;
static bool s_mounted;

/* Guards s_apps/s_count/s_mounted so a scan (app_registry_scan(), which can
 * run on the serial task via PUSH, or on the LVGL task via Refresh) can
 * never be observed half-done by a reader on another task. Lazily created:
 * the first call into this module is always app_registry_scan() from
 * app_main(), single-threaded, before serial_push or the Refresh button's
 * task exist to race the creation. */
static SemaphoreHandle_t s_lock;

static void registry_lock(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
}

static void registry_unlock(void)
{
    xSemaphoreGive(s_lock);
}

/* "weather_clock.lua" -> "weather clock" */
static void pretty_name(const char *filename, char *out, size_t out_size)
{
    size_t n = 0;
    for (const char *p = filename; *p && n + 1 < out_size; p++) {
        if (*p == '.') {
            break;                 /* stop at the extension */
        }
        out[n++] = (*p == '_' || *p == '-') ? ' ' : *p;
    }
    out[n] = '\0';

    if (out[0] >= 'a' && out[0] <= 'z') {
        out[0] -= 32;
    }
}

static bool has_lua_suffix(const char *name)
{
    size_t len = strlen(name);
    return len > 4 && strcasecmp(name + len - 4, ".lua") == 0;
}

/* Body of app_registry_scan(), without the lock. Callers that already hold
 * registry_lock() (namely app_registry_write_app()) must call this directly
 * -- s_lock is a plain mutex, not a recursive one, so taking it twice from
 * the same task deadlocks instead of succeeding. */
static esp_err_t scan_locked(void)
{
    s_count = 0;

    if (!s_mounted) {
        esp_err_t err = bsp_sdcard_mount();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "no SD card (%s) -- no apps to load", esp_err_to_name(err));
            return ESP_ERR_NOT_FOUND;
        }
        s_mounted = true;
        ESP_LOGI(TAG, "SD card mounted at %s", BSP_SD_MOUNT_POINT);
    }

    DIR *dir = opendir(APPS_DIR);
    if (dir == NULL) {
        /* Card present but no apps directory: create it so the user has an
         * obvious place to drop files. */
        if (mkdir(APPS_DIR, 0777) == 0) {
            ESP_LOGI(TAG, "created %s", APPS_DIR);
        } else {
            ESP_LOGW(TAG, "cannot open or create %s", APPS_DIR);
        }
        return ESP_OK;
    }

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && s_count < APP_MAX_COUNT) {
        if (ent->d_name[0] == '.' || !has_lua_suffix(ent->d_name)) {
            continue;
        }
        app_entry_t *app = &s_apps[s_count];
        int n = snprintf(app->path, sizeof(app->path), "%s/%s", APPS_DIR, ent->d_name);
        if (n < 0 || n >= (int)sizeof(app->path)) {
            ESP_LOGW(TAG, "skipping '%s': path too long", ent->d_name);
            continue;
        }
        pretty_name(ent->d_name, app->name, sizeof(app->name));
        ESP_LOGI(TAG, "found app '%s' (%s)", app->name, app->path);
        s_count++;
    }
    closedir(dir);

    ESP_LOGI(TAG, "%u app(s) found", (unsigned)s_count);
    return ESP_OK;
}

esp_err_t app_registry_scan(void)
{
    registry_lock();
    esp_err_t err = scan_locked();
    registry_unlock();
    return err;
}

size_t app_registry_count(void)
{
    registry_lock();
    size_t count = s_count;
    registry_unlock();
    return count;
}

bool app_registry_get_copy(size_t index, app_entry_t *out)
{
    registry_lock();
    bool ok = (index < s_count);
    if (ok) {
        *out = s_apps[index];
    }
    registry_unlock();
    return ok;
}

/* "/sdcard/apps/counter.lua" -> "counter.lua" */
static const char *entry_basename(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

bool app_registry_find_by_basename(const char *basename, app_entry_t *out)
{
    registry_lock();
    bool found = false;
    for (size_t i = 0; i < s_count; i++) {
        if (strcmp(entry_basename(s_apps[i].path), basename) == 0) {
            *out = s_apps[i];
            found = true;
            break;
        }
    }
    registry_unlock();
    return found;
}

bool app_registry_sd_mounted(void)
{
    registry_lock();
    bool mounted = s_mounted;
    registry_unlock();
    return mounted;
}

void app_registry_invalidate(void)
{
    registry_lock();
    if (s_mounted) {
        esp_err_t err = bsp_sdcard_unmount();
        if (err == ESP_OK) {
            s_mounted = false;
        } else {
            /* Mount point is likely still registered with the FAT driver.
             * Leaving s_mounted true stops the next scan from attempting a
             * doomed remount, which would otherwise fail forever and read
             * as "no SD card" with a good card inserted. */
            ESP_LOGW(TAG, "SD unmount failed (%s) -- leaving mount state as-is",
                     esp_err_to_name(err));
        }
    }
    s_count = 0;
    registry_unlock();
}

bool app_registry_write_app(const char *basename, const void *data, size_t len)
{
    registry_lock();
    int64_t lock_start_us = esp_timer_get_time();

    if (!s_mounted) {
        ESP_LOGI(TAG, "registry lock held %lld us (not mounted)",
                 (long long)(esp_timer_get_time() - lock_start_us));
        registry_unlock();
        return false;
    }

    char tmp_path[APP_PATH_MAX], final_path[APP_PATH_MAX];
    int n;

    n = snprintf(tmp_path, sizeof(tmp_path), "%s/apps/.push.tmp", BSP_SD_MOUNT_POINT);
    if (n < 0 || n >= (int)sizeof(tmp_path)) {
        registry_unlock();
        return false;
    }
    n = snprintf(final_path, sizeof(final_path), "%s/apps/%s", BSP_SD_MOUNT_POINT, basename);
    if (n < 0 || n >= (int)sizeof(final_path)) {
        registry_unlock();
        return false;
    }

    /* Write to a temp file then rename, so a power loss mid-write cannot
     * leave a half-written app that the launcher would try to run. */
    FILE *f = fopen(tmp_path, "wb");
    if (f == NULL) {
        registry_unlock();
        return false;
    }
    size_t written = fwrite(data, 1, len, f);
    fclose(f);

    if (written != len) {
        remove(tmp_path);
        registry_unlock();
        return false;
    }
    remove(final_path);
    if (rename(tmp_path, final_path) != 0) {
        registry_unlock();
        return false;
    }

    ESP_LOGI(TAG, "wrote %s (%u bytes)", basename, (unsigned)len);

    /* Pick up the freshly-written app now, still under the lock, so a RUN
     * sent the instant the host sees PUSH_OK cannot race the rescan and see
     * a stale registry. Call the unlocked body directly -- s_lock is not
     * recursive, so app_registry_scan() here would deadlock. */
    scan_locked();

    ESP_LOGI(TAG, "registry lock held %lld us (write+rescan)",
             (long long)(esp_timer_get_time() - lock_start_us));
    registry_unlock();
    return true;
}
