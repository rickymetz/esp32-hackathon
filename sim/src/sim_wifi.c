/* Simulator wifi module -- require("wifi") without a radio.
 *
 * The real module (components/lua_module_wifi/app_wifi.c) drives esp_wifi in
 * station mode and syncs the RTC over SNTP; none of that exists on the host.
 * Rather than report "off" forever (which would leave a networked app's happy
 * path untested), the sim runs a small deterministic state machine so a drive
 * script can exercise connect -> connecting -> connected:
 *
 *   connect(ssid, pw)  validates the same lengths the device does, then enters
 *                      CONNECTING with a 3-poll fuse.
 *   status()           each call while CONNECTING burns one poll; on the third
 *                      it flips to CONNECTED, publishes a fake IP, marks time
 *                      synced, and sets the RTC (the NTP-writes-the-clock path,
 *                      via app_sensors_rtc_set_tm) -- exactly the observable
 *                      the device gives an app polling from timer.every.
 *   disconnect/forget  return to OFF.
 *
 * connect() with no argument reports "no saved network" (the host has no
 * /sdcard/wifi.txt), matching the device. sim_wifi_reset() clears the state
 * between app runs so one app's connection can't leak into the next.
 */
#include "app_wifi.h"
#include "app_sensors.h"      /* app_sensors_rtc_set_tm -- the NTP path */
#include "cap_lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include <string.h>

#define SSID_MAX  32          /* strlen must be < this -- matches the device */
#define PASS_MAX  64          /* strlen must be < this, i.e. <= 63 chars */
#define CONNECT_POLLS  3      /* status() calls spent in "connecting" */

enum { WIFI_OFF, WIFI_CONNECTING, WIFI_CONNECTED, WIFI_FAILED };

static int  s_state = WIFI_OFF;
static int  s_polls_left = 0;
static bool s_have_creds = false;
static bool s_time_synced = false;
static bool s_will_fail = false;      /* set by the `wifi fail` sim command */
static char s_ip[16] = {0};

void sim_wifi_reset(void)
{
    s_state = WIFI_OFF;
    s_polls_left = 0;
    s_have_creds = false;
    s_time_synced = false;
    s_will_fail = false;
    s_ip[0] = '\0';
}

/* sim-only: choose whether the next connect resolves to connected or failed
 * (the device's "wrong password, gave up after five tries" path). */
void sim_wifi_set_outcome(bool succeed) { s_will_fail = !succeed; }

static void become_connected(void)
{
    s_state = WIFI_CONNECTED;
    strncpy(s_ip, "192.168.1.42", sizeof(s_ip) - 1);
    s_ip[sizeof(s_ip) - 1] = '\0';
    s_time_synced = true;
    /* NTP writes the wall clock -- same fixed reading the sensors stub uses. */
    app_sensors_rtc_set_tm(2026, 8, 22, 14, 30, 0, 6);
}

static int l_wifi_connect(lua_State *L)
{
    if (lua_isnoneornil(L, 1)) {
        if (!s_have_creds) {
            lua_pushnil(L);
            lua_pushliteral(L, "no saved network");
            return 2;
        }
    } else {
        const char *s = luaL_checkstring(L, 1);
        const char *p = luaL_optstring(L, 2, "");
        if (strlen(s) == 0 || strlen(s) >= SSID_MAX || strlen(p) >= PASS_MAX) {
            lua_pushnil(L);
            lua_pushliteral(L, "bad ssid or password length");
            return 2;
        }
        s_have_creds = true;   /* "saved" for a later argless connect() */
    }
    s_state = WIFI_CONNECTING;
    s_polls_left = CONNECT_POLLS;
    s_time_synced = false;
    s_ip[0] = '\0';
    lua_pushboolean(L, 1);
    return 1;
}

static int l_wifi_status(lua_State *L)
{
    if (s_state == WIFI_CONNECTING) {
        if (s_polls_left > 0) {
            s_polls_left--;
        }
        if (s_polls_left == 0) {
            if (s_will_fail) {
                s_state = WIFI_FAILED;   /* gave up (e.g. wrong password) */
            } else {
                become_connected();
            }
        }
    }
    switch (s_state) {
    case WIFI_CONNECTING: lua_pushliteral(L, "connecting"); break;
    case WIFI_CONNECTED:  lua_pushliteral(L, "connected");  break;
    case WIFI_FAILED:     lua_pushliteral(L, "failed");     break;
    default:              lua_pushliteral(L, "off");        break;
    }
    return 1;
}

static int l_wifi_ip(lua_State *L)
{
    if (s_state == WIFI_CONNECTED && s_ip[0]) {
        lua_pushstring(L, s_ip);
    } else {
        lua_pushnil(L);
    }
    return 1;
}

static int l_wifi_time_synced(lua_State *L)
{
    lua_pushboolean(L, s_time_synced);
    return 1;
}

static int l_wifi_disconnect(lua_State *L)
{
    /* The device drops the connection but leaves s_time_synced alone -- NTP
     * already set the RTC "this boot", and that stays true after a disconnect. */
    s_state = WIFI_OFF;
    s_polls_left = 0;
    s_ip[0] = '\0';
    lua_pushboolean(L, 1);
    return 1;
}

static int l_wifi_forget(lua_State *L)
{
    /* The device only deletes the saved credentials; a live connection stays
     * up (status() still reports "connected"). Mirror that -- clear the saved
     * flag, touch nothing else. */
    s_have_creds = false;
    lua_pushboolean(L, 1);
    return 1;
}

static const luaL_Reg wifi_funcs[] = {
    {"connect", l_wifi_connect},
    {"status", l_wifi_status},
    {"ip", l_wifi_ip},
    {"disconnect", l_wifi_disconnect},
    {"time_synced", l_wifi_time_synced},
    {"forget", l_wifi_forget},
    {NULL, NULL},
};

static int luaopen_wifi(lua_State *L) { luaL_newlib(L, wifi_funcs); return 1; }

void app_wifi_autostart(void) { /* no card, no saved creds on the host */ }

esp_err_t app_wifi_register(void)
{
    return cap_lua_register_module("wifi", luaopen_wifi);
}
