-- Dice -- tap to roll 1-4 six-sided dice. Big sum, the individual rolls
-- below, PWR to reroll. Tap the corner chip to change how many dice.
--
-- Install: ./.venv/bin/python tools/push.py apps/dice.lua

local lvgl = require("lvgl")
local ui = require("ui")
local button = require("button")

lvgl.init({ buffer_lines = 40 })

local scr = lvgl.create_screen()
scr:set_style({ bg_color = "#101014" })

ui.title(scr, "Dice")

local num_dice = 1

local total = lvgl.label(scr, {
    text = "-", align = "center", y = -70, text_color = "#ffffff",
})
total:set_style({ font = lvgl.font(48) })

local detail = lvgl.label(scr, {
    text = "tap Roll", align = "center", y = 0, text_color = "#8a8a96",
})

local function roll()
    local parts, sum = {}, 0
    for i = 1, num_dice do
        local r = math.random(1, 6)
        parts[i] = tostring(r)
        sum = sum + r
    end
    total:set_text(tostring(sum))
    detail:set_text(num_dice == 1 and "" or table.concat(parts, " + "))
end

-- Corner chip cycles the die count 1 -> 4 -> 1. Declared before assignment so
-- the closure sees the local (not a nil global) -- and corner_button returns a
-- { button, visual } table, so the label lives on chip.visual.
local chip
chip = ui.corner_button(scr, {
    text = "1d6", align = "top_right", w = 120,
    on_click = function()
        num_dice = num_dice % 4 + 1
        chip.visual:set_text(num_dice .. "d6")
        roll()
    end,
})

local roll_btn = lvgl.button(scr, {
    text = lvgl.symbol.refresh .. " Roll",
    align = "bottom_mid", y = -60, w = 320, h = 120,
    bg_color = "#2f80ed", text_color = "#ffffff",
})
roll_btn:on("clicked", roll)

-- PWR rerolls; the on-screen Roll button stays the primary path.
button.on("pwr", "pressed", roll)

scr:load()
