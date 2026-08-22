-- ui: shared UI primitives with the design guide's sizes baked in.
-- Ships inside the launcher (apps cannot require() files off the card).
-- Pure Lua over the lvgl/timer bindings; see docs/APP_CONTRACT.md.

local lvgl = require("lvgl")
local timer = require("timer")

local M = {}

local ROW_H = 104          -- design guide: preferred row height
local TARGET = 88          -- design guide: minimum tappable square

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
        text_color = opts.text_color or "#9FB4C7",
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
        font = lvgl.font(40),
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
            h.title = lvgl.label(scr, {
                text = opts.title,
                x = 104, y = 26,
                text_color = "#FFFFFF",
                font = lvgl.font(40),
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

    local function finish(yes)
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

    h.label = lvgl.label(h.row, {
        text = opts.text or "",
        align = "left_mid", x = 16,
        text_color = opts.dim and "#8A8A99" or "#FFFFFF",
    })

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
            text_color = "#8A8A99",
        })
    end

    if opts.on_click and not opts.dim then
        h.row:on("clicked", opts.on_click)
    end
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

    local h = { rows = {}, selected = opts.selected or 1 }

    for i, text in ipairs(options) do
        local row
        row = M.row(parent, {
            text = text,
            kind = "check",
            checked = (i == h.selected),
            dim = disabled[i],
            on_click = function()
                if i == h.selected then
                    -- Tapping the current choice is confirmation, not a
                    -- dead tap (review: users confirm by re-tapping).
                    if opts.on_reselect then opts.on_reselect(i) end
                    return
                end
                h.rows[h.selected].set_checked(false)
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
            h.rows[h.selected].set_checked(false)
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

    local function finish(result)
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
                     disabled = opts.disabled,
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
            text_color = "#8A8A99",
        })
    end

    local function mark(page)
        for i = 1, count do
            h.labels[i]:set_style({
                text_color = (i == page) and "#FFFFFF" or "#8A8A99",
            })
        end
    end
    mark(1)

    tv:on("value_changed", function()
        local page = tv:get_active_index()
        if page then mark(page) end
        -- A fast multi-page fling coalesces events and the immediate read
        -- can be stale (dot 1 lit on page 3, caught by harness). Re-read
        -- once the snap animation has settled; costs one timer slot for
        -- 400ms per swipe.
        timer.after(400, function()
            local settled = tv:get_active_index()
            if settled then mark(settled) end
        end)
    end)
    return h
end

-- ----------------------------------------------------------------- toast
-- Transient status pill, self-dismissing. Costs one timer slot while up.
function M.toast(scr, text, ms)
    -- 2.5s default (1.5s vanished before a glance landed -- the review's
    -- own screenshot captured no toast at all) and lifted clear of the
    -- page dots instead of covering the only paging affordance.
    local pill = lvgl.button(scr, {
        text = text,
        align = "bottom_mid", y = -48,
        w = 300, h = 72,
        bg_color = "#24303C", text_color = "#FFFFFF", radius = 36,
    })
    timer.after(ms or 2500, function()
        pill:delete()
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
    local fmt = opts.label or "%d"

    h.row = lvgl.container(parent, {
        w = 344, h = ROW_H,
        bg_color = "#1E1E28", bg_opa = 255, border_width = 0, radius = 12,
    })

    h.label = lvgl.label(h.row, {
        text = string.format(fmt, value),
        align = "center",
        text_color = "#FFFFFF",
        font = lvgl.font(40),
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
            align = side .. "_mid", x = (side == "left") and 12 or -12,
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
            text_color = "#8A8A99",
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
    })

    local label = lvgl.label(scr, {
        text = "",
        align = "center", y = 20,
        text_color = "#FFFFFF",
        font = lvgl.font(48),
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
    M.header(scr, { title = opts.title,
                    on_back = function()
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
local BTN_BG = { primary = "#2F80ED", secondary = "#3A3A44", danger = "#C0392B" }
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
    c:set_flex({ flow = "column", pad_row = opts.pad_row or 12 })
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
            text_color = "#8A8A99",
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
    return lvgl.label(scr, {
        text = text or "",
        align = "center", y = opts.y or 0,
        text_color = opts.color or "#55555F",
        font = lvgl.font(opts.size or 32),
    })
end

return M
