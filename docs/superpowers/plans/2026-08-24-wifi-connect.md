# Wi-Fi Connect Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let the board find Wi-Fi networks instead of making people spell them, and reconnect on its own instead of failing permanently.

**Architecture:** Two additions to `app_wifi.c` — a polled scan API (`scan_start`/`scan_results`, matching how `status()` already works) and a reason-aware retry state machine that distinguishes a wrong password (stop) from an absent AP (back off and keep trying). `apps/wifi_setup.lua` is rewritten to pick from a list. The sim's stub mirrors the API so the app stays testable headlessly.

**Tech Stack:** ESP-IDF v5.5.5 (`esp_wifi`, `esp_timer`), Lua 5.5, LVGL 9.5, the headless sim, Python test harnesses.

**Spec:** `docs/superpowers/specs/2026-08-24-wifi-connect-design.md`

## Global Constraints

- **Nothing blocks.** Every Lua-facing call returns immediately. A scan takes 2–4s and is polled, never awaited.
- **Lua API names are frozen once written**: `wifi.scan_start()`, `wifi.scan_results()`, `wifi.error()`. Record fields: `ssid` (string), `rssi` (integer), `secure` (boolean).
- **`SSID_MAX` 32, `PASS_MAX` 64, `SCAN_MAX` 20, `MAX_RETRIES` 5.** Backoff table: `{30, 60, 120, 300}` seconds, capped at the last entry.
- **The device and sim APIs must not diverge.** `sim/src/sim_wifi.c` mirrors `app_wifi.c`'s `wifi_funcs[]` exactly; Task 5 asserts this mechanically.
- **`app_wifi.c` has no unit-test path.** It is not compiled into the sim (the sim substitutes `sim_wifi.c`), and this repo has no C test framework. Device-side tasks are verified on hardware; the plan says so at each step rather than inventing test files.
- Timer state is armed **only** in `WIFI_RETRYING`; every transition out of it cancels the timer.
- Touch targets ≥ 200×100 preferred, never below 88×88 (`docs/DESIGN_GUIDE.md`).

---

### Task 1: Sim stub — scan, error, retrying

Do this first: the sim stub is the test harness every later Lua task needs. Nothing here ships to the board.

**Files:**
- Modify: `sim/src/sim_wifi.c`
- Modify: `sim/src/sim_stubs.c` (register the new `wifi scan` verb — see Step 3)
- Modify: `sim/simctl.py:30-40` (document the verb in the usage block)
- Test: `sim/fixtures/wifi_api.lua` (create), `sim/wifi_api_test.py` (create)

**Interfaces:**
- Consumes: nothing.
- Produces: the Lua-visible API every later task uses —
  `wifi.scan_start() -> true | nil, string`,
  `wifi.scan_results() -> nil | table of {ssid=string, rssi=integer, secure=boolean}`,
  `wifi.error() -> nil | string`,
  and `wifi.status()` gaining the `"retrying"` return.
  Sim-only injection: `sim_wifi_set_network_count(int n)`, driven by the `wifi scan <n>` verb.

- [ ] **Step 1: Write the failing test**

Create `sim/fixtures/wifi_api.lua`:

```lua
-- Exercises the wifi scan API against the sim stub. Not an app; lives
-- outside apps/ so the launcher never lists it. Prints for wifi_api_test.py.
local wifi  = require("wifi")
local timer = require("timer")
local lvgl  = require("lvgl")

lvgl.init({ buffer_lines = 40 })
local scr = lvgl.create_screen()
scr:set_style({ bg_color = "#000000" })
scr:load()

-- scan_results() must be nil BEFORE a scan is ever started.
print("PRESCAN " .. tostring(wifi.scan_results()))

local ok, err = wifi.scan_start()
print("START ok=" .. tostring(ok) .. " err=" .. tostring(err))

-- Idempotent: a second start during a scan is a no-op that still returns true.
print("RESTART " .. tostring(wifi.scan_start()))

local saw_nil = false
timer.every(100, function()
    local nets = wifi.scan_results()
    if not nets then
        saw_nil = true            -- the "still scanning" branch was reached
        return
    end
    print("SAWNIL " .. tostring(saw_nil))
    print("COUNT " .. #nets)
    for i, n in ipairs(nets) do
        print(string.format("NET %d ssid=%s rssi=%d secure=%s",
                            i, n.ssid, n.rssi, tostring(n.secure)))
    end
    -- Idempotent: reading twice gives the same answer.
    print("AGAIN " .. #wifi.scan_results())
    print("DONE")
end)
```

Create `sim/wifi_api_test.py`:

```python
#!/usr/bin/env python3
"""Contract test for the wifi scan API, against the sim stub.

app_wifi.c is not compiled into the sim, so this cannot test the device.
What it DOES pin down is the shape every caller depends on: the nil-while-
scanning branch, idempotent reads, field names and types, and RSSI ordering.
Device behaviour is verified on hardware (Tasks 3-4).

    sim/wifi_api_test.py        # exits non-zero on failure
"""
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
SIM = os.path.join(HERE, "build", "sim")
FIXTURE = os.path.join("sim", "fixtures", "wifi_api.lua")

if not os.path.exists(SIM):
    sys.exit("sim not built -- run sim/build.sh")

out = subprocess.run([SIM, "--sdroot", REPO, "run", FIXTURE, ":", "sleep", "3"],
                     cwd=REPO, capture_output=True, text=True, timeout=60).stdout

fails = []


def need(pattern, why):
    m = re.search(pattern, out)
    if not m:
        fails.append(f"{why} (no match for {pattern!r})")
    return m


need(r"PRESCAN nil", "scan_results() must be nil before any scan")
need(r"START ok=true err=nil", "scan_start() must succeed on an idle stub")
need(r"RESTART true", "scan_start() during a scan must return true, not error")
need(r"SAWNIL true", "the still-scanning branch was never reached -- the stub "
                     "resolved instantly, so the nil path is untested")
need(r"DONE", "fixture never completed")

m = need(r"COUNT (\d+)", "no result count")
if m and int(m.group(1)) < 2:
    fails.append(f"expected >=2 fake networks, got {m.group(1)}")

again = re.search(r"AGAIN (\d+)", out)
if m and again and m.group(1) != again.group(1):
    fails.append(f"scan_results() is not idempotent: {m.group(1)} then {again.group(1)}")

nets = re.findall(r"NET \d+ ssid=(\S+) rssi=(-?\d+) secure=(true|false)", out)
if not nets:
    fails.append("no NET lines -- field names or types are wrong")
else:
    rssis = [int(r) for _, r, _ in nets]
    if rssis != sorted(rssis, reverse=True):
        fails.append(f"results not sorted strongest-first: {rssis}")
    if not any(s == "true" for _, _, s in nets):
        fails.append("no secured network in the fake list -- the lock-glyph "
                     "path in wifi_setup.lua would go untested")
    if not any(s == "false" for _, _, s in nets):
        fails.append("no open network in the fake list -- the connect-without-"
                     "password path would go untested")

print(out.strip()[-400:] if fails else f"wifi api: {len(nets)} networks, ordering ok")

if fails:
    for f in fails:
        print("FAIL:", f)
    sys.exit(1)
print("wifi api: ok")
```

- [ ] **Step 2: Run test to verify it fails**

```bash
chmod +x sim/wifi_api_test.py
./sim/wifi_api_test.py
```
Expected: FAIL — the fixture errors because `wifi.scan_start` is nil (`attempt to call a nil value`), so no markers are printed.

- [ ] **Step 3: Implement the stub**

In `sim/src/sim_wifi.c`, add below the existing state block (after `static char s_ip[16] = {0};`):

```c
#define SCAN_POLLS  3       /* scan_results() calls spent "still scanning" */
#define SCAN_MAX    20

/* A fixed fake list. Deliberately includes both a secured and an open
 * network, and is already sorted strongest-first, so the app's lock-glyph
 * and no-password paths are both exercised headlessly. */
static const struct { const char *ssid; int rssi; bool secure; } s_fake[] = {
    { "HomeNetwork", -42, true  },
    { "Cafe-Guest",  -58, true  },
    { "OpenAP",      -71, false },
};
#define FAKE_N ((int)(sizeof(s_fake) / sizeof(s_fake[0])))

static int  s_scan_polls  = -1;         /* <0 = no scan has been started */
static int  s_scan_count  = FAKE_N;     /* sim-only: `wifi scan <n>` */
static const char *s_error = NULL;
```

Extend `sim_wifi_reset()` with:

```c
    s_scan_polls = -1;
    s_scan_count = FAKE_N;
    s_error = NULL;
```

Add the injection setter next to `sim_wifi_set_outcome`:

```c
/* sim-only: how many networks the next scan reports. 0 exercises the
 * "no networks found" screen, which is a real UI state. */
void sim_wifi_set_network_count(int n)
{
    if (n < 0) n = 0;
    if (n > FAKE_N) n = FAKE_N;
    s_scan_count = n;
}
```

Add the three Lua functions above `wifi_funcs[]`:

```c
static int l_wifi_scan_start(lua_State *L)
{
    if (s_state == WIFI_CONNECTING) {
        lua_pushnil(L);
        lua_pushliteral(L, "connecting");
        return 2;
    }
    if (s_scan_polls < 0 || s_scan_polls == 0) {
        s_scan_polls = SCAN_POLLS;   /* (re)start */
    }
    /* A start during an in-flight scan is a no-op that still succeeds. */
    lua_pushboolean(L, 1);
    return 1;
}

static int l_wifi_scan_results(lua_State *L)
{
    if (s_scan_polls < 0) {          /* no scan ever started */
        lua_pushnil(L);
        return 1;
    }
    if (s_scan_polls > 0) {          /* still scanning */
        s_scan_polls--;
        lua_pushnil(L);
        return 1;
    }
    lua_createtable(L, s_scan_count, 0);
    for (int i = 0; i < s_scan_count; i++) {
        lua_createtable(L, 0, 3);
        lua_pushstring(L, s_fake[i].ssid);   lua_setfield(L, -2, "ssid");
        lua_pushinteger(L, s_fake[i].rssi);  lua_setfield(L, -2, "rssi");
        lua_pushboolean(L, s_fake[i].secure); lua_setfield(L, -2, "secure");
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

static int l_wifi_error(lua_State *L)
{
    if (s_error) lua_pushstring(L, s_error); else lua_pushnil(L);
    return 1;
}
```

