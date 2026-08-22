#include <stdbool.h>
#include "app_timer.h"
#include "cap_lua.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lauxlib.h"
#include "lua_module_lvgl.h"

static const char *TAG = "app_timer";
static const char *TIMER_MT = "launcher.timer";

typedef struct {
    int64_t  next_us;
    int64_t  period_us;   /* 0 == one-shot */
    int      ref;         /* LUA_NOREF when the slot is free */
    uint32_t gen;         /* bumped each time this slot is (re)allocated */
} app_timer_t;

/* Userdata stored in the Lua handle: which slot, and which allocation of
 * that slot it refers to. A slot freed by a one-shot firing or a cancel can
 * be reused by a later timer.every()/timer.after(); comparing `gen` is how
 * we tell a stale handle (from the earlier occupant) apart from the current
 * one, so cancelling a stale handle can't kill someone else's timer. */
typedef struct {
    int      slot;
    uint32_t gen;
} timer_handle_t;

static app_timer_t s_timers[APP_TIMER_MAX];
static bool s_inited;

static void ensure_init(void)
{
    if (s_inited) {
        return;
    }
    for (int i = 0; i < APP_TIMER_MAX; i++) {
        s_timers[i].ref = LUA_NOREF;
    }
    s_inited = true;
}

void app_timer_reset(lua_State *L)
{
    ensure_init();
    for (int i = 0; i < APP_TIMER_MAX; i++) {
        if (s_timers[i].ref != LUA_NOREF) {
            luaL_unref(L, LUA_REGISTRYINDEX, s_timers[i].ref);
            s_timers[i].ref = LUA_NOREF;
        }
    }
}

static int timer_add(lua_State *L, bool repeating)
{
    lua_Integer ms = luaL_checkinteger(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    if (ms < 1) {
        ms = 1;
    }
    ensure_init();

    int slot = -1;
    for (int i = 0; i < APP_TIMER_MAX; i++) {
        if (s_timers[i].ref == LUA_NOREF) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        return luaL_error(L, "too many timers (max %d)", APP_TIMER_MAX);
    }

    lua_pushvalue(L, 2);
    s_timers[slot].ref       = luaL_ref(L, LUA_REGISTRYINDEX);
    s_timers[slot].period_us = repeating ? (int64_t)ms * 1000 : 0;
    s_timers[slot].next_us   = esp_timer_get_time() + (int64_t)ms * 1000;
    s_timers[slot].gen++;

    timer_handle_t *handle = (timer_handle_t *)lua_newuserdatauv(L, sizeof(timer_handle_t), 0);
    handle->slot = slot;
    handle->gen  = s_timers[slot].gen;
    luaL_setmetatable(L, TIMER_MT);
    return 1;
}

static int l_timer_every(lua_State *L) { return timer_add(L, true); }
static int l_timer_after(lua_State *L) { return timer_add(L, false); }

static int l_timer_cancel(lua_State *L)
{
    timer_handle_t *handle = (timer_handle_t *)luaL_checkudata(L, 1, TIMER_MT);
    int slot = handle->slot;
    if (slot >= 0 && slot < APP_TIMER_MAX &&
        s_timers[slot].ref != LUA_NOREF && s_timers[slot].gen == handle->gen) {
        luaL_unref(L, LUA_REGISTRYINDEX, s_timers[slot].ref);
        s_timers[slot].ref = LUA_NOREF;
    }
    /* Stale handle (slot reused by a later timer, or already cancelled):
     * no-op. The timer this handle meant is already gone, which isn't an
     * error, and we must not touch whatever now occupies the slot. */
    handle->slot = -1;
    lua_pushboolean(L, 1);
    return 1;
}

int64_t app_timer_run_due(lua_State *L)
{
    ensure_init();
    int64_t now     = esp_timer_get_time();
    int64_t soonest = INT64_MAX;

    for (int i = 0; i < APP_TIMER_MAX; i++) {
        if (s_timers[i].ref == LUA_NOREF) {
            continue;
        }
        if (s_timers[i].next_us <= now) {
            uint32_t gen_before = s_timers[i].gen;

            lua_rawgeti(L, LUA_REGISTRYINDEX, s_timers[i].ref);
            if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
                ESP_LOGE(TAG, "timer callback error: %s", lua_tostring(L, -1));
                lua_pop(L, 1);
                /* The error may have unwound out of an LVGL binding holding
                 * the display lock. */
                lua_lvgl_force_unlock_if_held();
            }
            /* The callback may have cancelled this timer -- or cancelled it
             * and created a new one that reused slot i (timer_add() picks
             * the first free slot by linear scan). gen changing means slot
             * i is now someone else's timer entirely; touching next_us or
             * unref'ing it below would corrupt/kill that new timer instead
             * of just skipping our own bookkeeping for the one that fired. */
            if (s_timers[i].ref == LUA_NOREF || s_timers[i].gen != gen_before) {
                continue;
            }
            if (s_timers[i].period_us > 0) {
                s_timers[i].next_us = esp_timer_get_time() + s_timers[i].period_us;
            } else {
                luaL_unref(L, LUA_REGISTRYINDEX, s_timers[i].ref);
                s_timers[i].ref = LUA_NOREF;
                continue;
            }
        }
        if (s_timers[i].next_us < soonest) {
            soonest = s_timers[i].next_us;
        }
    }
    return soonest;
}

/* Monotonic milliseconds since boot. Exists because measuring elapsed
 * time by counting timer ticks drifts: periodic timers re-arm from
 * dispatch time, so every cycle stretches by pump latency (the flagship
 * stopwatch shipped with exactly that bug). */
static int l_timer_now_ms(lua_State *L)
{
    lua_pushinteger(L, (lua_Integer)(esp_timer_get_time() / 1000));
    return 1;
}

static const luaL_Reg timer_funcs[] = {
    {"every", l_timer_every},
    {"after", l_timer_after},
    {"now_ms", l_timer_now_ms},
    {NULL, NULL},
};

static int luaopen_app_timer(lua_State *L)
{
    ensure_init();

    luaL_newmetatable(L, TIMER_MT);
    lua_newtable(L);
    lua_pushcfunction(L, l_timer_cancel);
    lua_setfield(L, -2, "cancel");
    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);

    luaL_newlib(L, timer_funcs);
    return 1;
}

esp_err_t app_timer_register(void)
{
    return cap_lua_register_module("timer", luaopen_app_timer);
}
