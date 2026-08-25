/* Simulator sensors module -- require("rtc"/"imu"/"battery") without the chips.
 *
 * The real module (components/lua_module_sensors/app_sensors.c) reaches the
 * PCF85063A clock, QMI8658 IMU and AXP2101 PMU over I2C by direct register
 * access -- none of that bus exists on the host. The contract says all three
 * degrade rather than raise, so a faithful stub could return nil everywhere;
 * but a sim exists to *render* clock faces and dashboards, so instead it
 * serves deterministic, plausible readings:
 *
 *   rtc     -- a fixed wall clock (2026-08-22 14:30:00, Saturday), settable
 *              via rtc.set and by the NTP path (app_sensors_rtc_set_tm), so
 *              the watch faces render a real time.
 *   imu     -- the board lying flat and still: accel (0,0,1) g, gyro (0,0,0),
 *              die_temp ~25.9 C. Orientation maths that reads gravity works.
 *   battery -- 76%, 4.05 V, not charging, on external power.
 *
 * Same return shapes as the device module (checked against app_sensors.c):
 * rtc.now -> {year,month,day,hour,min,sec,wday} or nil,msg; imu.* -> numbers;
 * battery.* -> number/boolean. sim_sensors_reset() restores the defaults
 * between app runs so a rtc.set in one app can't leak into the next.
 */
#include "app_sensors.h"
#include "cap_lua.h"
#include "lauxlib.h"
#include "lualib.h"

/* Broken-down UTC the fake clock hands out. Mutable: rtc.set and the NTP
 * sync (app_sensors_rtc_set_tm, called by the wifi stub) write here. */
static struct {
    int year, mon, mday, hour, min, sec, wday;
    bool set;
} s_clock = { 2026, 8, 22, 14, 30, 0, 6, true };

static void clock_reset_default(void)
{
    s_clock.year = 2026; s_clock.mon = 8;  s_clock.mday = 22;
    s_clock.hour = 14;   s_clock.min = 30; s_clock.sec = 0;
    s_clock.wday = 6;    s_clock.set = true;
}

/* Mutable IMU / battery readings. Default to a board lying flat and still on
 * external power; the `accel`/`gyro`/`battery` sim commands overwrite them so
 * tilt, motion, and low-battery UI paths are testable board-free. */
static double s_ax = 0.0, s_ay = 0.0, s_az = 1.0;
static double s_gx = 0.0, s_gy = 0.0, s_gz = 0.0;
static int    s_bat_pct = 76;        /* <0 => "gauge not ready" */
static double s_bat_volts = 4.05;
static bool   s_bat_charging = false;
static bool   s_bat_external = true;

static void sensors_reset_default(void)
{
    s_ax = 0.0; s_ay = 0.0; s_az = 1.0;
    s_gx = 0.0; s_gy = 0.0; s_gz = 0.0;
    s_bat_pct = 76; s_bat_volts = 4.05;
    s_bat_charging = false; s_bat_external = true;
}

void sim_sensors_reset(void)
{
    clock_reset_default();
    sensors_reset_default();
}

/* ---- sim-only setters (driven by sim_main.c command verbs) ---- */

void sim_sensors_set_accel(double x, double y, double z) { s_ax = x; s_ay = y; s_az = z; }
void sim_sensors_set_gyro(double x, double y, double z)  { s_gx = x; s_gy = y; s_gz = z; }

void sim_sensors_set_battery(int pct, bool charging, bool external)
{
    s_bat_pct = pct;
    s_bat_charging = charging;
    s_bat_external = external;
    /* Track a plausible voltage with the charge so battery.volts moves too. */
    if (pct >= 0) {
        s_bat_volts = 3.30 + (double)pct / 100.0 * 0.90;   /* ~3.3V empty .. 4.2V full */
    }
}

void sim_sensors_set_rtc_unset(void) { s_clock.set = false; }

/* ---- rtc ---- */

static int l_rtc_now(lua_State *L)
{
    if (!s_clock.set) {
        lua_pushnil(L);
        lua_pushliteral(L, "rtc not set");
        return 2;
    }
    lua_newtable(L);
    lua_pushinteger(L, s_clock.sec);  lua_setfield(L, -2, "sec");
    lua_pushinteger(L, s_clock.min);  lua_setfield(L, -2, "min");
    lua_pushinteger(L, s_clock.hour); lua_setfield(L, -2, "hour");
    lua_pushinteger(L, s_clock.mday); lua_setfield(L, -2, "day");
    lua_pushinteger(L, s_clock.wday); lua_setfield(L, -2, "wday");
    lua_pushinteger(L, s_clock.mon);  lua_setfield(L, -2, "month");
    lua_pushinteger(L, s_clock.year); lua_setfield(L, -2, "year");
    return 1;
}

