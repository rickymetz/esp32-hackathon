#include <math.h>
#include <string.h>
#include <stdlib.h>
#include "app_audio.h"
#include "cap_lua.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "lauxlib.h"
#include "lua_module_lvgl.h"

#include "bsp/esp-bsp.h"
#include "esp_codec_dev.h"

static const char *TAG = "app_audio";

#define SAMPLE_RATE   16000
#define CHUNK_SAMPLES 256
#define QUEUE_LEN     16
#define MAX_MS        5000

typedef struct {
    int freq;      /* Hz; 0 = silence (a rest) */
    int ms;
} note_t;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static note_t s_queue[QUEUE_LEN];
static int s_head, s_count;
static volatile bool s_playing;
static volatile bool s_stop;
static volatile bool s_task_live;

static esp_codec_dev_handle_t s_spk;
static int s_volume = 70;

bool app_audio_is_playing(void)
{
    return s_playing;
}

/* Opened on FIRST USE, never at boot.
 *
 * This is a safety property, not laziness: anything that touches audio
 * hardware during app_main() can wedge the board before the launcher
 * exists, and recovering that needs a physical BOOT-button dance. Doing
 * it from a Lua call means the worst case is one dead app, which the
 * watchdog turns into a reboot back into a working launcher. */
static bool speaker_ready(void)
{
    if (s_spk != NULL) {
        return true;
    }
    s_spk = bsp_audio_codec_speaker_init();
    if (s_spk == NULL) {
        ESP_LOGW(TAG, "speaker unavailable");
        return false;
    }
    esp_codec_dev_set_out_vol(s_spk, s_volume);
    return true;
}

/* One note. A raised-cosine ramp on the first and last few milliseconds
 * keeps the speaker from clicking: a square-edged start is a step
 * function, and a step is audible as a pop regardless of the tone. */
static void play_note(const note_t *n, int16_t *buf)
{
    int total = (n->ms * SAMPLE_RATE) / 1000;
    int ramp = SAMPLE_RATE / 200;          /* 5 ms */
    if (ramp * 2 > total) ramp = total / 2;

    double phase = 0.0;
    double step = (n->freq > 0)
                      ? 2.0 * M_PI * (double)n->freq / (double)SAMPLE_RATE
                      : 0.0;

    for (int done = 0; done < total && !s_stop; ) {
        int chunk = total - done;
        if (chunk > CHUNK_SAMPLES) chunk = CHUNK_SAMPLES;

        for (int i = 0; i < chunk; i++) {
            int idx = done + i;
            double amp = 1.0;
            if (idx < ramp)              amp = (double)idx / ramp;
            else if (idx > total - ramp) amp = (double)(total - idx) / ramp;

            double v = (n->freq > 0) ? sin(phase) * amp * 12000.0 : 0.0;
            phase += step;
            buf[i] = (int16_t)v;
        }
        esp_codec_dev_write(s_spk, buf, chunk * sizeof(int16_t));
        done += chunk;
    }
}

static void audio_task(void *arg)
{
    (void)arg;
    int16_t *buf = malloc(CHUNK_SAMPLES * sizeof(int16_t));

    if (buf == NULL) {
        ESP_LOGE(TAG, "no memory for the mixing buffer");
        goto out;
    }

    esp_codec_dev_sample_info_t fs = {
        .sample_rate = SAMPLE_RATE,
        .channel = 1,
        .bits_per_sample = 16,
    };
    if (esp_codec_dev_open(s_spk, &fs) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "speaker open failed");
        goto out;
    }

    for (;;) {
        note_t n;
        portENTER_CRITICAL(&s_lock);
        bool have = (s_count > 0) && !s_stop;
        if (have) {
            n = s_queue[s_head];
            s_head = (s_head + 1) % QUEUE_LEN;
            s_count--;
        }
        portEXIT_CRITICAL(&s_lock);

        if (!have) {
            break;
        }
        play_note(&n, buf);
    }

    esp_codec_dev_close(s_spk);

out:
    if (buf) free(buf);
    portENTER_CRITICAL(&s_lock);
    s_count = 0;
    s_head = 0;
    s_stop = false;
    s_playing = false;
    s_task_live = false;
    portEXIT_CRITICAL(&s_lock);
    vTaskDelete(NULL);
}

