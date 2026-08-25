-- ui: shared UI primitives with the design guide's sizes baked in.
-- Ships inside the launcher (apps cannot require() files off the card).
-- Pure Lua over the lvgl/timer bindings; see docs/APP_CONTRACT.md.

local lvgl = require("lvgl")
local timer = require("timer")

local M = {}

local ROW_H = 104          -- design guide: preferred row height
local TARGET = 88          -- design guide: minimum tappable square

-- The eight faces lvgl.font() will accept. Anything else raises.
local FONT_SIZES = { 24, 26, 32, 40, 48, 60, 72, 120 }

-- Chrome vs content.
--
-- A user-set font scale multiplies every lvgl.font() request, which is right
-- for content -- the number on a stopwatch, the body of a note -- and wrong
-- for the text that labels a FIXED-SIZE box. A header title, a row label and a
-- stepper readout all sit in a container whose height the design guide pins
-- (ROW_H is 104 because that is the tap target, not because of the text), so
-- when they grow the text leaves the box: at 1.3 the "Date & time" title
-- wrapped and was clipped by the list under it, row labels overflowed their
-- 104px card, and "Volume 70%" was eaten at both ends by its own +/- slabs.
--
-- So chrome caps its RENDERED size at the unscaled nominal. Below 1.0 the user
-- asked for smaller and nothing can overflow, so it is honoured as-is; above
-- 1.0 we step down to the largest face that still renders no larger than the
-- nominal did at 1.0. At exactly 1.0 this is the identity, so no existing
-- layout moves.
--
-- Content keeps using lvgl.font() directly and scales all the way to 1.3.
local function chrome_font(nominal)
    local scale = lvgl.font_scale() or 1.0
    if scale <= 1.0 then
        return lvgl.font(nominal)
    end
    local pick = FONT_SIZES[1]
    for _, size in ipairs(FONT_SIZES) do
        if size * scale <= nominal and size <= nominal then pick = size end
    end
    return lvgl.font(pick)
end

M.chrome_font = chrome_font

-- Largest chrome face whose rendered text actually FITS a given width.
--
-- chrome_font alone only caps GROWTH, so it fixed the scale>1.0 cases and left
-- a long string broken at 1.0: "Display & sound" at the 40px header face is
-- ~300px against 256px of room, so it wrapped and was clipped by the list
-- below -- at the default scale, on a shipped page, with no golden covering
-- it. Length matters as much as scale, and only one of the two was handled.
--
-- Lexend's average advance is ~0.55em; the 0.64 here is that plus margin, and
-- it only ever errs toward a smaller face. Exact metrics would need a
-- measure-text binding, which is not worth adding for chrome.
local function chrome_fit(text, avail_w, nominal)
    local n = #(text or "")
    if n == 0 then return chrome_font(nominal) end
    local scale = lvgl.font_scale() or 1.0
    local pick = FONT_SIZES[1]
    for _, size in ipairs(FONT_SIZES) do
        -- BOTH predicates. chrome_fit is chrome_font PLUS a width test, not
        -- instead of it: dropping the growth cap let a SHORT title sail past
        -- the width check and render at the full nominal, so at scale 1.3
        -- ui.header{title="Wi-Fi"} picked 40 (~52px) where chrome_font picked
        -- 26 (~34px) -- reintroducing the exact overflow chrome_font exists to
        -- prevent, for every short chrome title.
        if size <= nominal and (size * scale) <= nominal
           and (size * scale) * 0.64 * n <= avail_w then
            pick = size
        end
    end
    return lvgl.font(pick)
end

M.chrome_fit = chrome_fit

