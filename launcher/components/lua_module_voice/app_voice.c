#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include "app_voice.h"
#include "cap_lua.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lauxlib.h"
#include "lua_module_lvgl.h"

#include "bsp/esp-bsp.h"
#include "esp_codec_dev.h"
#include "esp_mn_iface.h"
#include "esp_mn_models.h"
#include "esp_mn_speech_commands.h"
#include "model_path.h"

static const char *TAG = "app_voice";

#define VOICE_MAX_COMMANDS  64
#define VOICE_CMD_MAX_LEN   64
#define VOICE_TIMEOUT_MS    6000
#define VOICE_SAMPLE_RATE   16000

/* ---- One-time model/mic state (app_main task, then read-only) ---- */
static srmodel_list_t *s_models;
static char *s_mn_name;
static esp_mn_iface_t *s_mn;
static esp_codec_dev_handle_t s_mic;
static model_iface_data_t *s_mn_data;   /* one persistent instance: the
    command table binds to it at alloc, so it must never be destroyed --
    the first version alloc'd against a probe then freed it, leaving the
    vocabulary pointing at dead state (recognition silently never used
    the app's commands) */
static bool s_available;

/* ---- Capture task <-> app task, under s_result_lock ---- */
typedef enum {
    VOICE_IDLE,
    VOICE_RUNNING,
    VOICE_STOPPING,
} voice_state_t;

static portMUX_TYPE s_result_lock = portMUX_INITIALIZER_UNLOCKED;
static volatile voice_state_t s_state = VOICE_IDLE;
static int  s_result_id;          /* -1 = none, -2 = timeout, else phrase id */
static bool s_result_pending;

/* ---- App-task-only state ---- */
typedef enum {
    MODE_LISTEN,   /* one-shot vocabulary match */
    MODE_SPELL,    /* NATO letters accumulate until "done" or timeout */
} voice_mode_t;

static voice_mode_t s_mode = MODE_LISTEN;
static int  s_callback_ref = LUA_NOREF;
static char s_commands[VOICE_MAX_COMMANDS][VOICE_CMD_MAX_LEN];
static int  s_command_count;
static char s_spell_buf[64];
static size_t s_spell_len;

/* NATO alphabet: multi-syllable on purpose -- MultiNet is unreliable on
 * short words ("lap" fails where "start" works), and letter names
 * collide acoustically (B/C/D/E/G/P/T/V/Z). Controls are phrases for
 * the same reason. */
typedef struct { const char *word; char ch; } spell_entry_t;
static const spell_entry_t SPELL_VOCAB[] = {
    {"alpha",'A'},{"bravo",'B'},{"charlie",'C'},{"delta",'D'},{"echo",'E'},
    {"foxtrot",'F'},{"golf",'G'},{"hotel",'H'},{"india",'I'},{"juliet",'J'},
    {"kilo",'K'},{"lima",'L'},{"mike",'M'},{"november",'N'},{"oscar",'O'},
    {"papa",'P'},{"quebec",'Q'},{"romeo",'R'},{"sierra",'S'},{"tango",'T'},
    {"uniform",'U'},{"victor",'V'},{"whiskey",'W'},{"x ray",'X'},
    {"yankee",'Y'},{"zulu",'Z'},
    {"zero",'0'},{"one",'1'},{"two",'2'},{"three",'3'},{"four",'4'},
    {"five",'5'},{"six",'6'},{"seven",'7'},{"eight",'8'},{"niner",'9'},
    {"space",' '},
    {"delete that",'\b'},
    /* "finished" matched "five" (shared fi- onset, learned on device);
     * "over" is the radio convention and shares an onset with nothing
     * else in this vocabulary. */
    {"over",'\0'},
};
#define SPELL_VOCAB_COUNT (sizeof(SPELL_VOCAB) / sizeof(SPELL_VOCAB[0]))

static void voice_capture_task(void *arg);

