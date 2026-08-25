/* Simulator stand-in for the `prefs` module.
 *
 * On device prefs is NVS. The host has no NVS, so this keeps the same API
 * backed by a small in-memory table. Each sim invocation is a fresh process,
 * so settings do not leak between runs -- which is what golden and scenario
 * determinism needs. Persistence across runs is a device behaviour and is
 * verified there, not here.
 */
#include "lua_module_prefs.h"
#include "cap_lua.h"
#include "lauxlib.h"

#include <string.h>
#include <stdlib.h>

#define PREFS_MAX      32
#define PREFS_KEY_MAX  15

typedef struct {
    char  key[PREFS_KEY_MAX + 1];
    bool  is_num;
    long  num;
    char *str;
} entry_t;

static entry_t s_entries[PREFS_MAX];
static int     s_count;

static entry_t *find(const char *key)
{
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_entries[i].key, key) == 0) return &s_entries[i];
    }
    return NULL;
}

static const char *check_key(lua_State *L, int idx)
{
    size_t len = 0;
    const char *key = luaL_checklstring(L, idx, &len);
    if (len == 0 || len > PREFS_KEY_MAX) {
        luaL_error(L, "prefs: key must be 1-%d characters", PREFS_KEY_MAX);
    }
    return key;
}

static int l_prefs_get(lua_State *L)
{
    entry_t *e = find(check_key(L, 1));
    if (e == NULL) {
        lua_settop(L, 2);
        return 1;
    }
    if (e->is_num) lua_pushinteger(L, e->num);
    else           lua_pushstring(L, e->str ? e->str : "");
    return 1;
}

static int l_prefs_set(lua_State *L)
{
    const char *key = check_key(L, 1);
    entry_t *e = find(key);
    if (e == NULL) {
        if (s_count >= PREFS_MAX) {
            lua_pushnil(L);
            lua_pushstring(L, "prefs full");
            return 2;
        }
        e = &s_entries[s_count++];
        snprintf(e->key, sizeof(e->key), "%s", key);
    }
    free(e->str);
    e->str = NULL;

    if (lua_isinteger(L, 2)) {
        e->is_num = true;  e->num = (long)lua_tointeger(L, 2);
    } else if (lua_isnumber(L, 2)) {
        e->is_num = true;  e->num = (long)(lua_tonumber(L, 2) + 0.5);
    } else {
        e->is_num = false; e->str = strdup(luaL_checkstring(L, 2));
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int l_prefs_clear(lua_State *L)
{
    const char *key = check_key(L, 1);
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_entries[i].key, key) == 0) {
            free(s_entries[i].str);
            /* Move the last entry down, then blank the slot it came from.
             * Without the blanking two slots hold the SAME char*, and the
             * next set() to reuse that slot free()s a string the live entry
             * is still pointing at -- get() then reads freed memory and a
             * later clear() double-frees it. */
            s_entries[i] = s_entries[--s_count];
            s_entries[s_count].str = NULL;
            s_entries[s_count].key[0] = '\0';
            break;
        }
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