-- A corner control drawn at watch scale but hit at ours: watchOS corner
-- buttons read ~60px on this panel, and Apple can hit them -- our
-- digitizer cannot. Two stacked widgets do the split: a 60px visual
-- button drawn first (buttons centre their glyphs, so the circle stays
-- a circle -- a padded label went oval on the narrow x glyph), then an
-- invisible 88px-or-wider button on top as the actual target. Z-order
-- means the visual never sees a tap, clickable or not.
-- Supports the two placements the headers use: top-left via x/y, and
-- align="top_right" with a negative x. Rule (Rick): an icon-only control
-- is a CIRCLE -- omit w. Pass w only for a text label, which gets a pill.
function M.corner_button(scr, opts)
    opts = opts or {}
    local w = opts.w or TARGET
    local vis_w, vis_h = w - 16, 72
    local inset_x = (w - vis_w) // 2
    local inset_y = (TARGET - vis_h) // 2

    local vx
    if opts.align == "top_right" then
        vx = (opts.x or 0) - inset_x
    else
        vx = (opts.x or 0) + inset_x
    end

    local visual = lvgl.button(scr, {
        text = opts.text or lvgl.symbol.close,
        align = opts.align, x = vx, y = (opts.y or 0) + inset_y,
        w = vis_w, h = vis_h,
        bg_color = opts.bg_color or "#1E1E28",
        text_color = opts.text_color or "#A0A0AE",   -- caption-grey token
        radius = vis_h // 2,
    })

    local btn = lvgl.button(scr, {
        x = opts.x, y = opts.y, align = opts.align,
        w = w, h = TARGET,
        bg_opa = 0,
    })

    if opts.on_click then
        btn:on("clicked", opts.on_click)
    end
    -- Plain table handle: widget userdata cannot carry extra fields.
    return { button = btn, visual = visual }
end

-- ------------------------------------------------------------ time zone
-- One implementation, shared. faces.lua and clock.lua each had their own
-- and they disagreed: one rolled the date across the offset and the
-- other returned the UTC date beside a local time, so in Tokyo a face
-- read 05:00 under yesterday's date.

M.ZONES = {
    { "Honolulu", -600 }, { "Anchorage", -540 }, { "Los Angeles", -480 },
    { "Denver", -420 }, { "Mexico City", -360 }, { "Chicago", -360 },
    { "New York", -300 }, { "Toronto", -300 }, { "Santiago", -240 },
    { "Sao Paulo", -180 }, { "London", 0 }, { "Lisbon", 0 },
    { "Berlin", 60 }, { "Paris", 60 }, { "Lagos", 60 }, { "Athens", 120 },
    { "Cairo", 120 }, { "Johannesburg", 120 }, { "Moscow", 180 },
    { "Nairobi", 180 }, { "Dubai", 240 }, { "Karachi", 300 },
    { "Delhi", 330 }, { "Kathmandu", 345 }, { "Dhaka", 360 },
    { "Bangkok", 420 }, { "Jakarta", 420 }, { "Singapore", 480 },
    { "Beijing", 480 }, { "Hong Kong", 480 }, { "Tokyo", 540 },
    { "Seoul", 540 }, { "Adelaide", 570 }, { "Sydney", 600 },
    { "Auckland", 720 },
}

-- The zone lives in NVS, in the same keys apps/settings.lua writes and the C
-- watch face reads. It used to be a file at /sdcard/tz.txt; nothing writes
-- that file any more, so this kept returning London/UTC no matter what the
-- user had chosen -- a silently wrong timezone rather than an error, in the
-- one helper the contract points apps at.
local prefs = require("prefs")

-- Returns city index, dst flag, offset in minutes. Defaults to London,
-- so an unconfigured device reads as UTC rather than as a guess.
function M.zone()
    local idx = prefs.get("tz_city", 11)
    if type(idx) ~= "number" or idx < 1 or idx > #M.ZONES then idx = 11 end
    local dst = prefs.get("tz_dst", 0) == 1
    return idx, dst, M.ZONES[idx][2] + (dst and 60 or 0)
end

-- Writes all three keys together. tz_min is the EFFECTIVE offset (summer time
-- already folded in) because the C face applies it directly and has no city
-- table; the other two are what let this function reconstruct the choice.
function M.save_zone(idx, dst)
    if type(idx) ~= "number" or idx < 1 or idx > #M.ZONES then return end
    prefs.set("tz_city", idx)
    prefs.set("tz_dst", dst and 1 or 0)
    prefs.set("tz_min", M.ZONES[idx][2] + (dst and 60 or 0))
