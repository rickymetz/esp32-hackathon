-- Timing regression fixture. Not an app -- it lives outside apps/ so the
-- launcher never lists it, and it prints results for sim/timing_test.py.
--
-- Guards the bug class that hit six apps at once: a periodic timer re-arms
-- AFTER its callback, so timer.every always runs slow and the error
-- accumulates in one direction. Correct code derives from timer.now_ms()
-- and its error stays bounded no matter how long it runs.
--
-- The test is drift-over-time, not per-tick accuracy, because that is what
-- separates the two: tick-counting drifts LINEARLY, so a long enough run
-- makes the difference unmistakable even on a host where per-tick overhead
-- is small.

local lvgl = require("lvgl")
local timer = require("timer")

lvgl.init({ buffer_lines = 40 })
local scr = lvgl.create_screen()
scr:set_style({ bg_color = "#000000" })
lvgl.label(scr, { text = "timing", align = "center", text_color = "#FFFFFF" })
scr:load()

local INTERVAL = 100
local BEATS = 40                      -- 4 seconds of run

-- (A) The WRONG pattern, measured so the test proves it can tell them apart.
local ticks, accum_started = 0, timer.now_ms()
local h_bad
h_bad = timer.every(INTERVAL, function()
    ticks = ticks + 1
    if ticks >= BEATS then
        h_bad:cancel()
        -- What a tick-counter would believe, vs what the clock says.
        local believed = ticks * INTERVAL
        local actual = timer.now_ms() - accum_started
        print(string.format("TICKCOUNT believed=%d actual=%d error=%d",
                            believed, actual, actual - believed))
    end
end)

-- (B) The RIGHT pattern: chain timer.after against an absolute target.
local n, started = 0, timer.now_ms()
local next_at = started + INTERVAL

local function schedule()
    local delay = math.floor(next_at - timer.now_ms())
    if delay < 1 then delay = 1 end
    timer.after(delay, function()
        n = n + 1
        if n >= BEATS then
            local actual = timer.now_ms() - started
            print(string.format("ABSOLUTE expected=%d actual=%d error=%d",
                                BEATS * INTERVAL, actual, actual - BEATS * INTERVAL))
            return
        end
        next_at = next_at + INTERVAL
        schedule()
    end)
end
schedule()
