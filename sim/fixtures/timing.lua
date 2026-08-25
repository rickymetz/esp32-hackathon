-- Timing regression fixture. Not an app -- it lives outside apps/ so the
-- launcher never lists it, and it prints results for sim/timing_test.py.
--
-- Guards the bug class that hit six apps at once: app_timer.c used to re-arm
-- a periodic timer from the moment its callback RETURNED, so timer.every
-- always ran slow and the error accumulated in one direction. That is fixed
-- (the deadline now advances by the period), so BOTH patterns below should
-- now come out accurate -- and this fixture is what proves it stayed that way.
--
-- The measurement is drift-over-time, not per-tick accuracy: the old bug
-- drifted LINEARLY, so a 4 s run makes a regression unmistakable even on a
-- host where per-tick overhead is small.
--
-- Pattern (B) is still the right thing to write in an app. It is robust
-- whatever the launcher does underneath, and it is what you need anyway when
-- pacing against something other than a fixed period.

local lvgl = require("lvgl")
local timer = require("timer")

lvgl.init({ buffer_lines = 40 })
local scr = lvgl.create_screen()
scr:set_style({ bg_color = "#000000" })
lvgl.label(scr, { text = "timing", align = "center", text_color = "#FFFFFF" })
scr:load()

local INTERVAL = 100
local BEATS = 40                      -- 4 seconds of run

-- (A) Tick-counting: what an app believes vs what the clock says. This was
-- the WRONG pattern and is now accurate, because timer.every itself is.
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

-- (B) Chain timer.after against an absolute target. Accurate regardless.
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
