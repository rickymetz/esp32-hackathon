-- keyboard: text entry sized for this digitizer. A QWERTY is unusable here
-- (~30px keys against a touch panel that drops half of 180x56 targets), so
-- letters go through two-stage group selection -- every key at least
-- 184x90 -- and digits through a roller, which needs no tap precision.
--
-- keyboard.open({ title=, mode="text"|"number", initial= }, function(text)
--     -- text == nil means cancelled
-- end)
--
-- Ships inside the launcher; see docs/APP_CONTRACT.md.

local lvgl = require("lvgl")

local M = {}

local HEADER_H = 88
local BODY_Y = HEADER_H + 0
local BODY_H = 448 - HEADER_H          -- 360: stage1 = 4x90, stage2 = 3x120

local GROUPS = {
    { label = "ABCDEF", chars = { "A", "B", "C", "D", "E", "F" } },
    { label = "GHIJKL", chars = { "G", "H", "I", "J", "K", "L" } },
    { label = "MNOPQR", chars = { "M", "N", "O", "P", "Q", "R" } },
    { label = "STUVWX", chars = { "S", "T", "U", "V", "W", "X" } },
    { label = "YZ.,-'", chars = { "Y", "Z", ".", ",", "-", "'" } },
}

local BACKSPACE = lvgl.symbol.backspace
local SPACE = "space"

function M.open(opts, cb)
    opts = opts or {}
    local mode = opts.mode or "text"
    local text = opts.initial or ""

    local caller = lvgl.active_screen()
    local scr = lvgl.create_screen()
    scr:set_style({ bg_color = "#000000" })

    local function finish(result)
        caller:load()
        scr:delete()
        if cb then cb(result) end
    end

    -- Header: x | readout | OK. The readout lives inline so the whole
    -- 360px below stays key surface (a separate readout band did not fit
    -- the panel -- review finding).
    local cancel = lvgl.button(scr, {
        text = lvgl.symbol.close,
        x = 0, y = 0, w = 88, h = HEADER_H,
        bg_color = "#1E1E28", text_color = "#9FB4C7", radius = 12,
    })
    cancel:on("clicked", function() finish(nil) end)

    local ok = lvgl.button(scr, {
        text = lvgl.symbol.ok,
        align = "top_right", x = 0, y = 0, w = 112, h = HEADER_H,
        bg_color = "#2F80ED", text_color = "#FFFFFF", radius = 12,
    })
    ok:on("clicked", function() finish(text) end)

    local readout = lvgl.label(scr, {
        text = "",
        align = "top_mid", y = 30,
        text_color = "#FFFFFF",
    })

    local function update_readout()
        -- Show the tail: on 368px minus two buttons there is room for
        -- roughly 8 glyphs of Lexend 32. Fixed position, so the layout
        -- never jumps as the text grows.
        local shown = text .. "_"
        if #shown > 9 then
            shown = "\u{2026}" .. shown:sub(-8)
        end
        readout:set_text(shown)
    end
    update_readout()

    -- One buttonmatrix reused for every view; maps swap in place.
    local bm = lvgl.buttonmatrix(scr, {
        x = 0, y = BODY_Y, w = 368, h = BODY_H,
        bg_color = "#000000", bg_opa = 255, border_width = 0, pad = 4,
    })

    local roller = nil
    local view   -- forward: "groups" | "letters" | "digits"
    local show_groups, show_letters, show_digits

    local function hide_roller()
        if roller then
            roller:delete()
            roller = nil
        end
    end

    show_groups = function()
        hide_roller()
        view = "groups"
        local map = {}
        for i = 1, 4, 2 do
            map[#map + 1] = GROUPS[i].label
            map[#map + 1] = GROUPS[i + 1].label
            map[#map + 1] = "\n"
        end
        map[#map + 1] = GROUPS[5].label
        map[#map + 1] = "123"
        map[#map + 1] = "\n"
        map[#map + 1] = BACKSPACE
        map[#map + 1] = SPACE
        bm:set_map(map)
    end

    local current_group = nil
    show_letters = function(group)
        view = "letters"
        current_group = group
        local c = group.chars
        bm:set_map({ c[1], c[2], "\n", c[3], c[4], "\n", c[5], c[6] })
    end

    show_digits = function()
        view = "digits"
        -- Roller for the digit itself; the matrix shrinks to one action
        -- row at the bottom: backspace | add | back-to-letters (or OK-only
        -- confirmation lives in the header as always).
        bm:set_map({ BACKSPACE, "+", (mode == "text") and "ABC" or "00" })
        bm:set_size(368, 120)
        bm:set_pos(0, 448 - 120)

        local digits = {}
        for d = 0, 9 do digits[#digits + 1] = tostring(d) end
        roller = lvgl.roller(scr, {
            options = digits, selected = 1, visible_rows = 3,
            align = "center", y = -20,
        })
    end

    local function restore_full_matrix()
        bm:set_size(368, BODY_H)
        bm:set_pos(0, BODY_Y)
    end

    bm:on("value_changed", function()
        local idx = bm:get_selected()
        if not idx then return end
        local t = bm:get_button_text(idx)
        if t == "" then return end

        if view == "groups" then
            if t == "123" then
                show_digits()
            elseif t == BACKSPACE then
                text = text:sub(1, -2); update_readout()
            elseif t == SPACE then
                text = text .. " "; update_readout()
            else
                for _, g in ipairs(GROUPS) do
                    if g.label == t then show_letters(g); break end
                end
            end
        elseif view == "letters" then
            text = text .. t
            update_readout()
            show_groups()
        elseif view == "digits" then
            if t == "+" then
                if roller then
                    text = text .. tostring(roller:get_value() - 1)
                    update_readout()
                end
            elseif t == BACKSPACE then
                text = text:sub(1, -2); update_readout()
            elseif t == "ABC" then
                hide_roller()
                restore_full_matrix()
                show_groups()
            elseif t == "00" then
                text = text .. "00"; update_readout()
            end
        end
    end)

    if mode == "number" then
        show_digits()
    else
        show_groups()
    end

    scr:load()
end

return M
