/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 *   - LVGL events fire on the LVGL task. The trampoline here MUST NOT call
 *     into the Lua state directly: the script lives on a separate cap_lua
 *     job task, and the Lua VM is single-threaded per state. Instead the
 *     trampoline only enqueues the matching `lua_lvgl_event_sub_t *` onto
 *     `s_lvgl.event_queue_head/tail`.
 *
 *   - The script periodically calls `lvgl.process_events([timeout_ms])` or
 *     `lvgl.run([opts])`. Both run on the script task. They pop subs off
 *     the queue (under lua_lvgl_lock()), then drop the lock and invoke
 *     `lua_pcall` so callbacks may freely call `lvgl.*` (which itself
 *     re-acquires the lock).
 *
 *   - All `luaL_unref` operations also need the script task. Anywhere we
 *     would unref but we might be on the LVGL task (for example inside
 *     `LV_EVENT_DELETE` triggered by `lv_obj_delete()` chained from a
 *     callback handler), the registry ref is appended to
 *     `s_lvgl.pending_unrefs` instead. The script task drains that list
 *     during process_events / run / deinit.
 *
 *   - `obj:off()` runs on the script task, so it can free non-queued subs
 *     immediately by calling luaL_unref directly. If the sub is currently
 *     queued, off() marks it `dead` and lets process_events do the free.
 */

#include "lua_lvgl_private.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "lua_lvgl_evt";

/* Direction of the gesture whose callback is currently being dispatched on
 * the script task; LV_DIR_NONE outside a gesture callback. Read only by
 * lua_lvgl_gesture_dir(), written only by the dispatch loop -- both on the
 * script task, so no lock is needed. */
static lv_dir_t s_dispatching_gesture_dir = LV_DIR_NONE;

typedef struct {
    const char *name;
    lv_event_code_t code;
} lua_lvgl_event_name_t;

static const lua_lvgl_event_name_t s_event_names[] = {
    {"clicked",       LV_EVENT_CLICKED},
    {"pressed",       LV_EVENT_PRESSED},
    {"released",      LV_EVENT_RELEASED},
    {"long_pressed",  LV_EVENT_LONG_PRESSED},
    {"value_changed", LV_EVENT_VALUE_CHANGED},
    {"focused",       LV_EVENT_FOCUSED},
    {"defocused",     LV_EVENT_DEFOCUSED},
    {"ready",         LV_EVENT_READY},
    {"cancel",        LV_EVENT_CANCEL},
    {"gesture",       LV_EVENT_GESTURE},
    {"long_pressed_repeat", LV_EVENT_LONG_PRESSED_REPEAT},
};

#define LUA_LVGL_EVENT_NAME_COUNT \
    (sizeof(s_event_names) / sizeof(s_event_names[0]))

static bool lua_lvgl_event_code_for_name(const char *name, lv_event_code_t *out)
{
    if (!name || !out) {
        return false;
    }
    for (size_t i = 0; i < LUA_LVGL_EVENT_NAME_COUNT; i++) {
        if (strcmp(name, s_event_names[i].name) == 0) {
            *out = s_event_names[i].code;
            return true;
        }
    }
    return false;
}

void lua_lvgl_queue_pending_unref_locked(int ref)
{
    lua_lvgl_pending_unref_t *node;

    if (ref == LUA_NOREF || ref == LUA_REFNIL) {
        return;
    }
    node = (lua_lvgl_pending_unref_t *)calloc(1, sizeof(*node));
    if (!node) {
        /* Best-effort: log and leak the registry slot rather than crash. */
        ESP_LOGE(TAG, "pending_unref alloc failed; leaking registry ref %d", ref);
        return;
    }
    node->ref = ref;
    node->next = s_lvgl.pending_unrefs;
    s_lvgl.pending_unrefs = node;
}

void lua_lvgl_drain_pending_unrefs_locked(lua_State *L)
{
    lua_lvgl_pending_unref_t *node = s_lvgl.pending_unrefs;

    s_lvgl.pending_unrefs = NULL;
    while (node) {
        lua_lvgl_pending_unref_t *next = node->next;

        if (L && node->ref != LUA_NOREF && node->ref != LUA_REFNIL) {
            luaL_unref(L, LUA_REGISTRYINDEX, node->ref);
        }
        free(node);
        node = next;
    }
}

