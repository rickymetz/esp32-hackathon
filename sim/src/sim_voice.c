/* Simulator voice module -- the require("voice") API surface without MultiNet.
 *
 * The real module (components/lua_module_voice/app_voice.c) needs the mic, the
 * audio codec, and the on-device MultiNet models -- none of which exist on the
 * host. The contract already requires apps to treat voice as optional and check
 * voice.available() first, so the sim reports unavailable. If an app calls
 * listen/spell anyway, we honour the "cb(nil) on no-match" contract by firing
 * the callback with nil on the next pump, so the app keeps moving.
 *
 * Provides the same C entry points the runner's pump loop calls
 * (app_voice_run_pending / app_voice_reset / app_voice_register).
 */
#include "app_voice.h"
#include "cap_lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include <stddef.h>

static int  s_pending_cb = LUA_NOREF;   /* callback to fire cb(nil) next pump */

static int l_voice_available(lua_State *L)
{
    lua_pushboolean(L, 0);
    return 1;
}

/* Stash arg 2 (the callback) to fire as cb(nil) on the next pump. */
static int arm_nil_callback(lua_State *L, int cb_index)
{
    if (lua_isfunction(L, cb_index)) {
        if (s_pending_cb != LUA_NOREF) {
            /* One capture at a time, like the real module: report busy. */
            lua_pushnil(L);
            lua_pushliteral(L, "busy");
            return 2;
        }
        lua_pushvalue(L, cb_index);
        s_pending_cb = luaL_ref(L, LUA_REGISTRYINDEX);
    }
    lua_pushboolean(L, 1);
    return 1;
}

/* voice.listen({ commands = {...} }, cb) */
static int l_voice_listen(lua_State *L)
{
    return arm_nil_callback(L, 2);
}

/* voice.spell(cb) */
static int l_voice_spell(lua_State *L)
{
    return arm_nil_callback(L, 1);
}

static int l_voice_stop(lua_State *L)
{
    if (s_pending_cb != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, s_pending_cb);
        s_pending_cb = LUA_NOREF;
    }
    lua_pushboolean(L, 1);
    return 1;
}

static const luaL_Reg voice_funcs[] = {
    {"available", l_voice_available},
    {"listen", l_voice_listen},
    {"spell", l_voice_spell},
    {"stop", l_voice_stop},
    {NULL, NULL},
};

static int luaopen_sim_voice(lua_State *L)
{
    luaL_newlib(L, voice_funcs);
    return 1;
}

void app_voice_run_pending(lua_State *L)
{
    if (s_pending_cb == LUA_NOREF) return;
    int ref = s_pending_cb;
    s_pending_cb = LUA_NOREF;
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    luaL_unref(L, LUA_REGISTRYINDEX, ref);
    lua_pushnil(L);
    /* Errors inside the callback are the app's problem; log-and-continue like
     * the real dispatch. A pcall keeps the pump alive. */
    (void)lua_pcall(L, 1, 0, 0);
    if (lua_gettop(L) > 0 && lua_isstring(L, -1)) lua_pop(L, 1);
}

void app_voice_reset(lua_State *L)
{
    if (s_pending_cb != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, s_pending_cb);
        s_pending_cb = LUA_NOREF;
    }
}

esp_err_t app_voice_register(void)
{
    return cap_lua_register_module("voice", luaopen_sim_voice);
}
