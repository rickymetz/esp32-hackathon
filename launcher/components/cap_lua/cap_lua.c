/*
 * Minimal cap_lua implementation -- see include/cap_lua.h.
 *
 * Module registration happens at init time from a single task, so a plain
 * fixed array without locking is sufficient. The stop flag is the one thing
 * touched from two tasks, so it is atomic.
 */

#include <string.h>
#include <stdatomic.h>
#include "cap_lua.h"
#include "esp_log.h"
#include "lauxlib.h"

#define MAX_MODULES  16
#define MAX_CLEANUPS  8

static const char *TAG = "cap_lua";

static cap_lua_module_t s_modules[MAX_MODULES];
static size_t s_module_count;

static cap_lua_exit_cleanup_fn_t s_cleanups[MAX_CLEANUPS];
static size_t s_cleanup_count;

static atomic_bool s_stop_requested;

esp_err_t cap_lua_register_module(const char *name, lua_CFunction open_fn)
{
    if (name == NULL || open_fn == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_module_count >= MAX_MODULES) {
        ESP_LOGE(TAG, "module table full, dropping '%s'", name);
        return ESP_ERR_NO_MEM;
    }
    s_modules[s_module_count++] = (cap_lua_module_t){ .name = name, .open_fn = open_fn };
    ESP_LOGI(TAG, "registered lua module '%s'", name);
    return ESP_OK;
}

esp_err_t cap_lua_register_exit_cleanup(cap_lua_exit_cleanup_fn_t cleanup_fn)
{
    if (cleanup_fn == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_cleanup_count >= MAX_CLEANUPS) {
        return ESP_ERR_NO_MEM;
    }
    s_cleanups[s_cleanup_count++] = cleanup_fn;
    return ESP_OK;
}

bool cap_lua_runtime_stop_requested(lua_State *L)
{
    (void)L;
    return atomic_load(&s_stop_requested);
}

esp_err_t launcher_lua_open_modules(lua_State *L)
{
    if (L == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < s_module_count; i++) {
        luaL_requiref(L, s_modules[i].name, s_modules[i].open_fn, 1 /* set as global */);
        lua_pop(L, 1);
        ESP_LOGD(TAG, "opened lua module '%s'", s_modules[i].name);
    }
    return ESP_OK;
}

void launcher_lua_run_exit_cleanup(lua_State *L)
{
    for (size_t i = 0; i < s_cleanup_count; i++) {
        s_cleanups[i](L);
    }
}

void launcher_lua_request_stop(bool stop)
{
    atomic_store(&s_stop_requested, stop);
}
