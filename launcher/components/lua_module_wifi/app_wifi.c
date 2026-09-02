#include <string.h>
#include <stdio.h>
#include <stdlib.h>
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
#include "esp_timer.h"
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
/* A PRIVATE namespace, deliberately not "shell".
 *
 * "shell" is the namespace the `prefs` Lua module exposes, so while the
 * credentials lived there any app could read the plaintext Wi-Fi password
 * with prefs.get("wifi_pass") -- one line, no privilege, nothing logged. The
 * launcher documents that apps are not sandboxed from each other's FILES;
 * it does not follow that they should get the user's network password, and
 * `prefs` was otherwise the one thing an app could not reach around.
 *
 * Nothing in Lua needs these: wifi.connect(ssid, pass) saves them from C, so
 * apps/settings.lua never had to write them itself. See also the denylist in
 * lua_module_prefs.c, which stops an app reading a value left behind by an
 * older build or planting one of its own. */
#define WIFI_NVS_NS   "wifinet"
#define WIFI_NVS_OLD  "shell"
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
    WIFI_RETRYING,
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

/* True only when the user (or the boot auto-connect) actually asked to join a
 * network. The radio has to be RUNNING to scan, and starting it raises
 * WIFI_EVENT_STA_START, whose handler used to call esp_wifi_connect()
 * unconditionally -- so a scan connected as a side effect. That was harmless
 * while connect_with() was the only thing that ever started the radio; it
 * stopped being harmless when scanning began starting it too.
 *
 * The sharp edge it created: esp_wifi_forget() clears OUR copies of the
 * credentials, but the driver keeps its own NVS-backed STA config, so the
 * first scan after "forget" silently rejoined the network just forgotten.
 * Scanning is an observation. Only a connect connects. */
static bool s_want_connect;

#define SCAN_MAX  20

typedef struct {
    char   ssid[SSID_MAX + 1];
    int8_t rssi;
    bool   secure;
} scan_rec_t;

/* -1 = no scan started, 1 = in flight, 0 = results ready. Written by the
 * event handler (system task), read by Lua (app task); s_scan[] is only
 * written before s_scan_state publishes it, same discipline as s_ip above. */
static volatile int s_scan_state = -1;
static scan_rec_t   s_scan[SCAN_MAX];
static int          s_scan_n;

/* Backoff after the fast retries are spent. Capped at the last entry. */
static const int s_backoff_s[] = { 30, 60, 120, 300 };
#define BACKOFF_N ((int)(sizeof(s_backoff_s) / sizeof(s_backoff_s[0])))

static esp_timer_handle_t s_retry_timer;
static int                s_backoff_idx;
static const char        *s_error;      /* why we are FAILED or RETRYING */

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

/* Read the credentials out of the old shared "shell" namespace, and erase
 * them from it. Runs once, on the first boot after they moved somewhere apps
 * cannot read. Erasing is the point: migrating without it would leave the
 * password readable by prefs.get() forever on every board already in use. */
static bool creds_save(const char *ssid, const char *pass);

