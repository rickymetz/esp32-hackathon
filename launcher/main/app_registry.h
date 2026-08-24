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
#define APP_ID_MAX    128   /* a folder name or file basename -- LFN can be long */
#define APP_PATH_MAX  320   /* mount point + '/apps/' + a 255-char LFN */
#define APP_MAX_COUNT 32

typedef struct {
    char name[APP_NAME_MAX];   /**< display name, from the filename/folder name */
    char path[APP_PATH_MAX];   /**< absolute path to the .lua file to load */
    char id[APP_ID_MAX];       /**< stable identity used by RUN/DELETE/rows:
                                *   a flat app's file basename ("counter.lua"),
                                *   or a folder app's directory name ("counter") */
    bool in_folder;            /**< true when the app is apps/<id>/main.lua */
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
 * @brief Find an app by its stable id and copy it out.
 *
 * The id is the app's file basename ("counter.lua") for a flat app, or its
 * folder name ("counter") for a folder app -- the same string LIST prints and
 * RUN/DELETE accept. Returns a copy rather than a pointer because the caller
 * may hold it while a concurrent PUSH rescans and rewrites the shared array.
 *
 * @return true if found and copied into *out.
 */
bool app_registry_find_by_id(const char *id, app_entry_t *out);

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
 * @param rel_path  destination under apps/: a bare filename ("counter.lua")
 *                  for a flat app, or one subdirectory deep ("mygame/main.lua",
 *                  "mygame/icon.bin") for a folder app. The parent folder is
 *                  created if needed. Callers validate the path first.
 * @return false if the card is not mounted or the write failed.
 */
bool app_registry_write_app(const char *rel_path, const void *data, size_t len);

/**
 * @brief Delete an app file from the SD card, under the registry lock.
 *
 * Same locking contract as app_registry_write_app(): the registry lock is
 * what stops a concurrent Refresh from unmounting the card mid-unlink.
 * Rescans before returning so the deleted app is gone from the list the
 * moment the caller sees success.
 *
 * @param id  the app id: a flat app's file basename ("counter.lua") is
 *            unlinked; a folder app's directory name ("mygame") has the whole
 *            apps/<id>/ folder removed recursively.
 * @return false if the card is not mounted or the unlink failed (which
 *         includes "no such file").
 */
bool app_registry_delete_app(const char *id);

#ifdef __cplusplus
}
#endif