static void lua_lvgl_enqueue_sub_locked(lua_lvgl_event_sub_t *sub)
{
    sub->queued = true;
    sub->queue_next = NULL;
    if (s_lvgl.event_queue_tail) {
        s_lvgl.event_queue_tail->queue_next = sub;
    } else {
        s_lvgl.event_queue_head = sub;
    }
    s_lvgl.event_queue_tail = sub;

    /* Wake the script task's drain loop. Runs on the LVGL task, which is the
     * whole point: without it the script task only discovers the event on its
     * next poll, up to 20 ms later.
     *
     * There is no lost wakeup, because a binary semaphore LATCHES -- a give
     * that lands before the corresponding take leaves it signalled and the
     * take returns immediately. Note that property does not depend on holding
     * the lock here; giving after lua_lvgl_unlock() would be equally safe. We
     * give here only because this is where the sub becomes visible. The one
     * real cost is that the woken task's next act is to contend for the mutex
     * we still hold, costing one extra context switch per event. */
    if (s_lvgl.event_signal) {
        xSemaphoreGive(s_lvgl.event_signal);
    }
}

static lua_lvgl_event_sub_t *lua_lvgl_dequeue_sub_locked(void)
{
    lua_lvgl_event_sub_t *head = s_lvgl.event_queue_head;

    if (!head) {
        return NULL;
    }
    s_lvgl.event_queue_head = head->queue_next;
    if (!s_lvgl.event_queue_head) {
        s_lvgl.event_queue_tail = NULL;
    }
    head->queue_next = NULL;
    head->queued = false;
    return head;
}

/* Note: We hold lua_lvgl_lock() because lv_timer_handler is invoked under
 * that lock by lua_lvgl_task. Coalesce repeated fires while a sub is still
 * pending so the script never gets a queue blowup; for typical UI events
 * (click/value_changed/focus) this matches user expectations of "act on
 * the latest event". */
static void lua_lvgl_event_trampoline(lv_event_t *e)
{
    lua_lvgl_event_sub_t *sub = (lua_lvgl_event_sub_t *)lv_event_get_user_data(e);

    if (!sub || sub->dead || sub->callback_ref == LUA_NOREF) {
        return;
    }
    /* Capture the gesture direction BEFORE the queued-coalesce check: a
     * second gesture arriving while the first is still pending must
     * overwrite the stored direction (latest-wins, as documented), and by
     * dispatch time lv_indev_get_gesture_dir() no longer reports it. */
    if (sub->code == LV_EVENT_GESTURE) {
        sub->gesture_dir = lv_indev_get_gesture_dir(lv_indev_active());
    }
    if (sub->queued) {
        return;
    }
    lua_lvgl_enqueue_sub_locked(sub);
}

/* Free a sub that is NOT in the dispatch queue. Caller holds the lock and
 * is responsible for having already removed it from record->events. */
static void lua_lvgl_destroy_unqueued_sub_locked(lua_lvgl_event_sub_t *sub)
{
    if (!sub) {
        return;
    }
    lua_lvgl_queue_pending_unref_locked(sub->callback_ref);
    sub->callback_ref = LUA_NOREF;
    free(sub);
}

/* Drop the sub from record->events (caller holds lock) and either free it
 * now (if not queued) or mark it dead (so process_events frees it on
 * dequeue). When the LVGL widget still exists, the caller should
 * lv_obj_remove_event_cb_with_user_data() before calling us; for the
 * record-release path the LVGL widget is being destroyed anyway. */
static void lua_lvgl_detach_sub_locked(lua_lvgl_obj_record_t *record,
                                       lua_lvgl_event_sub_t *sub)
{
    lua_lvgl_event_sub_t **link;

    if (!record || !sub) {
        return;
    }
    for (link = &record->events; *link != NULL; link = &(*link)->next) {
        if (*link == sub) {
            *link = sub->next;
            break;
        }
    }
    sub->next = NULL;
    sub->record = NULL;
    if (sub->queued) {
        sub->dead = true;
    } else {
        lua_lvgl_destroy_unqueued_sub_locked(sub);
    }
}

