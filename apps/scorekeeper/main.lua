-- Scorekeeper -- pick a sport and keep score in its own format. Self-contained
-- on the card: code, icon, and saved games all live here.
--
--   apps/scorekeeper/main.lua   this file
--   apps/scorekeeper/icon.*     source PNG + built .bin
--   state/scorekeeper.json      the in-progress game (so you can resume it) and
--                               a short history of finished results -- via store
--
-- Rules depth is "right format + limits": each sport shows its correct
-- structure and targets, and you drive the scoring with +/- (tennis is the one
-- exception -- points roll into games/sets because that IS how tennis reads).
-- Nothing declares a match winner; you decide when it's over and tap the check.

local lvgl  = require("lvgl")
local ui    = require("ui")
local store = require("store")
local rtc   = require("rtc")

lvgl.init({ buffer_lines = 40 })

local scr = lvgl.create_screen()
scr:set_style({ bg_color = "#000000" })

-- ---- persistence ---------------------------------------------------------
local game = store.get("game", nil)          -- the in-progress game, or nil
local hist = store.get("hist", {})           -- finished results, newest last
if type(hist) ~= "table" then hist = {} end

local function today()
    local t = rtc.now()
    return t and string.format("%d/%d", t.month, t.day) or ""
end

local function save()
    store.set("game", game); store.set("hist", hist); store.save()
end