static bool creds_migrate_from_shell(char *ssid, char *pass)
{
    nvs_handle_t h;
    size_t slen = SSID_MAX, plen = PASS_MAX;
    bool moved = false;

    if (nvs_open(WIFI_NVS_OLD, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    ssid[0] = pass[0] = '\0';
    if (nvs_get_str(h, WIFI_NVS_SSID, ssid, &slen) == ESP_OK && ssid[0] != '\0') {
        plen = PASS_MAX;
        if (nvs_get_str(h, WIFI_NVS_PASS, pass, &plen) != ESP_OK) {
            pass[0] = '\0';
        }
        moved = true;
    }
    if (nvs_erase_key(h, WIFI_NVS_SSID) == ESP_OK ||
            nvs_erase_key(h, WIFI_NVS_PASS) == ESP_OK) {
        nvs_commit(h);
    }
    nvs_close(h);
    if (moved) {
        ESP_LOGI(TAG, "moved saved network out of the app-readable namespace");
    }
    return moved;
}

static bool creds_load_nvs(char *ssid, char *pass)
{
    nvs_handle_t h;
    if (nvs_open(WIFI_NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        /* Namespace absent -- nothing has ever been written HERE. It may
         * still be sitting in the old shared namespace from a previous
         * build, so try that before giving up. */
        if (creds_migrate_from_shell(ssid, pass)) {
            creds_save(ssid, pass);
            return true;
        }
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
    /* Drop the intent too, not just the stored credentials: the driver keeps
     * its OWN NVS-backed STA config, which we do not erase, so leaving the
     * flag set would let the next scan rejoin the forgotten network. */
    s_want_connect = false;

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

static void retry_timer_cb(void *arg);

/* Every state change goes through here. The retry timer is armed ONLY on
 * entry to WIFI_RETRYING and cancelled on entry to anything else; that is
 * the whole reason this is a function rather than scattered assignments,
 * because the timer interacts with both scan_start() and connect(). */
static void wifi_set_state(wifi_state_t next, const char *err)
{
    if (next != WIFI_RETRYING && s_retry_timer) {
        esp_timer_stop(s_retry_timer);            /* harmless if not running */
    }
    if (next == WIFI_CONNECTING || next == WIFI_CONNECTED) {
        err = NULL;                               /* never report a stale reason */
    }
    s_error = err;
    s_state = next;

    if (next == WIFI_RETRYING) {
        int secs = s_backoff_s[s_backoff_idx];
        if (s_backoff_idx < BACKOFF_N - 1) {
            s_backoff_idx++;
        }
        if (!s_retry_timer) {
            const esp_timer_create_args_t a = {
                .callback = retry_timer_cb, .name = "wifi_retry",
            };
            if (esp_timer_create(&a, &s_retry_timer) != ESP_OK) {
                ESP_LOGE(TAG, "retry timer create failed; staying failed");
                s_state = WIFI_FAILED;
                return;
            }
        }
        esp_timer_start_once(s_retry_timer, (int64_t)secs * 1000000);
        ESP_LOGI(TAG, "retrying in %ds (%s)", secs, err ? err : "?");
    }
}

static void retry_timer_cb(void *arg)
{
    (void)arg;
    char ssid[SSID_MAX], pass[PASS_MAX];
    esp_err_t err;

    /* esp_timer_stop() in wifi_set_state() cannot recall a callback that has
     * ALREADY begun running on the esp_timer task. So disconnect() can land
     * in the middle of this one: it clears s_want_connect, stops the radio
     * and sets WIFI_OFF -- and then the lines below overwrote that with
     * WIFI_CONNECTING and called esp_wifi_connect() on a stopped radio.
     *
     * That call returns ESP_ERR_WIFI_NOT_STARTED, which was discarded, and
     * because the radio is down no event ever arrives to move the state on.
     * status() then answered "connecting" forever -- through app exit and
     * relaunch, since this state lives in the launcher; only a reboot cleared
     * it. Re-check the intent we may have just lost, and stop trusting that
     * esp_wifi_connect() succeeded. */
    if (!s_want_connect || !s_stack_up) {
        ESP_LOGI(TAG, "retry cancelled -- disconnected while the timer fired");
        return;
    }
    if (!creds_load(ssid, pass)) {
        wifi_set_state(WIFI_FAILED, "no saved network");
        return;
    }
    s_retries = 0;
    wifi_set_state(WIFI_CONNECTING, NULL);
    err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "retry connect failed: %s", esp_err_to_name(err));
        wifi_set_state(WIFI_FAILED, "radio unavailable");
    }
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)data;

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        /* Gated: STA_START also fires when the radio is started merely to
         * scan. See s_want_connect. */
        if (s_want_connect) {
            esp_wifi_connect();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)data;
        s_ip[0] = '\0';

        /* A wrong password will never succeed, so retrying it forever just
         * keeps the radio busy -- that is why this used to give up. But an
         * absent AP is the opposite case: it may well come back, and giving
         * up meant the board never reconnected after a router reboot or a
         * walk back into range. The reason code separates them. */
        bool auth = (d->reason == WIFI_REASON_AUTH_FAIL ||
                     d->reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
                     d->reason == WIFI_REASON_HANDSHAKE_TIMEOUT);
        if (auth) {
            ESP_LOGW(TAG, "auth failed (reason %d) -- not retrying", d->reason);
            wifi_set_state(WIFI_FAILED, "wrong password");
        } else if (!s_want_connect) {
            /* Not our connection to keep alive -- a scan interrupted an idle
             * radio, or the user just disconnected. Retrying here is what
             * would resurrect a forgotten network. */
            wifi_set_state(WIFI_OFF, NULL);
        } else if (s_retries < MAX_RETRIES) {
            s_retries++;
            wifi_set_state(WIFI_CONNECTING, NULL);
            esp_wifi_connect();
        } else {
            wifi_set_state(WIFI_RETRYING,
                           d->reason == WIFI_REASON_NO_AP_FOUND
                               ? "network not found" : "connection lost");
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        uint16_t n = SCAN_MAX;
        wifi_ap_record_t *recs = calloc(n, sizeof(*recs));
        s_scan_n = 0;
        if (recs && esp_wifi_scan_get_ap_records(&n, recs) == ESP_OK) {
            for (uint16_t i = 0; i < n && s_scan_n < SCAN_MAX; i++) {
                const char *ssid = (const char *)recs[i].ssid;
                if (ssid[0] == '\0') {
                    continue;            /* hidden network */
                }
                /* Dedupe by SSID, keeping the strongest. Repeaters and
                 * dual-band APs otherwise list the same name two or three
                 * times, which reads as a bug rather than as radio reality. */
                int found = -1;
                for (int j = 0; j < s_scan_n; j++) {
                    if (strcmp(s_scan[j].ssid, ssid) == 0) { found = j; break; }
                }
                if (found >= 0) {
                    if (recs[i].rssi > s_scan[found].rssi) {
                        s_scan[found].rssi = recs[i].rssi;
                    }
                    continue;
                }
                snprintf(s_scan[s_scan_n].ssid, sizeof(s_scan[s_scan_n].ssid), "%s", ssid);
                s_scan[s_scan_n].rssi   = recs[i].rssi;
                s_scan[s_scan_n].secure = (recs[i].authmode != WIFI_AUTH_OPEN);
                s_scan_n++;
            }
            /* Strongest first. Insertion sort: n <= 20. */
            for (int i = 1; i < s_scan_n; i++) {
                scan_rec_t key = s_scan[i];
                int j = i - 1;
                while (j >= 0 && s_scan[j].rssi < key.rssi) {
                    s_scan[j + 1] = s_scan[j];
                    j--;
                }
                s_scan[j + 1] = key;
            }
        }
        free(recs);
        esp_wifi_clear_ap_list();
        s_scan_state = 0;               /* publishes s_scan[] */
        /* Both counts: raw is what the radio saw, the first is what the UI
         * gets. They differ by the hidden networks dropped and the
         * dual-band/repeater duplicates merged -- if they are ever equal on a
         * normal home network, the dedupe has stopped working. */
        ESP_LOGI(TAG, "scan done: %d network(s) (%d raw)", s_scan_n, (int)n);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&e->ip_info.ip));
        s_retries = 0;
        s_backoff_idx = 0;
        wifi_set_state(WIFI_CONNECTED, NULL);
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
        wifi_set_state(WIFI_FAILED, "wifi stack failed");
        return false;
    }

    wifi_config_t wc = { 0 };
    strncpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid) - 1);
    if (pass && pass[0]) {
        strncpy((char *)wc.sta.password, pass, sizeof(wc.sta.password) - 1);
    }

    esp_wifi_stop();
    if (esp_wifi_set_config(WIFI_IF_STA, &wc) != ESP_OK) {
        wifi_set_state(WIFI_FAILED, "config failed");
        return false;
    }
    s_retries = 0;
    s_backoff_idx = 0;                  /* a manual connect restarts the ladder */
    s_want_connect = true;              /* this is the intent STA_START checks */
    wifi_set_state(WIFI_CONNECTING, NULL);
    if (esp_wifi_start() != ESP_OK) {   /* STA_START triggers connect */
        wifi_set_state(WIFI_FAILED, "wifi start failed");
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
    case WIFI_RETRYING:   lua_pushliteral(L, "retrying");   break;
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
    s_want_connect = false;        /* a later scan must not rejoin */
    if (s_stack_up) {
        s_retries = MAX_RETRIES;   /* stop the auto-retry chain */
        esp_wifi_disconnect();
        esp_wifi_stop();
    }
    wifi_set_state(WIFI_OFF, NULL);     /* also cancels any pending retry */
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

/* wifi.scan_start() -- begin an async scan. Non-blocking; poll
 * scan_results(). Scanning while CONNECTED briefly interrupts the link,
 * which is accepted so a user can switch networks without disconnecting
 * first; scanning while CONNECTING is refused because it would abort an
 * attempt already in progress. */
static int l_wifi_scan_start(lua_State *L)
{
    if (s_state == WIFI_CONNECTING) {
        lua_pushnil(L);
        lua_pushliteral(L, "connecting");
        return 2;
    }
    if (!stack_start()) {
        lua_pushnil(L);
        lua_pushliteral(L, "wifi stack failed");
        return 2;
    }
    /* The radio must be running to scan, even with no saved credentials --
     * connect_with() is otherwise the only thing that ever starts it. */
    esp_wifi_start();

    if (s_scan_state == 1) {
        lua_pushboolean(L, 1);     /* already scanning: no-op, still success */
        return 1;
    }
    /* Marked scanning BEFORE the start call, and rolled back on failure. The
     * other order loses the race against WIFI_EVENT_SCAN_DONE: an event landing
     * between the two lines has its results overwritten by this `1`, and the
     * loss is permanent rather than transient -- scan_results() then returns
     * nil forever, and a restart is a deliberate no-op while the state says 1. */
    s_scan_state = 1;
    if (esp_wifi_scan_start(NULL, false) != ESP_OK) {
        s_scan_state = 0;
        lua_pushnil(L);
        lua_pushliteral(L, "scan failed");
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

/* nil while scanning (or before any scan); otherwise an array of
 * {ssid=, rssi=, secure=}. Idempotent -- reading does not consume. */
static int l_wifi_scan_results(lua_State *L)
{
    if (s_scan_state != 0) {
        lua_pushnil(L);
        return 1;
    }
    lua_createtable(L, s_scan_n, 0);
    for (int i = 0; i < s_scan_n; i++) {
        lua_createtable(L, 0, 3);
        lua_pushstring(L, s_scan[i].ssid);    lua_setfield(L, -2, "ssid");
        lua_pushinteger(L, s_scan[i].rssi);   lua_setfield(L, -2, "rssi");
        lua_pushboolean(L, s_scan[i].secure); lua_setfield(L, -2, "secure");
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

/* Why the current failed/retrying state exists; nil otherwise. Cleared on
 * any transition into connecting or connected, so it never reports a stale
 * reason from an earlier attempt. */
static int l_wifi_error(lua_State *L)
{
    if (s_error) lua_pushstring(L, s_error); else lua_pushnil(L);
    return 1;
}

static const luaL_Reg wifi_funcs[] = {
    {"connect", l_wifi_connect},
    {"status", l_wifi_status},
    {"ip", l_wifi_ip},
    {"disconnect", l_wifi_disconnect},
    {"time_synced", l_wifi_time_synced},
    {"forget", l_wifi_forget},
    {"scan_start", l_wifi_scan_start},
    {"scan_results", l_wifi_scan_results},
    {"error", l_wifi_error},
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
