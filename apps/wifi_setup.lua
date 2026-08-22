-- Wi-Fi setup. Type the network name and password here on the device;
-- they are saved to the SD card and reused at every boot. Nothing is
-- sent over the serial link, so a password never leaves the board.
--
-- Once connected the launcher syncs the clock over NTP, so rtc.now()
-- is correct after a reboot without anyone typing the date.

local lvgl = require("lvgl")
local ui = require("ui")
local keyboard = require("keyboard")
local wifi = require("wifi")
local timer = require("timer")

lvgl.init({ buffer_lines = 40 })
local scr = lvgl.create_screen()
scr:set_style({ bg_color = "#000000" })

ui.title(scr, "Wi-Fi")

local ssid, pass = "", ""

local status = lvgl.label(scr, {
    text = "not connected",
    align = "top_mid", y = 92,
    text_color = "#8A8A99",
})

local list = lvgl.container(scr, {
    x = 0, y = 128, w = 368, h = 200,
    bg_opa = 0, border_width = 0, pad = 12,
})
list:set_flex({ flow = "column", pad_row = 12 })

local ssid_row, pass_row

ssid_row = ui.row(list, {
    text = "Network: not set", kind = "nav",
    on_click = function()
        keyboard.open({ title = "Network", mode = "text", initial = ssid }, function(t)
            if t then
                ssid = t
                ssid_row.label:set_text("Network: " .. (ssid == "" and "not set" or ssid))
            end
        end)
    end,
})

pass_row = ui.row(list, {
    text = "Password: none", kind = "nav",
    on_click = function()
        keyboard.open({ title = "Password", mode = "text", initial = pass }, function(t)
            if t then
                pass = t
                -- Never render the password back: show its length only.
                pass_row.label:set_text("Password: " .. (pass == "" and "none" or string.rep("*", #pass)))
            end
        end)
    end,
})

local connect_btn = lvgl.button(scr, {
    text = "Connect",
    align = "bottom_mid", y = -16,
    w = 344, h = 104,
    bg_color = "#2F80ED", text_color = "#FFFFFF", radius = 12,
})

connect_btn:on("clicked", function()
    if ssid == "" then
        ui.toast(scr, "enter a network first")
        return
    end
    status:set_text("connecting...")
    local ok, err = wifi.connect(ssid, pass)
    if not ok then status:set_text("error: " .. tostring(err)) end
end)

timer.every(1000, function()
    local st = wifi.status()
    if st == "connected" then
        status:set_text("connected  " .. (wifi.ip() or ""))
        if wifi.time_synced() then status:set_text("connected  clock synced") end
    elseif st == "connecting" then
        status:set_text("connecting...")
    elseif st == "failed" then
        status:set_text("failed - check the password")
    end
end)

scr:load()
