/* Simulator audio module -- require("audio") without the ES8311 speaker.
 *
 * The real module (components/lua_module_audio/app_audio.c) synthesises tones
 * and writes them to the codec over I2S; the host has no codec. The sim keeps
 * the *validation* identical (freq 0-8000, ms 1-5000, 1-16 notes, volume
 * 0-100), so an app that passes a bad argument fails the same way it would on
 * the board, but the playback itself is a no-op -- tone/play/beep queue nothing
 * and return true immediately, exactly the non-blocking shape the contract
 * promises. available() reports true so an audio app runs its whole path
 * (a silent beep still advances game/metronome logic), and is_playing() stays
 * false because nothing ever occupies the bus.
 */
#include "app_audio.h"
#include "cap_lua.h"
#include "lauxlib.h"
#include "lualib.h"

#define QUEUE_LEN 16
#define MAX_MS    5000

static int s_volume = 70;

/* audio.tone(freq, ms) */
static int l_audio_tone(lua_State *L)
{
    int freq = (int)luaL_checkinteger(L, 1);
    int ms = (int)luaL_optinteger(L, 2, 120);
    luaL_argcheck(L, freq >= 0 && freq <= 8000, 1, "freq must be 0-8000 Hz");
    luaL_argcheck(L, ms > 0 && ms <= MAX_MS, 2, "ms must be 1-5000");
    lua_pushboolean(L, 1);
    return 1;
}

/* audio.play{ {freq, ms}, ... } -- same range checks, no sound. */
static int l_audio_play(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    /* rawlen/rawgeti to mirror the device byte-for-byte (it ignores __len /
     * __index on the note table). */
    lua_Integer n = lua_rawlen(L, 1);
    if (n < 1 || n > QUEUE_LEN) {
        return luaL_error(L, "audio.play: 1-%d notes", QUEUE_LEN);
    }
    for (lua_Integer i = 1; i <= n; i++) {
        lua_rawgeti(L, 1, i);
        if (!lua_istable(L, -1)) {
            return luaL_error(L, "audio.play: note %d must be {freq, ms}", (int)i);
        }
        lua_rawgeti(L, -1, 1);
        lua_rawgeti(L, -2, 2);
        int freq = (int)lua_tointeger(L, -2);
        int ms = (int)lua_tointeger(L, -1);
        lua_pop(L, 3);
        if (freq < 0 || freq > 8000 || ms <= 0 || ms > MAX_MS) {
            return luaL_error(L, "audio.play: note %d out of range", (int)i);
        }
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int l_audio_beep(lua_State *L)
{
    lua_pushboolean(L, 1);
    return 1;
}

static int l_audio_stop(lua_State *L)
{
    lua_pushboolean(L, 1);
    return 1;
}

/* audio.volume([v]) -- set clamps 0-100; both forms return the level. */
static int l_audio_volume(lua_State *L)
{
    if (lua_gettop(L) >= 1 && !lua_isnil(L, 1)) {
        int v = (int)luaL_checkinteger(L, 1);
        luaL_argcheck(L, v >= 0 && v <= 100, 1, "volume must be 0-100");
        s_volume = v;
    }
    lua_pushinteger(L, s_volume);
    return 1;
}

static int l_audio_available(lua_State *L)
{
    lua_pushboolean(L, 1);
    return 1;
}

static const luaL_Reg audio_funcs[] = {
    {"tone", l_audio_tone},
    {"play", l_audio_play},
    {"beep", l_audio_beep},
    {"stop", l_audio_stop},
    {"volume", l_audio_volume},
    {"available", l_audio_available},
    {NULL, NULL},
};

static int luaopen_audio(lua_State *L) { luaL_newlib(L, audio_funcs); return 1; }

bool app_audio_is_playing(void) { return false; }

void app_audio_reset(lua_State *L) { (void)L; /* nothing queued in the sim */ }

esp_err_t app_audio_register(void)
{
    return cap_lua_register_module("audio", luaopen_audio);
}
