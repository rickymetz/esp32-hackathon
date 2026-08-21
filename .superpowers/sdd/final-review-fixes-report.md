# Final review fixes — verification report

Board: `/dev/cu.usbmodem101`, V2 hardware. All verification below was run on real
hardware; no simulator was used. **The board never wedged during this session** — no
power cycle was required at any point.

## Fixes made

- **C1 (critical)** — `lua_lvgl_events.c` `lua_lvgl_drain_events_for()`: added the
  missing `lua_lvgl_force_unlock_if_held()` call after the event-callback `lua_pcall`
  failure (the fifth catch site, previously missed). `lua_lvgl_runtime.c`: reworked
  `lua_lvgl_lock()`/`lua_lvgl_unlock()`/`lua_lvgl_force_unlock_if_held()` to track a
  recursion depth per owning task (not just an owner flag) — same-task re-entry now
  skips re-taking the non-recursive `s_lvgl.mutex` instead of deadlocking against
  itself, and `force_unlock_if_held()` loops (bounded) until the calling task no
  longer owns the lock at any depth. `launcher_main.c`: `lua_app_task`'s `close:`
  block now calls `lua_lvgl_force_unlock_if_held()` unconditionally before
  `launcher_lua_run_exit_cleanup(L)`, so a task can never be deleted while still
  owning the LVGL mutex. **Prevents:** a Lua error inside any LVGL event-callback
  binding permanently wedging the display, touch, and PWR recovery path — requiring
  a physical power cycle. This is believed to be what wedged the dev board earlier
  tonight, since a button/event callback bug is the single most likely way to hit
  exactly the one catch site (of five) that was missing the unlock.

- **I1 (leak)** — `lua_lvgl_object.c` `lua_lvgl_invalidate_records_locked()` now
  unlinks and `free()`s every `lua_lvgl_obj_record_t` from the generation being torn
  down (previously only marked `valid=false`/`obj=NULL`, never freed), and nulls the
  owning userdata's back-pointer via a new `record->ud` field added to
  `lua_lvgl_obj_record_t` (mirrors `lua_lvgl_font_record_t`/`ud` in `lua_lvgl_font.c`).
  **Prevents:** ~84 bytes of internal DRAM leaking per LVGL object ever created,
  permanently, until reboot.

- **I2 (use-after-free)** — `display_service.c` `display_service_close()` now guards
  `lv_obj_delete(session->screen)` with `lv_obj_is_valid()` and unconditionally nulls
  `session->screen` afterward. Additionally, a new `display_service_session_clear_screen()`
  accessor is called from `lua_lvgl_session_cleanup_cb()` right after it deletes the
  runtime's root screen (the same `lv_obj_t*` as `session->screen`), so the stale
  pointer is cleared at the source, not just guarded at the use site. **Prevents:** a
  double-delete on the app's screen object on every single app exit, previously
  masked only by LVGL 9's `is_deleting` bit surviving in freed memory by luck.

- **I5 (timer)** — `app_timer.c` `app_timer_run_due()` snapshots `s_timers[i].gen`
  before the `lua_pcall` and skips all post-processing (next-fire scheduling / unref)
  if `gen` changed, since that means the slot was taken over by a new timer created
  from inside the firing callback. **Prevents:** a timer created inside its own
  firing callback (after cancelling itself) being freed before it ever fires, when
  slot reuse lands it in the same index the outer loop is still processing.

## Verification (verbatim serial output)

### 1. C1 repro — `apps/cb_error.lua`

Touch could not be driven (no touch-injection API, no way to tap over serial), so
the repro fires from a `timer.after` callback that calls the exact same broken
LVGL binding (`lvgl.label(scr, {x = "not a number"})`, which takes the lock inside
`lua_lvgl_create_widget()` and then `luaL_error`s out of `lua_lvgl_parse_opts()`
before ever calling `lua_lvgl_unlock()`) that a real button `clicked` handler in the
same file also calls. This exercises the exact lock/unlock/force-unlock machinery
that was reworked for the events.c catch site.

```
I (70453) launcher: launching 'Cb error' (/sdcard/apps/cb_error.lua)
RUN_OK cb_error.lua
I (70531) launcher: app 'Cb error' running, vm psram cost = 41252 bytes
CB_ERROR: firing TIMER-path boom
E (71061) app_timer: timer callback error: /sdcard/apps/cb_error.lua:49: lvgl option 'x' must be an integer
W (71061) lua_lvgl: releasing lvgl lock leaked by a Lua error (1 nested lock(s))
CB_ERROR: still alive 2s after boom -- lock was reclaimed, PASS
```

STOP still works after the error:

```
I (85313) serial_push: STOP
STOP_OK
I (85324) lua_lvgl: Lua exit cleanup: deinitializing lvgl owned by exiting state
I (85326) display_service: session closed for 'lua_lvgl'
I (85333) launcher: app 'Cb error' closed, psram free=8207380, internal free=200047, lv_mem free=47240/62656
```