void lua_lvgl_record_release_events_locked(lua_lvgl_obj_record_t *record)
{
    lua_lvgl_event_sub_t *sub;

    if (!record) {
        return;
    }
    /* The widget that owned these subs is in the process of being deleted
     * (LV_EVENT_DELETE chain), so we deliberately do NOT call
     * lv_obj_remove_event_cb_*: LVGL is already cleaning up its own
     * callback list. */
    sub = record->events;
    record->events = NULL;
    while (sub) {
        lua_lvgl_event_sub_t *next = sub->next;

        sub->next = NULL;
        sub->record = NULL;
        if (sub->queued) {
            sub->dead = true;
        } else {
            lua_lvgl_destroy_unqueued_sub_locked(sub);
        }
        sub = next;
    }
}

int lua_lvgl_obj_on(lua_State *L)
{
    lua_lvgl_obj_ud_t *ud = lua_lvgl_check_ud(L, 1);
    const char *name = luaL_checkstring(L, 2);
    lv_event_code_t code = LV_EVENT_ALL;
    lua_lvgl_event_sub_t *sub;
    esp_err_t err;
    int callback_ref;
    lv_obj_t *obj;
    const char *obj_error = NULL;

    luaL_checktype(L, 3, LUA_TFUNCTION);
    if (!lua_lvgl_event_code_for_name(name, &code)) {
        return luaL_error(L, "lvgl unknown event name: %s", name);
    }

    /* Pin the callback in the registry first. We luaL_ref before taking
     * the lock so the brief Lua-stack manipulation never races with the
     * LVGL task taking the lock for tick handling. */
    lua_pushvalue(L, 3);
    callback_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    if (callback_ref == LUA_NOREF || callback_ref == LUA_REFNIL) {
        return luaL_error(L, "lvgl event ref allocation failed");
    }

    sub = (lua_lvgl_event_sub_t *)calloc(1, sizeof(*sub));
    if (!sub) {
        luaL_unref(L, LUA_REGISTRYINDEX, callback_ref);
        return luaL_error(L, "lvgl event sub allocation failed");
    }
    sub->callback_ref = callback_ref;
    sub->code = code;

    err = lua_lvgl_lock();
    if (err != ESP_OK) {
        luaL_unref(L, LUA_REGISTRYINDEX, callback_ref);
        free(sub);
        return lua_lvgl_error_esp(L, "lock", err);
    }
    obj = lua_lvgl_validate_ud_locked(ud, NULL, &obj_error);
    if (!obj) {
        lua_lvgl_unlock();
        luaL_unref(L, LUA_REGISTRYINDEX, callback_ref);
        free(sub);
        return luaL_error(L, "%s", obj_error);
    }

    /* Gestures only ever reach the screen: LV_OBJ_FLAG_GESTURE_BUBBLE is on
     * by default and LVGL walks delivery up to the topmost bubbling object,
     * so a handler on an ordinary widget would silently never fire. Refuse
     * at registration instead -- and note the scroll caveat, because it is
     * the other silent killer (indev_gesture() returns early whenever the
     * touch is scrolling something). */
    if (code == LV_EVENT_GESTURE && ud->record->type != LUA_LVGL_OBJ_SCREEN) {
        lua_lvgl_unlock();
        luaL_unref(L, LUA_REGISTRYINDEX, callback_ref);
        free(sub);
        return luaL_error(L, "lvgl 'gesture' events fire only on screens -- "
                             "register on the screen object (and note: a touch "
                             "that scrolls content never produces a gesture)");
    }

    sub->record = ud->record;
    sub->next = ud->record->events;
    ud->record->events = sub;
    lv_obj_add_event_cb(obj, lua_lvgl_event_trampoline, code, sub);

    lua_lvgl_unlock();

    /* Return the sub as a light userdata handle. Lua keeps a stable
     * pointer; subsequent obj:off(handle) validates it by walking
     * record->events so freed pointers are rejected. */
    lua_pushlightuserdata(L, sub);
    return 1;
}

