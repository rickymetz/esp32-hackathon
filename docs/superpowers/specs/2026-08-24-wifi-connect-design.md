# Wi-Fi connect — Design

Make the board find networks instead of making people spell them, and make it
reconnect on its own instead of failing permanently.

## Context

Two halves exist and both are readable:

- `launcher/components/lua_module_wifi/app_wifi.c` (319 lines) — station mode,
  credentials in `/sdcard/wifi.txt`, `connect / status / ip / disconnect /
  time_synced / forget`.
- `apps/wifi_setup.lua` — type the SSID by hand, type the password, Connect.

Three problems, in the order they hurt:

**1. You type the SSID blind.** There is no scan. You spell the network name
exactly right, on a two-stage keyboard, on a digitizer that `docs/APP_CONTRACT.md`
documents as not pixel-accurate. A typo is indistinguishable from a wrong
password: both end at `failed`.

**2. The board gives up forever.** `app_wifi_autostart()` → `connect_with()` →
5 retries → `WIFI_FAILED`, and there is no path back. Router slow to boot,
board booted out of range, AP rebooted mid-session — the board never reconnects
until a human opens the setup app and taps Connect. On a device whose stated
selling point is that the clock sets itself over NTP, that is the gap.

The existing comment explains why it gives up: *"a wrong password would
otherwise keep the radio busy for the life of the board."* That reasoning is
sound, but it treats two different failures as one. ESP-IDF gives us the
disconnect reason.

**3. The setup app hides what it knows.** It starts with `ssid = ""` even when
credentials are saved, so it always looks unconfigured. `wifi.forget()` exists
in C with no UI. It polls at exactly 1000 ms, which the contract's own timer
section says not to do against a source that changes on its own.

## Decisions taken

| Decision | Choice | Why |
| --- | --- | --- |
| Scan API shape | **Polled**, not callback | Identical to how `wifi.status()` already works. A callback would need `app_wifi_run_pending()` wired into the launcher pump loop (as voice and button do) — one more cross-task drain path to get right, for no gain the caller notices. |
| After 5 fast retries | **Reason-aware** | Retrying a wrong password forever is the thing the current code correctly avoids. Retrying an absent AP forever is the thing it should do. The reason code separates them. |

## Architecture

### `app_wifi.c` — scan

```
wifi.scan_start()   -> true | nil, err
wifi.scan_results() -> nil                                   -- still scanning
                     | { {ssid=, rssi=, secure=}, ... }      -- strongest first
```

`esp_wifi_scan_start(NULL, false)` — non-blocking; completion arrives as
`WIFI_EVENT_SCAN_DONE`, handled in the existing `on_wifi_event`. Results are
read with `esp_wifi_scan_get_ap_records()` into a bounded buffer, then:

- **deduped by SSID, keeping the strongest RSSI** — repeaters and dual-band APs
  otherwise list the same name two or three times, which reads as a bug
- **hidden SSIDs dropped** (empty name); manual entry remains the path to those
- **sorted by RSSI descending**
- **capped at `SCAN_MAX = 20`** (~1.6 KB of `wifi_ap_record_t`)

`secure` is `record.authmode != WIFI_AUTH_OPEN`.

Three semantics the first draft left ambiguous, made explicit:

- **`scan_results()` is idempotent.** It does not consume the results. Once a
  scan completes the same table is returned on every call until the next
  `scan_start()`. A poll loop that reads it twice gets the same answer.
- **`scan_start()` while a scan is already running returns `true` and does not
  restart it.** Re-arming mid-scan would discard partial results for no gain.
- **`wifi.error()` is cleared on any transition into CONNECTING or CONNECTED.**
  It reports why the *current* failed/retrying state exists, never a stale
  reason from an earlier attempt.

Reason constants verified against
`~/esp/esp-idf/components/esp_wifi/include/` rather than recalled: all six named
below exist.

Two constraints handled explicitly:

- A scan needs the stack up. `scan_start()` calls `stack_start()` and starts the
  radio even when no credentials are saved — the current code only ever brings
  the stack up from `connect_with()`.
- **Scanning while connected briefly interrupts the link.** `scan_start()`
  refuses while `WIFI_CONNECTING` (returns `nil, "connecting"`); while
  `WIFI_CONNECTED` it is allowed, and the contract documents the interruption.

### `app_wifi.c` — reason-aware retry

`WIFI_EVENT_STA_DISCONNECTED` carries `wifi_event_sta_disconnected_t.reason`.

```
reason
  WIFI_REASON_AUTH_FAIL
  WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT
  WIFI_REASON_HANDSHAKE_TIMEOUT      -> WIFI_FAILED, permanent
                                        s_error = "wrong password"

  everything else (NO_AP_FOUND,
  BEACON_TIMEOUT, ASSOC_LEAVE, ...)  -> fast retries, then WIFI_RETRYING
                                        backoff 30s -> 60s -> 2m -> 5m (capped)
                                        until connected or wifi.disconnect()
```