end

local function leap(y) return (y % 4 == 0 and y % 100 ~= 0) or y % 400 == 0 end
local function days_in(m, y)
    local d = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 }
    if m == 2 and leap(y) then return 29 end
    return d[m] or 30
end

-- Shift a UTC reading by minutes, rolling day, month and year properly.
-- Offsets are in MINUTES because India, Nepal and parts of Australia are
-- not on whole hours.
function M.shift(t, mins)
    local total = t.hour * 60 + t.min + mins
    local day, wday = t.day, t.wday
    local month, year = t.month, t.year or 2026

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

M.DAYS = { [0]="Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" }
M.MONTHS = { "Jan","Feb","Mar","Apr","May","Jun",
             "Jul","Aug","Sep","Oct","Nov","Dec" }

-- ---------------------------------------------------------------- header
-- Top-left back control (x for sheets, < for pushed screens), title,
-- optional top-right action. The gallery pattern from every watchOS app.
-- Title only -- exported so every page in an app can share one baseline
-- (review finding: titles jiggled between tile pages at different y).
function M.title(scr, text)
    return lvgl.label(scr, {
        text = text,
        align = "top_mid", y = 26,
        text_color = "#FFFFFF",
        font = chrome_fit(text, 336, 40),
    })
end

function M.header(scr, opts)
    opts = opts or {}
    local h = {}

    -- Grammar (review-driven): sheets get x, pushed screens get <, root
    -- screens get NEITHER -- a back control with no on_back would be a
    -- dead button, the worst kind of looks-tappable-isn't.
    if opts.on_back then
        local glyph = (opts.kind == "sheet") and lvgl.symbol.close or lvgl.symbol.left
        h.back = M.corner_button(scr, {
            text = glyph,
            x = 4, y = 4,
            on_click = opts.on_back,
        })
    end

    if opts.title then
        if h.back then
            -- With a corner control present, a centered title collides
            -- with it ("Delete all?" ran into the x on device). Apple's
            -- grammar: the title sits NEXT to the corner control,
            -- leading-aligned.
            local tw = 368 - 104 - (opts.action and 112 or 8)
            h.title = lvgl.label(scr, {
                text = opts.title,
                x = 104, y = 26,
                text_color = "#FFFFFF",
                font = chrome_fit(opts.title, tw, 40),
                -- Clamp so a long title can't run under a top-right action.
                w = tw,
            })
        else
            h.title = M.title(scr, opts.title)
        end
    end

    if opts.action then
        h.action = M.corner_button(scr, {
            text = opts.action,
            align = "top_right", x = -4, y = 4, w = TARGET + 24,
            on_click = opts.on_action,
        })
    end
    return h
end

-- --------------------------------------------------------------- confirm
-- Full-screen confirmation: the only sanctioned path to a destructive
-- action. cb(true) on confirm, cb(false) on cancel.
function M.confirm(opts, cb)
    opts = opts or {}
    local caller = lvgl.active_screen()
    local scr = lvgl.create_screen()
    scr:set_style({ bg_color = "#000000" })

    -- Idempotent: a real double-tap queues two events before the first
    -- finish deletes the screen, which would call cb twice (and delete a
    -- dead screen) -- unacceptable on the one sanctioned destructive path.
    local done = false
    local function finish(yes)
        if done then return end
        done = true
        caller:load()
        scr:delete()
        if cb then cb(yes) end
    end

    M.header(scr, { kind = "sheet", title = opts.title or "Confirm",
                    on_back = function() finish(false) end })

    lvgl.label(scr, {
        text = opts.message or "",
        align = "top_mid", y = 110,
        text_color = "#FFFFFF",
        w = 340,
    })

    -- Review consensus (all three personas): the confirm button must NOT
    -- sit where the triggering button was (bottom -- a double-tap bounce
    -- would complete the destructive action), and a full-size Cancel must
    -- exist -- the safe path cannot be the smallest target on screen. So:
    -- confirm at CENTER, Cancel full-width at the BOTTOM, where the
    -- sloppy second tap lands. The confirm button also ignores taps for
    -- its first 400ms.
    local armed = false
    timer.after(400, function() armed = true end)

    local btn = lvgl.button(scr, {
        text = opts.confirm_label or "OK",
        align = "center", y = 30,
        w = 344, h = ROW_H,
        bg_color = opts.destructive and "#B3261E" or "#2F80ED",
        text_color = "#FFFFFF", radius = 12,
    })
    btn:on("clicked", function()
        if armed then finish(true) end
    end)

    local cancel = lvgl.button(scr, {
        text = "Cancel",
        align = "bottom_mid", y = -12,
        w = 344, h = ROW_H,
        bg_color = "#1E1E28", text_color = "#FFFFFF", radius = 12,
    })
    cancel:on("clicked", function() finish(false) end)

    scr:load()
