-- Watch faces. Swipe between four takes on the same time.
--
--   Analog  hands with a real pinion, a 60-minute track, a unique 12
--   Rings   hour/minute/second as concentric arcs on a true 360 dial
--   Words   the time spelled out
--   Minimal one enormous hour over its minute
--
-- Rewritten after a three-persona review (horology, Wear OS, and
-- accessibility). Several comments below mark traps rather than
-- preferences -- they cost real debugging.
--
-- Timezone and date arithmetic come from `ui`, shared with clock.lua:
-- two copies of that logic disagreed, and one showed the UTC date
-- beside a local time.

local lvgl = require("lvgl")
local rtc = require("rtc")
local battery = require("battery")
local timer = require("timer")
local ui = require("ui")

lvgl.init({ buffer_lines = 40 })

local _, _, TZ_OFFSET = ui.zone()

local ACCENT = "#2F80ED"          -- reserved for SECONDS on every face
local DIM    = "#A0A0AE"          -- 8.1:1 caption token
local TRACK  = "#2A2A33"          -- unlit arc / faint tick
local TICK   = "#7A7A88"          -- 5.0:1, clears the 3:1 non-text floor
local FACE_PATH = "/sdcard/face.txt"

local scr = lvgl.create_screen()
scr:set_style({ bg_color = "#000000" })

local tv = lvgl.tileview(scr, {})
tv:set_style({ bg_color = "#000000", bg_opa = 0 })
tv:set_scroll({ scrollbar = "off" })

local p_analog  = tv:add_tile(1, 1, "right")
local p_rings   = tv:add_tile(2, 1, "hor")
local p_words   = tv:add_tile(3, 1, "hor")
local p_minimal = tv:add_tile(4, 1, "left")

local CX, CY = 184, 224

-- Points are {x=,y=} tables. An array pair silently yields (0,0) -- a
-- zero-length line that is present, invisible, and raises nothing.
local function polar(r, deg)
    local a = math.rad(deg - 90)
    return { x = math.floor(CX + r * math.cos(a) + 0.5),
             y = math.floor(CY + r * math.sin(a) + 0.5) }
end

-- ----------------------------------------------------- complications
-- In the panel's top and bottom bands, OUTSIDE the dial. They used to
-- sit inside it, where the hands are z-ordered above them and crossed
-- the date every hour. Those bands were dead black anyway, so this also
-- stops the layout pretending the panel is round.
-- y_bat stays clear of ui.dots at the panel bottom.
local function complications(parent, y_date, y_bat)
    return {
        date = lvgl.label(parent, { text = "", align = "center", y = y_date,
            text_color = DIM, font = lvgl.font(26) }),
        bat = lvgl.label(parent, { text = "", align = "center", y = y_bat,
            text_color = DIM, font = lvgl.font(26) }),
    }
end

local function update_complications(c, t)
    c.date:set_text(string.format("%s %d %s",
        ui.DAYS[t.wday] or "?", t.day, ui.MONTHS[t.month] or "?"))
    local pct = battery.percent()
    if pct then
        local icon = lvgl.symbol.battery_full
        if pct < 20 then icon = lvgl.symbol.battery_empty
        elseif pct < 50 then icon = lvgl.symbol.battery_2 end
        c.bat:set_text(string.format("%s %d%%%s", icon, pct,
            battery.charging() and " " .. lvgl.symbol.charge or ""))
    end
end

-- ---------------------------------------------------------- 1: analog

-- Three tick tiers. There were only 12 marks and no minute track, so
-- the minute hand had nothing to be read against: a precision hand on
-- an approximate dial.
for i = 0, 59 do
    local is_hour = (i % 5 == 0)
    -- 12 o'clock is drawn separately as a doubled bar below; without
    -- this skip it renders as three bars, not two.
    if i ~= 0 then
    local is_quarter = (i % 15 == 0)
    local r1 = is_quarter and 134 or (is_hour and 140 or 148)
    lvgl.line(p_analog, {
        x = 0, y = 0, w = 368, h = 448,
        points = { polar(r1, i * 6), polar(155, i * 6) },
        line_color = is_hour and (is_quarter and "#FFFFFF" or TICK) or TRACK,
        line_width = is_quarter and 6 or (is_hour and 4 or 2),
    })
    end
end

