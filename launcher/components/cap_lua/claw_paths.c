/*
 * Minimal claw_paths implementation -- see include/claw_paths.h.
 */

#include <stdio.h>
#include <string.h>
#include "claw_paths.h"

#define PATH_MAX_LEN 128

static char s_roots[CLAW_PATH_ROOT_MAX][PATH_MAX_LEN] = {
    [CLAW_PATH_DATA]   = "/sdcard",
    [CLAW_PATH_SYSTEM] = "/system",
};

esp_err_t claw_paths_set(claw_path_root_t root, const char *path)
{
    if (root >= CLAW_PATH_ROOT_MAX || path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(path) >= PATH_MAX_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }
    strcpy(s_roots[root], path);
    return ESP_OK;
}

const char *claw_paths_get(claw_path_root_t root)
{
    if (root >= CLAW_PATH_ROOT_MAX) {
        return NULL;
    }
    return s_roots[root];
}

esp_err_t claw_paths_join(claw_path_root_t root, const char *subpath, char *out, size_t out_size)
{
    if (root >= CLAW_PATH_ROOT_MAX || out == NULL || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    /* An absolute subpath wins outright. */
    if (subpath != NULL && subpath[0] == '/') {
        if (snprintf(out, out_size, "%s", subpath) >= (int)out_size) {
            return ESP_ERR_INVALID_SIZE;
        }
        return ESP_OK;
    }

    int n = snprintf(out, out_size, "%s/%s", s_roots[root], subpath ? subpath : "");
    if (n < 0 || n >= (int)out_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}
