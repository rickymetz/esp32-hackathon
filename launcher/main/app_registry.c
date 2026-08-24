#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>   /* rmdir(), used by remove_app_folder() below */

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
    /* A 1-2 letter first word is almost certainly an initialism -- the
     * review caught "ui_test.lua" rendering as "Ui test". Uppercase it
     * whole ("UI test"); longer words keep plain first-letter casing. */
    if (n >= 2 && out[1] >= 'a' && out[1] <= 'z' &&
        (out[2] == ' ' || out[2] == '\0')) {
        out[1] -= 32;
    }
}

static bool has_lua_suffix(const char *name)
{
    size_t len = strlen(name);
    return len > 4 && strcasecmp(name + len - 4, ".lua") == 0;
}

/* Remove a stray <dir>/.push.tmp left by a failed push. app_registry_write_app
 * co-locates its temp file with the destination (apps/.push.tmp for a flat
 * push, apps/<folder>/.push.tmp for a folder push), so a power loss between
 * fwrite and rename can strand one; scan_locked() sweeps both the apps root
 * and each app folder through here. ENOENT is the normal case. */
static void sweep_push_tmp(const char *dir)
{
    char tmp[APP_PATH_MAX];
    if (snprintf(tmp, sizeof(tmp), "%s/.push.tmp", dir) < (int)sizeof(tmp)) {
        remove(tmp);
    }
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

    sweep_push_tmp(APPS_DIR);   /* clear a stray temp from a failed flat push */

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && s_count < APP_MAX_COUNT) {
        if (ent->d_name[0] == '.') {
            continue;   /* dotfiles, ".", ".." */
        }

        char entry_path[APP_PATH_MAX];
        int n = snprintf(entry_path, sizeof(entry_path), "%s/%s", APPS_DIR, ent->d_name);
        if (n < 0 || n >= (int)sizeof(entry_path)) {
            ESP_LOGW(TAG, "skipping '%s': path too long", ent->d_name);
            continue;
        }

        /* d_type is unreliable on FATFS, so stat() to tell a folder app
         * (apps/<name>/main.lua) from a flat one (apps/<name>.lua). */
        struct stat st;
        bool is_dir = (stat(entry_path, &st) == 0 && S_ISDIR(st.st_mode));

        app_entry_t *app = &s_apps[s_count];

        if (is_dir) {
            sweep_push_tmp(entry_path);   /* stray temp from a failed folder push */

            char main_path[APP_PATH_MAX];
            n = snprintf(main_path, sizeof(main_path), "%s/main.lua", entry_path);
            if (n < 0 || n >= (int)sizeof(main_path)) {
                ESP_LOGW(TAG, "skipping folder '%s': path too long", ent->d_name);
                continue;
            }
            if (stat(main_path, &st) != 0 || !S_ISREG(st.st_mode)) {
                continue;   /* a plain directory, not an app folder */
            }
            memcpy(app->path, main_path, sizeof(app->path));
            app->in_folder = true;
        } else {
            if (!has_lua_suffix(ent->d_name)) {
                continue;
            }
            memcpy(app->path, entry_path, sizeof(app->path));
            app->in_folder = false;
        }

        if (snprintf(app->id, sizeof(app->id), "%s", ent->d_name) >= (int)sizeof(app->id)) {
            ESP_LOGW(TAG, "skipping '%s': id too long", ent->d_name);
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

bool app_registry_find_by_id(const char *id, app_entry_t *out)
{
    registry_lock();
    bool found = false;
    for (size_t i = 0; i < s_count; i++) {
        if (strcmp(s_apps[i].id, id) == 0) {
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

bool app_registry_write_app(const char *rel_path, const void *data, size_t len)
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

    n = snprintf(final_path, sizeof(final_path), "%s/apps/%s", BSP_SD_MOUNT_POINT, rel_path);
    if (n < 0 || n >= (int)sizeof(final_path)) {
        registry_unlock();
        return false;
    }

    /* A folder-app file ("mygame/main.lua") writes into apps/mygame/, which
     * may not exist yet -- create it, and put the temp file in the same
     * directory so the rename stays within one directory. A flat file
     * ("counter.lua") keeps the old apps/.push.tmp temp path. */
    const char *last_slash = strrchr(final_path, '/');
    char dir_path[APP_PATH_MAX];
    size_t dir_len = last_slash ? (size_t)(last_slash - final_path) : 0;
    if (dir_len == 0 || dir_len >= sizeof(dir_path)) {
        registry_unlock();
        return false;
    }
    memcpy(dir_path, final_path, dir_len);
    dir_path[dir_len] = '\0';
    mkdir(dir_path, 0777);   /* harmless if it already exists (e.g. apps/) */

    n = snprintf(tmp_path, sizeof(tmp_path), "%s/.push.tmp", dir_path);
    if (n < 0 || n >= (int)sizeof(tmp_path)) {
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

    ESP_LOGI(TAG, "wrote %s (%u bytes)", rel_path, (unsigned)len);

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

/* Remove apps/<id>/ and everything in it (main.lua, icon, generated files).
 * One level deep is all a folder app is: apps/<id>/ entries, with no nested
 * subdirectories, so a flat readdir+unlink then rmdir is enough. Returns 0
 * on success, like remove(). */
static int remove_app_folder(const char *dir_path)
{
    DIR *d = opendir(dir_path);
    if (d == NULL) {
        return -1;
    }
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        char child[APP_PATH_MAX];
        if (snprintf(child, sizeof(child), "%s/%s", dir_path, ent->d_name) >= (int)sizeof(child)) {
            continue;   /* skip an un-nameable child; rmdir below then fails cleanly */
        }
        remove(child);   /* files, and any (unexpected) empty subdir */
    }
    closedir(d);
    return rmdir(dir_path);
}

bool app_registry_delete_app(const char *id)
{
    registry_lock();

    if (!s_mounted) {
        registry_unlock();
        return false;
    }

    char path[APP_PATH_MAX];
    int n = snprintf(path, sizeof(path), "%s/apps/%s", BSP_SD_MOUNT_POINT, id);
    if (n < 0 || n >= (int)sizeof(path)) {
        registry_unlock();
        return false;
    }

    /* A folder app's id names its directory; a flat app's id is its file. Pick
     * the removal that fits so DELETE works for both. Under the same lock as
     * write/unmount for the same reason: a Refresh tap must not pull the
     * filesystem out from under the unlink. Either failing covers "no such
     * file" and an I/O error alike; the caller only needs "it is not on the
     * card afterwards" vs "it may still be". */
    struct stat st;
    int rc = (stat(path, &st) == 0 && S_ISDIR(st.st_mode))
                 ? remove_app_folder(path)
                 : remove(path);
    if (rc != 0) {
        registry_unlock();
        return false;
    }

    ESP_LOGI(TAG, "deleted %s", id);
    scan_locked();
    registry_unlock();
    return true;
}
