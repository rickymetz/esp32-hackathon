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

/**
 * @brief Copy the app at `index` out of the registry.
 *
 * Returns a copy because the shared array can be rewritten by a concurrent
 * PUSH rescan at any time; a pointer into it is only valid while the lock is
 * held, which callers cannot do.
 *
 * @return false if `index` is out of range at the moment of the call, which
 *         also means "stop iterating" -- the array may have shrunk.
 */
bool app_registry_get_copy(size_t index, app_entry_t *out);

/** Whether the SD card is currently mounted. */
bool app_registry_sd_mounted(void);

/**
 * @brief Forget the mounted SD card so the next scan remounts it.
 *
 * Without this a card that is removed and reinserted is never seen again,
 * because the mount flag latches on first success.
 */
void app_registry_invalidate(void);

/**
 * @brief Find an app by its file basename and copy it out.
 *
 * Returns a copy rather than a pointer because the caller may hold it while
 * a concurrent PUSH rescans and rewrites the shared array.
 *
 * @return true if found and copied into *out.
 */
bool app_registry_find_by_basename(const char *basename, app_entry_t *out);

/**
 * @brief Write an app file to the SD card, atomically and under the registry lock.
 *
 * The lock is what makes this safe: app_registry_invalidate() unmounts the card
 * under the same lock, so it cannot pull the filesystem out from under an
 * in-flight write.
 *
 * Writes to a temp file and renames, so a power loss cannot leave a
 * half-written app that the launcher would then try to run.
 *
 * @return false if the card is not mounted or the write failed.
 */
bool app_registry_write_app(const char *basename, const void *data, size_t len);

/**
 * @brief Delete an app file from the SD card, under the registry lock.
 *
 * Same locking contract as app_registry_write_app(): the registry lock is
 * what stops a concurrent Refresh from unmounting the card mid-unlink.
 * Rescans before returning so the deleted app is gone from the list the
 * moment the caller sees success.
 *
 * @return false if the card is not mounted or the unlink failed (which
 *         includes "no such file").
 */
bool app_registry_delete_app(const char *basename);

#ifdef __cplusplus
}
#endif
