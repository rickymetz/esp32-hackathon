-- I4 regression fixture: timer.every + print only, no lvgl at all. Before
-- the fix, pump_events() treated lvgl.process_events's "runtime is not
-- initialized" error (raised because this app never calls lvgl.init(),
-- which nothing in the app contract requires) as a genuine failure and
-- tore the app down after its first pump -- it flashed and vanished with
-- no error screen. It must now keep ticking indefinitely until STOP.
local timer = require("timer")

print("HEADLESS START")

local ticks = 0
timer.every(300, function()
    ticks = ticks + 1
    print("HEADLESS TICK " .. ticks)
end)