-- The 12 is doubled: previously 12 and 6 were identical, so a
-- half-glimpsed dial had no orientation anchor.
for _, dx in ipairs({ -7, 7 }) do
    lvgl.line(p_analog, {
        x = 0, y = 0, w = 368, h = 448,
        points = { { x = CX + dx, y = CY - 134 }, { x = CX + dx, y = CY - 155 } },
        line_color = "#FFFFFF", line_width = 6,
    })
end

local comp_analog = complications(p_analog, -186, 176)

-- Each hand is two segments: a narrow tail and the full-width body.
-- lv_line draws a constant-width slab, so full-width tails read as a
-- phantom fourth hand, and their convergence looked like a pivot 19px
-- off true centre.
local function hand(tail_r, tip_r, w, colour)
    local body = lvgl.line(p_analog, {
        x = 0, y = 0, w = 368, h = 448,
        points = { { x = CX, y = CY }, { x = CX, y = CY - tip_r } },
        line_color = colour, line_width = w,
    })
    local tail = lvgl.line(p_analog, {
        x = 0, y = 0, w = 368, h = 448,
        points = { { x = CX, y = CY }, { x = CX, y = CY + tail_r } },
        line_color = colour, line_width = math.max(2, w // 3),
    })
    return { body = body, tail = tail, tail_r = tail_r, tip_r = tip_r }
end

-- Hour and minute now differ in colour as well as width and length.
-- Two white bars 21 degrees apart were genuinely ambiguous, and 4px of
-- width difference is at the limit of acuity at this viewing distance.
local hand_h = hand(14, 96, 14, "#FFFFFF")
local hand_m = hand(16, 142, 6, "#C8C8D4")
local hand_s = hand(18, 150, 3, ACCENT)

local function point_hand(h, deg)
    h.body:set_points({ polar(0, deg), polar(h.tip_r, deg) })
    h.tail:set_points({ polar(0, deg), polar(-h.tail_r, deg) })
end

-- The pinion, created last so it draws above every hand. Without it the
-- three tails merge into a wedge and the eye reads that as the centre.
local pinion = lvgl.container(p_analog, {
    align = "center", w = 22, h = 22,
    bg_color = "#FFFFFF", bg_opa = 255, radius = 11, border_width = 0,
})
pinion:set_clickable(false)
local pinion_dot = lvgl.container(p_analog, {
    align = "center", w = 8, h = 8,
    bg_color = "#000000", bg_opa = 255, radius = 4, border_width = 0,
})
pinion_dot:set_clickable(false)

-- ----------------------------------------------------------- 2: rings

-- A true 360 dial starting at 12 o'clock. LVGL's default arc range is
-- 135..45 -- a 270 degree sweep with the gap at the bottom, the visual
-- signature of a car tachometer -- so ring position did not correspond
-- to hand position at all. track_color makes the unlit remainder
-- visible; without it the value arc and its background were the same
-- colour and a ring at 1% looked identical to one at 100%.
local function ring(parent, r, w, colour)
    local a = lvgl.arc(parent, {
        min = 0, max = 1000, value = 0,
        align = "center", w = r * 2, h = r * 2,
        arc_width = w,
        line_color = colour,
        track_color = TRACK,
        -- Full circle with 0 at twelve o'clock. Angles must be within
        -- 0..360: an earlier 270..630 computed a zero-width span and
        -- collapsed every ring to its knob dot.
        bg_start_angle = 0, bg_end_angle = 360, rotation = 270,
    })
    a:set_clickable(false)
    return a
end

local ring_h = ring(p_rings, 152, 16, "#FFFFFF")
local ring_m = ring(p_rings, 124, 14, "#C8C8D4")
local ring_s = ring(p_rings, 98, 6, ACCENT)

local rings_lbl = lvgl.label(p_rings, { text = "--:--", align = "center",
    text_color = "#FFFFFF", font = lvgl.font(60) })
local comp_rings = complications(p_rings, -186, 176)

-- ------------------------------------------------------------ 3: words

local words_lbl = lvgl.label(p_words, {
    text = "", x = 24, y = -20, align = "left_mid", w = 320,
    text_color = "#FFFFFF", font = lvgl.font(40),
})
local words_ampm = lvgl.label(p_words, {
    text = "", x = 24, y = 96, align = "left_mid",
    text_color = DIM, font = lvgl.font(26),
})
local comp_words = complications(p_words, -186, 176)

local ONES = { "twelve","one","two","three","four","five",
               "six","seven","eight","nine","ten","eleven" }
-- "25" rather than "twenty-five": spelled out at this size it needed
-- ~400px against a 320px box and silently reflowed to a third line for
-- ten minutes of every hour, so the face changed shape as you watched.
local MINS = { [0]="o'clock", "five past", "ten past", "quarter past",
               "twenty past", "25 past", "half past",
               "25 to", "twenty to", "quarter to", "ten to", "five to" }

local function in_words(h, m)
    -- The rollover is tested on the UNWRAPPED slot. Previously `% 12`
    -- collapsed slot 12 to 0 first, so the "...to the next hour" branch
    -- never ran at :58 or :59 and the face read a whole hour early.
    local raw = math.floor((m + 2) / 5)
    local slot = raw % 12
    local hour = h % 12
    if raw >= 7 then hour = (hour + 1) % 12 end
    if slot == 0 then return ONES[hour + 1] .. "\n" .. MINS[0] end
    return MINS[slot] .. "\n" .. ONES[hour + 1]
end

-- ---------------------------------------------------------- 4: minimal

-- 120px hero. The minute is WHITE and close to the hour: it was grey at
-- nearly the date's size, so half the time read as a caption, and equal
-- gaps above and below grouped it with the date rather than the hour.
local min_hour = lvgl.label(p_minimal, { text = "--", align = "center", y = -66,
    text_color = "#FFFFFF", font = lvgl.font(120) })
local min_min = lvgl.label(p_minimal, { text = "--", align = "center", y = 30,
    text_color = "#FFFFFF", font = lvgl.font(60) })
local comp_minimal = complications(p_minimal, -186, 176)

-- ---------------------------------------------------------------- dots

ui.dots(scr, tv, { count = 4 })

-- -------------------------------------------------------------- update

local COMPS = { comp_analog, comp_rings, comp_words, comp_minimal }
local last_min, last_page = -1, -1

local function tick()
    local raw = rtc.now()
    if not raw then
        words_lbl:set_text("clock\nnot set")
        return
    end
    local t = ui.shift(raw, TZ_OFFSET)
    local page = tv:get_active_index() or 1

    if page == 1 then
        point_hand(hand_h, (t.hour % 12) * 30 + t.min * 0.5)
        point_hand(hand_m, t.min * 6 + t.sec * 0.1)
        point_hand(hand_s, t.sec * 6)
    elseif page == 2 then
        ring_h:set_value(math.floor(((t.hour % 12) * 60 + t.min) / 720 * 1000))
        ring_m:set_value(math.floor(t.min / 60 * 1000))
        ring_s:set_value(math.floor(t.sec / 60 * 1000))
        rings_lbl:set_text(string.format("%02d:%02d", t.hour, t.min))
    elseif page == 3 then
        words_lbl:set_text(in_words(t.hour, t.min))
        words_ampm:set_text(t.hour < 12 and "AM" or "PM")
    else
        min_hour:set_text(string.format("%02d", t.hour))
        min_min:set_text(string.format("%02d", t.min))
    end

    -- Complications change once a minute, and only for the visible tile.
    -- The old version redrew all four every second and polled the
    -- battery over I2C four times a second, three quarters of it
    -- offscreen.
    -- Repaint on a page change as well as a minute change. Gating on
    -- the minute alone lost a page's complications entirely: a chain of
    -- swipes could set last_min while a different tile was active, and
    -- the tile you landed on then never repainted for a full minute.
    if t.min ~= last_min or page ~= last_page then
        last_min, last_page = t.min, page
        update_complications(COMPS[page], t)
    end
end

-- Remember the chosen face, the way a watch does.
tv:on("value_changed", function()
    last_page = -1
    timer.after(400, function()
        local p = tv:get_active_index()
        if p then
            local f = io.open(FACE_PATH, "w")
            if f then f:write(tostring(p)); f:close() end
        end
        tick()
    end)
end)

do
    local f = io.open(FACE_PATH, "r")
    if f then
        local p = tonumber(f:read("*l") or "1")
        f:close()
        if p and p >= 1 and p <= 4 then tv:set_tile_by_index(p, 1, false) end
    end
end

tick()
timer.every(1000, tick)
scr:load()