The first `MAX_RETRIES` (5) attempts stay immediate, as now — an AP that drops
one association should recover in seconds, not in thirty. The backoff begins
only after those are exhausted.

Backoff uses one `esp_timer` one-shot, re-armed each cycle.

New API surface:

```
wifi.status()  -> "off" | "connecting" | "connected" | "retrying" | "failed"
wifi.error()   -> nil | "wrong password" | "network not found" | ...
```

### State machine

The retry timer is the first mutable timing state in this module, and it
interacts with both `scan_start()` and `connect()`. Written as one explicit
transition function rather than scattered flags, because that is where a bug
would otherwise live:

| From | Event | To | Side effect |
| --- | --- | --- | --- |
| any | `connect(ssid,pass)` | CONNECTING | cancel retry timer, reset backoff |
| any | `disconnect()` | OFF | cancel retry timer, reset backoff |
| CONNECTING | GOT_IP | CONNECTED | reset retries + backoff, start NTP |
| CONNECTING | DISCONNECTED, auth reason | FAILED | record error, no timer |
| CONNECTING | DISCONNECTED, other, retries<5 | CONNECTING | immediate reconnect |
| CONNECTING | DISCONNECTED, other, retries=5 | RETRYING | arm timer at current backoff |
| RETRYING | timer fires | CONNECTING | reconnect, next backoff step |
| CONNECTED | DISCONNECTED | CONNECTING | retries=0, immediate reconnect |
| any | SCAN_DONE | unchanged | publish results |

Invariant: **the retry timer is armed only in RETRYING**, and every transition
out of RETRYING cancels it.

### `apps/wifi_setup.lua` — rewrite

Opens showing the saved network and live status. Scan starts on entry; results
render into a `ui.list` of `ui.row`s, each with a lock glyph when `secure`.

```
Wi-Fi
  connected  192.168.1.42          <- live, from status()/ip()/error()
  ┌──────────────────────────────┐
  │ 🔒 HomeNetwork               │  <- tap: opens password keyboard
  │ 🔒 Cafe-Guest                │
  │    OpenAP                    │
  │ Other network…               │  <- manual entry, for hidden SSIDs
  └──────────────────────────────┘
  [ Rescan ]            [ Forget ]
```

- Tapping a **secured** row opens the keyboard for the password only.
- Tapping an **open** row connects immediately.
- **Forget** goes through `ui.confirm` — it is destructive per the design guide.
- Status polls at **250 ms and repaints only on change**, per the contract.
- A saved network shows even before the scan returns.

### Sim and tests

`sim/src/sim_wifi.c` mirrors the real API and must gain both calls or
`wifi_setup.lua` breaks headless. It returns a fixed fake network list after
two poll cycles (so the "still scanning" branch is exercised, not skipped) and
supports the existing `wifi ok` / `wifi fail` injection.

- `sim/golden/wifi_setup.png` regenerates — the app is rewritten.
- New `sim/scenarios.py` case: scan → pick a secured network → type a password
  → connect.

## Compatibility

**`wifi.status()` gaining `"retrying"` is a behaviour change, not an addition.**
An app written as `if wifi.status() == "failed" then` will no longer fire in the
out-of-range case, because that case now reports `"retrying"`. `wifi_setup.lua`
is the only in-repo caller, but `docs/APP_CONTRACT.md` says five people write
against this API. The contract change is documented prominently rather than
buried as additive.

`wifi.scan_start` / `scan_results` / `error` are purely additive.

## Out of scope

Not selected during brainstorming, and deliberately excluded:

- **Captive-portal detection.** Already documented as a known limitation.
- WPA3 / WPA2-Enterprise.
- Signal-strength bars beyond the lock glyph and RSSI ordering.
- Moving credentials from `/sdcard/wifi.txt` into NVS.

## Verification

| Claim | How |
| --- | --- |
| Scan finds real networks | on hardware: `drive.py run wifi_setup.lua : sleep 5 : shot` |
| Dedupe works | scan near a dual-band AP; the name appears once |
| Wrong password stops permanently | connect with a bad password; `status()` reaches `failed`, `error()` says so, no further attempts in the log |
| Absent AP retries | connect to a network, power the AP off, confirm `retrying` and a reconnect when it returns |
| Retry timer never leaks | `connect()` mid-backoff; confirm one timer, via STATS/logging |
| Renders headless | `sim/test.sh`, `golden.py`, new scenario |
| No regression | full sim suite, `check_docs.py` |

The AP-off test needs a network whose power can be cycled. If that is not
available, it degrades to forcing the reason code in a debug build — noted so
the gap is visible rather than silently skipped.

## Risks

1. **The retry timer interacting with scan and manual connect** is the most
   likely bug. Mitigated by the explicit state table above and the
   armed-only-in-RETRYING invariant.
2. **Scan while connected drops the link briefly.** Accepted and documented;
   the alternative (refusing to scan while connected) makes changing networks
   impossible without disconnecting first.
3. **The `"retrying"` status change** could surprise an out-of-repo app. It is
   the correct behaviour and is documented, but it is a real break.