local function record(sport, result)
    hist[#hist + 1] = { sport = sport, result = result, date = today() }
    while #hist > 12 do table.remove(hist, 1) end
    game = nil
    save()
end

local A, B = 1, 2
local show_menu                              -- forward decl

-- One consistent scoring layout across sports so long titles ("Pickleball")
-- never fight a header action: back + title only (title fits), score cards up
-- top, controls in the middle band, a full-width Finish pinned to the bottom.
local CARDS_Y, CARDS_H = 118, 132
local CTRL_Y,  CTRL_H  = 258, 92

-- Two score cards side by side; returns their value labels to update.
local function two_cards(nameA, nameB)
    local function card(x, name)
        local c = ui.card(scr, { x = x, y = CARDS_Y, w = 160, h = CARDS_H })
        lvgl.label(c, { text = name, align = "top_mid", text_color = "#A0A0AE", font = lvgl.font(26) })
        return lvgl.label(c, { text = "0", align = "center", y = 6, text_color = "#FFFFFF", font = lvgl.font(60) })
    end
    return card(16, nameA), card(192, nameB)
end

local function finish_button(on_click)
    ui.button(scr, { text = lvgl.symbol.ok .. " Finish", kind = "secondary",
                     align = "bottom_mid", y = -8, w = 344, h = 88, on_click = on_click })
end

-- +1 (wide) and -1 (narrow) for one side, in the middle control band.
local function side_controls(side, on_plus, on_minus)
    local plus_x, minus_x = (side == A) and 16 or -16, (side == A) and 128 or -128
    local anchor = (side == A) and "top_left" or "top_right"
    ui.button(scr, { text = "+1", kind = "primary",   x = plus_x,  y = CTRL_Y, w = 104, h = CTRL_H, align = anchor, on_click = on_plus })
    ui.button(scr, { text = lvgl.symbol.minus, kind = "secondary", x = minus_x, y = CTRL_Y, w = 48, h = CTRL_H, align = anchor, on_click = on_minus })
end

-- ---- two-side sports (pickleball, cornhole) ------------------------------
local function two_side(title, subtitle, finish_fmt)
    scr:clean()
    ui.header(scr, { title = title, on_back = show_menu })
    lvgl.label(scr, { text = subtitle, align = "top_mid", y = 96, text_color = "#6E6E7A", font = lvgl.font(26) })

    local va, vb = two_cards("You", "Opp")
    local function bump(key, d)
        game[key] = math.max(0, game[key] + d); save()
        va:set_text(tostring(game.a)); vb:set_text(tostring(game.b))
    end
    va:set_text(tostring(game.a)); vb:set_text(tostring(game.b))
    side_controls(A, function() bump("a", 1) end, function() bump("a", -1) end)
    side_controls(B, function() bump("b", 1) end, function() bump("b", -1) end)
    finish_button(function() record(title, finish_fmt(game)); show_menu() end)
end

-- ---- golf ----------------------------------------------------------------
local PAR = 4                                -- par per hole (flat, keeps it simple)

local function golf_score()
    scr:clean()
    ui.header(scr, { title = "Golf", on_back = show_menu })

    local par_thru = (game.hole - 1) * PAR
    lvgl.label(scr, { text = string.format("Hole %d / %d  ", game.hole, game.holes) .. lvgl.symbol.bullet .. string.format("  par %d", PAR),
                      align = "top_mid", y = 96, text_color = "#6E6E7A", font = lvgl.font(26) })

    local list = ui.list(scr, { y = 140, h = 200, pad_row = 10 })
    for i = 1, game.np do
        local row = lvgl.container(list, { w = 344, h = 88, bg_color = "#1E1E28", radius = 12, border_width = 0, pad = 0 })
        local tp = game.tot[i] - par_thru
        local tag = (tp == 0) and "E" or (tp > 0 and ("+" .. tp) or tostring(tp))
        lvgl.label(row, { text = "P" .. i, align = "left_mid", x = 14, text_color = "#FFFFFF", font = lvgl.font(32) })
        lvgl.label(row, { text = string.format("%d (%s)", game.tot[i], tag), align = "left_mid", x = 66,
                          text_color = "#A0A0AE", font = lvgl.font(26) })
        local st = ui.stepper(row, { min = 0, max = 15, value = game.cur[i], label = "%d" }, function(v) game.cur[i] = v; save() end)
        st.row:set_size(176, 84); st.row:align("right_mid", -6, 0)
    end

    ui.button(scr, { text = (game.hole < game.holes) and ("Next hole " .. lvgl.symbol.right) or "Finish",
                     kind = "primary", align = "bottom_mid", y = -8, w = 300, h = 96,
        on_click = function()
            for i = 1, game.np do game.tot[i] = game.tot[i] + game.cur[i]; game.cur[i] = 0 end
            if game.hole >= game.holes then
                local par_total, parts = game.holes * PAR, {}
                for i = 1, game.np do
                    local d = game.tot[i] - par_total
                    parts[i] = string.format("P%d %d(%s)", i, game.tot[i], d == 0 and "E" or (d > 0 and "+" .. d or tostring(d)))
                end
                record("Golf", table.concat(parts, "  "))
                show_menu()
            else
                game.hole = game.hole + 1; save(); golf_score()
            end
        end })
end

-- ---- tennis --------------------------------------------------------------
local function tennis_points(pa, pb)
    local names = { [0] = "0", [1] = "15", [2] = "30", [3] = "40" }
    if pa < 3 or pb < 3 then return names[pa], names[pb] end
    if pa == pb then return "40", "40" end                 -- deuce
    if pa == pb + 1 then return "Ad", "40" end
    if pb == pa + 1 then return "40", "Ad" end
    return "40", "40"
end

local function tennis_score()
    scr:clean()
    ui.header(scr, { title = "Tennis", on_back = show_menu })
    lvgl.label(scr, { text = "points " .. lvgl.symbol.right .. " games " .. lvgl.symbol.right .. " sets", align = "top_mid", y = 96,
                      text_color = "#6E6E7A", font = lvgl.font(26) })

    local function tcard(x, name)
        local c = ui.card(scr, { x = x, y = CARDS_Y, w = 160, h = CARDS_H + 18 })
        lvgl.label(c, { text = name, align = "top_mid", text_color = "#A0A0AE", font = lvgl.font(26) })
        local big = lvgl.label(c, { text = "0", align = "center", y = -6, text_color = "#FFFFFF", font = lvgl.font(60) })
        local sub = lvgl.label(c, { text = "", align = "bottom_mid", text_color = "#6E6E7A", font = lvgl.font(26) })
        return big, sub
    end
    local va, sub_a = tcard(16, "You")
    local vb, sub_b = tcard(192, "Opp")
    local function paint()
        local da, db = tennis_points(game.pa, game.pb)
        va:set_text(da); vb:set_text(db)
        sub_a:set_text(string.format("S%d G%d", game.sa, game.ga))
        sub_b:set_text(string.format("S%d G%d", game.sb, game.gb))
    end

    local function won_game(who)
        if who == A then game.ga = game.ga + 1 else game.gb = game.gb + 1 end
        game.pa, game.pb = 0, 0
        local g  = (who == A) and game.ga or game.gb
        local og = (who == A) and game.gb or game.ga
        if g >= 6 and g - og >= 2 then                     -- take the set (no tiebreak)
            if who == A then game.sa = game.sa + 1 else game.sb = game.sb + 1 end
            game.ga, game.gb = 0, 0
        end
    end
    local function point(who)
        if who == A then game.pa = game.pa + 1 else game.pb = game.pb + 1 end
        if game.pa >= 3 and game.pb >= 3 then
            if math.abs(game.pa - game.pb) >= 2 then won_game(game.pa > game.pb and A or B) end
        elseif (who == A and game.pa >= 4) or (who == B and game.pb >= 4) then
            won_game(who)
        end
        save(); paint()
    end

    ui.button(scr, { text = "+", kind = "primary", x = 16,  y = CTRL_Y, w = 160, h = CTRL_H, align = "top_left",  on_click = function() point(A) end })
    ui.button(scr, { text = "+", kind = "primary", x = -16, y = CTRL_Y, w = 160, h = CTRL_H, align = "top_right", on_click = function() point(B) end })
    finish_button(function() record("Tennis", string.format("Sets %d-%d", game.sa, game.sb)); show_menu() end)
    paint()
end

-- ---- dispatch to the right scoring screen for the current `game` ----------
local function resume_game()
    if game.sport == "golf" then golf_score()
    elseif game.sport == "tennis" then tennis_score()
    elseif game.sport == "pickleball" then
        two_side("Pickleball", game.variant .. "  " .. lvgl.symbol.bullet .. "  to 11, win by 2",
                 function(g) return string.format("%d-%d", g.a, g.b) end)
    else
        two_side("Cornhole", "to 21", function(g) return string.format("%d-%d", g.a, g.b) end)
    end
end

-- ---- setup flows ---------------------------------------------------------
local function start_pickleball()
    ui.picker({ title = "Scoring", options = { "Standard", "Rally" } }, function(i)
        if not i then return end
        game = { sport = "pickleball", variant = i == 2 and "Rally" or "Standard", a = 0, b = 0 }
        save(); resume_game()
    end)
end

local function start_golf()
    ui.picker({ title = "Holes", options = { "9 holes", "18 holes" } }, function(i)
        if not i then return end
        local holes = i == 2 and 18 or 9
        ui.picker({ title = "Players", options = { "1", "2", "3", "4" }, selected = 2 }, function(n)
            if not n then return end
            game = { sport = "golf", holes = holes, np = n, hole = 1, tot = {}, cur = {} }
            for p = 1, n do game.tot[p] = 0; game.cur[p] = 0 end
            save(); golf_score()
        end)
    end)
end

-- ---- history view --------------------------------------------------------
local function show_history()
    scr:clean()
    ui.header(scr, { title = "History", on_back = show_menu })
    if #hist == 0 then
        ui.note(scr, "No finished games yet.", { y = 0, size = 26 })
        return
    end
    local list = ui.list(scr, { y = 96, h = 344 })
    for i = #hist, 1, -1 do
        local r = hist[i]
        local row = lvgl.container(list, { w = 344, h = 88, bg_color = "#1E1E28", radius = 12, border_width = 0, pad = 0 })
        lvgl.label(row, { text = r.sport, align = "left_mid", x = 16, text_color = "#FFFFFF", font = lvgl.font(32) })
        lvgl.label(row, { text = r.result, align = "right_mid", x = -16, text_color = "#A0A0AE", font = lvgl.font(24) })
    end
end

-- ---- main menu -----------------------------------------------------------
show_menu = function()
    scr:clean()
    ui.title(scr, "Scorekeeper")
    local list = ui.list(scr, { y = 90, h = 358, pad_row = 12 })

    if game then
        local names = { golf = "Golf", tennis = "Tennis", pickleball = "Pickleball", cornhole = "Cornhole" }
        ui.button(list, { text = lvgl.symbol.play .. " Resume " .. (names[game.sport] or ""), kind = "primary",
                          w = 344, h = 96, on_click = resume_game })
    end

    local sports = {
        { "Golf", start_golf },
        { "Pickleball", start_pickleball },
        { "Tennis", function() game = { sport = "tennis", pa = 0, pb = 0, ga = 0, gb = 0, sa = 0, sb = 0 }; save(); tennis_score() end },
        { "Cornhole", function() game = { sport = "cornhole", a = 0, b = 0 }; save(); resume_game() end },
    }
    for _, s in ipairs(sports) do
        ui.row(list, { text = s[1], kind = "nav", on_click = s[2] })
    end
    ui.row(list, { text = lvgl.symbol.list .. " History", kind = "nav", on_click = show_history })
end

show_menu()
scr:load()