/* Detach + destroy every sub matching the predicate. Returns the count. */
typedef bool (*lua_lvgl_sub_match_fn)(const lua_lvgl_event_sub_t *sub, const void *ctx);

static int lua_lvgl_off_matching_locked(lua_lvgl_obj_record_t *record,
                                        lv_obj_t *obj,
                                        lua_lvgl_sub_match_fn match,
                                        const void *ctx)
{
    lua_lvgl_event_sub_t *cur;
    lua_lvgl_event_sub_t *next;
    int removed = 0;

    if (!record) {
        return 0;
    }
    for (cur = record->events; cur != NULL; cur = next) {
        next = cur->next;
        if (match && !match(cur, ctx)) {
            continue;
        }
        if (obj && lv_obj_is_valid(obj)) {
            lv_obj_remove_event_cb_with_user_data(obj, lua_lvgl_event_trampoline, cur);
        }
        lua_lvgl_detach_sub_locked(record, cur);
        removed++;
    }
    return removed;
}

static bool lua_lvgl_match_handle(const lua_lvgl_event_sub_t *sub, const void *ctx)
{
    return sub == (const lua_lvgl_event_sub_t *)ctx;
}

static bool lua_lvgl_match_code(const lua_lvgl_event_sub_t *sub, const void *ctx)
{
    return sub->code == *(const lv_event_code_t *)ctx;
}

static bool lua_lvgl_match_all(const lua_lvgl_event_sub_t *sub, const void *ctx)
{
    (void)sub;
    (void)ctx;
    return true;
}

int lua_lvgl_obj_off(lua_State *L)
{
    lua_lvgl_obj_ud_t *ud = lua_lvgl_check_ud(L, 1);
    int top = lua_gettop(L);
    esp_err_t err;
    lv_obj_t *obj;
    const char *obj_error = NULL;
    int removed = 0;

    err = lua_lvgl_lock();
    if (err != ESP_OK) {
        return lua_lvgl_error_esp(L, "lock", err);
    }
    /* Tolerate already-deleted widgets: off() on a stale handle simply
     * removes whatever is still tracked on the record without touching
     * LVGL. validate_ud is only used to fetch the live obj pointer for
     * lv_obj_remove_event_cb_with_user_data; if the obj is gone we still
     * walk record->events to free remaining subs. */
    obj = lua_lvgl_validate_ud_locked(ud, NULL, &obj_error);
    (void)obj_error;

    if (top < 2 || lua_isnoneornil(L, 2)) {
        removed = lua_lvgl_off_matching_locked(ud->record, obj, lua_lvgl_match_all, NULL);
    } else if (lua_islightuserdata(L, 2)) {
        const lua_lvgl_event_sub_t *handle =
            (const lua_lvgl_event_sub_t *)lua_touserdata(L, 2);

        removed = lua_lvgl_off_matching_locked(ud->record, obj, lua_lvgl_match_handle, handle);
    } else if (lua_type(L, 2) == LUA_TSTRING) {
        const char *name = lua_tostring(L, 2);
        lv_event_code_t code = LV_EVENT_ALL;

        if (!lua_lvgl_event_code_for_name(name, &code)) {
            lua_lvgl_unlock();
            return luaL_error(L, "lvgl unknown event name: %s", name);
        }
        removed = lua_lvgl_off_matching_locked(ud->record, obj, lua_lvgl_match_code, &code);
    } else {
        lua_lvgl_unlock();
        return luaL_argerror(L, 2, "expected handle (light userdata), event name (string), or nothing");
    }

    /* obj:off runs on the script task, so we can drain immediately. */
    lua_lvgl_drain_pending_unrefs_locked(L);
    lua_lvgl_unlock();

    lua_pushinteger(L, removed);
    return 1;
}

/* Drain one queued event under the lock; returns the popped sub or NULL.
 * Also drains pending unrefs while the lock is held. */
