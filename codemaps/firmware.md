> Generated: 2026-08-24 | Token-lean format for LLM context

# Firmware modules (`launcher/`)

## `main/` — the launcher itself

### `launcher_main.c` (1301) — entry, app lifecycle, locking

| Symbol | Notes |
|---|---|
| `app_main()` | boot order; see architecture.md module graph |
| `release_panel_reset()` | pulses EXIO1/EXIO2 via TCA9554. **Without this the panel is black with no error** — BSP leaves RST as `GPIO_NUM_NC` |
| `lua_psram_alloc()` | Lua allocator → `MALLOC_CAP_SPIRAM`, so apps can't starve DRAM |
| `lua_setup_state()` | run under `lua_pcall` — an error here with no jump buffer calls `abort()` |
| `lua_app_task()` | one app run: VM, store path, load, pump, teardown |
| `pump_events()` | drains queued Lua callbacks; tolerates apps that never call `lvgl.init()` |
| `launcher_refresh_ui()` | **async** — posts to LVGL task via `lv_async_call`. Never holds `s_app_mutex` + display lock together |
| `refresh_ui_async_cb()` | runs on LVGL task; rebuilds, or sets `s_home_stale` |
| `app_row_clicked()` / `app_row_long_pressed()` | launch / open info sheet |
| `sheet_cancel_cb()` / `sheet_delete_cb()` | both take `s_app_mutex` (they mutate screen pointers) |
| `synth_indev_read()` | replays serial TAP/SWIPE through a real LVGL indev |
| `button_poll_task()` | 20ms, 2-sample debounce, BOOT + PWR |
| `traceback_handler()` / `show_error_screen()` | red full-screen error, waits for BOOT |

**Shared state and its guard:**

| Var | Guarded by |
|---|---|
| `s_app_task` | `s_app_mutex` |
| `s_launcher_screen`, `s_sheet_screen` | `s_app_mutex` (all transitions) |
| `s_home_stale` | `s_app_mutex` |
| `s_refresh_pending` | volatile; coalesces rebuild requests |
| `s_synth_q[8]`, `s_synth_cur` | `portMUX` spinlock |

Constants: `APP_TASK_STACK` 32KB · `EVENT_PUMP_MS` 100 · `ROW_HEIGHT` 104 ·
`MAX_VISIBLE_ROWS` 64 · `SYNTH_QUEUE` 8 · `SYNTH_GAP_US` 90000.

### `app_registry.c` (426) — SD scan

`s_apps[APP_MAX_COUNT=32]` under `s_lock`. Entries: `name[48]`, `path[320]`,
`id[128]`, `in_folder`.

| Symbol | Notes |
|---|---|
| `scan_locked()` | mount, readdir, classify flat vs folder app, `qsort` |
| `app_cmp_by_name()` | `strcasecmp(name)` then `strcmp(id)` — the id tie-break is what makes it a **total** order; qsort is not stable and pretty names collide |
| `pretty_name()` | `weather_clock.lua` → "Weather clock"; 1–2 letter first word uppercased whole ("UI test") |
| `app_registry_signature()` | FNV-1a over count + ids. Gates rebuilds so a folder app's N files don't cause N rebuilds |
| `app_registry_write_app()` | temp file + rename, rescans under the same lock |
| `app_registry_delete_app()` | flat unlink, or recursive folder removal |
| `sweep_push_tmp()` | clears `.push.tmp` stranded by a power loss |

Returns **copies**, never pointers into `s_apps[]` — a concurrent PUSH rescan
rewrites it in place.

### `serial_push.c` (692) — the host protocol

`serial_push_task` reads lines from stdin (USB-Serial-JTAG promoted to primary console).

| Command | Reply |
|---|---|
| `PUSH <name> <len> <crc>` + base64 + `ENDPUSH` | `PUSH_OK` / `PUSH_ERR bad_header\|bad_name\|bad_length\|no_memory\|truncated\|crc_mismatch\|write_failed` |
| `RUN <id>` | `RUN_OK` / `RUN_ERR bad_name\|not_found\|already_running` |
| `STOP` | `STOP_OK` / `STOP_ERR not_running` |
| `LIST` | `APP <id>` per line, `LIST_OK <n>` |
| `DELETE <id>` | `DELETE_OK` / `DELETE_ERR not_found\|delete_failed` |
| `SHOT` | `SHOT <w> <h> <stride>`, base64 RGB565, `ENDSHOT` |
| `TAP x y` / `SWIPE x0 y0 x1 y1 [ms]` / `PWR` | `*_OK` / `*_ERR bad_args` |
| `MEM` | `MEM <psram> <internal> <largest_internal>` — **positionally parsed by soak.py/chaos.py; do not widen** |
| `STATS` | `STATS_BEGIN` … one `STAT` per line … `STATS_END` |
| `PING` | `PONG launcher <proto> lvgl <x.y.z>` |
| anything else | `ERR unknown_command <verb>` |