static bool voice_start_capture(void)
{
    portENTER_CRITICAL(&s_result_lock);
    s_state = VOICE_RUNNING;
    s_result_pending = false;
    portEXIT_CRITICAL(&s_result_lock);
    if (xTaskCreate(voice_capture_task, "voice", 8 * 1024, NULL, 5, NULL) != pdPASS) {
        portENTER_CRITICAL(&s_result_lock);
        s_state = VOICE_IDLE;
        portEXIT_CRITICAL(&s_result_lock);
        return false;
    }
    return true;
}

/* The capture task: feed mic frames straight into MultiNet until it
 * reports a phrase or times out. No AFE, no wake word -- push-to-talk
 * with a close mic. Never touches the Lua state. */
static void voice_capture_task(void *arg)
{
    (void)arg;
    int16_t *buf = NULL;
    int result = -2;   /* timeout unless a phrase lands */

    s_mn->clean(s_mn_data);   /* reset recognizer state between captures */

    int chunk = s_mn->get_samp_chunksize(s_mn_data);
    /* I2S DMA path allocates internally; this working buffer is plain
     * PSRAM-eligible heap. */
    buf = malloc(chunk * sizeof(int16_t));
    if (buf == NULL) {
        ESP_LOGE(TAG, "no memory for %d-sample chunks", chunk);
        goto out;
    }

    esp_codec_dev_sample_info_t fs = {
        .sample_rate = VOICE_SAMPLE_RATE,
        .channel = 1,
        .bits_per_sample = 16,
    };
    if (esp_codec_dev_open(s_mic, &fs) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "mic open failed");
        goto out;
    }

    int64_t deadline = esp_timer_get_time() + (int64_t)VOICE_TIMEOUT_MS * 1000;
    while (s_state == VOICE_RUNNING &&
           esp_timer_get_time() < deadline &&
           !cap_lua_runtime_stop_requested(NULL)) {
        if (esp_codec_dev_read(s_mic, buf, chunk * sizeof(int16_t)) != ESP_CODEC_DEV_OK) {
            break;
        }
        /* detect() eats most of a core; without this breather the idle
         * task starves and the TWDT panics the board once spell mode
         * chains captures past ~10s (learned by rebooting it). */
        vTaskDelay(pdMS_TO_TICKS(2));
        esp_mn_state_t st = s_mn->detect(s_mn_data, buf);
        if (st == ESP_MN_STATE_DETECTED) {
            esp_mn_results_t *r = s_mn->get_results(s_mn_data);
            if (r != NULL && r->num > 0) {
                result = r->phrase_id[0];
                ESP_LOGI(TAG, "detected id=%d '%s'", result, r->string);
            }
            break;
        }
        if (st == ESP_MN_STATE_TIMEOUT) {
            esp_mn_results_t *r = s_mn->get_results(s_mn_data);
            ESP_LOGI(TAG, "mn timeout, raw='%s'",
                     (r && r->raw_string[0]) ? r->raw_string : "");
            break;
        }
    }
    esp_codec_dev_close(s_mic);

out:
    if (buf) free(buf);

    portENTER_CRITICAL(&s_result_lock);
    s_result_id = result;
    s_result_pending = true;
    s_state = VOICE_IDLE;
    portEXIT_CRITICAL(&s_result_lock);
    vTaskDelete(NULL);
}

static void voice_fire_callback(lua_State *L, const char *result)
{
    int ref = s_callback_ref;
    s_callback_ref = LUA_NOREF;

    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    if (result != NULL) {
        lua_pushstring(L, result);
    } else {
        lua_pushnil(L);
    }
    if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
        ESP_LOGE(TAG, "voice callback error: %s", lua_tostring(L, -1));
        lua_pop(L, 1);
        lua_lvgl_force_unlock_if_held();
    }
    luaL_unref(L, LUA_REGISTRYINDEX, ref);
}