Set `s_error` where the stub already fails. In `l_wifi_connect`, where it currently arranges a failure, add `s_error = "wrong password";` and in `become_connected()` add `s_error = NULL;`.

Register all three in `wifi_funcs[]`:

```c
static const luaL_Reg wifi_funcs[] = {
    {"connect", l_wifi_connect},
    {"status", l_wifi_status},
    {"ip", l_wifi_ip},
    {"disconnect", l_wifi_disconnect},
    {"time_synced", l_wifi_time_synced},
    {"forget", l_wifi_forget},
    {"scan_start", l_wifi_scan_start},
    {"scan_results", l_wifi_scan_results},
    {"error", l_wifi_error},
    {NULL, NULL},
};
```

In `sim/src/sim_stubs.c`, find where the existing `wifi ok` / `wifi fail` verb is parsed and add a third branch:

```c
    } else if (!strcmp(argv[0], "wifi") && argc >= 2 && !strcmp(argv[1], "scan")) {
        sim_wifi_set_network_count(argc >= 3 ? atoi(argv[2]) : 3);
```

Declare it in whichever header already declares `sim_wifi_set_outcome`:

```c
void sim_wifi_set_network_count(int n);
```

In `sim/simctl.py`, add to the fake-sensor injection block of the docstring:

```
  wifi scan <n>                    how many networks the next scan reports (0 = none)
```

- [ ] **Step 4: Run test to verify it passes**

```bash
(cd sim && ./build.sh) && ./sim/wifi_api_test.py
```
Expected: PASS — `wifi api: 3 networks, ordering ok` then `wifi api: ok`.

- [ ] **Step 5: Confirm the still-scanning branch is really tested**

Temporarily set `#define SCAN_POLLS 0`, rebuild, re-run.
Expected: **FAIL** with `the still-scanning branch was never reached`.
Restore `SCAN_POLLS 3`, rebuild, confirm PASS again.

This is the same discipline the timer tests needed: a test that cannot fail proves nothing.

- [ ] **Step 6: Commit**

```bash
git add sim/src/sim_wifi.c sim/src/sim_stubs.c sim/simctl.py \
        sim/fixtures/wifi_api.lua sim/wifi_api_test.py
git commit -m "sim: stub the wifi scan API so the setup app stays testable headlessly"
```

---

### Task 2: Rewrite `apps/wifi_setup.lua`

**Files:**
- Modify: `apps/wifi_setup.lua` (full rewrite)
- Modify: `sim/scenarios.py` (add two scenarios)
- Test: `sim/scenarios.py`, `sim/golden.py`

**Interfaces:**
- Consumes: `wifi.scan_start()`, `wifi.scan_results()`, `wifi.error()`, `wifi.status()` from Task 1.
- Produces: nothing other tasks consume.

- [ ] **Step 1: Write the failing scenario test**

In `sim/scenarios.py`, add two assertion functions above `SCENARIOS`:

```python
def wifi_lists_networks(w, h, rgb):
    """After a scan, the list area shows row cards rather than empty black.

    Rows are #1E1E28 on a #000000 screen. Sample the middle of where the
    first row lands (ui.list starts below the title/status block).
    """
    r, g, b = px(w, rgb, 184, 210)
    if (r, g, b) == (0, 0, 0):
        return "no row card at the first list position -- scan results never rendered"
    return None


def wifi_empty_state(w, h, rgb):
    """With `wifi scan 0`, the same position must NOT show a row card."""
    r, g, b = px(w, rgb, 184, 210)
    if (r, g, b) != (0, 0, 0) and not (r < 40 and g < 40 and b < 48):
        return f"expected the empty state, but found a row card rgb=({r},{g},{b})"
    return None
```

Add to `SCENARIOS`:

```python
    ("wifi-lists-networks", ["run", "apps/wifi_setup.lua", ":", "sleep", "1.5"],
     wifi_lists_networks),
    ("wifi-empty-state",    ["wifi", "scan", "0",
                             ":", "run", "apps/wifi_setup.lua", ":", "sleep", "1.5"],
     wifi_empty_state),
```

> `px(w, rgb, x, y)` is the existing helper at `sim/scenarios.py:50` — note
> the argument order, `w` before `rgb`. Verified, not assumed.

- [ ] **Step 2: Run to verify it fails**

```bash
./sim/scenarios.py
```
Expected: FAIL on `wifi-lists-networks` — the current app never scans, so that position is black.

- [ ] **Step 3: Rewrite the app**

Replace `apps/wifi_setup.lua` entirely:

```lua
-- Wi-Fi setup. Pick a network from the scan, type only the password.
-- Credentials are saved to the SD card and reused at every boot; nothing
-- is sent over the serial link, so a password never leaves the board.
--
-- Once connected the launcher syncs the clock over NTP, so rtc.now() is
-- correct after a reboot without anyone typing the date.

local lvgl     = require("lvgl")
local ui       = require("ui")
local keyboard = require("keyboard")
local wifi     = require("wifi")
local timer    = require("timer")

lvgl.init({ buffer_lines = 40 })
local scr = lvgl.create_screen()
scr:set_style({ bg_color = "#000000" })

ui.title(scr, "Wi-Fi")

local status = lvgl.label(scr, {
    text = "", align = "top_mid", y = 92, text_color = "#A0A0AE",
})

local list = ui.list(scr, { y = 128, h = 208 })

-- Connect, then let the status poller narrate. `pass` may be "" for an
-- open network.
local function connect_to(ssid, pass)
    local ok, err = wifi.connect(ssid, pass)
    if not ok then
        ui.toast(scr, "error: " .. tostring(err))
    end
end

local function pick(net)
    if not net.secure then
        connect_to(net.ssid, "")
        return
    end
    keyboard.open({ title = net.ssid, mode = "text" }, function(t)
        if t then connect_to(net.ssid, t) end
    end)
end

local function manual_entry()
    keyboard.open({ title = "Network", mode = "text" }, function(name)
        if not name or name == "" then return end
        keyboard.open({ title = "Password", mode = "text" }, function(pass)
            connect_to(name, pass or "")
        end)
    end)
end

-- Rebuild the list from a scan result. `nets` nil means still scanning.
local function render(nets)
    list:clean()
    if nets == nil then
        ui.note(list, "scanning...", { size = 26 })
    elseif #nets == 0 then
        ui.note(list, "no networks found", { size = 26 })
    else
        for _, net in ipairs(nets) do
            local label = net.secure and (lvgl.symbol.eye_close .. "  " .. net.ssid)
                                     or net.ssid
            ui.row(list, {
                text = label, kind = "nav",
                on_click = function() pick(net) end,
            })
        end
    end
    ui.row(list, {
        text = "Other network...", kind = "nav",
        on_click = manual_entry,
    })
end

ui.button(scr, {
    text = "Rescan", kind = "secondary",
    align = "bottom_left", x = 12, y = -12, w = 164,
    on_click = function()
        render(nil)
        wifi.scan_start()
    end,
})

ui.button(scr, {
    text = "Forget", kind = "danger",
    align = "bottom_right", x = -12, y = -12, w = 164,
    on_click = function()
        ui.confirm({
            title = "Forget network?",
            message = "The board will stop connecting on its own.",
            confirm_label = "Forget",
            destructive = true,
        }, function(yes)
            if yes then
                wifi.forget()
                wifi.disconnect()
                ui.toast(scr, "forgotten")
            end
        end)
    end,
})

render(nil)
wifi.scan_start()

-- Poll faster than the thing being watched and repaint only on change --
-- a 1000ms poll against a source that changes on its own misses updates
-- (docs/APP_CONTRACT.md, timer section).
local last_status, last_ip, scanning = nil, nil, true

timer.every(250, function()
    if scanning then
        local nets = wifi.scan_results()
        if nets then
            scanning = false
            render(nets)
        end
    end

    local st = wifi.status()
    local ip = wifi.ip()
    if st == last_status and ip == last_ip then return end
    last_status, last_ip = st, ip

    if st == "connected" then
        status:set_text("connected  " ..
            (wifi.time_synced() and "clock synced" or (ip or "")))
    elseif st == "connecting" then
        status:set_text("connecting...")
    elseif st == "retrying" then
        status:set_text("retrying - " .. (wifi.error() or "network not found"))
    elseif st == "failed" then
        status:set_text("failed - " .. (wifi.error() or "check the password"))
    else
        status:set_text("not connected")
    end
end)

scr:load()
```

> `lvgl.symbol.eye_close` stands in for a padlock: the symbol roster in
> `docs/APP_CONTRACT.md` has no lock glyph, and inventing one would fail
> `tools/check_docs.py`. If a lock is added to the font later, swap it here.

- [ ] **Step 4: Run tests to verify they pass**

```bash
(cd sim && ./build.sh)
./sim/scenarios.py
./sim/test.sh
```
Expected: both new scenarios PASS; `test.sh` still reports `0 failed`.

- [ ] **Step 5: Regenerate the golden**

The app is rewritten, so `sim/golden/wifi_setup.png` no longer matches by design.

```bash
./sim/golden.py --update          # if unsupported, read golden.py for its update flag
./sim/golden.py
```
Expected: `35 ok, 0 failed`. **Open `sim/golden/wifi_setup.png` and look at it** before accepting — a golden regenerated without inspection records whatever bug is on screen.

- [ ] **Step 6: Commit**

```bash
git add apps/wifi_setup.lua sim/scenarios.py sim/golden/wifi_setup.png
git commit -m "wifi_setup: pick a network from a scan instead of typing the SSID blind"
```

---

### Task 3: Device scan

**Files:**
- Modify: `launcher/components/lua_module_wifi/app_wifi.c`

**Interfaces:**
- Consumes: the API shape frozen in Task 1.
- Produces: the same three functions on the device. Task 5 asserts parity.

