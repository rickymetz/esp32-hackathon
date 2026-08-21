# I3 / I4 / I6 fixes — verification report

Board: ESP32-S3-Touch-AMOLED-1.8 (V2), `/dev/cu.usbmodem101`. All runs below are
verbatim serial console output captured on hardware.

## 1. Free internal DRAM at boot — before and after the pool increase

Before (unmodified, `CONFIG_LV_MEM_SIZE_KILOBYTES` unset → LVGL default 64 KB):

```
I (1238) launcher: ready: 20 app(s), internal free=205411 psram free=8207380
```

After (`CONFIG_LV_MEM_SIZE_KILOBYTES=128`):

```
I (1218) launcher: ready: 20 app(s), internal free=139959 psram free=8207380
```

`205411 - 139959 = 65452` bytes (~64 KB) — exactly the size of the added pool,
confirming it comes from internal DRAM as documented. **Affordable**: ~136.7 KB
of internal DRAM remains free at boot, well above zero, with no other internal-DRAM
consumer added by these fixes.

## 2. I4 — `apps/headless.lua` (timer.every + print, no lvgl at all)

Before the fix this app died instantly (pump_events treated LVGL's "runtime is
not initialized" error, raised because `lvgl.init()` was never called, as a
crash and tore the app down after its first pump — no error screen, just a
flash-and-vanish). After the fix:

```
I (44050) launcher: launching 'Headless' (/sdcard/apps/headless.lua)
I (44050) serial_push: RUN headless.lua
RUN_OK headless.lua
HEADLESS START
I (44078) launcher: app 'Headless' running, vm psram cost = 36160 bytes
I (44078) launcher: app 'Headless' running (callback errors appear under tag 'lua_lvgl_evt')
HEADLESS TICK 1
HEADLESS TICK 2
HEADLESS TICK 3
HEADLESS TICK 4
HEADLESS TICK 5
HEADLESS TICK 6
HEADLESS TICK 7
HEADLESS TICK 8
HEADLESS TICK 9
I (47090) serial_push: STOP
STOP_OK
I (47132) launcher: app 'Headless' closed, psram free=8207380, internal free=115631, lv_mem free=112004/127988
```

9 ticks over ~3 s (300 ms interval) — well past the 5-tick bar — then a clean
`STOP_OK` and normal close. No crash, no early teardown.

## 3. I3 — row cap + "not shown" report

The SD card holds 20 apps at boot (before `headless.lua` was pushed), so
`MAX_VISIBLE_ROWS = 16` is already exercised on every real boot in this
session:

```
I (1051) app_registry: 20 app(s) found
...
I (1218) launcher: ready: 20 app(s), internal free=139959 psram free=8207380
```

`build_launcher_ui()` renders the capped 16 rows plus one trailing
"4 more not shown" label (20 − 16 = 4) instead of building all 20 (or, on a
future card, up to `APP_MAX_COUNT` = 32) — the boot completing cleanly with
sane internal/PSRAM free numbers is the runtime evidence; the render path
itself (`launcher_main.c`, `build_launcher_ui`) was inspected directly since
this device has no camera/screen-capture path available to this session.

**Peak-pool arithmetic:**
- Before: `refresh_clicked()` builds the new screen before deleting the old
  one. At `APP_MAX_COUNT = 32` rows × ~450 B/row, peak ≈ 2 × 32 × 450 ≈
  **29 KB** of a 64 KB pool (~45%) — before fragmentation, which is what made
  a 17-to-32-app card a live path to `lv_malloc()` failure.
- After: capped at `MAX_VISIBLE_ROWS = 16` rows regardless of registry size,
  peak ≈ 2 × 16 × 450 ≈ **14.4 KB** of the now-128 KB pool (~11%) — both the
  cap and the larger pool independently shrink the danger margin, and
  together the peak is under half of what it was against 2× the pool.

## 4. I6 — protected Lua-state setup

Normal launches are unaffected — `counter.lua` regression run (below) shows
the same startup sequence and VM psram cost as before the change.

**Forced setup failure** (cheap to force: temporarily made `lua_setup_state()`
in `launcher_main.c` return `luaL_error(L, "FORCED_I6_TEST_FAILURE")` before
doing any real setup, built, flashed, and ran a launch; reverted immediately
after and rebuilt/reflashed the real image used for every other check here):

```
I (8519) launcher: launching 'Counter' (/sdcard/apps/counter.lua)
I (8519) serial_push: RUN counter.lua
RUN_OK counter.lua
E (8520) launcher: lua state setup failed for 'Counter': FORCED_I6_TEST_FAILURE
I (10559) serial_push: STOP
STOP_OK
I (10577) launcher: app 'Counter' closed, psram free=8207380, internal free=115919, lv_mem free=111800/127980
```

