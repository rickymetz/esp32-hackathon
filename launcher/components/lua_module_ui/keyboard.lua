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
local ui = require("ui")
local voice = require("voice")
local button = require("button")

local M = {}

local HEADER_H = 88
local BODY_Y = HEADER_H + 8            -- header sits 8px in from the glass
local BODY_H = 448 - BODY_Y            -- 352: both stages are 4 rows x 88

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

    -- Declared BEFORE finish(): a local's scope starts after its declaring
    -- statement, so referencing these inside a closure compiled earlier
    -- resolves to nil globals -- the exact trap the contract documents for
    -- timer handles, and exactly the bug the PR review caught here (the
    -- PWR subscription was never unsubscribed).
    local listening = false
    local pwr_sub

    local function finish(result)
        if pwr_sub then button.off(pwr_sub) end
        voice.stop()   -- no-op unless a capture is running
        caller:load()
        scr:delete()
        if cb then cb(result) end
    end

    -- Header: x | readout | OK. The readout lives inline so the whole
    -- 360px below stays key surface (a separate readout band did not fit
    -- the panel -- review finding).
    -- Corner controls drawn at watch scale, hit at ours (see
    -- ui.corner_button). The x guards non-empty text behind a confirm:
    -- a name is 15-25 taps of work and a 10px drift above the top key
    -- row lands here -- silent discard was the review's sharpest
    -- keyboard finding.
    ui.corner_button(scr, {
        text = lvgl.symbol.close,
        x = 4, y = 4,
        on_click = function()
            if text == "" then
                finish(nil)
            else
                ui.confirm({ title = "Discard?",
                             message = "Throw away what you typed?",
                             confirm_label = "Discard", destructive = true },
                    function(yes)
                        if yes then finish(nil) end
                    end)
            end
        end,
    })

    -- Icon-only corner controls are circles; only text labels get pills.
    ui.corner_button(scr, {
        text = lvgl.symbol.ok,
        align = "top_right", x = -4, y = 4,
        bg_color = "#2F80ED", text_color = "#FFFFFF",
        on_click = function() finish(text) end,
    })

    local readout = lvgl.label(scr, {
        text = "",
        align = "top_mid", y = 34,
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

    local upper = true   -- case for appended letters; the Aa cell flips it
    local view   -- forward: "groups" | "letters" | "digits"
    local show_groups, show_letters, show_digits

    local function cased(c)
        return upper and c:upper() or c:lower()
    end

    show_groups = function()
        view = "groups"
        local map = {}
        for i = 1, 4, 2 do
            map[#map + 1] = cased(GROUPS[i].label)
            map[#map + 1] = cased(GROUPS[i + 1].label)
            map[#map + 1] = "\n"
        end
        map[#map + 1] = cased(GROUPS[5].label)
        map[#map + 1] = "123"
        map[#map + 1] = "\n"
        -- The case cell shows what you SWITCH TO, phone-style.
        map[#map + 1] = upper and "abc" or "ABC"
        if voice.available() then
            map[#map + 1] = lvgl.symbol.microphone   -- NATO voice spelling
        end
        map[#map + 1] = SPACE
        map[#map + 1] = BACKSPACE
        bm:set_map(map)
    end

    local function start_voice()
        if not voice.available() then return end
        readout:set_text("say it...")
        listening = true
        local ok = voice.spell(function(t)
            listening = false
            if t and t ~= "" then
                text = text .. t
            end
            update_readout()
        end)
        if not ok then
            listening = false
            update_readout()
        end
    end

    -- PWR toggles the mic while the keyboard is open (Rick's call): one
    -- press starts listening, another delivers what was heard so far --
    -- voice.stop() ends the capture and the spell callback fires with the
    -- accumulated text. Binary and eyes-free: exactly what the button
    -- doctrine sanctions. Released in finish().
    if voice.available() then
        pwr_sub = button.on("pwr", "pressed", function()
            if listening then
                voice.stop()
            else
                start_voice()
            end
        end)
    end

    local current_group = nil
    show_letters = function(group)
        view = "letters"
        current_group = group
        local c = {}
        for i, ch in ipairs(group.chars) do c[i] = cased(ch) end
        -- Picking a letter STAYS here (repeats from one group are one tap
        -- each -- jumping back after every pick was tedious on device);
        -- the < cell returns to the groups. Backspace lives here too --
        -- a mistype is most likely at the moment of picking a letter,
        -- and it is pinned bottom-RIGHT in every view (review: it swapped
        -- corners between views, and muscle memory hit ABC instead).
        bm:set_map({ c[1], c[2], "\n", c[3], c[4], "\n", c[5], c[6], "\n",
                     lvgl.symbol.left, SPACE, BACKSPACE })
    end

    show_digits = function()
        view = "digits"
        -- Phone-dialer pad: one tap per digit. The roller this replaced
        -- needed a precise drag plus an add tap and was, in Rick's words,
        -- very challenging. Keys are 122x88 -- same class as the letter
        -- keys that verified fine on device.
        bm:set_map({ "1", "2", "3", "\n",
                     "4", "5", "6", "\n",
                     "7", "8", "9", "\n",
                     (mode == "text") and "ABC" or "00", "0", BACKSPACE })
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
            elseif t == "abc" or t == "ABC" then
                upper = not upper
                show_groups()
            elseif t == lvgl.symbol.microphone then
                start_voice()
            else
                for _, g in ipairs(GROUPS) do
                    if cased(g.label) == t then show_letters(g); break end
                end
            end
        elseif view == "letters" then
            if t == lvgl.symbol.left then
                show_groups()
            elseif t == SPACE then
                text = text .. " "; update_readout()
                upper = true            -- name/sentence case
                show_letters(current_group)
            elseif t == BACKSPACE then
                text = text:sub(1, -2); update_readout()
            else
                text = text .. t
                update_readout()
                if upper then
                    -- Auto-downshift after the first letter, phone-style:
                    -- the caps-lock default typed names as "RICK" (review).
                    upper = false
                    show_letters(current_group)
                end
            end
        elseif view == "digits" then
            if t == BACKSPACE then
                text = text:sub(1, -2); update_readout()
            elseif t == "ABC" then
                show_groups()
            else
                text = text .. t; update_readout()   -- digits and "00"
            end
        end
    end)

    -- Hold-to-repeat on backspace: standard on every keyboard, and
    -- deleting a long entry one tap per character is not acceptable.
    bm:on("long_pressed_repeat", function()
        local idx = bm:get_selected()
        if idx and bm:get_button_text(idx) == BACKSPACE and #text > 0 then
            text = text:sub(1, -2); update_readout()
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
