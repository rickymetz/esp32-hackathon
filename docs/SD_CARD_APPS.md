# An app that lives entirely on the SD card

Installing an app here is a **file copy, never a reflash**. This document is
about the next step: an app whose *code, its icon, and its saved state* all live
on the card together, so it installs, updates, and uninstalls without ever
touching the firmware.

If you just want to write app logic, read **[APP_CONTRACT.md](APP_CONTRACT.md)**
— that is the full API. This document is only about how an app is *laid out* on
the card and how its pieces get there.

---

## Two layouts

An app is either a single file or a folder. The launcher lists both, side by
side, and runs them the same way.

```
/sdcard/apps/
    counter.lua              <- flat app: one file, no icon of its own
    quicktap/                <- folder app: its own icon and assets
        main.lua             <- the code the launcher runs
        icon.png             <- source icon (you edit this)
        icon.bin             <- launcher-ready icon (generated on push)
```

**Flat** (`apps/<name>.lua`) is perfect for a quick app. Its display name comes
from the filename (`weather_clock.lua` → "Weather clock") and it gets a
generated letter/glyph avatar in the launcher.

**Folder** (`apps/<name>/main.lua`) is for an app that ships its own icon or
other assets. Its display name comes from the *folder* name, and everything the
app needs travels with it in one directory.

