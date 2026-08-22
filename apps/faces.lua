-- Watch faces. Swipe between four takes on the same time.
--
--   Analog  hands drawn with lines, updated in place via line:set_points
--   Rings   hour/minute/second as concentric arcs
--   Words   the time spelled out, read at a glance
--   Minimal one enormous hour, the minute small beneath it
--
-- Timezone comes from /sdcard/tz.txt, written by apps/clock.lua -- one
-- setting, shared, rather than each face asking again.

local lvgl = require("lvgl")
local rtc = require("rtc")
local battery = require("battery")
local timer = require("timer")

lvgl.init({ buffer_lines = 40 })

-- ---------------------------------------------------------------- zone

local ZONES = {
    ["Honolulu"]=-600, ["Anchorage"]=-540, ["Los Angeles"]=-480, ["Denver"]=-420,
    ["Mexico City"]=-360, ["Chicago"]=-360, ["New York"]=-300, ["Toronto"]=-300,
    ["Santiago"]=-240, ["Sao Paulo"]=-180, ["London"]=0, ["Lisbon"]=0,
    ["Berlin"]=60, ["Paris"]=60, ["Lagos"]=60, ["Athens"]=120, ["Cairo"]=120,
    ["Johannesburg"]=120, ["Moscow"]=180, ["Nairobi"]=180, ["Dubai"]=240,
    ["Karachi"]=300, ["Delhi"]=330, ["Kathmandu"]=345, ["Dhaka"]=360,
    ["Bangkok"]=420, ["Jakarta"]=420, ["Singapore"]=480, ["Beijing"]=480,
    ["Hong Kong"]=480, ["Tokyo"]=540, ["Seoul"]=540, ["Adelaide"]=570,
    ["Sydney"]=600, ["Auckland"]=720,
}

local offset = 0
do
    local f = io.open("/sdcard/tz.txt", "r")
    if f then
        local name = f:read("*l")
        local dst = f:read("*l")
        f:close()
        offset = (ZONES[name or ""] or 0) + (dst == "1" and 60 or 0)
    end
end

local function local_time()
    local t = rtc.now()
    if not t then return nil end
    local total = t.hour * 60 + t.min + offset
    while total < 0 do total = total + 1440 end
    while total >= 1440 do total = total - 1440 end
    return { hour = total // 60, min = total % 60, sec = t.sec,
             day = t.day, wday = t.wday, month = t.month }
end

-- ---------------------------------------------------------------- setup

local scr = lvgl.create_screen()
scr:set_style({ bg_color = "#000000" })

local tv = lvgl.tileview(scr, {})
tv:set_style({ bg_color = "#000000", bg_opa = 0 })
tv:set_scroll({ scrollbar = "off" })

local p_analog  = tv:add_tile(1, 1, "right")
local p_rings   = tv:add_tile(2, 1, "hor")
local p_words   = tv:add_tile(3, 1, "hor")
local p_minimal = tv:add_tile(4, 1, "left")

local CX, CY = 184, 224          -- panel centre
-- Points are {x=,y=} tables, not {x,y} pairs -- the binding reads named
-- fields, so an array pair silently yields (0,0): a zero-length line
-- that is present, invisible, and raises nothing. That is what made the
-- first analog face render pure black.
local function polar(r, deg)
    local a = math.rad(deg - 90)  -- 0 deg = 12 o'clock
    return { x = math.floor(CX + r * math.cos(a) + 0.5),
             y = math.floor(CY + r * math.sin(a) + 0.5) }
end

-- --------------------------------------------------- complications
-- Wear OS treats these as a core principle: a face should let you glance
-- at what you care about, not only the time. Date and battery are what
-- this device can honestly offer today. Dim and small -- the time stays
-- the subject, and the screen stays mostly black (their power rule).
local MONTHS = { "Jan","Feb","Mar","Apr","May","Jun",
                 "Jul","Aug","Sep","Oct","Nov","Dec" }
local DAYS = { [0]="Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" }

local function complications(parent, y_date, y_bat)
    return {
        date = lvgl.label(parent, { text = "", align = "center", y = y_date,
            text_color = "#8A8A99", font = lvgl.font(26) }),
        bat = lvgl.label(parent, { text = "", align = "center", y = y_bat,
            text_color = "#8A8A99", font = lvgl.font(26) }),
    }
end

local function update_complications(c, t)
    c.date:set_text(string.format("%s %d %s",
        DAYS[t.wday] or "?", t.day, MONTHS[t.month] or "?"))
    local pct = battery.percent()
    if pct then
        local icon = lvgl.symbol.battery_full
        if pct < 20 then icon = lvgl.symbol.battery_empty
        elseif pct < 50 then icon = lvgl.symbol.battery_2 end
        c.bat:set_text(string.format("%s %d%%", icon, pct))
    end