No panic, no reboot (uptime counter is continuous across the whole
sequence) — the error was caught by `lua_pcall` around `lua_setup_state`,
reported via `show_error_and_wait_for_stop()`, and `STOP` returned cleanly to
the launcher. Before the fix this class of failure (an error raised anywhere
inside `luaL_openlibs()`/`launcher_lua_open_modules()`, which is what runs
`luaopen_lvgl` and the timer module's opener, both outside any pcall) would
have hit Lua's default panic function and `abort()`ed the whole board.

## 5. Regression suite (final, reverted image)

`counter.lua` — unaffected by the I6 change:

```
I (13979) launcher: launching 'Counter' (/sdcard/apps/counter.lua)
I (13979) serial_push: RUN counter.lua
RUN_OK counter.lua
I (14009) display_service: session opened for 'lua_lvgl'
W (14011) lua_lvgl_font: default font data load failed: fonts/NotoSansSC-Regular-sub.ttf (ESP_ERR_NOT_FOUND)
W (14011) lua_lvgl: default font unavailable, using LVGL built-in font
I (14054) launcher: app 'Counter' running, vm psram cost = 40256 bytes
I (14055) launcher: app 'Counter' running (callback errors appear under tag 'lua_lvgl_evt')
I (16019) serial_push: STOP
STOP_OK
I (16032) lua_lvgl: Lua exit cleanup: deinitializing lvgl owned by exiting state
I (16034) display_service: session closed for 'lua_lvgl'
I (16042) launcher: app 'Counter' closed, psram free=8207380, internal free=115543, lv_mem free=111344/127972
```

`tick_test.lua`:

```
I (24161) launcher: launching 'Tick test' (/sdcard/apps/tick_test.lua)
I (24161) serial_push: RUN tick_test.lua
RUN_OK tick_test.lua
I (24190) display_service: session opened for 'lua_lvgl'
W (24191) lua_lvgl_font: default font data load failed: fonts/NotoSansSC-Regular-sub.ttf (ESP_ERR_NOT_FOUND)
W (24192) lua_lvgl: default font unavailable, using LVGL built-in font
I (24230) launcher: app 'Tick test' running, vm psram cost = 39088 bytes
I (24231) launcher: app 'Tick test' running (callback errors appear under tag 'lua_lvgl_evt')
ONESHOT fired
TICK 1
TICK 2
TICK 3
TICK 4
TICK 5
TIMER CANCELLED
I (26201) serial_push: STOP
STOP_OK
I (26220) lua_lvgl: Lua exit cleanup: deinitializing lvgl owned by exiting state
I (26221) display_service: session closed for 'lua_lvgl'
I (26229) launcher: app 'Tick test' closed, psram free=8207380, internal free=137563, lv_mem free=111348/127968
```

`broken.lua` — error screen + STOP:

```
I (38042) launcher: launching 'Broken' (/sdcard/apps/broken.lua)
I (38042) serial_push: RUN broken.lua
RUN_OK broken.lua
I (38067) display_service: session opened for 'lua_lvgl'
W (38069) lua_lvgl_font: default font data load failed: fonts/NotoSansSC-Regular-sub.ttf (ESP_ERR_NOT_FOUND)
W (38069) lua_lvgl: default font unavailable, using LVGL built-in font
E (38106) launcher: app 'Broken' failed: /sdcard/apps/broken.lua:6: attempt to index a nil value (local 't')
stack traceback:
	/sdcard/apps/broken.lua:6: in main chunk
I (40082) serial_push: STOP
STOP_OK
I (40092) lua_lvgl: Lua exit cleanup: deinitializing lvgl owned by exiting state
I (40165) display_service: session closed for 'lua_lvgl'
I (40170) launcher: app 'Broken' closed, psram free=8207380, internal free=115543, lv_mem free=111972/127980
```

`hook_bypass.lua` — still killable via STOP:

```
I (51403) launcher: launching 'Hook bypass' (/sdcard/apps/hook_bypass.lua)
I (51403) serial_push: RUN hook_bypass.lua
RUN_OK hook_bypass.lua
HOOK_BYPASS starting
HOOK_BYPASS pcall ok=false err=/sdcard/apps/hook_bypass.lua:11: module 'debug' not found:
	no field package.preload['debug']
	no file '/usr/local/share/lua/5.5/debug.lua'
	no file '/usr/local/share/lua/5.5/debug/init.lua'
	no file '/usr/local/lib/lua/5.5/debug.lua'
	no file '/usr/local/lib/lua/5.5/debug/init.lua'
	no file './debug.lua'
	no file './debug/init.lua'
	no file '/usr/local/lib/lua/5.5/debug.so'
	no file '/usr/local/lib/lua/5.5/loadall.so'
	no file './debug.so'
HOOK_BYPASS entering while true
I (53444) serial_push: STOP
STOP_OK
I (53449) launcher: app 'Hook bypass' stopped: app stopped by launcher
stack traceback:
	/sdcard/apps/hook_bypass.lua:17: in main chunk
I (53455) launcher: app 'Hook bypass' closed, psram free=8207380, internal free=115543, lv_mem free=111972/127980
```

## 6. Final board state

Board ended on a working launcher after every run above (each closes back to
`show_launcher_screen()` with no reboot in between). Final app count on the SD
card: **21** apps (`headless.lua` added for I4; see housekeeping note below):

```
I (1051) app_registry: 21 app(s) found
...
I (1219) launcher: ready: 21 app(s), internal free=139959 psram free=8207380
```

## Housekeeping — SD card cleanup

**Cannot be done**: the serial protocol (`serial_push.c`) only implements
`PUSH`, `RUN`, and `STOP` — there is no delete/remove command, and installing
an app is a file copy with no corresponding uninstall path. Deleting files on
the SD card requires physically removing the card and editing it on another
machine, which this session cannot do.

Apps currently on the card that are **not** in the keep list and should be
removed by hand:

- `pushed.lua`
- `racetest.lua`
- `stress_c1.lua`
- `boot_push.lua`
- `c2_test1.lua`
- `c2_test2.lua`
- `c2_check_tmp.lua`

(7 files to remove.) Everything else on the card matches the keep list:
`counter.lua`, `hello_world.lua`, `tick_test.lua`, `timer_reuse.lua`,
`timer_slot.lua`, `hook_bypass.lua`, `broken.lua`, `deep_error.lua`,
`trim_check.lua`, `runaway_bare.lua`, `runaway_pcall.lua`, `runaway_coro.lua`,
`cb_error.lua`, plus the newly added `headless.lua`. `stopwatch.lua` is in the
keep list and exists in the repo's `apps/` directory but was never pushed to
this card, so there is nothing to remove or keep for it there.
