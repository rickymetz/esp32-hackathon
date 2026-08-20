#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>

#include "app_registry.h"
#include "esp_log.h"
#include "bsp/esp-bsp.h"

static const char *TAG = "app_registry";

#define APPS_DIR  BSP_SD_MOUNT_POINT "/apps"

static app_entry_t s_apps[APP_MAX_COUNT];
static size_t s_count;
static bool s_mounted;

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

esp_err_t app_registry_scan(void)
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

size_t app_registry_count(void)
{
    return s_count;
}

const app_entry_t *app_registry_get(size_t index)
{
    return (index < s_count) ? &s_apps[index] : NULL;
}

bool app_registry_sd_mounted(void)
{
    return s_mounted;
}