end

-- ------------------------------------------------------------------- row
-- Full-width settings row. kind = "toggle" | "check" | "nav".
-- Returns a handle: h.row, plus per-kind accessors.
function M.row(parent, opts)
    opts = opts or {}
    local h = {}

    h.row = lvgl.button(parent, {
        w = 344, h = ROW_H,
        bg_color = "#1E1E28", radius = 12,
    })

    -- Width-limited so a long label wraps inside the row instead of
    -- running under (or past) the trailing switch/check/chevron --
    -- "Daylight saving" was losing its last letter to the switch, and a
    -- long picker entry ran off the panel entirely (Rick, on device).
    local trailing = (opts.kind == "toggle") and 92 or (opts.kind and 56 or 16)
    h.label = lvgl.label(h.row, {
        text = opts.text or "",
        align = "left_mid", x = 16,
        text_color = opts.dim and "#A0A0AE" or "#FFFFFF",
        w = 344 - 16 - trailing,
    })
    -- opts.size lets a dense list (a 35-city timezone picker) drop to 26 so
    -- entries fit on one line instead of wrapping to two. Set unconditionally
    -- rather than only when asked: leaving it to the theme let the label track
    -- the full font scale, which is exactly what overflows a fixed-height row.
    -- chrome_font is the identity at scale <= 1.0, so this changes nothing for
    -- a caller that was already happy.
    h.label:set_style({ font = chrome_font(opts.size or 32) })

    if opts.kind == "toggle" then
        h.switch = lvgl.switch(h.row, { checked = opts.checked and true or false })
        h.switch:align("right_mid", -16, 0)
        if opts.on_change then
            h.switch:on("value_changed", opts.on_change)
        end
        h.get = function() return h.switch:get_value() end
        -- The whole 344x104 row is the target, not just the small switch
        -- graphic (Rick: rows registered the tap but nothing toggled).
        -- set_value does not emit value_changed, so on_change is called
        -- by hand here; a tap on the switch itself takes the handler
        -- above instead -- child clicks do not bubble, so never both.
        h.row:on("clicked", function()
            local now = h.switch:get_value()
            h.switch:set_value(not (now and now ~= 0))
            if opts.on_change then opts.on_change() end
        end)
    elseif opts.kind == "check" then
        h.check = lvgl.label(h.row, {
            text = lvgl.symbol.ok,
            align = "right_mid", x = -16,
            text_color = "#2F80ED",
        })
        h.set_checked = function(on)
            h.check:set_style({ opa = on and 255 or 0 })
        end
        h.set_checked(opts.checked)
    elseif opts.kind == "nav" then
        lvgl.label(h.row, {
            text = lvgl.symbol.right,
            align = "right_mid", x = -16,
            text_color = "#A0A0AE",
        })
    end

    if opts.on_click and not opts.dim then
        h.row:on("clicked", opts.on_click)
    end
    -- Uniform handle: get()/set_checked() exist on every kind (the contract
    -- lists them on the row handle), so a caller need not branch on kind.
    h.get = h.get or function() return nil end
    h.set_checked = h.set_checked or function() end
    return h
end