static lua_lvgl_event_sub_t *lua_lvgl_pop_event_with_drain(lua_State *L)
{
    lua_lvgl_event_sub_t *sub;
    esp_err_t err = lua_lvgl_lock();

    if (err != ESP_OK) {
        return NULL;
    }
    sub = lua_lvgl_dequeue_sub_locked();
    lua_lvgl_drain_pending_unrefs_locked(L);
    lua_lvgl_unlock();
    return sub;
}

static void lua_lvgl_destroy_dead_sub(lua_State *L, lua_lvgl_event_sub_t *sub)
{
    esp_err_t err = lua_lvgl_lock();

    if (err == ESP_OK) {
        lua_lvgl_destroy_unqueued_sub_locked(sub);
        lua_lvgl_drain_pending_unrefs_locked(L);
        lua_lvgl_unlock();
    } else {
        /* Lock failed (very unlikely). Best-effort direct unref; we are on
         * the script task so it is safe even without our mutex. */
        if (sub->callback_ref != LUA_NOREF) {
            luaL_unref(L, LUA_REGISTRYINDEX, sub->callback_ref);
        }
        free(sub);
    }
}

/* Drain events for up to timeout_ms (0 = single non-blocking pass) or until
 * cap_lua signals stop. Returns the number of callbacks invoked. */
static int lua_lvgl_drain_events_for(lua_State *L, int timeout_ms)
{
    int processed = 0;
    int64_t deadline_us = (timeout_ms > 0)
                              ? esp_timer_get_time() + (int64_t)timeout_ms * 1000
                              : 0;

    for (;;) {
        lua_lvgl_event_sub_t *sub;
        int callback_ref;

#ifdef __EMSCRIPTEN__
        if (lua_lvgl_lock() == ESP_OK) {
            if (s_lvgl.runtime_initialized) {
                (void)lv_timer_handler();
            }
            lua_lvgl_unlock();
        }
#endif

        if (cap_lua_runtime_stop_requested(L)) {
            break;
        }
        if (deadline_us && esp_timer_get_time() >= deadline_us) {
            break;
        }

        sub = lua_lvgl_pop_event_with_drain(L);
        if (!sub) {
            if (timeout_ms <= 0) {
                /* Non-blocking single pass: caller asked for "drain what's
                 * there now". */
                break;
            }
#ifdef __EMSCRIPTEN__
            /* The browser RAF cadence needs headroom beyond a 20 ms sleep;
             * otherwise any render work drops the simulator below 50 fps. */
            vTaskDelay(pdMS_TO_TICKS(10));
#else
            /* Wait for the LVGL task to signal an event, or for this nap to
             * expire -- whichever comes first.
             *
             * This used to be a flat vTaskDelay(20), which made 20 ms the
             * floor on how long a tap could sit in the queue before its Lua
             * callback ran, however idle the board was. Blocking on the
             * semaphore instead means an event wakes this loop as soon as it
             * is enqueued. Measured on hardware, enqueue -> dispatch over 118
             * taps:
             *
             *     poll    median 24.69 ms   mean 23.37   p90 26.61
             *     signal  median  0.76 ms   mean  1.27   p90  1.38
             *
             * Measure this interval directly if you touch it -- a serial
             * round-trip benchmark cannot see it. The 24 ms here is buried in
             * a ~48 ms TAP-to-print round trip whose other terms (serial, and
             * where the tap lands relative to the indev read) swing by tens of
             * ms, and a fixed inter-tap cadence phase-locks against the LVGL
             * task period and will happily report a 3x difference between two
             * builds of identical firmware. Stamp esp_timer_get_time() at
             * enqueue and subtract it here.
             *
             * The 20 ms cap stays, and is now purely a stop-responsiveness
             * bound: cap_lua_runtime_stop_requested() is an atomic flag set by
             * another task with nothing to signal us, so BOOT/STOP is still
             * noticed within one nap. Do not raise it to the full remaining
             * budget -- that would make BOOT take up to EVENT_PUMP_MS to
             * register on an idle app.
             *
             * Clamped to the caller's deadline as well: the launcher's pump
             * asks for exactly the time until the next timer is due
             * (launcher_main.c, wait_ms), and sleeping past that is wrong on
             * its own terms. Note this clamp did NOT change periodic-timer
             * drift -- it was tried as a fix for that and moved the number by
             * 0.0 ms. That drift was structural, in how app_timer.c re-armed,
             * and is fixed there. */
            int64_t remain_us = deadline_us - esp_timer_get_time();
            int nap_ms = 20;
            if (remain_us < (int64_t)nap_ms * 1000) {
                nap_ms = (int)(remain_us / 1000);
            }
            /* Always yield at least one tick: a sub-millisecond remainder
             * would otherwise spin against the deadline check above. */
            TickType_t nap = nap_ms > 0 ? pdMS_TO_TICKS(nap_ms) : 1;
            if (s_lvgl.event_signal) {
                xSemaphoreTake(s_lvgl.event_signal, nap);
            } else {
                vTaskDelay(nap);
            }
#endif
            continue;
        }
        if (sub->dead) {
            lua_lvgl_destroy_dead_sub(L, sub);
            continue;
        }

        callback_ref = sub->callback_ref;
        /* sub may be detached by user-installed callback below (e.g.
         * obj:off in the handler). That path either frees sub or marks it
         * dead; either way we are done with it for this fire. Read the
         * gesture direction now for the same reason -- after the pcall the
         * sub may be gone. */
        s_dispatching_gesture_dir = (sub->code == LV_EVENT_GESTURE)
                                        ? sub->gesture_dir
                                        : LV_DIR_NONE;

        lua_rawgeti(L, LUA_REGISTRYINDEX, callback_ref);
        if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
            const char *msg = lua_tostring(L, -1);

            ESP_LOGE(TAG, "lvgl event callback error: %s", msg ? msg : "(nil)");
            lua_pop(L, 1);
            /* The error may have unwound out of an LVGL binding holding the
             * display lock. Without this the LVGL task blocks forever. */
            lua_lvgl_force_unlock_if_held();
        }
        s_dispatching_gesture_dir = LV_DIR_NONE;
        processed++;
    }
    return processed;
}

