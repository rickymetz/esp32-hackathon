-- Exercises the wifi scan API against the sim stub. Not an app; lives
-- outside apps/ so the launcher never lists it. Prints for wifi_api_test.py.
local wifi  = require("wifi")
local timer = require("timer")
local lvgl  = require("lvgl")

lvgl.init({ buffer_lines = 40 })
local scr = lvgl.create_screen()
scr:set_style({ bg_color = "#000000" })
scr:load()

-- scan_results() must be nil BEFORE a scan is ever started.
print("PRESCAN " .. tostring(wifi.scan_results()))

local ok, err = wifi.scan_start()
print("START ok=" .. tostring(ok) .. " err=" .. tostring(err))

-- Idempotent: a second start during a scan is a no-op that still returns true.
print("RESTART " .. tostring(wifi.scan_start()))

local saw_nil = false
local reported = false
timer.every(100, function()
    if reported then return end
    local nets = wifi.scan_results()
    if not nets then
        saw_nil = true            -- the "still scanning" branch was reached
        return
    end
    reported = true
    print("SAWNIL " .. tostring(saw_nil))
    print("COUNT " .. #nets)
    for i, n in ipairs(nets) do
        print(string.format("NET %d ssid=%s rssi=%d secure=%s",
                            i, n.ssid, n.rssi, tostring(n.secure)))
    end
    -- Idempotent: reading twice gives the same answer.
    print("AGAIN " .. #wifi.scan_results())
    print("DONE")
end)