**No unit test exists for this file** (see Global Constraints). Verification is on hardware.

- [ ] **Step 1: Add scan state**

Below `static bool s_stack_up;`:

```c
#define SCAN_MAX  20

typedef struct {
    char ssid[SSID_MAX + 1];
    int8_t rssi;
    bool secure;
} scan_rec_t;

/* -1 = no scan started, 1 = in flight, 0 = results ready. Written by the
 * event handler (system task), read by Lua (app task); word-sized, and
 * s_scan[] is only written before s_scan_state publishes it. */
static volatile int s_scan_state = -1;
static scan_rec_t   s_scan[SCAN_MAX];
static int          s_scan_n;
```

- [ ] **Step 2: Handle SCAN_DONE**

In `on_wifi_event`, add a branch before the closing brace:

```c
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        uint16_t n = SCAN_MAX;
        wifi_ap_record_t *recs = calloc(n, sizeof(*recs));
        s_scan_n = 0;
        if (recs && esp_wifi_scan_get_ap_records(&n, recs) == ESP_OK) {
            for (uint16_t i = 0; i < n && s_scan_n < SCAN_MAX; i++) {
                const char *ssid = (const char *)recs[i].ssid;
                if (ssid[0] == '\0') {
                    continue;            /* hidden network */
                }
                /* Dedupe by SSID, keeping the strongest. Repeaters and
                 * dual-band APs otherwise list the same name three times,
                 * which reads as a bug. */
                int found = -1;
                for (int j = 0; j < s_scan_n; j++) {
                    if (strcmp(s_scan[j].ssid, ssid) == 0) { found = j; break; }
                }
                if (found >= 0) {
                    if (recs[i].rssi > s_scan[found].rssi) {
                        s_scan[found].rssi = recs[i].rssi;
                    }
                    continue;
                }
                snprintf(s_scan[s_scan_n].ssid, sizeof(s_scan[s_scan_n].ssid), "%s", ssid);
                s_scan[s_scan_n].rssi   = recs[i].rssi;
                s_scan[s_scan_n].secure = (recs[i].authmode != WIFI_AUTH_OPEN);
                s_scan_n++;
            }
            /* Strongest first. Insertion sort: n <= 20. */
            for (int i = 1; i < s_scan_n; i++) {
                scan_rec_t key = s_scan[i];
                int j = i - 1;
                while (j >= 0 && s_scan[j].rssi < key.rssi) {
                    s_scan[j + 1] = s_scan[j];
                    j--;
                }
                s_scan[j + 1] = key;
            }
        }
        free(recs);
        esp_wifi_clear_ap_list();
        s_scan_state = 0;               /* publishes s_scan[] */
        ESP_LOGI(TAG, "scan done: %d network(s)", s_scan_n);
    }
```

Add `#include <stdlib.h>` at the top if not already present.

- [ ] **Step 3: Add the Lua functions**

Above `wifi_funcs[]`:

```c
/* wifi.scan_start() -- begin an async scan. Non-blocking; poll
 * scan_results(). Scanning while CONNECTED briefly interrupts the link,
 * which is accepted so a user can switch networks without disconnecting
 * first; scanning while CONNECTING is refused because it would abort an
 * attempt already in progress. */
static int l_wifi_scan_start(lua_State *L)
{
    if (s_state == WIFI_CONNECTING) {
        lua_pushnil(L);
        lua_pushliteral(L, "connecting");
        return 2;
    }
    if (!stack_start()) {
        lua_pushnil(L);
        lua_pushliteral(L, "wifi stack failed");
        return 2;
    }
    /* The radio must be running to scan, even with no saved credentials --
     * connect_with() is otherwise the only thing that ever starts it. */
    esp_wifi_start();

    if (s_scan_state == 1) {
        lua_pushboolean(L, 1);     /* already scanning: no-op, still success */
        return 1;
    }
    if (esp_wifi_scan_start(NULL, false) != ESP_OK) {
        lua_pushnil(L);
        lua_pushliteral(L, "scan failed");
        return 2;
    }
    s_scan_state = 1;
    lua_pushboolean(L, 1);
    return 1;
}

/* nil while scanning (or before any scan); otherwise an array of
 * {ssid=, rssi=, secure=}. Idempotent -- reading does not consume. */
static int l_wifi_scan_results(lua_State *L)
{
    if (s_scan_state != 0) {
        lua_pushnil(L);
        return 1;
    }
    lua_createtable(L, s_scan_n, 0);
    for (int i = 0; i < s_scan_n; i++) {
        lua_createtable(L, 0, 3);
        lua_pushstring(L, s_scan[i].ssid);       lua_setfield(L, -2, "ssid");
        lua_pushinteger(L, s_scan[i].rssi);      lua_setfield(L, -2, "rssi");
        lua_pushboolean(L, s_scan[i].secure);    lua_setfield(L, -2, "secure");
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}
```

Register in `wifi_funcs[]`:

```c
    {"scan_start", l_wifi_scan_start},
    {"scan_results", l_wifi_scan_results},
```

- [ ] **Step 4: Build**

