-- Pomodoro -- a focus timer with saved presets, self-contained on the card.
--
--   apps/pomodoro/main.lua   this file
--   apps/pomodoro/icon.png   source icon      apps/pomodoro/icon.bin  built on push
--   state/pomodoro.json      selected preset, custom preset, in-progress
--                            session (so you resume where you left off), and
--                            today's completed count -- all via require("store")
--
-- Install:  ./.venv/bin/python tools/push.py apps/pomodoro

local lvgl   = require("lvgl")
local ui     = require("ui")
local timer  = require("timer")
local store  = require("store")
local audio  = require("audio")
local button = require("button")
local rtc    = require("rtc")

lvgl.init({ buffer_lines = 40 })

-- ---- presets -------------------------------------------------------------
-- Four presets; "Custom" is editable and its values persist. Durations are
-- whole minutes.
local PRESETS = {
    { name = "Classic", work = 25, brk = 5,  long = 15, cycles = 4 },
    { name = "Focus",   work = 50, brk = 10, long = 20, cycles = 3 },
    { name = "Short",   work = 15, brk = 3,  long = 10, cycles = 4 },
    { name = "Custom",  work = 30, brk = 8,  long = 25, cycles = 3 },
}
local CUSTOM = #PRESETS   -- index of the editable one

local saved_custom = store.get("custom", nil)
if type(saved_custom) == "table" then
    for k, v in pairs(saved_custom) do PRESETS[CUSTOM][k] = v end
end

local sel = store.get("preset", 1)
if type(sel) ~= "number" or sel < 1 or sel > #PRESETS then sel = 1 end

-- ---- today's completed count (history) -----------------------------------
local function today()
    local t = rtc.now()
    if not t then return nil end
    return string.format("%04d-%02d-%02d", t.year, t.month, t.day)
end

local hist = store.get("hist", { date = today(), count = 0 })
if hist.date ~= today() then hist = { date = today(), count = 0 } end

-- ---- session state -------------------------------------------------------
-- phase: "work" | "break" | "long".  done: completed work sessions in this set.
local phase, done, remaining, running
local tick                                   -- the 250ms sampler; declared first
local last_persist = 0

local function preset() return PRESETS[sel] end

local function phase_secs(p)
    local pr = preset()
    if p == "work" then return pr.work * 60 end
    if p == "long" then return pr.long * 60 end
    return pr.brk * 60
end

local function phase_label()
    if phase == "work" then return "Work" end
    if phase == "long" then return "Long break" end
    return "Break"
end

local function save_session()
    store.set("session", { phase = phase, done = done, remaining = remaining })
    store.set("hist", hist)
    store.save()
    last_persist = timer.now_ms()
end

-- Restore an interrupted session (always paused), else start fresh at work.
local sess = store.get("session", nil)
if type(sess) == "table" and sess.phase and sess.remaining then
    phase, done, remaining = sess.phase, sess.done or 0, sess.remaining
else
    phase, done, remaining = "work", 0, phase_secs("work")
end
running = false

-- ---- UI ------------------------------------------------------------------
local scr = lvgl.create_screen()
scr:set_style({ bg_color = "#000000" })

ui.title(scr, "Pomodoro")

local phase_lbl = lvgl.label(scr, {
    text = phase_label(), align = "top_mid", y = 84,
    text_color = "#A0A0AE", font = lvgl.font(26),
})

local clock = lvgl.label(scr, {
    text = "00:00", align = "center", y = -40,
    text_color = "#FFFFFF", font = lvgl.font(72),
})

local set_lbl = lvgl.label(scr, {
    text = "", align = "center", y = 24,
    text_color = "#A0A0AE", font = lvgl.font(26),
})

local today_lbl = lvgl.label(scr, {
    text = "", align = "bottom_mid", y = -8,
    text_color = "#6E6E7A", font = lvgl.font(26),
})

local start_btn                              -- built below; updated by refresh()