Constants: `LINE_MAX` 256 · `NAME_MAX` 128 · `PAYLOAD_MAX` 64KB ·
`SHOT_CHUNK` 720 · `SHOT_B64_MAX` 964 · `BODY_LINE_MIN` 32.

Notes worth knowing:
- `drain_push_payload()` — a rejected PUSH must consume its body, or the
  unknown-command reply answers ~1150 orphaned base64 lines.
- `looks_like_base64()` needs its **length floor**: `"FROB"` is valid base64,
  so charset alone would swallow the typos the reply exists to surface.
- SHOT is ~1.5s, ~94% transfer. `usb_serial_jtag_write()` loops one char at a
  time; the cost is per *byte*, so bigger chunks bought only ~6%.
- `handle_shot()`'s line buffer is `static` — 964B on this task's stack
  overflowed it outright.

### `launcher_home.c` (659) — the home screen, shared with the sim

Parameterised so the sim renders it with a fake app list: data via `get_app()`,
behaviour via `lv_event_cb_t` pointers. Two layouts (`LAUNCHER_VIEW_LIST` /
`_GRID`, 2×2 swipeable tiles) plus the long-press app-info sheet.

Icon fallback chain: card `icon.bin` → compiled-in bitmap (`launcher_icons.c`)
→ FontAwesome glyph → letter avatar on a name-hashed colour.

### `app_timer.c` (236) — `require("timer")`

16 slots. `{next_us, period_us, ref, gen}`; `gen` distinguishes a stale handle
from the slot's current occupant.

`app_timer_run_due()` advances `next_us += period_us` — **from the previous
deadline, not from now**. Rebasing on dispatch time was the drift bug (5.0 →
0.0 ms/tick). The catch-up guard snaps forward on the original grid, skipping
missed slots rather than replaying them.

### `app_sandbox.c` (142)

Removes `debug`, `package`, `os.exit`, `os.execute` — self-defense for the
launcher (they can disable its watchdog hook or exit its process), **not**
containment. Apps have full `io` and can rewrite each other's files.
Also installs the interrupt hook BOOT/STOP uses to unwind a runaway app.

## `components/`

| Component | Lua module | Lines | Notes |
|---|---|---|---|
| `lua_module_lvgl` | `lvgl` | 7127 | 16 files; widgets, events, fonts, layout, indev |
| `lua_module_ui` | `ui`, `keyboard` | 1133 | `ui.lua` + `keyboard.lua`, embedded as blobs |
| `lua_module_voice` | `voice` | 441 | MultiNet 7, one persistent instance |
| `lua_module_sensors` | `rtc`, `imu`, `battery` | 371 | three modules, one component |
| `lua_module_wifi` | `wifi` | 523 | station only, non-blocking, NTP → RTC, polled scan, reason-aware retry |
| `lua_module_audio` | `audio` | 306 | ES8311; shares I2S with voice |
| `lua_module_button` | `button` | 282 | PWR only; BOOT is not interceptable |
| `lua_module_store` | `store` | 226 | per-app JSON at `<sd>/state/<id>.json` |
| `display_service` | — | 203 | display seam the sim substitutes |
| `cap_lua` | — | 138 | module registry; **opens in registration order** |
| `fonts_lexend` | — | 103k | 8 Lexend faces + icon fallbacks, `--no-compress` |

### `lua_module_lvgl/src/` key files

| File | Role |
|---|---|
| `lua_lvgl_events.c` | trampoline enqueues on LVGL task; drain loop dispatches on script task; `event_signal` binary semaphore wakes it |
| `lua_lvgl_runtime.c` | init/teardown, `lua_lvgl_lock()` (non-recursive mutex + same-task re-entry guard) |
| `lua_lvgl_font.c` | 8 built-in sizes, global font scale, Tiny TTF from card |
| `lua_lvgl_{core,complex,extra}_widgets.c` | constructors |
| `lua_lvgl_indev.c` | touch indev |
| `lua_lvgl_private.h` | `s_lvgl` state struct, sub/record types |

Cross-task rules in the binding: the trampoline must never touch the Lua state
(wrong task); `luaL_unref` from the LVGL task defers onto `pending_unrefs`.
