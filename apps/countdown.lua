-- Timer -- a kitchen-style countdown. Set minutes with the stepper, Start to
-- run; the stepper's readout becomes a live M:SS and a toast fires at zero.
-- PWR starts/pauses; Reset returns to the set time.
--
-- Install: ./.venv/bin/python tools/push.py apps/countdown.lua

local lvgl = require("lvgl")
local ui = require("ui")
local audio = require("audio")
local timer = require("timer")
local button = require("button")

lvgl.init({ buffer_lines = 40 })

local scr = lvgl.create_screen()
scr:set_style({ bg_color = "#000000" })

ui.title(scr, "Timer")

local minutes = 5
local remaining = minutes * 60      -- seconds left when last paused
local running = false
local deadline                      -- timer.now_ms() at which we hit zero
local tick

local function fmt_mmss(s)
    return string.format("%d:%02d", s // 60, s % 60)
end

-- The stepper is both the minute selector and (while running) the readout.
-- opts.label is a format string; its big label is what we overwrite with M:SS.
local stepper
stepper = ui.stepper(scr, { min = 1, max = 60, step = 1, value = 5, label = "%d min" },
    function(v)
        if running then
            -- The stepper advances its own value before this callback;
            -- while counting down, snap it straight back so a stray +/-
            -- can neither corrupt the readout nor bank a jump that lands
            -- all at once after Pause (review finding).
            stepper.set(minutes)   -- next 1s tick repaints the countdown
        else
            minutes = v
            remaining = minutes * 60
        end
    end)
stepper.row:align("top_mid", 0, 76)

-- Seconds left, always derived from the clock rather than accumulated.
-- Counting ticks (remaining = remaining - 1) drifts without bound: a
-- periodic timer re-arms AFTER its callback, so each tick is a little over
-- a second. Measured on this board at ~24 ms per tick, which makes a
-- 25-minute countdown finish about half a minute late.
local function left()
    if not running then return remaining end
    local ms = deadline - timer.now_ms()
    if ms < 0 then ms = 0 end
    return (ms + 999) // 1000       -- round up, so "1" shows for a full second
end

local function show(s) stepper.label:set_text(fmt_mmss(s)) end
local function show_minutes() stepper.label:set_text(string.format("%d min", minutes)) end
local function stop_tick() if tick then tick:cancel(); tick = nil end end

local start_btn = lvgl.button(scr, {
    text = "Start", align = "bottom_mid", y = -150, w = 320, h = 110,
    bg_color = "#2f80ed", text_color = "#ffffff",
})
local reset_btn = lvgl.button(scr, {
    text = "Reset", align = "bottom_mid", y = -30, w = 320, h = 110,
    bg_color = "#3a3a44", text_color = "#ffffff",
})

local function finish()
    -- Three rising notes: a countdown that ends silently is a countdown
    -- you have to watch, which defeats the point of setting one.
    audio.play({ { 784, 180 }, { 988, 180 }, { 1319, 400 } })
    running = false
    remaining = 0
    stop_tick()
    start_btn:set_text("Start")
    show(0)
    ui.toast(scr, lvgl.symbol.bell .. " Time's up")
end

local function toggle()
    if running then
        remaining = left()          -- bank the real remaining time
        running = false
        stop_tick()
        start_btn:set_text("Start")
    else
        if remaining <= 0 then remaining = minutes * 60 end
        running = true
        deadline = timer.now_ms() + remaining * 1000
        start_btn:set_text("Pause")
        show(remaining)
        -- Polls at 200 ms and repaints only when the displayed second
        -- changes, so the readout can never skip a second the way a 1000 ms
        -- tick does. It renders; it does not keep time.
        local shown = remaining
        tick = timer.every(200, function()
            local s = left()
            if s == shown then return end
            shown = s
            if s <= 0 then finish() else show(s) end
        end)
    end
end

start_btn:on("clicked", toggle)

reset_btn:on("clicked", function()
    running = false
    stop_tick()
    start_btn:set_text("Start")
    remaining = minutes * 60
    -- A tap on the stepper while running advances its internal value even
    -- though we ignore it (cb is guarded by `not running`); resync it here so
    -- the next +/- steps from `minutes`, not from a drifted value.
    stepper.set(minutes)
    show_minutes()
end)

-- PWR starts/pauses; the on-screen Start/Pause button is the primary path.
button.on("pwr", "pressed", toggle)

scr:load()