-- ---------------------------------------------------------------- select
-- Single-select group of check rows. Owns the one-checked invariant.
-- cb(index) on every change. Returns h with get()/set(i).
function M.select(parent, opts, cb)
    opts = opts or {}
    local options = opts.options or {}
    local disabled = {}
    for _, d in ipairs(opts.disabled or {}) do disabled[d] = true end

    -- Clamp a stale/out-of-range selected (e.g. a persisted index for a list
    -- that has since shrunk) to a valid row -- otherwise nothing renders
    -- checked and the first tap indexes h.rows[nil], leaving the select inert.
    local sel = opts.selected or 1
    if not options[sel] then sel = 1 end
    local h = { rows = {}, selected = sel }

    for i, text in ipairs(options) do
        local row
        row = M.row(parent, {
            text = text,
            kind = "check",
            size = opts.size,
            checked = (i == h.selected),
            dim = disabled[i],
            on_click = function()
                if i == h.selected then
                    -- Tapping the current choice is confirmation, not a
                    -- dead tap (review: users confirm by re-tapping).
                    if opts.on_reselect then opts.on_reselect(i) end
                    return
                end
                local prev = h.rows[h.selected]
                if prev then prev.set_checked(false) end
                h.selected = i
                row.set_checked(true)
                if cb then cb(i) end
            end,
        })
        -- A disabled row still acknowledges the tap: on this digitizer a
        -- silent no-op is indistinguishable from a dropped tap, which
        -- reads as "broken" (review). Acknowledge, then refuse.
        if disabled[i] then
            row.row:on("clicked", function()
                M.toast(lvgl.active_screen(), "Unavailable")
            end)
        end
        h.rows[i] = row
    end

    h.get = function() return h.selected end
    h.set = function(i)
        if h.rows[i] and i ~= h.selected then
            local prev = h.rows[h.selected]
            if prev then prev.set_checked(false) end
            h.selected = i
            h.rows[i].set_checked(true)
        end
    end
    return h
end

-- ---------------------------------------------------------------- picker
-- Full-page dropdown replacement: header + select on its own screen.
-- cb(index) on choice; cb(nil) on cancel.
function M.picker(opts, cb)
    opts = opts or {}
    local caller = lvgl.active_screen()
    local scr = lvgl.create_screen()
    scr:set_style({ bg_color = "#000000" })

    local done = false
    local function finish(result)
        if done then return end
        done = true
        caller:load()
        scr:delete()
        if cb then cb(result) end
    end

    -- Pushed-screen grammar: rows with a chevron push, so this gets <,
    -- not x -- the sheet glyph belongs to the keyboard (review finding).
    M.header(scr, { title = opts.title,
                    on_back = function() finish(nil) end })

    local list = lvgl.container(scr, {
        x = 0, y = TARGET + 12, w = 368, h = 448 - TARGET - 12,
        bg_opa = 0, border_width = 0, pad = 12,
    })
    list:set_flex({ flow = "column", pad_row = 16 })
    list:set_scroll({ dir = "ver", scrollbar = "active" })

    M.select(list, { options = opts.options, selected = opts.selected,
                     disabled = opts.disabled, size = opts.size,
                     on_reselect = function(i) finish(i) end },
             function(i) finish(i) end)

    scr:load()
end

-- ------------------------------------------------------------------ dots
-- Page dots for a tileview, bottom centre, current one bright. Pure
-- observation: infers the page from the active tile's x offset.
function M.dots(scr, tv, opts)
    opts = opts or {}
    local count = opts.count or 2
    local h = { labels = {} }

    local total_w = count * 24
    for i = 1, count do
        h.labels[i] = lvgl.label(scr, {
            text = lvgl.symbol.bullet,
            align = "bottom_mid",
            x = (i - 1) * 24 - (total_w - 24) // 2,
            y = -8,
            text_color = "#A0A0AE",
        })
    end

    local function mark(page)
        for i = 1, count do
            h.labels[i]:set_style({
                text_color = (i == page) and "#FFFFFF" or "#A0A0AE",
            })
        end
    end
    mark(1)

    local pending
    tv:on("value_changed", function()
        local page = tv:get_active_index()
        if page then mark(page) end
        -- A fast multi-page fling coalesces events and the immediate read
        -- can be stale (dot 1 lit on page 3, caught by harness). Re-read
        -- once the snap animation has settled. Coalesce: cancel any prior
        -- pending re-read so at most one 400ms slot is outstanding.
        if pending then pending:cancel() end
        pending = timer.after(400, function()
            pending = nil
            local settled = tv:get_active_index()
            if settled then mark(settled) end
        end)
    end)
    return h
