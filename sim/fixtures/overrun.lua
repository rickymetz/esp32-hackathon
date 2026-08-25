-- Catch-up guard fixture. Not an app -- it lives outside apps/ so the launcher
-- never lists it, and it prints results for sim/overrun_test.py.
--
-- Covers the newest and riskiest branch of the timer fix: what happens when a
-- periodic callback takes LONGER than its own period. The deadline has already
-- passed by the time the callback returns, so app_timer_run_due() must SKIP the
-- missed slots rather than replay them -- replaying would fire back-to-back
-- forever and starve the app it is supposed to be serving.
--
-- Two properties are asserted by the test:
--   1. ticks are skipped, so the count is ~wall_clock/period_actually_achievable
--      rather than wall_clock/nominal_period;
--   2. no burst -- consecutive fires are never closer together than the period,
--      which is what "replayed the backlog" would look like.
--
-- The guard also snaps to the ORIGINAL grid rather than rebasing on now, so the
-- gaps stay whole multiples of the interval; the test checks that too.

local lvgl  = require("lvgl")
local timer = require("timer")

lvgl.init({ buffer_lines = 40 })
local scr = lvgl.create_screen()
scr:set_style({ bg_color = "#000000" })
lvgl.label(scr, { text = "overrun", align = "center", text_color = "#FFFFFF" })
scr:load()

local INTERVAL = 50      -- nominal period
local BUSY     = 120     -- every callback overruns it, by more than 2x
local FIRES    = 12

local n, last = 0, nil
local started = timer.now_ms()
local min_gap = nil

local h
h = timer.every(INTERVAL, function()
    local now = timer.now_ms()
    n = n + 1
    if last then
        local gap = now - last
        if min_gap == nil or gap < min_gap then min_gap = gap end
    end
    last = now

    if n >= FIRES then
        h:cancel()
        local elapsed = timer.now_ms() - started
        print(string.format("OVERRUN fires=%d elapsed=%d interval=%d busy=%d min_gap=%d",
                            n, elapsed, INTERVAL, BUSY, min_gap or -1))
        return
    end

    -- Burn well past the period, using the clock rather than a loop count so
    -- the overrun is real time on any host.
    local until_ms = timer.now_ms() + BUSY
    while timer.now_ms() < until_ms do end
end)