void app_voice_run_pending(lua_State *L)
{
    bool pending;
    int id;

    portENTER_CRITICAL(&s_result_lock);
    pending = s_result_pending;
    id = s_result_id;
    s_result_pending = false;
    portEXIT_CRITICAL(&s_result_lock);

    if (!pending || s_callback_ref == LUA_NOREF) {
        return;
    }

    if (s_mode == MODE_LISTEN) {
        voice_fire_callback(L,
            (id >= 0 && id < s_command_count) ? s_commands[id] : NULL);
        return;
    }

    /* MODE_SPELL: accumulate and keep listening until "finished" or a
     * silent window (timeout with nothing new). */
    if (id >= 0 && id < (int)SPELL_VOCAB_COUNT) {
        char ch = SPELL_VOCAB[id].ch;
        if (ch == '\0') {                       /* "finished" */
            voice_fire_callback(L, s_spell_buf);
            return;
        }
        if (ch == '\b') {                       /* "delete that" */
            if (s_spell_len > 0) s_spell_buf[--s_spell_len] = '\0';
        } else if (s_spell_len + 1 < sizeof(s_spell_buf)) {
            s_spell_buf[s_spell_len++] = ch;
            s_spell_buf[s_spell_len] = '\0';
        }
        if (!voice_start_capture()) {
            voice_fire_callback(L, s_spell_buf);
        }
        return;
    }

    /* Timeout: silence ends the spell -- nil if nothing was said. */
    voice_fire_callback(L, s_spell_len > 0 ? s_spell_buf : NULL);
}

void app_voice_reset(lua_State *L)
{
    /* Ask a running capture to stop; it self-terminates and posts a
     * result nobody will read (callback ref is dropped below). */
    portENTER_CRITICAL(&s_result_lock);
    if (s_state == VOICE_RUNNING) {
        s_state = VOICE_STOPPING;
    }
    s_result_pending = false;
    portEXIT_CRITICAL(&s_result_lock);

    if (s_callback_ref != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, s_callback_ref);
        s_callback_ref = LUA_NOREF;
    }
    s_command_count = 0;
    s_spell_len = 0;
    s_spell_buf[0] = '\0';
    s_mode = MODE_LISTEN;
}

/* ---- Lua API ---- */

static int l_voice_available(lua_State *L)
{
    lua_pushboolean(L, s_available);
    return 1;
}

/* voice.listen({ commands = {"start", "stop", ...} }, cb)
 * One-shot: registers the vocabulary, starts a capture, and calls
 * cb(word) on recognition or cb(nil) on timeout/no-match. */
static int l_voice_listen(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    luaL_checktype(L, 2, LUA_TFUNCTION);

    if (!s_available) {
        lua_pushnil(L);
        lua_pushliteral(L, "unavailable");
        return 2;
    }
    if (s_state != VOICE_IDLE) {
        lua_pushnil(L);
        lua_pushliteral(L, "busy");
        return 2;
    }

    lua_getfield(L, 1, "commands");
    if (!lua_istable(L, -1)) {
        return luaL_error(L, "voice.listen: opts.commands must be a list of strings");
    }

    esp_mn_commands_clear();
    s_command_count = 0;
    for (int i = 1; i <= VOICE_MAX_COMMANDS; i++) {
        lua_rawgeti(L, -1, i);
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            break;
        }
        const char *cmd = lua_tostring(L, -1);
        if (cmd == NULL || strlen(cmd) >= VOICE_CMD_MAX_LEN) {
            return luaL_error(L, "voice.listen: command %d invalid", i);
        }
        strncpy(s_commands[s_command_count], cmd, VOICE_CMD_MAX_LEN - 1);
        /* phrase_id is our index into s_commands */
        esp_mn_commands_add(s_command_count, s_commands[s_command_count]);
        s_command_count++;
        lua_pop(L, 1);
    }
    lua_pop(L, 1);   /* commands table */

    if (s_command_count == 0) {
        return luaL_error(L, "voice.listen: no commands given");
    }
    esp_mn_error_t *err = esp_mn_commands_update();
    if (err != NULL && err->num > 0) {
        ESP_LOGW(TAG, "%d command(s) not accepted by the model", err->num);
    }

    lua_pushvalue(L, 2);
    s_callback_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    s_mode = MODE_LISTEN;

    if (!voice_start_capture()) {
        luaL_unref(L, LUA_REGISTRYINDEX, s_callback_ref);
        s_callback_ref = LUA_NOREF;
        return luaL_error(L, "voice task create failed");
    }

    lua_pushboolean(L, 1);
    return 1;
}

