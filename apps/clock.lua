-- Clock -- the device's watch face. Reference app for the RTC, the hero
-- font, and reading the battery.
--
-- Timezone matters here: NTP sets the RTC in UTC, so a naive clock shows
-- the wrong time everywhere except Greenwich. Pick a city (nobody thinks
-- in UTC offsets); the choice is saved to the card and survives reboots.

local lvgl = require("lvgl")
local rtc = require("rtc")
local battery = require("battery")
local timer = require("timer")
local ui = require("ui")

lvgl.init({ buffer_lines = 40 })

local TZ_PATH = "/sdcard/tz.txt"

-- Offsets are STANDARD time in minutes -- minutes, not hours, because
-- India, parts of Australia and Nepal are not on whole-hour offsets and
-- a whole-hour table would simply have to omit them. Daylight saving is
-- a separate toggle: it cannot be derived from an offset, and its rules
-- differ per country and change by legislation, so the honest move is to
-- let the wearer say.
local ZONES = {
    { "Honolulu",     -600 },
    { "Anchorage",    -540 },
    { "Los Angeles",  -480 },
    { "Denver",       -420 },
    { "Mexico City",  -360 },
    { "Chicago",      -360 },
    { "New York",     -300 },
    { "Toronto",      -300 },
    { "Santiago",     -240 },
    { "Sao Paulo",    -180 },
    { "London",          0 },
    { "Lisbon",          0 },
    { "Berlin",         60 },
    { "Paris",          60 },
    { "Lagos",          60 },
    { "Athens",        120 },
    { "Cairo",         120 },
    { "Johannesburg",  120 },
    { "Moscow",        180 },
    { "Nairobi",       180 },
    { "Dubai",         240 },
    { "Karachi",       300 },
    { "Delhi",         330 },
    { "Kathmandu",     345 },
    { "Dhaka",         360 },
    { "Bangkok",       420 },
    { "Jakarta",       420 },
    { "Singapore",     480 },
    { "Beijing",       480 },
    { "Hong Kong",     480 },
    { "Tokyo",         540 },
    { "Seoul",         540 },
    { "Adelaide",      570 },
    { "Sydney",        600 },
    { "Auckland",      720 },
}

local zone_index = 11   -- London: offset 0, so an unset clock reads as UTC
local dst = false

local function load_tz()
    local f = io.open(TZ_PATH, "r")
    if not f then return end
    local name = f:read("*l")
    local d = f:read("*l")
    f:close()
    for i, z in ipairs(ZONES) do
        if z[1] == name then zone_index = i break end
    end
    dst = (d == "1")
end

local function save_tz()
    local f = io.open(TZ_PATH, "w")
    if f then
        f:write(ZONES[zone_index][1] .. "\n" .. (dst and "1" or "0") .. "\n")
        f:close()
    end
end

load_tz()

local function offset_minutes()
    return ZONES[zone_index][2] + (dst and 60 or 0)
end

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
    text_color = "#A0A0AE",
})

local zone_lbl = lvgl.label(scr, {
    text = "",
    align = "center", y = 74,
    text_color = "#A0A0AE",
    font = lvgl.font(26),
})

local bat_lbl = lvgl.label(scr, {
    text = "",
    align = "bottom_mid", y = -20,
    text_color = "#A0A0AE",
})

local hint = lvgl.label(scr, {
    text = "",
    align = "top_mid", y = 24,
    text_color = "#A0A0AE",
    font = lvgl.font(26),
})

local DAYS = { [0]="Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" }
local MONTHS = { "Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec" }

local function leap(y) return (y % 4 == 0 and y % 100 ~= 0) or y % 400 == 0 end
local function days_in(m, y)
    local d = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 }
    if m == 2 and leap(y) then return 29 end
    return d[m]
end

