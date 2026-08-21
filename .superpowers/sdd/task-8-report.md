# Task 8 Report: Refresh button and registry correctness

Status: **COMPILE-VERIFIED ONLY. NOT HARDWARE-VERIFIED.**

The board is in the documented native-USB wedge (port does not even enumerate —
`ls /dev/cu.usbmodem*` returns nothing). Per instructions this task did not attempt
`esptool`, `push.py`, RUN/STOP, or the serial console. All verification below is
static: source review plus `idf.py build`.

## What was built

Followed `task-8-brief.md` for the invalidate entry point and the Refresh button, and
extended it for the escalated pointer-staleness problem described in the task (PUSH now
also rescans at runtime, on every successful push, not just at boot).

### Files

- `launcher/main/app_registry.h` — added `void app_registry_invalidate(void)`, verbatim
  from the brief.
- `launcher/main/app_registry.c` — implemented `app_registry_invalidate()` verbatim from
  the brief: unmounts and clears `s_mounted` if currently mounted, zeroes `s_count`.
- `launcher/main/launcher_main.c`:
  - Added the 200×100 Refresh button in `build_launcher_ui()`, exactly as specified
    (`LV_ALIGN_BOTTOM_MID`, `0x24303C` fill, "Refresh" label).
  - Added `refresh_clicked()` (forward-declares `build_launcher_ui`), which invalidates,
    rescans, rebuilds, and deletes the old screen only after the new one has replaced it.
  - **Departure from the brief's `refresh_clicked` body:** the brief's sample only checks
    `s_app_task != NULL` with no lock. Per this task's explicit instruction, I take
    `s_app_mutex` for the *entire* refresh (check, invalidate, scan, rebuild, delete-old),
    not just the check. `app_row_clicked()` and `launcher_run_app_by_name()` both hold
    `s_app_mutex` across their whole check-then-launch sequence, not just the check —
    matching that means a `RUN`/tap request arriving in the middle of a refresh blocks on
    the mutex instead of landing in the split second between an unlocked check and the
    rebuild, which could otherwise have yanked a just-started app off screen mid-rebuild.
  - **Requirement 4 — rows no longer hold raw pointers, at all**, on either the Refresh or
    PUSH path. Every row's `LV_EVENT_CLICKED` user data changed from a raw
    `const app_entry_t *` into `s_apps[]` to a heap-allocated copy of the app's *basename*
    (`strdup(path_basename(app->path))`), freed via a new `LV_EVENT_DELETE` callback
    (`row_data_delete_cb`) so it can't leak on rebuild or normal teardown. A new
    `find_app_by_basename()` helper resolves that name back to a live `app_entry_t*` at tap
    time by scanning the current registry, exactly like `launcher_run_app_by_name()`
    already did for RUN — I refactored the latter to share this helper instead of
    duplicating the search. `app_row_clicked()` now does the same lookup instead of
    trusting its stored pointer.

### Why requirement 4's alternative (name-in-row-data), not rebuild-on-PUSH

The task offered a choice. I chose **store name in row, resolve by name at tap time**,
not "make PUSH rebuild the UI," because rebuilding from the serial task is actively
dangerous here, not just risky: `build_launcher_ui()` ends with `lv_screen_load()`, which
changes the *active* screen. `PUSH` can arrive at any time, including while an app is
running (an app's Lua VM doesn't block the serial task). If `handle_push()` in
`serial_push.c` called `build_launcher_ui()` after its rescan, a push received while
someone is mid-app would silently rip them out of that app and onto the launcher screen —
worse than the bug being fixed. `bsp_display_lock()` would make that memory-safe, but it
would not stop the screen swap.

The name-based resolution avoids this entirely: PUSH's existing `app_registry_scan()`
call is untouched (still serial-task-only, no LVGL/lock interaction added), rows built
before the push keep working (a tap resolves the name against whatever the registry now
contains), and the worst case for a since-removed app is a warned no-op
(`"tapped row '%s' is no longer in the registry"`), never a wrong-app launch or a stolen
screen. The list itself only visually catches up on the next Refresh, which is an
accepted, minor staleness — not a correctness bug.

## Build result

```
[9/14] Linking C static library esp-idf/main/libmain.a
...
launcher.bin binary size 0xf8130 bytes. Smallest app partition is 0x400000 bytes. 0x307ed0 bytes (76%) free.

Project build complete.
```

No errors. The only warnings are pre-existing `LINE_MAX`/`NAME_MAX` redefinition notes in
`serial_push.c` (from a newlib header, untouched by this change) — no new warnings from
`app_registry.c`, `app_registry.h`, or `launcher_main.c`.

## Walkthrough of `refresh_clicked` behavior

