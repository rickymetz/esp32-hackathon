-- Clock -- the device's watch face. Reference app for the RTC, the hero
-- font, and reading the battery.
--
-- Timezone matters here: NTP sets the RTC in UTC, so a naive clock shows
-- the wrong time everywhere except Greenwich. The offset is set on the
-- device (tap the corner) and saved to the card, so it survives reboots.

local lvgl = require("lvgl")
local rtc = require("rtc")
local battery = require("battery")
local timer = require("timer")
local ui = require("ui")

lvgl.init({ buffer_lines = 40 })

local TZ_PATH = "/sdcard/tz.txt"

local function load_tz()
    local f = io.open(TZ_PATH, "r")
    if not f then return 0 end
    local v = tonumber(f:read("*l") or "0") or 0
    f:close()
    return v
end

local function save_tz(v)
    local f = io.open(TZ_PATH, "w")
    if f then f:write(tostring(v) .. "\n"); f:close() end
end

local tz = load_tz()

local scr = lvgl.create_screen()
scr:set_style({ bg_color = "#000000" })

local time_lbl = lvgl.label(scr, {
    text = "--:--",
    align = "center", y = -30,
    text_color = "#FFFFFF",
    font = lvgl.font(60),
})

local date_lbl = lvgl.label(scr, {
    text = "",
    align = "center", y = 30,
    text_color = "#8A8A99",
})

local bat_lbl = lvgl.label(scr, {
    text = "",
    align = "bottom_mid", y = -20,
    text_color = "#8A8A99",
})

local hint = lvgl.label(scr, {
    text = "",
    align = "top_mid", y = 24,
    text_color = "#8A8A99",
    font = lvgl.font(26),
})

local DAYS = { [0]="Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" }
local MONTHS = { "Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec" }

-- Shift a UTC reading by the offset. Only hours are adjusted, and the
-- date rolls with them -- enough for whole-hour zones, which is all the
-- stepper offers.
local function shifted(t, hours)
    local h = t.hour + hours
    local day, wday, month, year = t.day, t.wday, t.month, t.year
    if h < 0 then
        h = h + 24
        day = day - 1
        wday = (wday + 6) % 7
    elseif h > 23 then
        h = h - 24
        day = day + 1
        wday = (wday + 1) % 7
    end
    return { hour = h, min = t.min, sec = t.sec,
             day = day, wday = wday, month = month, year = year }
end

local function tick()
    local t, err = rtc.now()
    if not t then
        time_lbl:set_text("--:--")
        date_lbl:set_text("")
        hint:set_text(err == "rtc not set" and "clock not set - join wifi" or tostring(err))
        return
    end
    hint:set_text("")

    local l = shifted(t, tz)
    time_lbl:set_text(string.format("%02d:%02d", l.hour, l.min))
    date_lbl:set_text(string.format("%s %d %s   :%02d",
        DAYS[l.wday] or "?", l.day, MONTHS[l.month] or "?", l.sec))

    local pct = battery.percent()
    local icon = lvgl.symbol.battery_full
    if pct then
        if pct < 20 then icon = lvgl.symbol.battery_empty
        elseif pct < 50 then icon = lvgl.symbol.battery_2 end
        bat_lbl:set_text(string.format("%s %d%%%s", icon, pct,
            battery.charging() and "  " .. lvgl.symbol.charge or ""))
    else
        bat_lbl:set_text("")
    end
end

-- Timezone screen: a stepper, because whole-hour offsets are exactly the
-- clamped +/- case it exists for.
ui.corner_button(scr, {
    text = lvgl.symbol.settings,
    align = "top_right", x = -4, y = 4,
    on_click = function()
        local caller = lvgl.active_screen()
        local s = lvgl.create_screen()
        s:set_style({ bg_color = "#000000" })

        ui.header(s, {
            title = "Zone",
            on_back = function()
                save_tz(tz)
                caller:load()
                s:delete()
                tick()
            end,
        })

        local st = ui.stepper(s, { min = -12, max = 14, step = 1, value = tz,
                                   label = "UTC%+d" },
            function(v) tz = v end)
        st.row:align("center", 0, 0)

        lvgl.label(s, {
            text = "NTP sets the clock in UTC.\nSet your offset here.",
            align = "bottom_mid", y = -40,
            text_color = "#8A8A99",
            font = lvgl.font(26),
            w = 320,
        })

        s:load()
    end,
})

tick()
timer.every(1000, tick)
scr:load()
