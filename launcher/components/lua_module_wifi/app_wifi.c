#include <string.h>
#include <stdio.h>
#include <time.h>
#include "app_wifi.h"
#include "app_sensors.h"
#include "cap_lua.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_event.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lauxlib.h"

static const char *TAG = "app_wifi";

/* Credentials live in NVS, in the same "shell" namespace the launcher and the
 * `prefs` Lua module share -- apps/settings.lua already writes wifi_ssid /
 * wifi_pass there through prefs. Until this change the C side still read and
 * wrote the card, so the two disagreed: Settings saved a network to NVS, the
 * boot auto-connect read a different (or absent) one off the card, and on a
 * card-less board Settings appeared to save while nothing was ever stored. */
#define WIFI_NVS_NS   "shell"
#define WIFI_NVS_SSID "wifi_ssid"
#define WIFI_NVS_PASS "wifi_pass"

/* Read once, to import a network saved by an older build. Never written. */
#define LEGACY_CREDS_PATH "/sdcard/wifi.txt"
#define SSID_MAX     32
#define PASS_MAX     64
#define MAX_RETRIES  5

typedef enum {
    WIFI_OFF,
    WIFI_CONNECTING,
    WIFI_CONNECTED,
    WIFI_FAILED,
} wifi_state_t;

/* Written by the event handler (system task), read by Lua (app task).
 * Both are single writes of a word-sized value, and the strings are only
 * written before the state that publishes them -- no lock needed, but
 * volatile so the reader cannot cache it. */
static volatile wifi_state_t s_state = WIFI_OFF;
static volatile bool s_time_synced;
static char s_ip[16];
static int  s_retries;
static bool s_stack_up;

/* ---- credentials ---- */

static bool creds_save(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    if (nvs_open(WIFI_NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "nvs open failed -- credentials not saved");
        return false;
    }
    esp_err_t e1 = nvs_set_str(h, WIFI_NVS_SSID, ssid);
    /* An open network has no password. Store the empty string rather than
     * erasing the key, so "saved, no password" and "never saved" stay
     * distinguishable. */
    esp_err_t e2 = nvs_set_str(h, WIFI_NVS_PASS, pass ? pass : "");
    esp_err_t e3 = nvs_commit(h);
    nvs_close(h);

    if (e1 != ESP_OK || e2 != ESP_OK || e3 != ESP_OK) {
        ESP_LOGE(TAG, "nvs write failed -- credentials not saved");
        return false;
    }
    return true;
}

static bool creds_load_nvs(char *ssid, char *pass)
{
    nvs_handle_t h;
    if (nvs_open(WIFI_NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        /* Namespace absent -- nothing has ever been written. Not an error. */
        return false;
    }
    size_t slen = SSID_MAX, plen = PASS_MAX;
    ssid[0] = pass[0] = '\0';
    esp_err_t err = nvs_get_str(h, WIFI_NVS_SSID, ssid, &slen);
    if (err == ESP_OK) {
        /* A password is optional (open network), the SSID is not. */
        plen = PASS_MAX;
        if (nvs_get_str(h, WIFI_NVS_PASS, pass, &plen) != ESP_OK) {
            pass[0] = '\0';
        }
    }
    nvs_close(h);
    return err == ESP_OK && ssid[0] != '\0';
}

/* The pre-NVS format: SSID on the first line, password on the second. Read
 * only to migrate a board that was set up by an older build. */
static bool creds_load_legacy(char *ssid, char *pass)
{
    FILE *f = fopen(LEGACY_CREDS_PATH, "r");
    if (f == NULL) {
        return false;
    }
    ssid[0] = pass[0] = '\0';
    bool ok = (fgets(ssid, SSID_MAX, f) != NULL);
    if (ok) {
        if (fgets(pass, PASS_MAX, f) == NULL) {
            pass[0] = '\0';
        }
    }
    fclose(f);

    for (char *p = ssid; *p; p++) if (*p == '\n' || *p == '\r') { *p = '\0'; break; }
    for (char *p = pass; *p; p++) if (*p == '\n' || *p == '\r') { *p = '\0'; break; }
    return ok && ssid[0] != '\0';
}

static void creds_forget(void)
{
    nvs_handle_t h;
    if (nvs_open(WIFI_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        /* ESP_ERR_NVS_NOT_FOUND here just means it was never saved. */
        nvs_erase_key(h, WIFI_NVS_SSID);
        nvs_erase_key(h, WIFI_NVS_PASS);
        nvs_commit(h);
        nvs_close(h);
    }
    /* The legacy file too, or the next boot's migration would import the very
     * network the user just asked to forget. This is the one place deleting it
     * is right: "forget" is exactly that request. */
    remove(LEGACY_CREDS_PATH);
}

static bool creds_load(char *ssid, char *pass)
{
    if (creds_load_nvs(ssid, pass)) {
        return true;
    }
    /* Nothing in NVS: import the card's file once, so a board that already had
     * a network keeps it across this change without anyone retyping a password
     * on a touchscreen. The file is left in place rather than deleted -- it is
     * the user's data, and NVS is checked first from here on, so a network
     * changed in Settings wins regardless. */
    if (creds_load_legacy(ssid, pass)) {
        ESP_LOGI(TAG, "migrating credentials from %s to NVS", LEGACY_CREDS_PATH);
        creds_save(ssid, pass);
        return true;
    }
    return false;
}

/* ---- NTP ---- */

static void sync_rtc_from_system_time(void)
{
    time_t now = 0;
    struct tm t;

    time(&now);
    gmtime_r(&now, &t);
    if (t.tm_year + 1900 < 2024) {
        return;   /* SNTP has not landed yet */
    }
    if (app_sensors_rtc_set_tm(t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                               t.tm_hour, t.tm_min, t.tm_sec, t.tm_wday) == ESP_OK) {
        s_time_synced = true;
        ESP_LOGI(TAG, "RTC set from NTP: %04d-%02d-%02d %02d:%02d:%02d UTC",
                 t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
    }
}

static void ntp_callback(struct timeval *tv)
{
    (void)tv;
    sync_rtc_from_system_time();
}

static void start_ntp(void)
{
    static bool started;

    if (started) {
        return;
    }
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    cfg.sync_cb = ntp_callback;
    cfg.start = true;
    if (esp_netif_sntp_init(&cfg) == ESP_OK) {
        started = true;
        ESP_LOGI(TAG, "NTP started");
    }
}

/* ---- events ---- */

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)data;

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_ip[0] = '\0';
        if (s_retries < MAX_RETRIES) {
            s_retries++;
            s_state = WIFI_CONNECTING;
            esp_wifi_connect();
        } else {
            /* Give up rather than retry forever: a wrong password would
             * otherwise keep the radio busy for the life of the board. */
            s_state = WIFI_FAILED;
            ESP_LOGW(TAG, "giving up after %d attempts", MAX_RETRIES);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&e->ip_info.ip));
        s_retries = 0;
        s_state = WIFI_CONNECTED;
        ESP_LOGI(TAG, "connected, ip=%s", s_ip);
        start_ntp();
    }
}

