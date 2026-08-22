-- Tally -- a counter for the wrist: count reps, laps, people, birds.
-- Big +1 / -1 targets, PWR as a +1 accelerator, and a confirm-gated reset.
--
-- Install: ./.venv/bin/python tools/push.py apps/tally.lua

local lvgl = require("lvgl")
local ui = require("ui")
local button = require("button")

lvgl.init({ buffer_lines = 40 })

local scr = lvgl.create_screen()
scr:set_style({ bg_color = "#101014" })

ui.title(scr, "Tally")

local count = 0

local count_label = lvgl.label(scr, {
    text = "0", align = "center", y = -70, text_color = "#ffffff",
})
count_label:set_style({ font = lvgl.font(48) })

local function render()
    count_label:set_text(tostring(count))
end

-- Big touch targets per the ~200x100 rule; stacked so both fit on 368 wide.
local plus = lvgl.button(scr, {
    text = "+1", align = "bottom_mid", y = -150, w = 320, h = 110,
    bg_color = "#2f80ed", text_color = "#ffffff",
})
local minus = lvgl.button(scr, {
    text = lvgl.symbol.minus .. "1", align = "bottom_mid", y = -30, w = 320, h = 110,
    bg_color = "#3a3a44", text_color = "#ffffff",
})

plus:on("clicked", function()
    count = count + 1
    render()
end)

minus:on("clicked", function()
    if count > 0 then
        count = count - 1
        render()
    end
end)

-- Reset loses the count, so it goes through the sanctioned confirm path.
ui.corner_button(scr, {
    text = "Reset", align = "top_right", w = 140,
    on_click = function()
        ui.confirm({
            title = "Reset tally?",
            message = "The count returns to 0.",
            confirm_label = "Reset",
            destructive = true,
        }, function(ok)
            if ok then
                count = 0
                render()
            end
        end)
    end,
})

-- PWR (bottom-right) accelerates the obvious action: +1. The on-screen +1
-- button remains the primary, visible path, as the button rules require.
button.on("pwr", "pressed", function()
    count = count + 1
    render()
end)

scr:load()
