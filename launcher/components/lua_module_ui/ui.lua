-- ui: shared UI primitives with the design guide's sizes baked in.
-- Ships inside the launcher (apps cannot require() files off the card).
-- Pure Lua over the lvgl/timer bindings; see docs/APP_CONTRACT.md.

local lvgl = require("lvgl")
local timer = require("timer")

local M = {}

local ROW_H = 104          -- design guide: preferred row height
local TARGET = 88          -- design guide: minimum tappable square

-- ---------------------------------------------------------------- header
-- Top-left back control (x for sheets, < for pushed screens), title,
-- optional top-right action. The gallery pattern from every watchOS app.
function M.header(scr, opts)
    opts = opts or {}
    local h = {}

    local glyph = (opts.kind == "sheet") and lvgl.symbol.close or lvgl.symbol.left
    -- Inset 8px from the edges: the panel's glass corners are rounded and
    -- clip anything drawn at x=0,y=0 (found by Rick on device).
    h.back = lvgl.button(scr, {
        text = glyph,
        x = 8, y = 8, w = TARGET, h = TARGET,
        bg_color = "#1E1E28", text_color = "#9FB4C7", radius = 16,
    })
    if opts.on_back then
        h.back:on("clicked", opts.on_back)
    end

    if opts.title then
        h.title = lvgl.label(scr, {
            text = opts.title,
            align = "top_mid", y = 32,
            text_color = "#FFFFFF",
        })
    end

    if opts.action then
        h.action = lvgl.button(scr, {
            text = opts.action,
            align = "top_right", x = -8, y = 8, w = TARGET + 24, h = TARGET,
            bg_color = "#1E1E28", text_color = "#9FB4C7", radius = 16,
        })
        if opts.on_action then
            h.action:on("clicked", opts.on_action)
        end
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
        align = "center", y = -20,
        text_color = "#FFFFFF",
        w = 340,
    })

    local btn = lvgl.button(scr, {
        text = opts.confirm_label or "OK",
        align = "bottom_mid", y = -12,
        w = 344, h = ROW_H,
        bg_color = opts.destructive and "#B3261E" or "#2F80ED",
        text_color = "#FFFFFF", radius = 12,
    })
    btn:on("clicked", function() finish(true) end)

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
                if i == h.selected then return end
                h.rows[h.selected].set_checked(false)
                h.selected = i
                row.set_checked(true)
                if cb then cb(i) end
            end,
        })
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

    M.header(scr, { kind = "sheet", title = opts.title,
                    on_back = function() finish(nil) end })

    local list = lvgl.container(scr, {
        x = 0, y = TARGET + 8, w = 368, h = 448 - TARGET - 8,
        bg_opa = 0, border_width = 0, pad = 12,
    })
    list:set_flex({ flow = "column", pad_row = 16 })
    list:set_scroll({ dir = "ver" })

    M.select(list, { options = opts.options, selected = opts.selected,
                     disabled = opts.disabled },
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
        local tile = tv:get_active_tile()
        if not tile then return end
        local x = tile:get_pos()
        local w = tv:get_size()
        if w and w > 0 then
            mark(math.floor(x / w + 0.5) + 1)
        end
    end)
    return h
end

-- ----------------------------------------------------------------- toast
-- Transient status pill, self-dismissing. Costs one timer slot while up.
function M.toast(scr, text)
    local pill = lvgl.button(scr, {
        text = text,
        align = "bottom_mid", y = -16,
        w = 300, h = 72,
        bg_color = "#24303C", text_color = "#FFFFFF", radius = 36,
    })
    timer.after(1500, function()
        pill:delete()
    end)
    return pill
end

-- ------------------------------------------------------------------ fill
-- Big drag-to-set value control. An arc, not a slider: the binding has no
-- per-part styling, and a screen-sized slider's indicator renders as an
-- illegible blob (found by Rick on device). The arc is theme-native, the
-- reading sits in its hole on true black, and dragging the ring needs no
-- tap precision. cb() on change; read with h.get().
function M.fill(scr, opts, cb)
    opts = opts or {}
    local h = {}

    h.arc = lvgl.arc(scr, {
        min = opts.min or 0, max = opts.max or 100,
        value = opts.value or 0,
        align = "center",
        w = 320, h = 320,
        arc_width = 28,
    })

    h.label = lvgl.label(scr, {
        text = "",
        align = "center",
        text_color = "#FFFFFF",
        font = lvgl.font(48),
    })

    local fmt = opts.label or "%d"
    local function update()
        h.label:set_text(string.format(fmt, h.arc:get_value()))
    end
    update()

    h.arc:on("value_changed", function()
        update()
        if cb then cb() end
    end)

    h.get = function() return h.arc:get_value() end
    return h
end

return M