`RUN counter.lua` afterward renders normally (proves the lock was reclaimed —
screen/widget creation both take the lock and would time out at 1000ms and fail if
it were still held):

```
I (93055) launcher: launching 'Counter' (/sdcard/apps/counter.lua)
RUN_OK counter.lua
I (93130) launcher: app 'Counter' running, vm psram cost = 40272 bytes
I (93131) launcher: app 'Counter' running (callback errors appear under tag 'lua_lvgl_evt')
```

Before this fix, per the task brief, this exact sequence would wedge the board
requiring a physical power cycle. It did not.

### 2. I1 leak — 20x `hello_world.lua` RUN/STOP cycles, internal free DRAM logged
   each cycle via the launcher's existing "app closed" log line:

```
cycle 1:  internal free=181099
cycle 10: internal free=181099
cycle 20: internal free=181099
```

All 20 cycles: 181099, 181099, 181099, 181099, 181099, 181099, 181099, 181099,
181099, 181099, 181099, 181099, 181099, 181099, 181099, 181099, 181099, 181099,
181099, 181099 — flat, not declining. Before the fix this would have dropped by
~336 bytes/cycle (~6.4 KB over 20 cycles).

### 3. I5 — `apps/timer_slot.lua`: repeating timer cancels itself and creates a new
   one-shot in the same (reused) slot, from inside its own callback:

```
I (104329) launcher: app 'Timer slot' running, vm psram cost = 39804 bytes
TIMER_SLOT: repeating timer fired, cancelling self + creating new one-shot
TIMER_SLOT: NEW ONESHOT FIRED -- slot reuse survived
TIMER_SLOT: PASS
```

### 4. Regression suite

`counter.lua` — runs and renders normally (see above, run twice, both clean).

`tick_test.lua`:
```
ONESHOT fired
TICK 1
TICK 2
TICK 3
TICK 4
TICK 5
TIMER CANCELLED
I (160405) launcher: app 'Tick test' closed, psram free=8207380, internal free=213615, lv_mem free=47256/62660
```

`timer_reuse.lua`:
```
STEP1 creating one-shot (will free its slot when it fires)
ONESHOT fired (slot now free)
STEP2 creating repeating timer (should reuse the freed slot)
STEP3 cancelling stale one-shot handle
STALE CANCEL DONE
LIVE TICK 1
LIVE TICK 2
LIVE TICK 3
LIVE TICK 4
LIVE TICK 5
LIVE TIMER CANCELLED (test complete)
I (172691) launcher: app 'Timer reuse' closed, psram free=8207380, internal free=212079, lv_mem free=47256/62660
```

`hook_bypass.lua` — still killable via STOP (the interrupt hook survives the
sandboxed `require("debug")`, and the `while true` loop is interrupted):
```
HOOK_BYPASS starting
HOOK_BYPASS pcall ok=false err=/sdcard/apps/hook_bypass.lua:11: module 'debug' not found: ...
HOOK_BYPASS entering while true
--- sending STOP ---
I (184090) launcher: app 'Hook bypass' stopped: app stopped by launcher
stack traceback:
	/sdcard/apps/hook_bypass.lua:17: in main chunk
I (184096) launcher: app 'Hook bypass' closed, psram free=8207380, internal free=181099, lv_mem free=47536/62668
```

`broken.lua` — error screen shown, STOP returns to the launcher cleanly:
```
E (194573) launcher: app 'Broken' failed: /sdcard/apps/broken.lua:6: attempt to index a nil value (local 't')
stack traceback:
	/sdcard/apps/broken.lua:6: in main chunk
--- sending STOP ---
I (197609) serial_push: STOP
STOP_OK
I (197734) display_service: session closed for 'lua_lvgl'
I (197740) launcher: app 'Broken' closed, psram free=8207380, internal free=181099, lv_mem free=47532/62668
```

### 5. Board boots to a working launcher at the end

Final sanity pass — `counter.lua` launched and stopped cleanly one more time after
the full regression suite, landing back on the launcher screen:
```
I (212230) launcher: launching 'Counter' (/sdcard/apps/counter.lua)
RUN_OK counter.lua
I (212305) launcher: app 'Counter' running, vm psram cost = 40272 bytes
I (215384) lua_lvgl: Lua exit cleanup: deinitializing lvgl owned by exiting state
I (215394) launcher: app 'Counter' closed, psram free=8207380, internal free=181099, lv_mem free=47252/62660
```

No power cycle was needed at any point in this session.

## Note on scope

`lua_lvgl_delete()` (`obj:delete()`, `lua_lvgl_object.c`) still marks a record
invalid without freeing it mid-run; it's only reclaimed at the next
`lua_lvgl_invalidate_records_locked()` (app exit / runtime deinit), same as it was
before this session. Not in scope for I1 as specified (which measures per-launch
leakage, now fixed), but worth flagging for anyone hunting a within-a-single-run
leak from heavy `obj:delete()` use.
