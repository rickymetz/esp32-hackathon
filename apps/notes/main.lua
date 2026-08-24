-- Notes -- a self-contained folder app: code, icon, and your saved notes all
-- live on the SD card. Add notes with the on-screen keyboard; they persist
-- across runs and reboots via require("store"), and tapping one offers to
-- delete it through the sanctioned confirm flow.
--
--   apps/notes/main.lua   this file
--   apps/notes/icon.png   the source icon (a dev edits this)
--   apps/notes/icon.bin   the launcher-ready icon (push.py builds it)
--   state/notes.json      the saved notes (require("store") writes it)
--
-- Install:  ./.venv/bin/python tools/push.py apps/notes

local lvgl     = require("lvgl")
local ui       = require("ui")
local store    = require("store")
local keyboard = require("keyboard")

lvgl.init({ buffer_lines = 40 })

local scr = lvgl.create_screen()
scr:set_style({ bg_color = "#000000" })

-- store.get(key, default): a plain Lua array of strings round-trips as JSON.
local items = store.get("items", {})

local list          -- the scrollable row container, built below
local empty_note    -- the "no notes yet" message when the list is empty

local function save()
    store.set("items", items)
    store.save()
end

local function render()
    list:clean()
    if empty_note then empty_note:delete(); empty_note = nil end

    if #items == 0 then
        empty_note = ui.note(scr, "No notes yet.\nTap + to add one.", { y = 0, size = 26 })
        return
    end

    -- Newest first. Lua gives each loop iteration its own `i`, so every row's
    -- delete closure captures the right index.
    for i = #items, 1, -1 do
        local text = items[i]
        ui.row(list, {
            text = text, kind = "nav",
            on_click = function()
                -- ui.confirm is the only sanctioned path to a destructive
                -- action: full-width Cancel, an armed Delete.
                ui.confirm({
                    title = "Delete note?",
                    message = text,
                    confirm_label = "Delete",
                    destructive = true,
                }, function(ok)
                    if ok then
                        table.remove(items, i)
                        save()
                        render()
                    end
                end)
            end,
        })
    end
end

local function add()
    keyboard.open({ title = "New note", mode = "text" }, function(t)
        -- t == nil means the entry was cancelled; keep the list unchanged.
        if t and t ~= "" then
            items[#items + 1] = t
            save()
            render()
        end
    end)
end

-- Root screen: no back control (BOOT exits). The + in the top-right adds a note.
ui.header(scr, { title = "Notes", action = lvgl.symbol.plus, on_action = add })

list = ui.list(scr, { y = 96, h = 340 })
render()

scr:load()