end

-- ----------------------------------------------------------------- toast
-- Transient status pill, self-dismissing. Costs one timer slot while up.
local active_toast, active_timer
function M.toast(scr, text, ms)
    -- 2.5s default (1.5s vanished before a glance landed -- the review's
    -- own screenshot captured no toast at all) and lifted clear of the
    -- page dots instead of covering the only paging affordance.
    --
    -- One toast at a time: replace any live one, so rapid triggers (e.g.
    -- repeated taps on a disabled select row) can't stack 16 pills and
    -- exhaust the timer budget, and a pill can't outlive a torn-down screen.
    if active_timer then active_timer:cancel(); active_timer = nil end
    if active_toast then pcall(function() active_toast:delete() end); active_toast = nil end
    local pill = lvgl.button(scr, {
        text = text,
        align = "bottom_mid", y = -48,
        w = 300, h = 72,
        bg_color = "#24303C", text_color = "#FFFFFF", radius = 36,
    })
    active_toast = pill
    active_timer = timer.after(ms or 2500, function()
        active_timer = nil
        if active_toast == pill then active_toast = nil end
        pcall(function() pill:delete() end)
    end)
    return pill
end

-- --------------------------------------------------------------- stepper
-- Numeric +/- row: value label centre, minus left, plus right, clamped,
-- with hold-to-repeat -- the interaction math every timer/reps app would
-- otherwise hand-roll. cb(value) on every change; h.get()/h.set(v).
function M.stepper(parent, opts, cb)
    opts = opts or {}
    local h = {}
    local min = opts.min or 0
    local max = opts.max or 99
    local step = opts.step or 1
    local value = opts.value or min
    if value < min then value = min elseif value > max then value = max end
    local fmt = opts.label or "%d"

    h.row = lvgl.container(parent, {
        w = 344, h = ROW_H,
        bg_color = "#1E1E28", bg_opa = 255, border_width = 0, radius = 12,
        pad = 0,   -- let the +/- slabs and their hit areas reach the row edges
    })

    -- The readout sits BETWEEN the two 96px hit areas, so it has 152px, not
    -- 344. Sized to that: unbounded, "Volume 100%" simply drew over its own
    -- +/- slabs and lost characters at both ends.
    local READOUT_W = 344 - 96 * 2
    h.label = lvgl.label(h.row, {
        text = string.format(fmt, value),
        align = "center",
        text_color = "#FFFFFF",
        w = READOUT_W,
        text_align = "center",
        font = chrome_fit(string.format(fmt, max), READOUT_W, 40),
    })

    local function apply(v)
        if v < min then v = min end
        if v > max then v = max end
        if v == value then return end
        value = v
        h.label:set_text(string.format(fmt, value))
        if cb then cb(value) end
    end

    -- Same split as corner_button (Rick: full-height slabs read
    -- oversized): a 72x64 rounded rect is what you see -- rects, not
    -- circles, inside rows (Rick's call) -- and the invisible
    -- 96 x ROW_H button on top is what you hit.
    local function side_button(glyph, side, on_fire)
        lvgl.button(h.row, {
            text = glyph,
            align = side .. "_mid", x = 0,   -- flush to the row's outer edge
            w = 72, h = 64,
            bg_color = "#24303C", text_color = "#FFFFFF", radius = 16,
        })
        local hit = lvgl.button(h.row, {
            align = side .. "_mid", x = 0,
            w = 96, h = ROW_H,
            bg_opa = 0,
        })
        hit:on("clicked", on_fire)
        hit:on("long_pressed_repeat", on_fire)
    end
    side_button(lvgl.symbol.minus, "left", function() apply(value - step) end)
    side_button(lvgl.symbol.plus, "right", function() apply(value + step) end)

    h.get = function() return value end
    h.set = function(v) apply(v) end
    return h
end

-- ------------------------------------------------------------------ busy
-- Modal wait state on its own screen: spinner + message. Returns a
-- handle whose :done() restores the caller's screen. For anything that
-- takes more than a beat (voice capture, SD churn) -- a frozen screen
-- with no feedback reads as a crash on a watch.
function M.busy(opts)
    opts = opts or {}
    local caller = lvgl.active_screen()
    local scr = lvgl.create_screen()
    scr:set_style({ bg_color = "#000000" })
    scr:set_scroll({ dir = "none", scrollbar = "off" })

    lvgl.spinner(scr, {
        align = "center", y = -20,
        w = 140, h = 140,
    })
    if opts.text then
        lvgl.label(scr, {
            text = opts.text,
            align = "bottom_mid", y = -40,
            text_color = "#A0A0AE",
        })
    end

    scr:load()

    local h = {}
    h.done = function()
        caller:load()
        scr:delete()
    end
    return h
end

-- ------------------------------------------------------------------ fill
-- Big drag-to-set value control, on ITS OWN SCREEN -- like Apple Home's
-- brightness fill, which is always a pushed screen with an x, never a
-- paged tile: a drag surface and horizontal paging fight over the same
-- gesture, and embedding this in a tileview made page swipes nearly
-- impossible (found by Rick on device).
--
-- An arc, not a slider (the binding has no per-part styling, and a
-- screen-sized slider's indicator renders as an illegible blob); the
-- reading sits in the arc's hole on true black, and dragging the ring
-- needs no tap precision.
--
-- on_change() fires live on every change; the x closes the screen and
-- calls done(final_value).
function M.fill(opts, on_change, done)
    opts = opts or {}
    local caller = lvgl.active_screen()
    local scr = lvgl.create_screen()
    scr:set_style({ bg_color = "#000000" })

    local arc = lvgl.arc(scr, {
        min = opts.min or 0, max = opts.max or 100,
        value = opts.value or 0,
        align = "center", y = 20,
        w = 300, h = 300,
        arc_width = 28,
        -- Without these the value band and its track render the same hue, so
        -- 1% looks identical to 100% -- the one thing this control exists to show.
        line_color = "#2F80ED", track_color = "#1E1E28",
    })

    local label = lvgl.label(scr, {
        text = "",
        align = "center", y = 20,
        text_color = "#FFFFFF",
        font = lvgl.font(60),   -- hero size: the value IS this screen
    })

    local fmt = opts.label or "%d"
    local function update()
        label:set_text(string.format(fmt, arc:get_value()))
    end
    update()

    arc:on("value_changed", function()
        update()
        if on_change then on_change(arc:get_value()) end
    end)

    -- Pushed-screen grammar: < means "go back", and values apply live
    -- (on_change) -- so leaving commits by nature, and no glyph lies. The
    -- old x-that-saved was the review's sharpest semantics finding: the
    -- discard glyph must never commit.
    local closed = false
    M.header(scr, { title = opts.title,
                    on_back = function()
                        if closed then return end
                        closed = true
                        local v = arc:get_value()
                        caller:load()
                        scr:delete()
                        if done then done(v) end
                    end })

    scr:set_scroll({ dir = "none", scrollbar = "off" })
    scr:load()
end

-- ---------------------------------------------------------------- button
-- The primary action button every app hand-rolls, at the design's tap size
-- (>= ~200x100) and palette. kind = "primary" | "secondary" | "danger".
-- Returns the button widget so :on()/:set_text() compose directly.
-- Palette tokens (design guide): secondary/raised = #1E1E28, destructive =
-- #B3261E (the same red ui.confirm uses -- danger buttons should route through
-- ui.confirm, but if used, they match).
local BTN_BG = { primary = "#2F80ED", secondary = "#1E1E28", danger = "#B3261E" }
function M.button(parent, opts)
    opts = opts or {}
    local btn = lvgl.button(parent, {
        text = opts.text or "",
        align = opts.align, x = opts.x, y = opts.y,
        w = opts.w or 320, h = opts.h or 110,
        bg_color = opts.bg_color or BTN_BG[opts.kind or "primary"] or BTN_BG.primary,
        text_color = opts.text_color or "#FFFFFF",
        radius = opts.radius or 16,
    })
    if opts.on_click then btn:on("clicked", opts.on_click) end
    return btn
end

-- ---------------------------------------------------------------- list
-- A scrollable vertical stack for rows/cards -- the container M.row and
-- M.select expect. Returns the container; parent your rows to it.
function M.list(parent, opts)
    opts = opts or {}
    local c = lvgl.container(parent, {
        x = opts.x or 0, y = opts.y or 0,
        w = opts.w or 368, h = opts.h or 448,
        bg_opa = 0, border_width = 0, pad = opts.pad or 12,
    })
    c:set_flex({ flow = "column", pad_row = opts.pad_row or 16 })   -- guide: 16px between rows
    c:set_scroll({ scrollbar = opts.scrollbar or "active" })
    return c
end

-- ---------------------------------------------------------------- card
-- The launcher's standard grouped-content surface: a rounded panel. Parent
-- content to it. Returns the container.
function M.card(parent, opts)
    opts = opts or {}
    return lvgl.container(parent, {
        x = opts.x, y = opts.y, align = opts.align,
        w = opts.w or 344, h = opts.h or 120,
        bg_color = opts.bg_color or "#1E1E28",
        radius = opts.radius or 16, border_width = 0,
        pad = opts.pad or 16,
    })
end

-- ---------------------------------------------------------------- stat
-- A big readout: a value over a small caption -- for stopwatches, counters,
-- temperatures. opts.size picks the value font (default 48). Returns h with
-- h.set(value) to update the number.
function M.stat(parent, opts)
    opts = opts or {}
    local size = opts.size or 48
    local align = opts.align or "center"
    local h = {}
    h.value = lvgl.label(parent, {
        text = tostring(opts.value or ""),
        align = align, x = opts.x, y = opts.y,
        text_color = opts.color or "#FFFFFF",
        font = lvgl.font(size),
    })
    if opts.label then
        h.caption = lvgl.label(parent, {
            text = opts.label,
            align = align, x = opts.x,
            y = (opts.y or 0) + size // 2 + 20,   -- just below the value
            text_color = "#A0A0AE",
            font = lvgl.font(26),
        })
    end
    h.set = function(v) h.value:set_text(tostring(v)) end
    return h
end

-- ---------------------------------------------------------------- note
-- A centred, dim message for empty states and hints ("Nothing yet",
-- "Tap + to start"). Returns the label so you can hide/replace it.
function M.note(scr, text, opts)
    opts = opts or {}
    -- Width-limited so the text wraps inside the panel. Without it a label
    -- shrinks to its content and a long line runs off BOTH edges (centred, so
    -- it loses characters at each end) -- which is what happened to the
    -- "Clock keeps UTC." note at the 130% font scale, where even a hand-broken
    -- line no longer fits. 336 = 368 minus the 16px margin each side.
    -- opts.align lets a page pin the note to an edge instead of offsetting it
    -- from the centre. A centred note grows in BOTH directions as it wraps, so
    -- at a large font scale a hint that fits at 1.0 walks off the bottom of the
    -- screen; "bottom_mid" grows upward into the page instead, which is what a
    -- footer hint wants. Default stays "center" so existing callers are
    -- unaffected.
    return lvgl.label(scr, {
        text = text or "",
        align = opts.align or "center", y = opts.y or 0,
        text_color = opts.color or "#A0A0AE",
        font = lvgl.font(opts.size or 32),
        w = opts.w or 336,
        text_align = "center",
    })
end

return M
