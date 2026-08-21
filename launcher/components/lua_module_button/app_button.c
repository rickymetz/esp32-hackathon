#include <stdbool.h>
#include <string.h>
#include "app_button.h"
#include "cap_lua.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "lauxlib.h"
#include "lua_module_lvgl.h"

static const char *TAG = "app_button";
static const char *BUTTON_MT = "launcher.button";

#define APP_BUTTON_MAX_SUBS   8
#define APP_BUTTON_RING_SIZE  8
#define APP_BUTTON_LONG_MS    2000

typedef enum {
    APP_BUTTON_EV_PRESSED,
    APP_BUTTON_EV_RELEASED,
    APP_BUTTON_EV_LONG_PRESSED,
    APP_BUTTON_EV_COUNT,
} app_button_event_t;

/* One subscription created by button.on("pwr", event, fn). Slot + generation
 * mirrors app_timer's handle pattern: a slot freed by button.off() can be
 * reused, and comparing `gen` is how a stale handle from the earlier
 * occupant is told apart from the current one. */
typedef struct {
    int                ref;   /* LUA_NOREF when the slot is free */
    app_button_event_t event;
    uint32_t           gen;
} app_button_sub_t;

typedef struct {
    int      slot;
    uint32_t gen;
} button_handle_t;

/* ---- State shared with the poller task, under s_edge_lock only ---- */

typedef struct {
    bool    pressed;
    int64_t at_us;
} app_button_edge_t;

static portMUX_TYPE s_edge_lock = portMUX_INITIALIZER_UNLOCKED;
static app_button_edge_t s_ring[APP_BUTTON_RING_SIZE];
static int  s_ring_head;              /* next write */
static int  s_ring_count;             /* entries pending (ring overwrites oldest) */
static bool s_down;                   /* current debounced level */
static int64_t s_press_at_us;         /* timestamp of the current press, 0 = none */

/* ---- App-task-only state ---- */

static app_button_sub_t s_subs[APP_BUTTON_MAX_SUBS];
static bool s_inited;
static bool s_long_fired;             /* long_pressed sent for the current press */

static void ensure_init(void)
{
    if (s_inited) {
        return;
    }
    for (int i = 0; i < APP_BUTTON_MAX_SUBS; i++) {
        s_subs[i].ref = LUA_NOREF;
    }
    s_inited = true;
}

void app_button_record_edge(bool pressed)
{
    int64_t now = esp_timer_get_time();

    portENTER_CRITICAL(&s_edge_lock);
    s_ring[s_ring_head].pressed = pressed;
    s_ring[s_ring_head].at_us = now;
    s_ring_head = (s_ring_head + 1) % APP_BUTTON_RING_SIZE;
    if (s_ring_count < APP_BUTTON_RING_SIZE) {
        s_ring_count++;
    }
    s_down = pressed;
    s_press_at_us = pressed ? now : 0;
    portEXIT_CRITICAL(&s_edge_lock);
}

/* Invoke every subscription for `event`. Runs on the app task with no locks
 * held: callbacks re-enter lvgl.* (which takes the display lock), and the
 * lock order puts module locks innermost -- holding s_edge_lock across a
 * callback would invert it. Errors are logged and dispatch continues,
 * matching the LVGL event contract. */
static void dispatch_event(lua_State *L, app_button_event_t event)
{
    for (int i = 0; i < APP_BUTTON_MAX_SUBS; i++) {
        if (s_subs[i].ref == LUA_NOREF || s_subs[i].event != event) {
            continue;
        }
        lua_rawgeti(L, LUA_REGISTRYINDEX, s_subs[i].ref);
        if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
            ESP_LOGE(TAG, "button callback error: %s", lua_tostring(L, -1));
            lua_pop(L, 1);
            /* The error may have unwound out of an LVGL binding holding
             * the display lock. */
            lua_lvgl_force_unlock_if_held();
        }
    }
}

void app_button_run_pending(lua_State *L)
{
    app_button_edge_t edges[APP_BUTTON_RING_SIZE];
    int count;
    bool fire_long = false;

    ensure_init();

    /* Copy out under the spinlock, dispatch after releasing it. */
    portENTER_CRITICAL(&s_edge_lock);
    count = s_ring_count;
    if (count > 0) {
        int start = (s_ring_head - count + APP_BUTTON_RING_SIZE) % APP_BUTTON_RING_SIZE;
        for (int i = 0; i < count; i++) {
            edges[i] = s_ring[(start + i) % APP_BUTTON_RING_SIZE];
        }
        s_ring_count = 0;
    }
    /* long_pressed fires once per press, from the hold duration rather than
     * an edge, so a press with no release yet still triggers it. */
    if (!s_long_fired && s_down && s_press_at_us != 0 &&
        esp_timer_get_time() - s_press_at_us >= (int64_t)APP_BUTTON_LONG_MS * 1000) {
        fire_long = true;
    }
    portEXIT_CRITICAL(&s_edge_lock);

    for (int i = 0; i < count; i++) {
        if (edges[i].pressed) {
            s_long_fired = false;
            dispatch_event(L, APP_BUTTON_EV_PRESSED);
        } else {
            dispatch_event(L, APP_BUTTON_EV_RELEASED);
        }
    }
    if (fire_long) {
        s_long_fired = true;
        dispatch_event(L, APP_BUTTON_EV_LONG_PRESSED);
    }
}