local function mmss(sec)
    if sec < 0 then sec = 0 end
    return string.format("%02d:%02d", sec // 60, sec % 60)
end

local function refresh()
    phase_lbl:set_text(phase_label())
    clock:set_text(mmss(remaining))
    set_lbl:set_text(string.format("Session %d/%d", math.min(done + 1, preset().cycles), preset().cycles))
    local h = today() and string.format("Today: %d %s", hist.count, lvgl.symbol.check_circle) or ""
    today_lbl:set_text(h)
    if start_btn then start_btn:set_text(running and (lvgl.symbol.pause .. " Pause")
                                                  or (lvgl.symbol.play .. " Start")) end
end

-- ---- timer engine --------------------------------------------------------
local deadline = 0

local function advance_phase()
    audio.play({ { 880, 150 }, { 1175, 150 }, { 1568, 300 } })
    if phase == "work" then
        done = done + 1
        if today() then hist.count = hist.count + 1 end
        if done >= preset().cycles then
            phase = "long"
        else
            phase = "break"
        end
    else                                     -- a break ended -> back to work
        if phase == "long" then done = 0 end
        phase = "work"
    end
    remaining = phase_secs(phase)
    deadline = timer.now_ms() + remaining * 1000
    save_session()
    refresh()
end

local function ensure_tick()
    if tick then return end
    tick = timer.every(250, function()
        if not running then return end
        local left = math.ceil((deadline - timer.now_ms()) / 1000)
        if left <= 0 then
            advance_phase()
            return
        end
        if left ~= remaining then
            remaining = left
            clock:set_text(mmss(remaining))
            -- Persist coarsely (~30s) so a resume is never far off, without
            -- writing the card every second.
            if timer.now_ms() - last_persist > 30000 then save_session() end
        end
    end)
end

local function set_running(on)
    running = on
    if on then
        deadline = timer.now_ms() + remaining * 1000
        ensure_tick()
    end
    save_session()
    refresh()
end

local function reset_all()
    running = false
    phase, done = "work", 0
    remaining = phase_secs("work")
    save_session()
    refresh()
end

-- ---- controls ------------------------------------------------------------
start_btn = ui.button(scr, {
    text = "Start", kind = "primary",
    align = "center", y = 118, w = 300, h = 110,
    on_click = function() set_running(not running) end,
})

ui.corner_button(scr, {
    text = lvgl.symbol.refresh, align = "bottom_left", x = 8, y = -8,
    on_click = reset_all,
})

-- Presets: a full-page picker; "Custom" opens a stepper editor.
local function edit_custom()
    local caller = lvgl.active_screen()
    local escr = lvgl.create_screen()
    escr:set_style({ bg_color = "#000000" })
    local c = PRESETS[CUSTOM]
    ui.header(escr, { title = "Custom", on_back = function()
        store.set("custom", c)
        store.set("preset", sel)
        store.save()
        if not running then remaining = phase_secs(phase) end
        caller:load(); escr:delete(); refresh()
    end })
    local list = ui.list(escr, { y = 96, h = 340, pad_row = 12 })
    ui.stepper(list, { label = "Work %d min",  min = 1, max = 90, value = c.work },   function(v) c.work = v end)
    ui.stepper(list, { label = "Break %d min", min = 1, max = 60, value = c.brk },    function(v) c.brk = v end)
    ui.stepper(list, { label = "Long %d min",  min = 1, max = 60, value = c.long },   function(v) c.long = v end)
    ui.stepper(list, { label = "Cycles %d",    min = 1, max = 12, value = c.cycles }, function(v) c.cycles = v end)
    escr:load()
end

local function open_presets()
    local names = {}
    for i, p in ipairs(PRESETS) do names[i] = p.name end
    ui.picker({ title = "Preset", options = names, selected = sel }, function(i)
        if not i then return end
        sel = i
        store.set("preset", sel); store.save()
        if i == CUSTOM then
            edit_custom()
        else
            if not running then remaining = phase_secs(phase) end
            refresh()
        end
    end)
end

ui.corner_button(scr, {
    text = lvgl.symbol.list, align = "bottom_right", x = -8, y = -8,
    on_click = open_presets,
})

-- PWR: start/pause accelerator (the on-screen button does the same).
button.on("pwr", "pressed", function() set_running(not running) end)

refresh()
scr:load()