/* voice.spell(cb) -- NATO spelling: say "romeo india charlie kilo",
 * then "finished" (or go silent). "delete that" removes the last
 * character. cb(text), or cb(nil) if nothing was heard. */
static int l_voice_spell(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TFUNCTION);

    if (!s_available) {
        lua_pushnil(L);
        lua_pushliteral(L, "unavailable");
        return 2;
    }
    if (s_state != VOICE_IDLE) {
        lua_pushnil(L);
        lua_pushliteral(L, "busy");
        return 2;
    }

    esp_mn_commands_clear();
    for (int i = 0; i < (int)SPELL_VOCAB_COUNT; i++) {
        esp_mn_commands_add(i, SPELL_VOCAB[i].word);
    }
    esp_mn_commands_update();
    s_command_count = 0;   /* listen()'s table is not in play */
    s_spell_len = 0;
    s_spell_buf[0] = '\0';

    lua_pushvalue(L, 1);
    s_callback_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    s_mode = MODE_SPELL;

    if (!voice_start_capture()) {
        luaL_unref(L, LUA_REGISTRYINDEX, s_callback_ref);
        s_callback_ref = LUA_NOREF;
        return luaL_error(L, "voice task create failed");
    }

    lua_pushboolean(L, 1);
    return 1;
}

static int l_voice_stop(lua_State *L)
{
    (void)L;
    portENTER_CRITICAL(&s_result_lock);
    if (s_state == VOICE_RUNNING) {
        s_state = VOICE_STOPPING;
    }
    portEXIT_CRITICAL(&s_result_lock);
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

static int luaopen_app_voice(lua_State *L)
{
    luaL_newlib(L, voice_funcs);
    return 1;
}

esp_err_t app_voice_register(void)
{
    /* Model + mic init once at boot. Failure degrades to
     * voice.available() == false, never an error -- absent hardware must
     * not kill an app (contract constraint). */
    s_models = esp_srmodel_init("model");
    if (s_models != NULL) {
        s_mn_name = esp_srmodel_filter(s_models, ESP_MN_PREFIX, ESP_MN_ENGLISH);
    }
    if (s_mn_name != NULL) {
        s_mn = esp_mn_handle_from_name(s_mn_name);
    }
    if (s_mn != NULL) {
        s_mic = bsp_audio_codec_microphone_init();
    }
    if (s_mic != NULL) {
        s_mn_data = s_mn->create(s_mn_name, VOICE_TIMEOUT_MS);
        if (s_mn_data != NULL) {
            esp_mn_commands_alloc((esp_mn_iface_t *)s_mn, s_mn_data);
            /* Default threshold accepts weak matches -- "kilo" landed on
             * "five" on device. Raise it: better a missed word (say it
             * again) than a wrong letter silently appended. */
            if (s_mn->set_det_threshold) {
                s_mn->set_det_threshold(s_mn_data, 0.6f);
            }
            s_available = true;
        }
    }
    ESP_LOGI(TAG, "voice %s (model=%s)",
             s_available ? "available" : "unavailable",
             s_mn_name ? s_mn_name : "none");

    return cap_lua_register_module("voice", luaopen_app_voice);
}
