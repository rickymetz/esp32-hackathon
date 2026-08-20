/*
 * App discovery: finds Lua apps on the SD card.
 *
 * Installing an app is a file copy into /sdcard/apps/ -- no reflash, no
 * rebuild. That is the whole point of the launcher.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define APP_NAME_MAX  48
#define APP_PATH_MAX  320   /* mount point + '/apps/' + a 255-char LFN */
#define APP_MAX_COUNT 32

typedef struct {
    char name[APP_NAME_MAX];   /**< display name, from the filename */
    char path[APP_PATH_MAX];   /**< absolute path to the .lua file */
} app_entry_t;

/**
 * @brief Mount the SD card and scan the apps directory.
 *
 * Safe to call when no card is present: returns ESP_ERR_NOT_FOUND and leaves
 * the app list empty rather than failing the boot.
 */
esp_err_t app_registry_scan(void);

/** Number of apps found by the last scan. */
size_t app_registry_count(void);

/** App at `index`, or NULL if out of range. */
const app_entry_t *app_registry_get(size_t index);

/** Whether the SD card is currently mounted. */
bool app_registry_sd_mounted(void);

#ifdef __cplusplus
}
#endif
