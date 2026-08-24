-- Wi-Fi setup. Pick a network from the scan, type only the password.
-- Credentials are saved to the SD card and reused at every boot; nothing
-- is sent over the serial link, so a password never leaves the board.
--
-- Once connected the launcher syncs the clock over NTP, so rtc.now() is
-- correct after a reboot without anyone typing the date.

local lvgl     = require("lvgl")
local ui       = require("ui")
local keyboard = require("keyboard")
local wifi     = require("wifi")
local timer    = require("timer")

lvgl.init({ buffer_lines = 40 })
local scr = lvgl.create_screen()
scr:set_style({ bg_color = "#000000" })

ui.title(scr, "Wi-Fi")

local status = lvgl.label(scr, {
    text = "", align = "top_mid", y = 92, text_color = "#A0A0AE",
})

-- 2 rows fit and the rest scrolls: the panel is 448px, ui.row's touch floor
-- is 104px, and the title/status/buttons take the rest. That is the panel's
-- constraint, not a layout bug -- do not shrink the rows to fit more.
local list = ui.list(scr, { y = 118, h = 226 })

local scanning = true

-- `pass` may be "" for an open network.
local function connect_to(ssid, pass)
    local ok, err = wifi.connect(ssid, pass)
    if not ok then
        ui.toast(scr, "error: " .. tostring(err))
    end
end

local function pick(net)
    if not net.secure then
        connect_to(net.ssid, "")
        return
    end
    keyboard.open({ title = net.ssid, mode = "text" }, function(t)
        if t then connect_to(net.ssid, t) end
    end)
end

-- Hidden networks are omitted from a scan, so manual entry stays reachable.
local function manual_entry()
    keyboard.open({ title = "Network", mode = "text" }, function(name)
        if not name or name == "" then return end
        keyboard.open({ title = "Password", mode = "text" }, function(pass)
            connect_to(name, pass or "")
        end)
    end)
end

-- nil means "still scanning".
local function render(nets)
    list:clean()
    if nets == nil then
        ui.note(list, "scanning...", { size = 26 })
    elseif #nets == 0 then
        ui.note(list, "no networks found", { size = 26 })
    else
        for _, net in ipairs(nets) do
            -- No padlock exists in the symbol roster (docs/APP_CONTRACT.md);
            -- eye_close is the nearest available "closed" glyph. Swap it if a
            -- lock is ever added to the icon font.
            local label = net.secure
                and (lvgl.symbol.eye_close .. "  " .. net.ssid)
                or net.ssid
            ui.row(list, {
                text = label, kind = "nav",
                on_click = function() pick(net) end,
            })
        end
    end
    ui.row(list, {
        text = "Other network...", kind = "nav",
        on_click = manual_entry,
    })
end

ui.button(scr, {
    text = "Rescan", kind = "secondary",
    align = "bottom_left", x = 12, y = -10, w = 164, h = 88,
    on_click = function()
        scanning = true
        render(nil)
        wifi.scan_start()
    end,
})

ui.button(scr, {
    text = "Forget", kind = "danger",
    align = "bottom_right", x = -12, y = -10, w = 164, h = 88,
    on_click = function()
        ui.confirm({
            title = "Forget network?",
            message = "The board will stop connecting on its own.",
            confirm_label = "Forget",
            destructive = true,
        }, function(yes)
            if yes then
                wifi.forget()
                wifi.disconnect()
                ui.toast(scr, "forgotten")
            end
        end)
    end,
})

render(nil)
wifi.scan_start()

-- Poll faster than the thing being watched and repaint only on change -- a
-- 1000ms poll against a source that changes on its own misses updates
-- (docs/APP_CONTRACT.md, timer section).
local last_status, last_ip

timer.every(250, function()
    if scanning then
        local nets = wifi.scan_results()
        if nets then
            scanning = false
            render(nets)
        end
    end

    local st = wifi.status()
    local ip = wifi.ip()
    if st == last_status and ip == last_ip then return end
    last_status, last_ip = st, ip

    if st == "connected" then
        status:set_text("connected  " ..
            (wifi.time_synced() and "clock synced" or (ip or "")))
    elseif st == "connecting" then
        status:set_text("connecting...")
    elseif st == "retrying" then
        status:set_text("retrying - " .. (wifi.error() or "network not found"))
    elseif st == "failed" then
        status:set_text("failed - " .. (wifi.error() or "check the password"))
    else
        status:set_text("not connected")
    end
end)

scr:load()