```bash
. ~/esp/esp-idf/export.sh && cd launcher && idf.py build
```
Expected: `Built target app`, no new warnings.

- [ ] **Step 5: Verify on hardware**

```bash
export PORT=$(printf '%s\n' /dev/cu.usbmodem* | head -1)
cd launcher && idf.py -p $PORT flash && cd ..
./.venv/bin/python tools/drive.py run wifi_setup.lua : sleep 5 : shot /tmp/scan.png
```

Open `/tmp/scan.png`. Expected: real nearby networks listed, strongest first, each appearing **once**. Check the serial log for `scan done: N network(s)`.

If a dual-band AP is in range, confirm its name appears once — that is the dedupe working, and it is the part most likely to be wrong.

- [ ] **Step 6: Commit**

```bash
git add launcher/components/lua_module_wifi/app_wifi.c
git commit -m "wifi: add a polled scan API (scan_start/scan_results)"
```

---

### Task 4: Reason-aware retry

**Files:**
- Modify: `launcher/components/lua_module_wifi/app_wifi.c`

**Interfaces:**
- Consumes: the scan state from Task 3 (the transition helper must not disturb it).
- Produces: `wifi.error()`, and `wifi.status()` returning `"retrying"`.

**No unit test exists for this file.** Verified on hardware.

- [ ] **Step 1: Add the state, timer and transition helper**

Extend the state enum:

```c
typedef enum {
    WIFI_OFF,
    WIFI_CONNECTING,
    WIFI_CONNECTED,
    WIFI_RETRYING,
    WIFI_FAILED,
} wifi_state_t;
```

Add below the existing state block:

```c
/* Backoff after the fast retries are spent. Capped at the last entry. */
static const int s_backoff_s[] = { 30, 60, 120, 300 };
#define BACKOFF_N ((int)(sizeof(s_backoff_s) / sizeof(s_backoff_s[0])))

static esp_timer_handle_t s_retry_timer;
static int                s_backoff_idx;
static const char        *s_error;      /* why we are FAILED or RETRYING */
```

Add `#include "esp_timer.h"`.

The transition helper — **the single place the timer is armed or cancelled**, so the invariant "armed only in RETRYING" is checkable by reading one function:

```c
static void retry_timer_cb(void *arg);

/* Every state change goes through here. The retry timer is armed ONLY on
 * entry to WIFI_RETRYING and cancelled on entry to anything else; that is
 * the whole reason this is a function rather than scattered assignments. */
static void wifi_set_state(wifi_state_t next, const char *err)
{
    if (next != WIFI_RETRYING && s_retry_timer) {
        esp_timer_stop(s_retry_timer);            /* harmless if not running */
    }
    if (next == WIFI_CONNECTING || next == WIFI_CONNECTED) {
        err = NULL;                               /* never report a stale reason */
    }
    s_error = err;
    s_state = next;

    if (next == WIFI_RETRYING) {
        int secs = s_backoff_s[s_backoff_idx];
        if (s_backoff_idx < BACKOFF_N - 1) {
            s_backoff_idx++;
        }
        if (!s_retry_timer) {
            const esp_timer_create_args_t a = {
                .callback = retry_timer_cb, .name = "wifi_retry",
            };
            if (esp_timer_create(&a, &s_retry_timer) != ESP_OK) {
                ESP_LOGE(TAG, "retry timer create failed; staying failed");
                s_state = WIFI_FAILED;
                return;
            }
        }
        esp_timer_start_once(s_retry_timer, (int64_t)secs * 1000000);
        ESP_LOGI(TAG, "retrying in %ds (%s)", secs, err ? err : "?");
    }
}

static void retry_timer_cb(void *arg)
{
    (void)arg;
    char ssid[SSID_MAX], pass[PASS_MAX];
    if (!creds_load(ssid, pass)) {
        wifi_set_state(WIFI_FAILED, "no saved network");
        return;
    }
    s_retries = 0;
    wifi_set_state(WIFI_CONNECTING, NULL);
    esp_wifi_connect();
}
```

- [ ] **Step 2: Route the disconnect handler through it**

Replace the `WIFI_EVENT_STA_DISCONNECTED` branch:

```c
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)data;
        s_ip[0] = '\0';

        /* A wrong password will never succeed, so retrying it forever just
         * keeps the radio busy -- that is why the original code gave up.
         * An absent AP is the opposite: it may well come back, and giving
         * up meant the board never reconnected after a router reboot.
         * The reason code is what separates them. */
        bool auth = (d->reason == WIFI_REASON_AUTH_FAIL ||
                     d->reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
                     d->reason == WIFI_REASON_HANDSHAKE_TIMEOUT);
        if (auth) {
            ESP_LOGW(TAG, "auth failed (reason %d) -- not retrying", d->reason);
            wifi_set_state(WIFI_FAILED, "wrong password");
        } else if (s_retries < MAX_RETRIES) {
            s_retries++;
            wifi_set_state(WIFI_CONNECTING, NULL);
            esp_wifi_connect();
        } else {
            wifi_set_state(WIFI_RETRYING,
                           d->reason == WIFI_REASON_NO_AP_FOUND
                               ? "network not found" : "connection lost");
        }
    }
```

