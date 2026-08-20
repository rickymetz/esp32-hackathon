/*
 * Minimal stand-in for esp-claw's claw_paths, used by lua_module_lvgl to
 * resolve asset paths relative to a data root.
 */

#pragma once

#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CLAW_PATH_DATA = 0,   /**< Writable data root -- the SD card mount point */
    CLAW_PATH_SYSTEM,     /**< Read-only firmware-baked root */
    CLAW_PATH_ROOT_MAX,
} claw_path_root_t;

esp_err_t claw_paths_set(claw_path_root_t root, const char *path);
const char *claw_paths_get(claw_path_root_t root);
esp_err_t claw_paths_join(claw_path_root_t root, const char *subpath, char *out, size_t out_size);

#ifdef __cplusplus
}
#endif
