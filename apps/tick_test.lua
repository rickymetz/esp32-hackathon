-- Verifies timer.every, timer.after and handle:cancel().
local lvgl  = require("lvgl")
local timer = require("timer")

lvgl.init({ buffer_lines = 40 })
local scr = lvgl.create_screen()
scr:set_style({ bg_color = "#101014" })

local label = lvgl.label(scr, {
    text = "ticks: 0", align = "center", text_color = "#ffffff",
})

local ticks = 0
-- `local h` must be declared before the closure that references it: the
-- closure is parsed as part of timer.every()'s argument, which happens
-- before "local h = ..." would put h in scope, so a combined
-- declaration+assignment makes h resolve to a global (nil) inside the
-- callback. Declaring first and assigning separately puts h in scope early.
local h
h = timer.every(200, function()
    ticks = ticks + 1
    label:set_text("ticks: " .. ticks)
    print("TICK " .. ticks)
    if ticks == 5 then
        h:cancel()
        print("TIMER CANCELLED")
    end
end)

timer.after(100, function() print("ONESHOT fired") end)

scr:load()
