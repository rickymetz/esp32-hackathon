-- Scorekeeper -- pick a sport and keep score in its own format. Self-contained
-- on the card: code, icon, and saved games all live here.
--
--   apps/scorekeeper/main.lua   this file
--   apps/scorekeeper/icon.*     source PNG + built .bin
--   state/scorekeeper.json      the in-progress game (so you can resume it) and
--                               a short history of finished results -- via store
--
-- Rules depth is "right format + limits": each sport shows its correct
-- structure and targets, and you drive the scoring (tennis is the one exception
-- -- points roll into games/sets because that IS how tennis reads). Nothing
-- declares a match winner; you decide when it's over and tap Finish.
--
-- Scoring gesture: tap a side's big card to add a point; the full-width button
-- under it removes one (tennis: a single Undo reverts the last point, since a
-- point can roll a game/set). Finish and starting-over both confirm first.

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

-- ---- shared scoring widgets ----------------------------------------------
-- A big tappable score card = the +1 target (huge, with press feedback). The
-- whole card is the button, so the score itself is what you tap to add a point.
local CARD_Y = 96

local function score_card(x, name, h, on_tap)
    local b = lvgl.button(scr, { x = x, y = CARD_Y, w = 160, h = h, align = "top_left",
                                 bg_color = "#1E1E28", radius = 16 })
    b:set_style({ shadow_width = 0, pad = 0 })
    lvgl.label(b, { text = name, align = "top_mid", y = 8, text_color = "#A0A0AE", font = lvgl.font(26) })
    local val = lvgl.label(b, { text = "0", align = "center", y = 6, text_color = "#FFFFFF", font = lvgl.font(60) })
    local sub = lvgl.label(b, { text = "", align = "bottom_mid", y = -6, text_color = "#6E6E7A", font = lvgl.font(26) })
    if on_tap then b:on("clicked", on_tap) end
    return val, sub
end

local function finish_button(sport, result_fmt)
    ui.button(scr, { text = lvgl.symbol.ok .. " Finish", kind = "secondary",
                     align = "bottom_mid", y = -8, w = 344, h = 88,
        on_click = function()
            ui.confirm({ title = "Finish?", message = "Save the result and start over.",
                         confirm_label = "Finish" }, function(ok)
                if ok then record(sport, result_fmt()); show_menu() end
            end)
        end })
end

-- ---- two-side sports (pickleball, cornhole) ------------------------------
local function two_side(title, subtitle, finish_fmt)
    scr:clean()
    ui.header(scr, { title = title, on_back = show_menu })
    lvgl.label(scr, { text = subtitle, align = "top_mid", y = 70, text_color = "#6E6E7A", font = lvgl.font(26) })

    local va, vb
    local function paint() va:set_text(tostring(game.a)); vb:set_text(tostring(game.b)) end
    local function bump(key, d) game[key] = math.max(0, game[key] + d); save(); paint() end

    va = (score_card(16,  "You", 132, function() bump("a", 1) end))
    vb = (score_card(192, "Opp", 132, function() bump("b", 1) end))
    -- Full-size -1 under each card (the +1 is the card itself).
    ui.button(scr, { text = lvgl.symbol.minus, kind = "secondary", x = 16,  y = 238, w = 160, h = 96, align = "top_left",  on_click = function() bump("a", -1) end })
    ui.button(scr, { text = lvgl.symbol.minus, kind = "secondary", x = 192, y = 238, w = 160, h = 96, align = "top_left",  on_click = function() bump("b", -1) end })
    paint()
    finish_button(title, function() return finish_fmt(game) end)
end

-- ---- golf ----------------------------------------------------------------
local function par_of(h) return (game.par and game.par[h]) or 4 end

