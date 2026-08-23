-- Breathe -- a paced breathing guide. The circle grows as you inhale, holds,
-- shrinks as you exhale. Start/Stop with the button or PWR. One big target,
-- no small controls -- suits the watch.
--
-- Install: ./.venv/bin/python tools/push.py apps/breathe.lua

local lvgl = require("lvgl")
local timer = require("timer")
local button = require("button")

lvgl.init({ buffer_lines = 40 })

local scr = lvgl.create_screen()
scr:set_style({ bg_color = "#000000" })

-- The paced cycle: grow in, hold, shrink out, hold. Max diameter is kept so
-- the fully-inflated circle clears the Start button below it.
local PHASES = {
    { name = "Breathe in",  dur = 4000, from = 96,  to = 240 },
    { name = "Hold",        dur = 1600, from = 240, to = 240 },
    { name = "Breathe out", dur = 4000, from = 240, to = 96 },
    { name = "Hold",        dur = 1600, from = 96,  to = 96 },
}
local CIRCLE_Y = -46      -- centre offset; keeps the grown circle off the button

local circle = lvgl.container(scr, {
    align = "center", y = CIRCLE_Y, w = 96, h = 96, radius = 48,
    bg_color = "#2f80ed", border_width = 0,
})
local caption = lvgl.label(scr, {
    text = "Ready", align = "center", y = CIRCLE_Y, text_color = "#ffffff",
})
caption:set_style({ font = lvgl.font(26) })

local running = false
local anim_h
local phase_i = 1
local phase_start                -- timer.now_ms() when this phase began

local function set_size(d)
    circle:set_size(d, d)
    circle:set_style({ radius = d // 2 })
    circle:align("center", 0, CIRCLE_Y)   -- re-center after the resize
end

local function reset_visual()
    set_size(96)
    caption:set_text(running and PHASES[phase_i].name or "Ready")
end

-- Phase position comes from the clock, not from counting ticks.
--
-- phase_elapsed = phase_elapsed + 50 on a 50 ms timer looks equivalent and is
-- not: a periodic timer re-arms AFTER its callback, so each "50 ms" tick costs
-- 50 ms plus dispatch, and at this period the overhead is a large fraction of
-- the interval. The pacing would run slow by that fraction -- and the whole
-- point of a breathing app is that "inhale for four seconds" lasts four
-- seconds. Advancing phase_start by exactly p.dur keeps the cycle honest even
-- when an individual tick lands late.
local function tick()
    local p = PHASES[phase_i]
    local elapsed = timer.now_ms() - phase_start

    while elapsed >= p.dur do          -- while, not if: a long stall may span phases
        phase_start = phase_start + p.dur
        elapsed = elapsed - p.dur
        phase_i = phase_i % #PHASES + 1
        p = PHASES[phase_i]
        caption:set_text(p.name)
    end

    local t = elapsed / p.dur
    set_size(math.floor(p.from + (p.to - p.from) * t))
end

local start_btn = lvgl.button(scr, {
    text = "Start", align = "bottom_mid", y = -24, w = 300, h = 100,
    bg_color = "#2F80ED", text_color = "#ffffff", radius = 16,
})

local function toggle()
    running = not running
    if running then
        phase_i, phase_start = 1, timer.now_ms()
        caption:set_text(PHASES[1].name)
        start_btn:set_text("Stop")
        start_btn:set_style({ bg_color = "#1E1E28" })   -- on-palette; red is confirm-only
        anim_h = timer.every(50, tick)
    else
        if anim_h then anim_h:cancel(); anim_h = nil end
        start_btn:set_text("Start")
        start_btn:set_style({ bg_color = "#2F80ED" })
        reset_visual()
    end
end

start_btn:on("clicked", toggle)
button.on("pwr", "pressed", toggle)

scr:load()