static bool enqueue(int freq, int ms)
{
    bool started = false;

    portENTER_CRITICAL(&s_lock);
    if (s_count < QUEUE_LEN) {
        int slot = (s_head + s_count) % QUEUE_LEN;
        s_queue[slot].freq = freq;
        s_queue[slot].ms = ms;
        s_count++;
    }
    if (!s_task_live) {
        s_task_live = true;
        s_playing = true;
        started = true;
    }
    portEXIT_CRITICAL(&s_lock);

    if (started) {
        if (xTaskCreate(audio_task, "audio", 4096, NULL, 5, NULL) != pdPASS) {
            portENTER_CRITICAL(&s_lock);
            s_task_live = false;
            s_playing = false;
            s_count = 0;
            portEXIT_CRITICAL(&s_lock);
            return false;
        }
    }
    return true;
}

/* ---- Lua API ---- */

static int l_audio_tone(lua_State *L)
{
    int freq = (int)luaL_checkinteger(L, 1);
    int ms = (int)luaL_optinteger(L, 2, 120);

    luaL_argcheck(L, freq >= 0 && freq <= 8000, 1, "freq must be 0-8000 Hz");
    luaL_argcheck(L, ms > 0 && ms <= MAX_MS, 2, "ms must be 1-5000");

    if (!speaker_ready()) {
        lua_pushnil(L);
        lua_pushliteral(L, "speaker unavailable");
        return 2;
    }
    if (!enqueue(freq, ms)) {
        lua_pushnil(L);
        lua_pushliteral(L, "audio task failed");
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

/* audio.play{ {freq, ms}, {freq, ms}, ... } -- a melody or alarm
 * pattern. freq 0 is a rest. */
static int l_audio_play(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);

    if (!speaker_ready()) {
        lua_pushnil(L);
        lua_pushliteral(L, "speaker unavailable");
        return 2;
    }

    int n = (int)lua_rawlen(L, 1);
    if (n < 1 || n > QUEUE_LEN) {
        return luaL_error(L, "audio.play: 1-%d notes", QUEUE_LEN);
    }
    for (int i = 1; i <= n; i++) {
        lua_rawgeti(L, 1, i);
        if (!lua_istable(L, -1)) {
            return luaL_error(L, "audio.play: note %d must be {freq, ms}", i);
        }
        lua_rawgeti(L, -1, 1);
        lua_rawgeti(L, -2, 2);
        int freq = (int)lua_tointeger(L, -2);
        int ms = (int)lua_tointeger(L, -1);
        lua_pop(L, 3);

        if (freq < 0 || freq > 8000 || ms <= 0 || ms > MAX_MS) {
            return luaL_error(L, "audio.play: note %d out of range", i);
        }
        enqueue(freq, ms);
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int l_audio_beep(lua_State *L)
{
    lua_pushinteger(L, 880);
    lua_pushinteger(L, 60);
    lua_replace(L, 2);
    lua_replace(L, 1);
    return l_audio_tone(L);
}

static int l_audio_stop(lua_State *L)
{
    portENTER_CRITICAL(&s_lock);
    s_stop = true;
    s_count = 0;
    portEXIT_CRITICAL(&s_lock);
    lua_pushboolean(L, 1);
    return 1;
}

void app_audio_set_volume(int volume)
{
    if (volume < 0 || volume > 100) return;
    s_volume = volume;
    if (s_spk != NULL) {
        esp_codec_dev_set_out_vol(s_spk, s_volume);
    }
}

static int l_audio_volume(lua_State *L)
{
    if (!lua_isnoneornil(L, 1)) {
        int v = (int)luaL_checkinteger(L, 1);
        luaL_argcheck(L, v >= 0 && v <= 100, 1, "volume must be 0-100");
        s_volume = v;
        if (s_spk) {
            esp_codec_dev_set_out_vol(s_spk, v);
        }
    }
    lua_pushinteger(L, s_volume);
    return 1;
}

static int l_audio_available(lua_State *L)
{
    /* Reports whether the speaker CAN be opened, without opening it --
     * the BSP is present on every board this runs on. */
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

void app_audio_reset(lua_State *L)
{
    (void)L;
    portENTER_CRITICAL(&s_lock);
    s_stop = true;
    s_count = 0;
    portEXIT_CRITICAL(&s_lock);
}

esp_err_t app_audio_register(void)
{
    /* Registration touches NO hardware -- see speaker_ready(). */
    return cap_lua_register_module("audio", luaopen_audio);
}
