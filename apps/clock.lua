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

-- Timezone data, persistence and the date-rolling shift all live in the ui
-- module: faces.lua reads the same /sdcard/tz.txt, so picking a city here
-- moves the watch faces too. Duplicating the table would let the two drift.
local ZONES = ui.ZONES
local zone_index, dst = ui.zone()

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

local DAYS, MONTHS = ui.DAYS, ui.MONTHS

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

    local l = ui.shift(t, offset_minutes())
    time_lbl:set_text(string.format("%02d:%02d", l.hour, l.min))
    -- Just the date under the time: a floating ":07" of seconds read as a stray
    -- fragment. (Sampling still keys on t.sec to catch the minute rollover.)
    date_lbl:set_text(string.format("%s %d %s",
        DAYS[l.wday] or "?", l.day, MONTHS[l.month] or "?"))
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
                ui.save_zone(zone_index, dst)
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

        -- "UTC+0", not "UTC+0:00" -- the :00 pushed "London  UTC+0:00" onto a
        -- second line in the row. Only the handful of half-hour zones (Delhi,
        -- Kathmandu, Adelaide) need the minutes at all.
        local function utc_label(mins)
            local sign = mins < 0 and "-" or "+"
            local a = math.abs(mins)
            if a % 60 == 0 then
                return string.format("UTC%s%d", sign, a // 60)
            end
            return string.format("UTC%s%d:%02d", sign, a // 60, a % 60)
        end

        local function city_text()
            local z = ZONES[zone_index]
            return string.format("%s  %s", z[1], utc_label(z[2]))
        end

        city_row = ui.row(list, {
            text = city_text(), kind = "nav",
            on_click = function()
                local names = {}
                for i, z in ipairs(ZONES) do
                    names[i] = string.format("%s  %s", z[1], utc_label(z[2]))
                end
                ui.picker({ title = "City", options = names, selected = zone_index,
                            size = 26 },
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
