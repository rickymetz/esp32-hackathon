/* `prefs` -- NVS-backed device settings. See lua_module_prefs.h. */
#include "lua_module_prefs.h"
#include "cap_lua.h"
#include "lauxlib.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"

#include <string.h>

static const char *TAG = "lua_prefs";

/* One namespace, shared with the C shell (launcher_main.c's SHELL_NVS_NS) so
 * Settings and the watch face read and write the same keys. */
#define PREFS_NS "shell"

/* NVS keys are capped at 15 chars by the API; reject longer ones with a clear
 * Lua error rather than letting nvs_set_* fail opaquely at runtime. */
#define PREFS_KEY_MAX 15

/* Keys no app may read or write through this module.
 *
 * `prefs` is the one shared store an app cannot reach around -- there is no
 * other NVS binding -- so it is the one place where a denylist actually
 * holds. The Wi-Fi credentials now live in a private namespace
 * (app_wifi.c's WIFI_NVS_NS) and are migrated out of "shell" on first boot,
 * but the names stay blocked here so an app cannot read a value left behind
 * by an older build, nor plant one for the C side to pick up.
 *
 * This is NOT a sandbox and does not pretend to be one: apps share the SD
 * card and can read each other's files, which the contract says plainly. It
 * is specifically the user's network password, which is not the app's
 * business and was one prefs.get() away. */
static const char *const PREFS_DENY[] = { "wifi_ssid", "wifi_pass" };

static const char *check_key(lua_State *L, int idx)
{
    size_t len = 0;
    const char *key = luaL_checklstring(L, idx, &len);
    if (len == 0 || len > PREFS_KEY_MAX) {
        luaL_error(L, "prefs: key must be 1-%d characters", PREFS_KEY_MAX);
    }
    for (size_t i = 0; i < sizeof(PREFS_DENY) / sizeof(PREFS_DENY[0]); i++) {
        if (strcmp(key, PREFS_DENY[i]) == 0) {
            luaL_error(L, "prefs: '%s' is not readable by apps; "
                          "use the wifi module to connect", key);
        }
    }
    return key;
}

/* prefs.get(key [, default]) -> number | string | default
 *
 * The stored type decides what comes back: an i32 key returns a number, a
 * string key a string. `default` is returned untouched when the key has never
 * been written (or NVS itself is unavailable), so a caller can always rely on
 * getting something usable. */
static int l_prefs_get(lua_State *L)
{
    const char *key = check_key(L, 1);
    nvs_handle_t h;

    if (nvs_open(PREFS_NS, NVS_READONLY, &h) != ESP_OK) {
        /* No namespace yet -- nothing has ever been written. Not an error. */
        lua_settop(L, 2);
        return 1;
    }

    int32_t i32 = 0;
    if (nvs_get_i32(h, key, &i32) == ESP_OK) {
        nvs_close(h);
        lua_pushinteger(L, i32);
        return 1;
    }

    size_t len = 0;
    if (nvs_get_str(h, key, NULL, &len) == ESP_OK && len > 0) {
        char *buf = lua_newuserdatauv(L, len, 0);   /* freed by the collector */
        if (nvs_get_str(h, key, buf, &len) == ESP_OK) {
            nvs_close(h);
            lua_pushlstring(L, buf, len > 0 ? len - 1 : 0);   /* drop the NUL */
            return 1;
        }
    }

    nvs_close(h);
    lua_settop(L, 2);
    return 1;
}

/* prefs.set(key, value) -> true | nil, reason
 *
 * Commits immediately: these are single user actions (picking a timezone,
 * nudging brightness), not a hot loop, and a setting that survives only until
 * the next reboot is worse than useless. Integers store as i32 and everything
 * else stores as a string, so prefs.get() round-trips the same type. */
static int l_prefs_set(lua_State *L)
{
    const char *key = check_key(L, 1);
    nvs_handle_t h;

    esp_err_t err = nvs_open(PREFS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        lua_pushnil(L);
        lua_pushstring(L, "prefs unavailable");
        return 2;
    }

    if (lua_isinteger(L, 2)) {
        err = nvs_set_i32(h, key, (int32_t)lua_tointeger(L, 2));
    } else if (lua_isnumber(L, 2)) {
        /* NVS has no float type. Rounding here rather than silently storing a
         * truncated string keeps get/set symmetric for the scalar settings
         * this module exists for. */
        err = nvs_set_i32(h, key, (int32_t)(lua_tonumber(L, 2) + 0.5));
    } else {
        err = nvs_set_str(h, key, luaL_checkstring(L, 2));
    }

    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "set '%s' failed: %s", key, esp_err_to_name(err));
        lua_pushnil(L);
        lua_pushstring(L, esp_err_to_name(err));
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

/* prefs.clear(key) -> true | nil, reason -- forget one setting. */
static int l_prefs_clear(lua_State *L)
{
    const char *key = check_key(L, 1);
    nvs_handle_t h;

    if (nvs_open(PREFS_NS, NVS_READWRITE, &h) != ESP_OK) {
        lua_pushnil(L);
        lua_pushstring(L, "prefs unavailable");
        return 2;
    }
    esp_err_t err = nvs_erase_key(h, key);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);

    /* Erasing something that was never set is a success from the caller's
     * point of view: afterwards, the key is not there. */
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        lua_pushnil(L);
        lua_pushstring(L, esp_err_to_name(err));
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

static const luaL_Reg prefs_funcs[] = {
    { "get",   l_prefs_get },
    { "set",   l_prefs_set },
    { "clear", l_prefs_clear },
    { NULL, NULL },
};

static int luaopen_prefs(lua_State *L)
{
    luaL_newlib(L, prefs_funcs);
    return 1;
}

esp_err_t lua_module_prefs_register(void)
{
    return cap_lua_register_module("prefs", luaopen_prefs);
}