local function golf_score()
    scr:clean()
    ui.header(scr, { title = "Golf", on_back = show_menu })

    -- Per-hole par, tapped to cycle 3 -> 4 -> 5. corner_button gives a compact
    -- pill with a full 88px hit target. par-thru sums the holes already played.
    local par_thru = 0
    for h = 1, game.hole - 1 do par_thru = par_thru + par_of(h) end
    ui.corner_button(scr, { text = "Par " .. par_of(game.hole), align = "top_right", x = -4, y = 4, w = 120,
        on_click = function()
            game.par = game.par or {}
            local p = par_of(game.hole)
            game.par[game.hole] = (p >= 5) and 3 or (p + 1)
            save(); golf_score()
        end })
    lvgl.label(scr, { text = string.format("Hole %d / %d", game.hole, game.holes),
                      align = "top_mid", y = 96, text_color = "#6E6E7A", font = lvgl.font(26) })

    local list = ui.list(scr, { y = 130, h = 210, pad_row = 10 })
    for i = 1, game.np do
        local row = lvgl.container(list, { w = 344, h = 96, bg_color = "#1E1E28", radius = 12, border_width = 0, pad = 0 })
        local tp = game.tot[i] - par_thru
        local tag = (tp == 0) and "E" or (tp > 0 and ("+" .. tp) or tostring(tp))
        lvgl.label(row, { text = "P" .. i, align = "left_mid", x = 14, text_color = "#FFFFFF", font = lvgl.font(32) })
        lvgl.label(row, { text = string.format("%d (%s)", game.tot[i], tag), align = "left_mid", x = 64,
                          text_color = "#A0A0AE", font = lvgl.font(26) })
        local st = ui.stepper(row, { min = 0, max = 15, value = game.cur[i], label = "%d" }, function(v) game.cur[i] = v; save() end)
        st.row:set_size(200, 96); st.row:align("right_mid", 0, 0)
    end

    ui.button(scr, { text = (game.hole < game.holes) and ("Next hole " .. lvgl.symbol.right) or (lvgl.symbol.ok .. " Finish"),
                     kind = "primary", align = "bottom_mid", y = -8, w = 300, h = 96,
        on_click = function()
            local last = game.hole >= game.holes
            local function commit()
                for i = 1, game.np do game.tot[i] = game.tot[i] + game.cur[i]; game.cur[i] = 0 end
                if last then
                    local par_total, parts = 0, {}
                    for h = 1, game.holes do par_total = par_total + par_of(h) end
                    for i = 1, game.np do
                        local d = game.tot[i] - par_total
                        parts[i] = string.format("P%d %d(%s)", i, game.tot[i], d == 0 and "E" or (d > 0 and "+" .. d or tostring(d)))
                    end
                    record("Golf", table.concat(parts, "  ")); show_menu()
                else
                    game.hole = game.hole + 1; save(); golf_score()
                end
            end
            if last then
                ui.confirm({ title = "Finish?", message = "Save the card and start over.", confirm_label = "Finish" },
                           function(ok) if ok then commit() end end)
            else
                commit()
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
    lvgl.label(scr, { text = "points " .. lvgl.symbol.right .. " games " .. lvgl.symbol.right .. " sets",
                      align = "top_mid", y = 70, text_color = "#6E6E7A", font = lvgl.font(26) })

    local va, sub_a, vb, sub_b
    local function paint()
        local da, db = tennis_points(game.pa, game.pb)
        va:set_text(da); vb:set_text(db)
        sub_a:set_text(string.format("S%d G%d", game.sa, game.ga))
        sub_b:set_text(string.format("S%d G%d", game.sb, game.gb))
    end

    local prev = nil                        -- one-level undo snapshot
    local function snapshot()
        prev = { pa = game.pa, pb = game.pb, ga = game.ga, gb = game.gb, sa = game.sa, sb = game.sb }
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
        snapshot()
        if who == A then game.pa = game.pa + 1 else game.pb = game.pb + 1 end
        if game.pa >= 3 and game.pb >= 3 then
            if math.abs(game.pa - game.pb) >= 2 then won_game(game.pa > game.pb and A or B) end
        elseif (who == A and game.pa >= 4) or (who == B and game.pb >= 4) then
            won_game(who)
        end
        save(); paint()
    end

    va, sub_a = score_card(16,  "You", 150, function() point(A) end)
    vb, sub_b = score_card(192, "Opp", 150, function() point(B) end)
    ui.button(scr, { text = lvgl.symbol.left .. " Undo point", kind = "secondary",
                     align = "top_mid", y = 256, w = 344, h = 88,
        on_click = function()
            if not prev then return end
            game.pa, game.pb = prev.pa, prev.pb
            game.ga, game.gb = prev.ga, prev.gb
            game.sa, game.sb = prev.sa, prev.sb
            prev = nil; save(); paint()
        end })
    paint()
    finish_button("Tennis", function() return string.format("Sets %d-%d", game.sa, game.sb) end)
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
            game = { sport = "golf", holes = holes, np = n, hole = 1, par = {}, tot = {}, cur = {} }
            for p = 1, n do game.tot[p] = 0; game.cur[p] = 0 end
            -- Fill par for every hole so the array stays contiguous (1..holes) and
            -- survives the JSON round-trip as an array -- a sparse par table would
            -- come back string-keyed and integer lookups would miss (par -> 4).
            for h = 1, holes do game.par[h] = 4 end
            save(); golf_score()
        end)
    end)
end

local function start_tennis()
    game = { sport = "tennis", pa = 0, pb = 0, ga = 0, gb = 0, sa = 0, sb = 0 }; save(); tennis_score()
end
local function start_cornhole()
    game = { sport = "cornhole", a = 0, b = 0 }; save(); resume_game()
end

-- Starting a new game discards an in-progress one -- confirm first.
local function new_game(start_fn)
    if game then
        ui.confirm({ title = "Start over?", message = "This discards your current game.",
                     confirm_label = "Discard", destructive = true },
                   function(ok) if ok then start_fn() end end)
    else
        start_fn()
    end
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
        { "Golf", start_golf }, { "Pickleball", start_pickleball },
        { "Tennis", start_tennis }, { "Cornhole", start_cornhole },
    }
    for _, s in ipairs(sports) do
        ui.row(list, { text = s[1], kind = "nav", on_click = function() new_game(s[2]) end })
    end
    ui.row(list, { text = lvgl.symbol.list .. " History", kind = "nav", on_click = show_history })
end

show_menu()
scr:load()