static int lua_lvgl_process_events(lua_State *L)
{
    int timeout_ms = lua_isnoneornil(L, 1) ? 0 : (int)luaL_checkinteger(L, 1);
    int count;

    if (!s_lvgl.runtime_initialized) {
        return luaL_error(L, "lvgl runtime is not initialized");
    }
    if (timeout_ms < 0) {
        timeout_ms = 0;
    }
    count = lua_lvgl_drain_events_for(L, timeout_ms);
    lua_pushinteger(L, count);
    return 1;
}

static int lua_lvgl_run(lua_State *L)
{
    int period_ms = 200;
    int total = 0;

    if (!s_lvgl.runtime_initialized) {
        return luaL_error(L, "lvgl runtime is not initialized");
    }
    if (lua_lvgl_opt_table(L, 1)) {
        period_ms = lua_lvgl_get_opt_int_field(L, 1, "period_ms", period_ms);
        if (period_ms <= 0) {
            period_ms = 1;
        }
    }
    while (!cap_lua_runtime_stop_requested(L)) {
        total += lua_lvgl_drain_events_for(L, period_ms);
    }
    lua_pushinteger(L, total);
    return 1;
}

/* lvgl.gesture_dir() -> "left"|"right"|"top"|"bottom" inside a gesture
 * callback, nil anywhere else. Mirrors the direction vocabulary accepted by
 * lua_lvgl_parse_dir() so the two sides of the API read identically. */
static int lua_lvgl_gesture_dir(lua_State *L)
{
    switch (s_dispatching_gesture_dir) {
    case LV_DIR_LEFT:   lua_pushliteral(L, "left");   break;
    case LV_DIR_RIGHT:  lua_pushliteral(L, "right");  break;
    case LV_DIR_TOP:    lua_pushliteral(L, "top");    break;
    case LV_DIR_BOTTOM: lua_pushliteral(L, "bottom"); break;
    default:            lua_pushnil(L);               break;
    }
    return 1;
}

const luaL_Reg lua_lvgl_event_module_funcs[] = {
    {"process_events", lua_lvgl_process_events},
    {"run", lua_lvgl_run},
    {"gesture_dir", lua_lvgl_gesture_dir},
    {NULL, NULL},
};