end

-- ---------------------------------------------------------- 1: analog

-- Tick marks are static, so they are built once.
for i = 0, 11 do
    local long = (i % 3 == 0)
    lvgl.line(p_analog, {
        x = 0, y = 0, w = 368, h = 448,
        points = { polar(long and 136 or 144, i * 30), polar(155, i * 30) },
        line_color = long and "#FFFFFF" or "#4A4A57",
        line_width = long and 6 or 3,
    })
end

-- Each hand owns the whole panel: an LVGL line sizes to its points'
-- bounding box otherwise, and panel coordinates would be clipped.
local function hand(w, colour)
    return lvgl.line(p_analog, {
        x = 0, y = 0, w = 368, h = 448,
        points = { { x = CX, y = CY }, { x = CX, y = CY - 10 } },
        line_color = colour, line_width = w,
    })
end
local comp_analog = complications(p_analog, -74, 82)
local hand_h = hand(10, "#FFFFFF")
local hand_m = hand(6,  "#FFFFFF")
local hand_s = hand(3,  "#2F80ED")

-- ----------------------------------------------------------- 2: rings

local function ring(parent, r, w, colour)
    return lvgl.arc(parent, {
        min = 0, max = 1000, value = 0,
        align = "center", w = r * 2, h = r * 2,
        arc_width = w,
        line_color = colour,   -- now reaches the arc indicator
    })
end

local ring_h = ring(p_rings, 160, 18, "#2F80ED")
local ring_m = ring(p_rings, 126, 18, "#8A8A99")
local ring_s = ring(p_rings, 92, 10, "#B3261E")
-- Display-only: an interactive arc swallows horizontal drags, so the
-- face could not be swiped past (the drag-vs-paging rule, in practice).
ring_h:set_clickable(false)
ring_m:set_clickable(false)
ring_s:set_clickable(false)
local rings_lbl = lvgl.label(p_rings, { text = "--:--", align = "center",
    text_color = "#FFFFFF", font = lvgl.font(40) })
local comp_rings = complications(p_rings, 42, 76)

-- ------------------------------------------------------------ 3: words

local words_lbl = lvgl.label(p_words, {
    text = "", align = "center", w = 330,
    text_color = "#FFFFFF", font = lvgl.font(48),
})

local comp_words = complications(p_words, 124, 158)

local ONES = { "twelve","one","two","three","four","five",
               "six","seven","eight","nine","ten","eleven" }
local MINS = { [0]="o'clock", "five past", "ten past", "quarter past",
               "twenty past", "twenty-five past", "half past",
               "twenty-five to", "twenty to", "quarter to", "ten to", "five to" }

local function in_words(h, m)
    local slot = math.floor((m + 2) / 5) % 12
    local hour = h % 12
    if slot >= 7 then hour = (hour + 1) % 12 end     -- "...to" the next hour
    if slot == 0 then return ONES[hour + 1] .. "\n" .. MINS[0] end
    return MINS[slot] .. "\n" .. ONES[hour + 1]
end

-- ---------------------------------------------------------- 4: minimal

local min_hour = lvgl.label(p_minimal, { text = "--", align = "center", y = -30,
    text_color = "#FFFFFF", font = lvgl.font(60) })
local min_min = lvgl.label(p_minimal, { text = "--", align = "center", y = 60,
    text_color = "#8A8A99", font = lvgl.font(40) })
local comp_minimal = complications(p_minimal, 134, 168)

-- ------------------------------------------------------------- update

local function tick()
    local t = local_time()
    if not t then
        words_lbl:set_text("clock\nnot set")
        return
    end

    -- analog: hands move continuously, not in steps
    local sd = t.sec * 6
    local md = t.min * 6 + t.sec * 0.1
    local hd = (t.hour % 12) * 30 + t.min * 0.5
    hand_h:set_points({ polar(-20, hd), polar(90, hd) })
    hand_m:set_points({ polar(-25, md), polar(135, md) })
    hand_s:set_points({ polar(-30, sd), polar(150, sd) })

    -- rings
    ring_h:set_value(math.floor(((t.hour % 12) * 60 + t.min) / 720 * 1000))
    ring_m:set_value(math.floor(t.min / 60 * 1000))
    ring_s:set_value(math.floor(t.sec / 60 * 1000))
    rings_lbl:set_text(string.format("%02d:%02d", t.hour, t.min))

    -- words
    words_lbl:set_text(in_words(t.hour, t.min))

    -- minimal
    min_hour:set_text(string.format("%02d", t.hour))
    min_min:set_text(string.format("%02d", t.min))

    for _, c in ipairs({ comp_analog, comp_rings, comp_words, comp_minimal }) do
        update_complications(c, t)
    end
end

tick()
timer.every(1000, tick)
scr:load()