/* rtc.set{...} -- same field names, defaults and ranges as the device. */
static int l_rtc_set(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    struct { const char *k; int def, lo, hi, v; } f[] = {
        { "sec",   0, 0, 59,   0 },
        { "min",   0, 0, 59,   0 },
        { "hour",  0, 0, 23,   0 },
        { "day",   1, 1, 31,   0 },
        { "wday",  0, 0, 6,    0 },
        { "month", 1, 1, 12,   0 },
        { "year",  2026, 2000, 2099, 0 },
    };
    for (size_t i = 0; i < sizeof(f) / sizeof(f[0]); i++) {
        lua_getfield(L, 1, f[i].k);
        int v = lua_isnil(L, -1) ? f[i].def : (int)luaL_checkinteger(L, -1);
        lua_pop(L, 1);
        if (v < f[i].lo || v > f[i].hi) {
            return luaL_error(L, "rtc.set: %s out of range", f[i].k);
        }
        f[i].v = v;
    }
    s_clock.sec = f[0].v; s_clock.min = f[1].v; s_clock.hour = f[2].v;
    s_clock.mday = f[3].v; s_clock.wday = f[4].v; s_clock.mon = f[5].v;
    s_clock.year = f[6].v; s_clock.set = true;
    lua_pushboolean(L, 1);
    return 1;
}

static const luaL_Reg rtc_funcs[] = {
    {"now", l_rtc_now},
    {"set", l_rtc_set},
    {NULL, NULL},
};
static int luaopen_rtc(lua_State *L) { luaL_newlib(L, rtc_funcs); return 1; }

/* Shared by the NTP path in the wifi stub -- same signature as the device. */
esp_err_t app_sensors_rtc_set_tm(int year, int mon, int mday,
                                 int hour, int min, int sec, int wday)
{
    s_clock.year = year; s_clock.mon = mon; s_clock.mday = mday;
    s_clock.hour = hour; s_clock.min = min; s_clock.sec = sec;
    s_clock.wday = wday; s_clock.set = true;
    return ESP_OK;
}

/* ---- imu: board flat and still ---- */

static int l_imu_accel(lua_State *L)
{
    lua_pushnumber(L, s_ax);
    lua_pushnumber(L, s_ay);
    lua_pushnumber(L, s_az);  /* default (0,0,1): board face-up, |a| = 1 g */
    return 3;
}

static int l_imu_gyro(lua_State *L)
{
    lua_pushnumber(L, s_gx);
    lua_pushnumber(L, s_gy);
    lua_pushnumber(L, s_gz);
    return 3;
}

static int l_imu_die_temp(lua_State *L)
{
    lua_pushnumber(L, 25.9);  /* silicon on a powered board, not the room */
    return 1;
}

static const luaL_Reg imu_funcs[] = {
    {"accel", l_imu_accel},
    {"gyro", l_imu_gyro},
    {"die_temp", l_imu_die_temp},
    {NULL, NULL},
};
static int luaopen_imu(lua_State *L) { luaL_newlib(L, imu_funcs); return 1; }

/* ---- battery ---- */

static int l_bat_percent(lua_State *L)
{
    if (s_bat_pct < 0) {          /* mirror the device's un-settled gauge */
        lua_pushnil(L);
        lua_pushliteral(L, "gauge not ready");
        return 2;
    }
    lua_pushinteger(L, s_bat_pct);
    return 1;
}
static int l_bat_volts(lua_State *L)    { lua_pushnumber(L, s_bat_volts); return 1; }
static int l_bat_charging(lua_State *L) { lua_pushboolean(L, s_bat_charging); return 1; }
static int l_bat_external(lua_State *L) { lua_pushboolean(L, s_bat_external); return 1; }

static const luaL_Reg bat_funcs[] = {
    {"percent", l_bat_percent},
    {"volts", l_bat_volts},
    {"charging", l_bat_charging},
    {"external", l_bat_external},
    {NULL, NULL},
};
static int luaopen_battery(lua_State *L) { luaL_newlib(L, bat_funcs); return 1; }

esp_err_t app_sensors_register(void)
{
    esp_err_t err = cap_lua_register_module("rtc", luaopen_rtc);
    if (err != ESP_OK) return err;
    err = cap_lua_register_module("imu", luaopen_imu);
    if (err != ESP_OK) return err;
    return cap_lua_register_module("battery", luaopen_battery);
}