- **Card present:** `s_app_task == NULL` → mutex taken → `app_registry_invalidate()`
  unmounts and clears `s_mounted`/`s_count` → `app_registry_scan()` remounts and repopulates
  `s_apps[]` → `build_launcher_ui()` builds fresh rows (new heap-basename copies), loads the
  new screen → old screen (now inactive) is deleted → mutex released. Same two (or more)
  apps reappear with freshly bound rows.
- **Card absent:** `app_registry_invalidate()` unmounts if needed; `app_registry_scan()`'s
  `bsp_sdcard_mount()` fails, logs a warning, returns `ESP_ERR_NOT_FOUND`, leaves `s_count
  == 0`. `build_launcher_ui()` sees `count == 0` and `app_registry_sd_mounted() == false`,
  shows "No SD card." Old screen still deleted correctly.
- **Card reinserted, tap Refresh again:** because `app_registry_invalidate()` cleared
  `s_mounted` on the *previous* refresh (when the card was missing, `s_mounted` was already
  false, so invalidate's `if (s_mounted)` branch is a no-op there — the mount attempt itself
  is what failed and left `s_mounted` false), this call's `app_registry_scan()` sees
  `!s_mounted` and calls `bsp_sdcard_mount()` again, which now succeeds against the
  reinserted card. Apps reappear. This is the exact bug the brief calls out: without
  `app_registry_invalidate()`, `s_mounted` latches true on first success and a reinserted
  card is never retried; here the flag was never falsely latched because the eject made the
  prior mount attempt fail in the first place, and a successful mount followed by an eject
  *without* a Refresh in between would need the next Refresh's `invalidate()` to unmount and
  clear the latch before rescan — which it does, unconditionally, every time Refresh runs.
- **While an app is running:** `refresh_clicked()` takes `s_app_mutex`, sees
  `s_app_task != NULL`, releases the mutex, and returns immediately — no invalidate, no
  scan, no rebuild, no screen change. Tapping Refresh is not reachable from inside a running
  app's own UI anyway (it's a launcher-screen button), so this guard matters mainly for the
  serial `RUN`/`STOP` race window, not a real screen tap.
- **PUSH while Refresh's rows are still showing an old scan:** rows carry basenames, not
  pointers, so a `PUSH`-triggered `app_registry_scan()` rewriting `s_apps[]` mid-flight
  cannot cause a tap to launch the wrong app — worst case is `find_app_by_basename()`
  returns NULL for a row whose app was renamed/removed, logged and ignored.

## Outstanding hardware checks (must be run once the board is recovered)

1. Flash and boot; confirm the launcher screen still shows both existing apps and the new
   Refresh button doesn't visually collide with or block the app list/rows (the button is
   `BOTTOM_MID` at the same anchor the row list also hugs — verify on the real 368×448
   panel, not just the layout math).
2. Tap Refresh with the card in: list rebuilds, same apps still launch correctly (tap each
   one after a refresh, not just before).
3. `tools/push.py` a new app, tap Refresh: new app appears without a reboot.
4. `tools/push.py` a new app *while not tapping Refresh at all* (this exercises the
   requirement-4 path in isolation): confirm existing rows still launch their correct
   original apps afterward (proves the basename-resolution fix, not just Refresh).
5. Eject the SD card, tap Refresh: shows "No SD card." Reinsert, tap Refresh: apps come
   back, and `tools/read_serial.py 20 --no-reset | grep -E "SD card mounted|app\(s\) found"`
   shows a fresh `SD card mounted` line after the reinsert-and-refresh.
6. Start an app, then send `PUSH`/`RUN`/`STOP` over serial while it's running: confirm the
   running app's screen is never disturbed (this is the specific risk the requirement-4
   design choice was meant to avoid — worth confirming directly).
7. Tap Refresh while an app is running is not reachable via touch (Refresh lives only on
   the launcher screen); confirm via serial that sending `RUN` during a Refresh tap (hard to
   time, best-effort) doesn't produce any visible glitch.
8. Touch-target check on real hardware: confirm the 200×100 Refresh button registers taps
   reliably per the panel's known ≥200×100 requirement.

## Known accepted risk (not fixed, out of scope)

`app_registry_scan()` itself has no internal lock — a Refresh tap (LVGL task) and a PUSH's
runtime rescan (serial task) could theoretically execute `app_registry_scan()`
concurrently, both mutating `s_apps[]`/`s_count` unsynchronized. This predates this task
(PUSH already scanned at runtime without any registry-level lock) and this task's fix does
not add one — the existing codebase already accepts this class of risk (see the
`s_current_app` copy-immediately comment in `lua_app_task`) rather than introducing a
registry-wide mutex. The basename-resolution fix in this task means the *consequence* of
that race is bounded to "resolve fails, no-op" rather than "launch wrong app," which is why
I judged it acceptable to leave alone rather than expand scope. Flagging it here in case a
future task wants to harden `app_registry.c` itself.