-- Shift a UTC reading by an offset in minutes, rolling the date (and the
-- month, and the year) properly rather than only the hour.
local function shifted(t, mins)
    local total = t.hour * 60 + t.min + mins
    local day, wday, month, year = t.day, t.wday, t.month, t.year

    while total < 0 do
        total = total + 1440
        day, wday = day - 1, (wday + 6) % 7
        if day < 1 then
            month = month - 1
            if month < 1 then month, year = 12, year - 1 end
            day = days_in(month, year)
        end
    end
    while total >= 1440 do
        total = total - 1440
        day, wday = day + 1, (wday + 1) % 7
        if day > days_in(month, year) then
            day, month = 1, month + 1
            if month > 12 then month, year = 1, year + 1 end
        end
    end

    return { hour = total // 60, min = total % 60, sec = t.sec,
             day = day, wday = wday, month = month, year = year }
end

-- Poll faster than the RTC's 1 Hz edge and repaint only on change: a 1000 ms
-- periodic timer re-arms after its callback, so it runs slightly slower than
-- the clock it samples and silently drops a second now and then.
local last_sec = -1

local function tick()
    local t, err = rtc.now()
    if not t then
        time_lbl:set_text("--:--")
        date_lbl:set_text("")
        zone_lbl:set_text("")
        hint:set_text(err == "rtc not set" and "clock not set - join wifi" or tostring(err))
        return
    end
    hint:set_text("")

    if t.sec == last_sec then return end
    last_sec = t.sec

    local l = shifted(t, offset_minutes())
    time_lbl:set_text(string.format("%02d:%02d", l.hour, l.min))
    date_lbl:set_text(string.format("%s %d %s   :%02d",
        DAYS[l.wday] or "?", l.day, MONTHS[l.month] or "?", l.sec))
    zone_lbl:set_text(ZONES[zone_index][1] .. (dst and "  DST" or ""))

    local pct = battery.percent()
    if pct then
        local icon = lvgl.symbol.battery_full
        if pct < 20 then icon = lvgl.symbol.battery_empty
        elseif pct < 50 then icon = lvgl.symbol.battery_2 end
        bat_lbl:set_text(string.format("%s %d%%%s", icon, pct,
            battery.charging() and "  " .. lvgl.symbol.charge or ""))
    else
        bat_lbl:set_text("")
    end
end

-- Zone screen: pick a city, toggle daylight saving.
ui.corner_button(scr, {
    text = lvgl.symbol.settings,
    align = "top_right", x = -4, y = 4,
    on_click = function()
        local caller = lvgl.active_screen()
        local s = lvgl.create_screen()
        s:set_style({ bg_color = "#000000" })

        local city_row, dst_row

        ui.header(s, {
            title = "Zone",
            on_back = function()
                save_tz()
                caller:load()
                s:delete()
                tick()
            end,
        })

        local list = lvgl.container(s, {
            x = 0, y = 108, w = 368, h = 300,
            bg_opa = 0, border_width = 0, pad = 12,
        })
        list:set_flex({ flow = "column", pad_row = 16 })

        local function city_text()
            local z = ZONES[zone_index]
            local sign = z[2] < 0 and "-" or "+"
            local a = math.abs(z[2])
            return string.format("%s  UTC%s%d:%02d", z[1], sign, a // 60, a % 60)
        end

        city_row = ui.row(list, {
            text = city_text(), kind = "nav",
            on_click = function()
                local names = {}
                for i, z in ipairs(ZONES) do
                    local sign = z[2] < 0 and "-" or "+"
                    local a = math.abs(z[2])
                    names[i] = string.format("%s  UTC%s%d:%02d", z[1], sign, a // 60, a % 60)
                end
                ui.picker({ title = "City", options = names, selected = zone_index },
                    function(i)
                        if i then
                            zone_index = i
                            city_row.label:set_text(city_text())
                        end
                    end)
            end,
        })

        dst_row = ui.row(list, {
            text = "Daylight saving", kind = "toggle", checked = dst,
            on_change = function() dst = not dst end,
        })

        s:load()
    end,
})

tick()
timer.every(250, tick)
scr:load()
