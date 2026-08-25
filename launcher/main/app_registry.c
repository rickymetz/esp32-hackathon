#include <stdio.h>
#include <stdlib.h>   /* qsort(), used by scan_locked() below */
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

/* readdir() hands entries back in FAT directory order -- which is creation
 * order, reshuffled by every delete-and-re-add. That made the launcher list
 * arbitrary and, worse, unstable: pushing an app moved unrelated rows. Sort by
 * display name so a row stays where the user last saw it.
 *
 * strcasecmp so "Breathe" and "breathe" sort together rather than all
 * upper-case names sorting ahead of all lower-case ones.
 *
 * The id tie-break is what actually makes the order STABLE, which is the whole
 * point. Comparing display names alone is not a total order: pretty_name()
 * maps both '_' and '-' to a space and truncates at APP_NAME_MAX (48), so
 * "my_app.lua" and "my-app.lua" -- or any two ids sharing a 47-character
 * prefix -- compare equal. qsort is not stable, so those rows could still swap
 * on every rescan, reintroducing exactly the "pushing an app moved unrelated
 * rows" symptom this sort exists to remove. Ids are unique within a directory,
 * so falling back to them is a total order. */
static int app_cmp_by_name(const void *a, const void *b)
{
    const app_entry_t *x = (const app_entry_t *)a;
    const app_entry_t *y = (const app_entry_t *)b;
    int c = strcasecmp(x->name, y->name);
    return c ? c : strcmp(x->id, y->id);
}

/* Body of app_registry_scan(), without the lock. Callers that already hold
 * registry_lock() (namely app_registry_write_app()) must call this directly
 * -- s_lock is a plain mutex, not a recursive one, so taking it twice from
 * the same task deadlocks instead of succeeding. */
/* ---- Built-in apps ---------------------------------------------------
 *
 * Baked into the binary by EMBED_TXTFILES (see main/CMakeLists.txt), so the
 * device has a usable app set with no SD card in the slot -- which is the
 * point: every device SETTING already survives a missing card (NVS), and
 * until now the app that changes them did not.
 *
 * A card app with the same id SHADOWS its built-in, so an author can iterate
 * on settings.lua by pushing over it exactly as before. Delete the card copy
 * and the built-in comes back; the built-in itself cannot be deleted.
 *
 * The slate is deliberately small. clock.lua, faces.lua and wifi_setup.lua
 * were on the original list and are all gone -- the faces are C now and
 * wifi_setup was folded into settings -- so three slots are free. What (if
 * anything) rides in flash forever is a product decision, not a technical
 * one, so they are left free rather than filled to a number. */
#define BUILTIN_DECL(sym) \
    extern const char sym##_start[] asm("_binary_" #sym "_start")

BUILTIN_DECL(settings_lua);
BUILTIN_DECL(counter_lua);
BUILTIN_DECL(stopwatch_lua);
BUILTIN_DECL(countdown_lua);
BUILTIN_DECL(flashlight_lua);

static const struct {
    const char *id;
    const char *src;
} k_builtins[] = {
    { "settings.lua",   settings_lua_start   },
    { "counter.lua",    counter_lua_start    },
    { "stopwatch.lua",  stopwatch_lua_start  },
    { "countdown.lua",  countdown_lua_start  },
    { "flashlight.lua", flashlight_lua_start },
};
#define BUILTIN_COUNT (sizeof(k_builtins) / sizeof(k_builtins[0]))

_Static_assert(BUILTIN_COUNT < APP_MAX_COUNT,
               "built-ins alone must not fill the registry");

/* Seeded before the card is even mounted, so they survive every early return
 * in scan_locked() -- the no-card path is exactly the case they exist for. */
static void seed_builtins_locked(void)
{
    for (size_t i = 0; i < BUILTIN_COUNT && s_count < APP_MAX_COUNT; i++) {
        app_entry_t *app = &s_apps[s_count];
        memset(app, 0, sizeof(*app));
        snprintf(app->id, sizeof(app->id), "%s", k_builtins[i].id);
        pretty_name(k_builtins[i].id, app->name, sizeof(app->name));
        /* Not openable. Kept human-readable because it reaches the log and
         * the app-info sheet. */
        snprintf(app->path, sizeof(app->path), "<built-in>/%s", k_builtins[i].id);
        app->builtin_src = k_builtins[i].src;
        s_count++;
    }
}