In the `IP_EVENT_STA_GOT_IP` branch, replace `s_state = WIFI_CONNECTED;` with:

```c
        s_retries = 0;
        s_backoff_idx = 0;
        wifi_set_state(WIFI_CONNECTED, NULL);
```

- [ ] **Step 3: Route the remaining state writes through the helper**

Replace every other bare `s_state = ...` in the file:

- in `connect_with()`: the three `s_state = WIFI_FAILED;` become
  `wifi_set_state(WIFI_FAILED, "wifi stack failed");`, and
  `s_state = WIFI_CONNECTING;` becomes `s_backoff_idx = 0; wifi_set_state(WIFI_CONNECTING, NULL);`
- in `l_wifi_disconnect()`: `s_state = WIFI_OFF;` becomes `wifi_set_state(WIFI_OFF, NULL);`

Verify none remain:

```bash
grep -n "s_state = " launcher/components/lua_module_wifi/app_wifi.c
```
Expected: only the two assignments **inside** `wifi_set_state()` itself.

- [ ] **Step 4: Expose the new status and error**

In `l_wifi_status`, add before `default:`:

```c
    case WIFI_RETRYING:   lua_pushliteral(L, "retrying");   break;
```

Add:

```c
/* Why the current failed/retrying state exists; nil otherwise. Cleared on
 * any transition into connecting or connected. */
static int l_wifi_error(lua_State *L)
{
    if (s_error) lua_pushstring(L, s_error); else lua_pushnil(L);
    return 1;
}
```

Register: `{"error", l_wifi_error},`

- [ ] **Step 5: Build and flash**

```bash
. ~/esp/esp-idf/export.sh && cd launcher && idf.py build \
  && idf.py -p $(printf '%s\n' /dev/cu.usbmodem* | head -1) flash && cd ..
```
Expected: builds clean, flashes.

- [ ] **Step 6: Verify the wrong-password path stops**

Connect to a real network with a deliberately wrong password via the app.
Expected: after 5 fast attempts the log shows `auth failed (reason 202) -- not retrying`, the app reads `failed - wrong password`, and **no further attempts appear in the log over the next two minutes**.

- [ ] **Step 7: Verify the absent-AP path retries**

Connect successfully, then power the AP off.
Expected: log shows `retrying in 30s (network not found)`, the app reads `retrying - network not found`, and when the AP returns the board reconnects on its own.

> If no power-cyclable AP is available: connect to a real network, then
> `wifi.forget()` is **not** a substitute. Instead move the board out of
> range, or temporarily change the saved SSID to a nonexistent name and
> confirm the retrying state and backoff schedule in the log. Record in the
> PR which of these was actually done — do not report this step as verified
> if only the wrong-password path was exercised.

- [ ] **Step 8: Verify the timer invariant**

While in `retrying`, open the app and tap a network to connect.
Expected: the log shows a connect attempt immediately (not after the backoff), and no second `retrying in Ns` line races it.

- [ ] **Step 9: Commit**

```bash
git add launcher/components/lua_module_wifi/app_wifi.c
git commit -m "wifi: retry an absent AP on a backoff, stop on a wrong password"
```

---

### Task 5: Docs, parity check, codemaps

**Files:**
- Modify: `docs/APP_CONTRACT.md` (wifi section)
- Modify: `codemaps/data.md` (module roster note), `codemaps/firmware.md` (wifi row)
- Modify: `.github/workflows/sim.yml`
- Test: `sim/wifi_parity_test.py` (create)

**Interfaces:**
- Consumes: the function tables from Tasks 1, 3, 4.
- Produces: nothing.

- [ ] **Step 1: Write the failing parity test**

The device and sim APIs diverging silently is the failure mode this guards.

Create `sim/wifi_parity_test.py`:

```python
#!/usr/bin/env python3
"""The sim's wifi stub must expose exactly the device's wifi API.

sim/src/sim_wifi.c substitutes for app_wifi.c, so a function added to one
and not the other means apps pass headlessly and break on hardware (or the
reverse). Compares the two wifi_funcs[] tables textually.

    sim/wifi_parity_test.py     # exits non-zero on mismatch
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
DEV = os.path.join(REPO, "launcher/components/lua_module_wifi/app_wifi.c")
SIM = os.path.join(HERE, "src/sim_wifi.c")


def names(path):
    src = open(path).read()
    m = re.search(r"wifi_funcs\[\]\s*=\s*\{(.*?)\};", src, re.S)
    if not m:
        sys.exit(f"no wifi_funcs[] table found in {path}")
    return set(re.findall(r'\{\s*"(\w+)"\s*,', m.group(1)))


dev, sim = names(DEV), names(SIM)
only_dev, only_sim = sorted(dev - sim), sorted(sim - dev)

if only_dev or only_sim:
    if only_dev:
        print(f"FAIL: on the device but not the sim: {only_dev}")
    if only_sim:
        print(f"FAIL: in the sim but not the device: {only_sim}")
    sys.exit(1)
print(f"wifi parity: ok ({len(dev)} functions)")
```

