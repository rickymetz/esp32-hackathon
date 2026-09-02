# What changed, and what it means for your app

Written for the five people writing apps against `docs/APP_CONTRACT.md`.
**One change can break an existing app; everything else only fixes things that
were already broken.** Read the first section, skim the rest.

Nothing here changes the shape of an app: build your UI, wire callbacks,
return. That is unchanged and is not going to change.

---

## 1. The one thing that can break your app

**`prefs.get("wifi_ssid")` and `prefs.get("wifi_pass")` now raise.** So does
`prefs.set` on either name.

The Wi-Fi credentials used to live in the same NVS namespace `prefs` hands to
every app, so any app could read the user's plaintext network password in one
line. They have moved to a namespace `prefs` does not expose, and boards
upgrading move them automatically on first boot.

If you were reading them — to show which network is joined, say — use the
`wifi` module instead:

```lua
local wifi = require("wifi")
wifi.status()   -- "off" | "connecting" | "connected" | "retrying" | "failed"
wifi.ip()       -- "192.168.1.42", or nil
```

To join a network, `wifi.connect(ssid, password)` saves the credentials
itself. You never needed to write them through `prefs`.

Every other `prefs` key is untouched: `volume`, `font_pct`, `face`, `tz_min`,
`tz_city`, `tz_dst`, `fps`.

This is not a sandbox and does not pretend to be one — apps share the SD card
and can read each other's files, exactly as the contract says. It is
specifically the household network password.

---

## 2. Things the contract promised that had never worked

If you hit one of these and worked around it, you can delete the workaround.
If you hit one and assumed you were holding it wrong: you weren't.

| What | Was | Now |
| --- | --- | --- |
| `audio.beep()` | Raised on **every** call — it had never made a sound | Works |
| `ui.row` toggle `set_checked()` | Silent no-op | Sets the switch |
| `ui.row` check `get()` | Always `nil` | Returns the state |
| `lvgl.image{src="apps/x/icon.bin"}` | Drew nothing | Loads |
| `wifi.status()` after a failed connect | Could stick on `"connecting"` until reboot | Resolves |

Two of these deserve a note.

**`audio.beep()`** was invisible because both call sites are event callbacks,
where an error is logged and swallowed. If your app calls `beep()` inside a
button handler, it has been silently failing the whole time.

**`lvgl.image`** only ever opened paths carrying LVGL's drive letter
(`"D:/apps/x/icon.bin"`), while the contract teaches the card-relative form
everywhere else. Both spellings work now. `..` is rejected.

---

## 3. Crashes that app code could trigger

Five of these. Two are demonstrated segfaults, not theory:

- **`lvgl.font(32):set_size(40)`** wrote into read-only flash and panicked the
  board. Built-in fonts cannot be resized — ask for another size with
  `lvgl.font(size)`. It now raises and says so.
- **Using a chart series with a different chart** than the one it came from
  wrote out of bounds on the heap. It now raises.

Also fixed: a widget handle you dropped and let the garbage collector take
could leave the C side writing into freed memory; a screen could be restored
after being freed; and `lvgl.indev_register` — undocumented, and an
arbitrary-pointer dereference — is gone from the module table.

You do not need to change anything for these. They are listed so you know the
failure mode if you saw it: a board that rebooted with no Lua error.

---

## 4. Text scale

`ui` chrome (headers, row labels, readouts) used to get **smaller** when the
user turned the font scale up — the cap was computed against unscaled pixels
and applied to snapped ones. A nominal-40 header rendered 32px at 130%.

Now chrome holds at its nominal size instead of shrinking. **Known limit,
stated plainly: chrome still does not grow above 1.0.** Making it grow means
letting the 104px rows grow with it, which has not been done.

Text you set yourself with `lvgl.font(size)` scales the whole way, 0.6–1.3, and
always did. If your app must look right at 1.3, test it there —
`sim/simctl.py --scale 1.3` or Settings on the device.

---

## 5. Display

The zebra striping when opening an app is fixed. It was not a rendering race:
the flush needed a 73,600-byte contiguous DMA buffer that could not be
allocated with an app running, so those bands were never sent and the panel
kept the previous screen's pixels. Bands are smaller now.

Nothing to do on your side. If you still see striping, say so — it would mean
the diagnosis is incomplete.

---

## 6. Your store no longer needs a perfectly-placed `save()`

**If your app forgets `store.save()`, the launcher writes the store for you
when the app exits** — including on BOOT mid-anything, and after a crash.

There is still no `on_exit` hook, and there deliberately isn't one: running
app Lua during teardown, after a stop that may have been delivered by
interrupting the interpreter mid-statement, is not a thing to add days before
a freeze. This solves what that hook would have been used for without it. The
launcher calls its own three-line function, not yours.

Keep calling `save()` at the natural moments anyway — it is the only thing
that survives a power cut, and it makes the write happen when you *meant* it.
What changed is that forgetting is no longer silent data loss.

---

## 7. Limits

`APP_MAX_COUNT` is **64**, up from 32. The card already carries 24 apps. Past
the limit apps used to be dropped silently; the launcher now warns.

Everything else in the contract's Limits table is unchanged: 16 timers, 32 KB
Lua stack, one app at a time, 368x448, tap targets >= 200x100.

---

## 8. Known broken — do not build on these

- **Swiping the watch face does not change the face.** `CLAUDE.md` and the
  architecture notes say it cycles Digital/Analog/Rings/Words/Minimal. The
  handler is registered and the screen does receive presses, but LVGL never
  emits the gesture there — while the identical synthetic swipe *does*
  produce gestures on an app's own screen. Under investigation. Your app's
  own `scr:on("gesture", ...)` is unaffected and works.
- **The FPS overlay cannot be screenshotted** — it draws on the display's
  system layer, which `SHOT` cannot reach. Look at the panel.

---

## 9. Testing your app without the board

The simulator gained checks that will fail your app if it trips them:

```bash
sim/simctl.py run apps/myapp.lua : tap 184 224 : shot out.png
sim/simctl.py --scale 1.3 run apps/myapp.lua : shot big.png
```

Worth knowing: the simulator's `audio` is a stub that always succeeds. It
could never have caught the `audio.beep()` bug, and it cannot catch the next
one. Anything about sound has to be confirmed on the board.