/* A card app replaces its built-in IN PLACE rather than appending, or the
 * list would show the same app twice. Returns the slot to fill, which is the
 * shadowed built-in's if there is one and a fresh slot otherwise. */
static app_entry_t *slot_for_locked(const char *id)
{
    for (size_t i = 0; i < s_count; i++) {
        if (s_apps[i].builtin_src != NULL && strcmp(s_apps[i].id, id) == 0) {
            return &s_apps[i];
        }
    }
    return (s_count < APP_MAX_COUNT) ? &s_apps[s_count] : NULL;
}

static esp_err_t scan_locked(void)
{
    s_count = 0;

    /* Before the mount, deliberately: everything below can return early, and
     * the no-card path is precisely the one the built-ins exist to serve. */
    seed_builtins_locked();

    if (!s_mounted) {
        esp_err_t err = bsp_sdcard_mount();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "no SD card (%s) -- built-in apps only",
                     esp_err_to_name(err));
            qsort(s_apps, s_count, sizeof(s_apps[0]), app_cmp_by_name);
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
        qsort(s_apps, s_count, sizeof(s_apps[0]), app_cmp_by_name);
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

        /* Resolve the slot from the id BEFORE filling it, so a card app lands
         * on top of the built-in it shadows instead of beside it. */
        char cand_id[APP_ID_MAX];
        if (snprintf(cand_id, sizeof(cand_id), "%s", ent->d_name) >= (int)sizeof(cand_id)) {
            ESP_LOGW(TAG, "skipping '%s': id too long", ent->d_name);
            continue;
        }
        app_entry_t *app = slot_for_locked(cand_id);
        if (app == NULL) {
            continue;   /* registry full */
        }
        bool shadowing = (app->builtin_src != NULL);

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

        /* Cleared HERE, not at slot resolution: several paths above `continue`
         * after the slot is chosen (a directory with no main.lua, a path too
         * long). Clearing early meant a directory literally named
         * "settings.lua" wiped the built-in's source pointer and then skipped
         * the fill, leaving a listed entry whose path is the "<built-in>"
         * label and whose loader is luaL_loadfile -- a permanently broken
         * Settings for as long as that directory existed. */
        app->builtin_src = NULL;   /* a card app is never a built-in */
        memcpy(app->id, cand_id, sizeof(app->id));
        pretty_name(ent->d_name, app->name, sizeof(app->name));
        ESP_LOGI(TAG, "found app '%s' (%s)%s", app->name, app->path,
                 shadowing ? " [shadows built-in]" : "");
        if (!shadowing) {
            s_count++;
        }
    }
    closedir(dir);

    qsort(s_apps, s_count, sizeof(s_apps[0]), app_cmp_by_name);

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

bool app_registry_is_builtin(const char *id)
{
    registry_lock();
    bool found = false;
    for (size_t i = 0; i < s_count; i++) {
        if (strcmp(s_apps[i].id, id) == 0) {
            found = (s_apps[i].builtin_src != NULL);
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

uint32_t app_registry_signature(void)
{
    registry_lock();
    /* FNV-1a over the count and every id. Ids are unique per directory and the
     * array is sorted, so equal hashes mean the same set in the same order. */
    uint32_t hz = 2166136261u;
    hz = (hz ^ (uint32_t)s_count) * 16777619u;
    for (size_t i = 0; i < s_count; i++) {
        for (const char *p = s_apps[i].id; *p; p++) {
            hz = (hz ^ (uint32_t)(unsigned char)*p) * 16777619u;
        }
        hz = (hz ^ 0xffu) * 16777619u;   /* separator: "ab","c" != "a","bc" */
    }
    registry_unlock();
    return hz;
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

    /* A built-in lives in flash and has no file to unlink. If a card app is
     * shadowing it the id refers to the CARD copy, which is deletable -- and
     * removing it makes the built-in resurface on the next scan, which is the
     * intended way to undo a bad push of settings.lua. */
    for (size_t i = 0; i < s_count; i++) {
        if (strcmp(s_apps[i].id, id) == 0 && s_apps[i].builtin_src != NULL) {
            ESP_LOGW(TAG, "refusing to delete built-in '%s'", id);
            registry_unlock();
            return false;
        }
    }

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