void app_button_reset(lua_State *L)
{
    ensure_init();
    for (int i = 0; i < APP_BUTTON_MAX_SUBS; i++) {
        if (s_subs[i].ref != LUA_NOREF) {
            luaL_unref(L, LUA_REGISTRYINDEX, s_subs[i].ref);
            s_subs[i].ref = LUA_NOREF;
        }
    }
    s_long_fired = false;
    portENTER_CRITICAL(&s_edge_lock);
    s_ring_count = 0;
    s_ring_head = 0;
    /* Deliberately do NOT clear s_down/s_press_at_us: they mirror the
     * physical level, which persists across app boundaries. Clearing the
     * timestamp is what prevents a stale long_pressed: if PWR is somehow
     * held across an app launch, re-stamp so the 2 s window starts now. */
    if (s_down) {
        s_press_at_us = esp_timer_get_time();
    }
    portEXIT_CRITICAL(&s_edge_lock);
}

/* ---- Lua API ---- */

static app_button_event_t check_event_name(lua_State *L, int index)
{
    const char *name = luaL_checkstring(L, index);

    if (strcmp(name, "pressed") == 0)      return APP_BUTTON_EV_PRESSED;
    if (strcmp(name, "released") == 0)     return APP_BUTTON_EV_RELEASED;
    if (strcmp(name, "long_pressed") == 0) return APP_BUTTON_EV_LONG_PRESSED;
    luaL_error(L, "button: unknown event '%s' (pressed, released, long_pressed)", name);
    return APP_BUTTON_EV_COUNT; /* unreachable */
}

static void check_button_name(lua_State *L, int index)
{
    const char *name = luaL_checkstring(L, index);

    if (strcmp(name, "boot") == 0) {
        luaL_error(L, "button: BOOT is the Home button -- it always returns "
                      "to the launcher and apps cannot see it. The app button "
                      "is \"pwr\" (bottom right)");
    }
    if (strcmp(name, "pwr") != 0) {
        luaL_error(L, "button: unknown button '%s' (only \"pwr\")", name);
    }
}

static int l_button_on(lua_State *L)
{
    check_button_name(L, 1);
    app_button_event_t event = check_event_name(L, 2);
    luaL_checktype(L, 3, LUA_TFUNCTION);
    ensure_init();

    int slot = -1;
    for (int i = 0; i < APP_BUTTON_MAX_SUBS; i++) {
        if (s_subs[i].ref == LUA_NOREF) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        return luaL_error(L, "too many button subscriptions (max %d)", APP_BUTTON_MAX_SUBS);
    }

    lua_pushvalue(L, 3);
    s_subs[slot].ref = luaL_ref(L, LUA_REGISTRYINDEX);
    s_subs[slot].event = event;
    s_subs[slot].gen++;

    button_handle_t *handle = (button_handle_t *)lua_newuserdatauv(L, sizeof(button_handle_t), 0);
    handle->slot = slot;
    handle->gen = s_subs[slot].gen;
    luaL_setmetatable(L, BUTTON_MT);
    return 1;
}

static int l_button_off(lua_State *L)
{
    button_handle_t *handle = (button_handle_t *)luaL_checkudata(L, 1, BUTTON_MT);
    int slot = handle->slot;

    if (slot >= 0 && slot < APP_BUTTON_MAX_SUBS &&
        s_subs[slot].ref != LUA_NOREF && s_subs[slot].gen == handle->gen) {
        luaL_unref(L, LUA_REGISTRYINDEX, s_subs[slot].ref);
        s_subs[slot].ref = LUA_NOREF;
    }
    /* Stale handle (slot reused, or already off): no-op, same as app_timer. */
    handle->slot = -1;
    lua_pushboolean(L, 1);
    return 1;
}

static int l_button_is_down(lua_State *L)
{
    bool down;

    check_button_name(L, 1);
    portENTER_CRITICAL(&s_edge_lock);
    down = s_down;
    portEXIT_CRITICAL(&s_edge_lock);
    lua_pushboolean(L, down);
    return 1;
}

static const luaL_Reg button_funcs[] = {
    {"on", l_button_on},
    {"off", l_button_off},
    {"is_down", l_button_is_down},
    {NULL, NULL},
};

static int luaopen_app_button(lua_State *L)
{
    ensure_init();

    luaL_newmetatable(L, BUTTON_MT);
    lua_newtable(L);
    lua_pushcfunction(L, l_button_off);
    lua_setfield(L, -2, "off");
    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);

    luaL_newlib(L, button_funcs);
    return 1;
}

esp_err_t app_button_register(void)
{
    return cap_lua_register_module("button", luaopen_app_button);
}
