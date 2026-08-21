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
scr:set_style({ bg_color = "#0b0d12" })

-- The paced cycle: grow in, hold, shrink out, hold.
local PHASES = {
    { name = "Breathe in",  dur = 4000, from = 96,  to = 268 },
    { name = "Hold",        dur = 1600, from = 268, to = 268 },
    { name = "Breathe out", dur = 4000, from = 268, to = 96 },
    { name = "Hold",        dur = 1600, from = 96,  to = 96 },
}

local circle = lvgl.container(scr, {
    align = "center", y = -20, w = 96, h = 96, radius = 48,
    bg_color = "#2f80ed", border_width = 0,
})
local caption = lvgl.label(scr, {
    text = "Ready", align = "center", y = -20, text_color = "#ffffff",
})
caption:set_style({ font = lvgl.font(26) })

local running = false
local anim_h
local phase_i, phase_elapsed = 1, 0

local function set_size(d)
    circle:set_size(d, d)
    circle:set_style({ radius = d // 2 })
    circle:align("center", 0, -20)   -- re-center after the resize
end

local function reset_visual()
    set_size(96)
    caption:set_text(running and PHASES[phase_i].name or "Ready")
end

local function tick()
    local p = PHASES[phase_i]
    phase_elapsed = phase_elapsed + 50
    if phase_elapsed >= p.dur then
        phase_elapsed = 0
        phase_i = phase_i % #PHASES + 1
        p = PHASES[phase_i]
        caption:set_text(p.name)
    end
    local t = phase_elapsed / p.dur
    set_size(math.floor(p.from + (p.to - p.from) * t))
end

local start_btn = lvgl.button(scr, {
    text = "Start", align = "bottom_mid", y = -28, w = 300, h = 96,
    bg_color = "#27ae60", text_color = "#ffffff",
})

local function toggle()
    running = not running
    if running then
        phase_i, phase_elapsed = 1, 0
        caption:set_text(PHASES[1].name)
        start_btn:set_text("Stop")
        start_btn:set_style({ bg_color = "#c0392b" })
        anim_h = timer.every(50, tick)
    else
        if anim_h then anim_h:cancel(); anim_h = nil end
        start_btn:set_text("Start")
        start_btn:set_style({ bg_color = "#27ae60" })
        reset_visual()
    end
end

start_btn:on("clicked", toggle)
button.on("pwr", "pressed", toggle)

scr:load()