Both keep their saved state in a sibling directory, `state/<name>.json`, written
for you by `require("store")` (see [Persistence](#persistence)). So an app's
three pieces on the card are:

| Piece | Flat app | Folder app |
| --- | --- | --- |
| Code | `apps/name.lua` | `apps/name/main.lua` |
| Icon | (generated avatar) | `apps/name/icon.bin` |
| State | `state/name.json` | `state/name.json` |

The launcher scans `/sdcard/apps` at boot and after every push. A `.lua` file is
a flat app; a directory containing `main.lua` is a folder app; a directory
without `main.lua` is ignored.

---

## Quickstart: a folder app with an icon

```bash
# 1. Lay out the folder.
mkdir -p apps/myapp
cp apps/counter.lua apps/myapp/main.lua     # start from the template
#   ... draw a square icon at apps/myapp/icon.png (any size; 120x120 is ideal)

# 2. Push the whole folder over USB. This converts icon.png -> icon.bin and
#    sends every file into /sdcard/apps/myapp/ on the card.
./.venv/bin/python tools/push.py apps/myapp

# 3. Launch it -- tap its row (push already refreshed the list), or over serial:
./.venv/bin/python tools/drive.py run myapp
```

Edit, re-run steps 2–3, repeat. Pushing a folder always regenerates `icon.bin`
when `icon.png` is newer, so you only ever edit the PNG.

---

## Icons

The launcher has **no PNG or JPEG decoder** compiled in — adding one costs PSRAM
and only exists on the device, where it can't be tested from CI. So a folder
app ships its icon in LVGL's own binary image format, `icon.bin`, which the
launcher streams straight off the card. `tools/png2icon.py` produces it:

```bash
tools/png2icon.py apps/myapp/icon.png                 # -> apps/myapp/icon.bin
tools/png2icon.py logo.png icon.bin --size 96         # explicit output + size
tools/png2icon.py logo.png icon.bin --bg "#101018"    # flatten alpha onto this
```

- **Format**: a 12-byte LVGL header + RGB565 little-endian pixels. That is what
  the panel is, so there is no colour conversion on the device.
- **Transparency**: RGB565 has no alpha channel, so `png2icon` flattens the PNG
  onto a background colour (`--bg`, default black). The launcher's tiles sit on
  true black, so a transparent PNG composited on black looks clean — a
  full-bleed disc or square is the safe shape.
- **Size**: `png2icon` emits at `--size`, default **128** — the launcher's grid
  tile size. Keep it: LVGL *upscales* a smaller file image with a bug that clips
  the circle flat on its right and bottom edges, so a 120px icon comes out
  visibly cut. At 128 the launcher only ever downscales (in the 64px list),
  which is clean. **Author the source PNG larger** — 240 or 480 square — and let
  it downscale: `png2icon` box-filters on the way down (Pillow's LANCZOS when
  installed), so a full-bleed disc comes out smooth and fills the tile like the
  built-ins.
- **Fallback**: if `icon.bin` is missing or unreadable, the launcher falls back
  to the same glyph/letter avatar a flat app gets — so a folder app without an
  icon still looks fine.

`push.py apps/myapp` runs this for you (regenerating only when `icon.png` is
newer than `icon.bin`) and never sends the source `.png` to the device. If you'd
rather generate `icon.bin` a different way and keep it, push still uses your
committed `.bin` as long as it is newer than any `icon.png`.

You can eyeball your icon on the launcher home screen without a board: the
simulator's home preview shows a real card icon when one exists under `--sdroot`
at `apps/<name>/icon.bin` (see below).

---

## Persistence

`require("store")` gives each app its own JSON file on the card, keyed by the
app's name — you never name the path. Values are strings, numbers, booleans, and
nested tables of those.

```lua
local store = require("store")

local best = store.get("best", 0)   -- second arg is the default if never saved
if score > best then
    store.set("best", score)        -- in memory
    store.save()                    -- THIS writes the card; nothing persists until you call it
end
```

`get`/`set` only touch memory, so a tight loop can `set` freely; call `save()`
once at a natural moment. See the `store` section of
[APP_CONTRACT.md](APP_CONTRACT.md) for the full API (`all`, `clear`, …).

The saved file is `state/<name>.json` on the card — a sibling of `apps/`, not
inside the app's folder. Copying an app's folder to another board therefore
gives the recipient a **clean slate**, not your saved scores or notes.

Two shipped folder apps use all of this end to end:
`apps/quicktap/` (a high-score game) and `apps/notes/` (a persistent list).
Copy either as a starting point.

---

## Installing, updating, uninstalling

Everything is addressed by the app's **id** — a flat app's filename
(`counter.lua`) or a folder app's directory name (`myapp`).

**Over USB** (`tools/push.py`, no card shuffling):

```bash
tools/push.py apps/myapp            # a folder: pushes main.lua + icon.bin
tools/push.py apps/myapp/main.lua   # just one file of a folder app
tools/push.py apps/counter.lua      # a flat app
tools/push.py --list                # every installed app id
tools/push.py --delete myapp        # remove it (a folder is removed whole)
```

**Over serial** (the raw protocol `push.py`/`drive.py` speak; ids, not paths):

```
RUN myapp            ->  RUN_OK myapp        (folder app, by id)
RUN counter.lua      ->  RUN_OK counter.lua  (flat app, by id)
LIST                 ->  APP <id> per line, then LIST_OK <n>
DELETE myapp         ->  DELETE_OK           (removes apps/myapp/ and its contents)
```

**On the device**: **long-press** an app on the home screen to open its info
sheet — icon, name, size — with an armed **Delete** (and Cancel). This is the
same registry delete as `DELETE`-over-serial; a normal tap still just launches.

**By hand**: copy the folder (or the `.lua` file) into `/apps/` on the card with
the card in a computer. `/apps` on the card is the same directory the device
sees as `/sdcard/apps`. Tap **Refresh** on the device to rescan.

Deleting a folder app removes the whole `apps/<id>/` directory — code and icon
together. Its `state/<id>.json` is left in place, so reinstalling keeps the
saved state; delete that file too for a full reset.

---

## Developing without the board

The headless simulator runs the launcher's real Lua↔LVGL bindings against
desktop LVGL, so what it renders is what the device renders. It resolves a
folder app by id, by folder path, or by its full `main.lua` path:

```bash
(cd sim && ./setup.sh && ./build.sh)      # once

sim/simctl.py run apps/myapp : tap 184 224 : shot out.png   # folder, by path
sim/simctl.py run myapp      : shot out.png                 # folder, by id
```

`sim/test.sh` render-tests every app — flat *and* folder — so a broken folder
app fails CI the same as a broken flat one. What the simulator can't show you is
the hardware itself (touch imprecision, real sensors, failure modes); confirm on
the board. Details in `sim/README.md`.

---

## Where each piece lives — the whole map

```
/sdcard/
    apps/
        counter.lua                 flat app: code (name from filename)
        myapp/
            main.lua                folder app: code (name from folder)
            icon.bin                streamed by the launcher as the app's icon
            icon.png                source only; never read by the device
    state/
        counter.json                counter.lua's saved state
        myapp.json                  myapp's saved state
```

That is the whole story: drop a folder under `apps/`, and the app — its code,
its icon, and everything it remembers — lives on the card and nowhere else.
