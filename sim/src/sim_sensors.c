/* Desktop stand-ins for the hardware capability modules.
 *
 * Without these, every app that touches rtc/battery/imu/audio/wifi dies at
 * its require() line and cannot be rendered at all -- which was most of the
 * design-heavy apps, the watch faces included. The point of the simulator is
 * to see an app's layout, so these return plausible, STEADY values rather
 * than pretending to be sensors:
 *
 *   - rtc mirrors the host clock in UTC, so a watch face shows a real time
 *   - battery/imu report fixed, believable readings
 *   - audio accepts and discards; wifi is always "off"
 *
 * What they deliberately do NOT do is model failure. On the device these
 * modules degrade -- they return nil, "reason" when hardware is absent or
 * not yet ready -- and an app must handle that. The simulator always
 * succeeds, so it cannot prove you handled it. Test that path on the board.
 */
#include <time.h>
#include "cap_lua.h"
#include "lauxlib.h"
#include "sim_sensors.h"

/* ---- rtc: the host clock, UTC, matching the device's convention ---- */

static int l_rtc_now(lua_State *L)
{
    time_t now = time(NULL);
    struct tm t;

    gmtime_r(&now, &t);
    lua_newtable(L);
    lua_pushinteger(L, t.tm_sec);         lua_setfield(L, -2, "sec");
    lua_pushinteger(L, t.tm_min);         lua_setfield(L, -2, "min");
    lua_pushinteger(L, t.tm_hour);        lua_setfield(L, -2, "hour");
    lua_pushinteger(L, t.tm_mday);        lua_setfield(L, -2, "day");
    lua_pushinteger(L, t.tm_wday);        lua_setfield(L, -2, "wday");
    lua_pushinteger(L, t.tm_mon + 1);     lua_setfield(L, -2, "month");
    lua_pushinteger(L, t.tm_year + 1900); lua_setfield(L, -2, "year");
    return 1;
}

/* Accepted and dropped: the host clock is not ours to set. */
static int l_rtc_set(lua_State *L) { lua_pushboolean(L, 1); return 1; }

static const luaL_Reg rtc_funcs[] = {
    {"now", l_rtc_now}, {"set", l_rtc_set}, {NULL, NULL},
};

/* ---- battery ---- */

static int l_bat_percent(lua_State *L)  { lua_pushinteger(L, 72); return 1; }
static int l_bat_volts(lua_State *L)    { lua_pushnumber(L, 3.92); return 1; }
static int l_bat_charging(lua_State *L) { lua_pushboolean(L, 0); return 1; }
static int l_bat_external(lua_State *L) { lua_pushboolean(L, 1); return 1; }

static const luaL_Reg bat_funcs[] = {
    {"percent", l_bat_percent}, {"volts", l_bat_volts},
    {"charging", l_bat_charging}, {"external", l_bat_external}, {NULL, NULL},
};

/* ---- imu: face-up and still, which is 1g on Z ---- */

static int l_imu_accel(lua_State *L)
{
    lua_pushnumber(L, 0.0); lua_pushnumber(L, 0.0); lua_pushnumber(L, 1.0);
    return 3;
}

static int l_imu_gyro(lua_State *L)
{
    lua_pushnumber(L, 0.0); lua_pushnumber(L, 0.0); lua_pushnumber(L, 0.0);
    return 3;
}

static int l_imu_die_temp(lua_State *L) { lua_pushnumber(L, 26.0); return 1; }

static const luaL_Reg imu_funcs[] = {
    {"accel", l_imu_accel}, {"gyro", l_imu_gyro},
    {"die_temp", l_imu_die_temp}, {NULL, NULL},
};

/* ---- audio: silent, but argument-checked so range bugs still surface ---- */

static int l_audio_tone(lua_State *L)
{
    int freq = (int)luaL_checkinteger(L, 1);
    int ms = (int)luaL_optinteger(L, 2, 120);
    luaL_argcheck(L, freq >= 0 && freq <= 8000, 1, "freq must be 0-8000 Hz");
    luaL_argcheck(L, ms > 0 && ms <= 5000, 2, "ms must be 1-5000");
    lua_pushboolean(L, 1);
    return 1;
}

static int l_audio_play(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_pushboolean(L, 1);
    return 1;
}

static int l_audio_true(lua_State *L) { lua_pushboolean(L, 1); return 1; }

static int l_audio_volume(lua_State *L)
{
    static int vol = 70;
    if (!lua_isnoneornil(L, 1)) {
        int v = (int)luaL_checkinteger(L, 1);
        luaL_argcheck(L, v >= 0 && v <= 100, 1, "volume must be 0-100");
        vol = v;
    }
    lua_pushinteger(L, vol);
    return 1;
}

static const luaL_Reg audio_funcs[] = {
    {"tone", l_audio_tone}, {"play", l_audio_play}, {"beep", l_audio_true},
    {"stop", l_audio_true}, {"volume", l_audio_volume},
    {"available", l_audio_true}, {NULL, NULL},
};

/* ---- wifi: permanently off, so setup screens render their idle state ---- */

static int l_wifi_off(lua_State *L)     { lua_pushliteral(L, "off"); return 1; }
static int l_wifi_nil(lua_State *L)     { lua_pushnil(L); return 1; }
static int l_wifi_false(lua_State *L)   { lua_pushboolean(L, 0); return 1; }
static int l_wifi_true(lua_State *L)    { lua_pushboolean(L, 1); return 1; }

static int l_wifi_connect(lua_State *L)
{
    lua_pushnil(L);
    lua_pushliteral(L, "no radio in the simulator");
    return 2;
}

static const luaL_Reg wifi_funcs[] = {
    {"connect", l_wifi_connect}, {"status", l_wifi_off}, {"ip", l_wifi_nil},
    {"disconnect", l_wifi_true}, {"time_synced", l_wifi_false},
    {"forget", l_wifi_true}, {NULL, NULL},
};

#define OPENER(name, tbl) \
    static int luaopen_##name(lua_State *L) { luaL_newlib(L, tbl); return 1; }

OPENER(rtc, rtc_funcs)
OPENER(battery, bat_funcs)
OPENER(imu, imu_funcs)
OPENER(audio, audio_funcs)
OPENER(wifi, wifi_funcs)

esp_err_t sim_sensors_register(void)
{
    esp_err_t err;

    if ((err = cap_lua_register_module("rtc", luaopen_rtc)) != ESP_OK) return err;
    if ((err = cap_lua_register_module("battery", luaopen_battery)) != ESP_OK) return err;
    if ((err = cap_lua_register_module("imu", luaopen_imu)) != ESP_OK) return err;
    if ((err = cap_lua_register_module("audio", luaopen_audio)) != ESP_OK) return err;
    return cap_lua_register_module("wifi", luaopen_wifi);
}