- [ ] **Step 2: Run it**

```bash
chmod +x sim/wifi_parity_test.py && ./sim/wifi_parity_test.py
```
Expected after Tasks 1–4: PASS, `wifi parity: ok (9 functions)`.

Confirm it can fail: temporarily comment out `{"error", l_wifi_error},` in the sim table, re-run, expect FAIL, restore.

- [ ] **Step 3: Update the app contract**

In `docs/APP_CONTRACT.md`'s "Networking: `require("wifi")`" section, replace the API list with:

```
wifi.connect()                  -- use the saved network
wifi.connect(ssid, password)    -- use these, and save them for next boot
wifi.status()                   -- "off" | "connecting" | "connected"
                                --   | "retrying" | "failed"
wifi.error()                    -- why it failed or is retrying, or nil
wifi.ip()                       -- "192.168.1.42", or nil
wifi.scan_start()               -- begin an async scan
wifi.scan_results()             -- nil while scanning; else a list of
                                --   { ssid=, rssi=, secure= }
wifi.time_synced()              -- true once NTP has set the clock this boot
wifi.disconnect() / wifi.forget()
```

And add below it:

```markdown
**`"retrying"` is new, and it changes what `"failed"` means.** The board used
to give up permanently after five attempts. Now a **wrong password** still
gives up — retrying it would only keep the radio busy — but an **absent
network** backs off (30s → 5min) and keeps trying, so the board reconnects on
its own after a router reboot or a walk back into range.

If you wrote `if wifi.status() == "failed"`, that branch no longer fires when
the network is simply out of range; it reports `"retrying"` instead. Test both,
or test `wifi.error() ~= nil`.

**Scanning is polled, like `status()`:**

```lua
wifi.scan_start()

timer.every(250, function()
    local nets = wifi.scan_results()
    if not nets then return end          -- still scanning
    for _, n in ipairs(nets) do
        print(n.ssid, n.rssi, n.secure)  -- strongest first, deduped
    end
end)
```

Results are sorted strongest-first, deduped by name (a dual-band AP appears
once), and hidden networks are omitted — offer manual entry for those.
`scan_results()` does not consume: reading twice gives the same answer until
the next `scan_start()`.

**A scan briefly interrupts an active connection.** That is accepted so you can
switch networks without disconnecting first. `scan_start()` returns
`nil, "connecting"` if a connection attempt is already in flight.
```

- [ ] **Step 4: Update the codemaps**

In `codemaps/firmware.md`, change the `lua_module_wifi` row's note to:
`station only, non-blocking, NTP → RTC, polled scan, reason-aware retry`.

In `codemaps/data.md`, under "Lua module roster", leave the list unchanged (no
new module) — it is the `wifi` entry's behaviour that changed, not the roster.

- [ ] **Step 5: Wire both tests into CI**

In `.github/workflows/sim.yml`, after the `Timer catch-up guard` step:

```yaml
      - name: Wi-Fi scan API contract
        run: ./sim/wifi_api_test.py

      - name: Wi-Fi sim/device API parity
        run: ./sim/wifi_parity_test.py
```

- [ ] **Step 6: Run everything**

```bash
./.venv/bin/python tools/check_docs.py
./sim/wifi_api_test.py && ./sim/wifi_parity_test.py
./sim/scenarios.py && ./sim/golden.py && ./sim/test.sh
```
Expected: all pass.

- [ ] **Step 7: Commit**

```bash
git add docs/APP_CONTRACT.md codemaps/firmware.md \
        sim/wifi_parity_test.py .github/workflows/sim.yml
git commit -m "docs: document the wifi scan API and the retrying state change"
```

---

## Self-Review

**Spec coverage.** Every spec section maps to a task: scan → 1, 3; reason-aware
retry → 4; setup app rewrite → 2; sim + tests → 1, 2, 5; contract change → 5;
state machine invariant → 4 (Step 1 helper, Step 3 grep, Step 8 hardware check).
The three ambiguities resolved in spec self-review are each pinned by a test in
Task 1 (`PRESCAN`, `RESTART`, `AGAIN`) or by the helper in Task 4 Step 1
(error cleared on CONNECTING/CONNECTED).

**Type consistency.** `scan_start` / `scan_results` / `error` and the record
fields `ssid` / `rssi` / `secure` are identical in Tasks 1, 2, 3, 5. The sim
uses `bool secure` and pushes with `lua_pushboolean`; the device does the same.
`wifi_set_state(wifi_state_t, const char *)` is used consistently in Task 4.

**Known gaps, stated rather than hidden:**

1. `app_wifi.c` has **no automated test**. Tasks 3 and 4 are hardware-verified.
   The parity test (Task 5) catches API drift but not behaviour.
2. Task 4 Step 7 needs a power-cyclable AP. The fallback is named, and the step
   requires reporting which method was used rather than silently passing.
3. Task 2 Step 1's pixel coordinates assume `ui.list` at `y=128` puts the first
   row near `y=210`. If the layout differs, read the rendered PNG and adjust —
   the assertion is a starting point, not a measured constant.
