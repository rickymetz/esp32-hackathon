-- Sensor smoke test: prints to serial so it can be read without eyes.
local rtc = require("rtc")
local imu = require("imu")
local battery = require("battery")
local timer = require("timer")
local lvgl = require("lvgl")
local ui = require("ui")

lvgl.init({ buffer_lines = 40 })
local scr = lvgl.create_screen()
scr:set_style({ bg_color = "#000000" })
ui.title(scr, "Sensors")
local body = lvgl.label(scr, { text = "reading...", align = "center",
    text_color = "#ffffff", w = 340 })

local function report()
    local ax, ay, az = imu.accel()
    local gx, gy, gz = imu.gyro()
    local t = imu.die_temp()
    local pct, perr = battery.percent()
    local v = battery.volts()
    local chg = battery.charging()
    local now, nerr = rtc.now()

    print(string.format("IMU_ACCEL %s %s %s", tostring(ax), tostring(ay), tostring(az)))
    print(string.format("IMU_GYRO %s %s %s", tostring(gx), tostring(gy), tostring(gz)))
    print(string.format("IMU_TEMP %s", tostring(t)))
    print(string.format("BAT %s%% %sV charging=%s err=%s",
        tostring(pct), tostring(v), tostring(chg), tostring(perr)))
    if now then
        print(string.format("RTC %04d-%02d-%02d %02d:%02d:%02d wday=%d",
            now.year, now.month, now.day, now.hour, now.min, now.sec, now.wday))
    else
        print("RTC nil " .. tostring(nerr))
    end

    body:set_text(string.format("accel %.2f %.2f %.2f\nbat %s%%  %sV",
        ax or 0, ay or 0, az or 0, tostring(pct), tostring(v)))
end

report()
timer.every(1000, report)
scr:load()
