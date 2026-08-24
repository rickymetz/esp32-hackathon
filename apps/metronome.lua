-- Metronome -- an audible beat. Set the tempo with the stepper, Start to
-- run; each beat ticks and flashes. The tick is a short high tone, which
-- is what a metronome is actually for -- this app used to apologise in a
-- comment for being silent.
--
-- Install: ./.venv/bin/python tools/push.py apps/metronome.lua

local lvgl = require("lvgl")
local ui = require("ui")
local timer = require("timer")
local button = require("button")
local audio = require("audio")

lvgl.init({ buffer_lines = 40 })

local scr = lvgl.create_screen()
scr:set_style({ bg_color = "#000000" })

ui.title(scr, "Metronome")

local bpm = 100
local running = false
local beat_h

local dot = lvgl.button(scr, {
    align = "center", y = -90, w = 140, h = 140, radius = 70,
    bg_color = "#2a2a33",
})

local function flash()
    -- 30ms is short enough to read as a click rather than a note.
    audio.tone(1200, 30)
    dot:set_style({ bg_color = "#2f80ed" })
    timer.after(90, function() dot:set_style({ bg_color = "#2a2a33" }) end)
end

local function stop_beat()
    if beat_h then beat_h:cancel(); beat_h = nil end
end

-- Beats are scheduled against an absolute clock, not by timer.every.
--
-- A periodic timer re-arms AFTER its callback returns, so every beat would
-- cost 60000/bpm PLUS the tone, the redraw and the pump's dispatch latency.
-- On a metronome that is not a rounding error: at 100 bpm a 20 ms overhead
-- makes the real tempo about 97 bpm, and the error compounds, so playing
-- along drifts a whole beat within a minute. Nothing on screen would say so.
--
-- Chaining timer.after against a running next_at fixes the average exactly:
-- a late beat does not push the one after it, because the target time was
-- computed from the start of the run rather than from the previous callback.
local next_at

local function schedule()
    local interval = 60000 / bpm
    local now = timer.now_ms()

    -- If something stalled us past a whole beat, resync instead of catching
    -- up. Clamping a very negative delay to 1 would fire a burst of clicks
    -- to "repay" the missed beats, which on a metronome is worse than the
    -- gap it is trying to correct.
    if next_at < now - interval then
        next_at = now + interval
    end

    -- next_at stays a float so the interval never rounds cumulatively, but
    -- timer.after takes an integer (luaL_checkinteger raises on a fraction).
    local delay = math.floor(next_at - now)
    if delay < 1 then delay = 1 end        -- never schedule into the past
    beat_h = timer.after(delay, function()
        beat_h = nil
        flash()
        next_at = next_at + interval
        schedule()
    end)
end

local function start_beat()
    stop_beat()
    next_at = timer.now_ms() + (60000 / bpm)
    flash()                                 -- beat immediately on Start
    schedule()
end

local stepper
stepper = ui.stepper(scr, { min = 40, max = 240, step = 5, value = bpm, label = "%d bpm" },
    function(v)
        -- Just record the tempo. schedule() recomputes the interval from bpm on
        -- its next iteration, so the change takes effect on the next beat --
        -- calling start_beat() here would re-flash and re-anchor on every step,
        -- machine-gunning clicks under hold-to-repeat.
        bpm = v
    end)
stepper.row:align("center", 0, 48)

local start_btn = lvgl.button(scr, {
    text = "Start", align = "bottom_mid", y = -20, w = 320, h = 100,
    bg_color = "#2F80ED", text_color = "#ffffff", radius = 16,
})

local function toggle()
    running = not running
    if running then
        start_btn:set_text("Stop")
        start_btn:set_style({ bg_color = "#1E1E28" })   -- on-palette; red is confirm-only
        flash()
        start_beat()
    else
        start_btn:set_text("Start")
        start_btn:set_style({ bg_color = "#2F80ED" })
        stop_beat()
    end
end

start_btn:on("clicked", toggle)
button.on("pwr", "pressed", toggle)

scr:load()