/* Bring the stack up once. Returns false if anything failed, in which
 * case wifi.status() reports "failed" and the launcher carries on --
 * networking is never allowed to stop the device booting. */
static bool stack_start(void)
{
    if (s_stack_up) {
        return true;
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs init failed: %s", esp_err_to_name(err));
        return false;
    }

    if (esp_netif_init() != ESP_OK) return false;
    if (esp_event_loop_create_default() != ESP_OK) return false;
    if (esp_netif_create_default_wifi_sta() == NULL) return false;

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&cfg) != ESP_OK) return false;

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi_event, NULL, NULL);

    if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK) return false;
    s_stack_up = true;
    return true;
}

static bool connect_with(const char *ssid, const char *pass)
{
    if (!stack_start()) {
        s_state = WIFI_FAILED;
        return false;
    }

    wifi_config_t wc = { 0 };
    strncpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid) - 1);
    if (pass && pass[0]) {
        strncpy((char *)wc.sta.password, pass, sizeof(wc.sta.password) - 1);
    }

    esp_wifi_stop();
    if (esp_wifi_set_config(WIFI_IF_STA, &wc) != ESP_OK) {
        s_state = WIFI_FAILED;
        return false;
    }
    s_retries = 0;
    s_state = WIFI_CONNECTING;
    if (esp_wifi_start() != ESP_OK) {   /* STA_START triggers connect */
        s_state = WIFI_FAILED;
        return false;
    }
    return true;
}

/* ---- Lua API ---- */

/* wifi.connect()             -- use saved credentials
 * wifi.connect(ssid, pass)   -- use these, and save them for next boot */
static int l_wifi_connect(lua_State *L)
{
    char ssid[SSID_MAX], pass[PASS_MAX];

    if (lua_isnoneornil(L, 1)) {
        if (!creds_load(ssid, pass)) {
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
        strncpy(ssid, s, sizeof(ssid) - 1); ssid[sizeof(ssid) - 1] = '\0';
        strncpy(pass, p, sizeof(pass) - 1); pass[sizeof(pass) - 1] = '\0';
        creds_save(ssid, pass);
    }

    if (!connect_with(ssid, pass)) {
        lua_pushnil(L);
        lua_pushliteral(L, "wifi stack failed to start");
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int l_wifi_status(lua_State *L)
{
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

static int l_wifi_disconnect(lua_State *L)
{
    if (s_stack_up) {
        s_retries = MAX_RETRIES;   /* stop the auto-retry chain */
        esp_wifi_disconnect();
        esp_wifi_stop();
    }
    s_state = WIFI_OFF;
    s_ip[0] = '\0';
    lua_pushboolean(L, 1);
    return 1;
}

/* True once NTP has set the RTC this boot. */
static int l_wifi_time_synced(lua_State *L)
{
    lua_pushboolean(L, s_time_synced);
    return 1;
}

static int l_wifi_forget(lua_State *L)
{
    creds_forget();
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

esp_err_t app_wifi_register(void)
{
    return cap_lua_register_module("wifi", luaopen_wifi);
}

void app_wifi_autostart(void)
{
    char ssid[SSID_MAX], pass[PASS_MAX];

    if (!creds_load(ssid, pass)) {
        ESP_LOGI(TAG, "no saved network; wifi idle");
        return;
    }
    ESP_LOGI(TAG, "auto-connecting to '%s'", ssid);
    connect_with(ssid, pass);
}
