# Design Guide — ESP32-S3-Touch-AMOLED-1.8

How to make apps readable at arm's length on a 29 × 35 mm screen.

Written after measuring the panel and reading Apple's watchOS HIG, Google's Wear OS
guidance and Samsung's One UI Watch docs. Where those platforms agree, the number here is
theirs. Where they are silent, that is said plainly rather than filled with invention.

---

## Rules for app authors — start here

If you are writing an app, these eight are the whole guide. Everything below them is the
measurement and platform research they came from, plus launcher-maintainer detail.

1. **Body text 32 px. Never below 24 px.** The default is now 32; do not shrink it.
2. **Aim for 200 × 100 tappable; never below 88 × 88.** Prefer 104 px tall rows.
3. **True black background** (`0x000000`) — battery and legibility.
4. **White text for anything that matters.** Grey is for captions only.
5. **One idea per screen**, two or three controls at most.
6. **Scroll vertically; page horizontally.** Sibling pages swipe left/right
   (`lvgl.tileview` + `ui.dots`) — allowed precisely because Home is a hardware
   button, so no edge is reserved. Never mix a drag control into a paged view.
7. **Trim long text**; do not scroll it sideways.
8. **Hero numbers 60 px** via `lvgl.font(60)` — compiled in, cannot go
   missing. A watch face whose whole point is the time can use `lvgl.font(120)`
   (digits and `.:` only).

---

## The single most useful fact

**This panel is pixel-for-pixel an Apple Watch 44mm.**

| | This board | Apple Watch 44mm |
| --- | --- | --- |
| Resolution | 368 × 448 | **368 × 448** |
| Density | 322 PPI | ~326 PPI |
| Physical | 29.0 × 35.3 mm | ~28.6 × 34.9 mm |

Apple's own HIG device table lists the 44mm (Series 4/5/6/SE) as exactly 368 × 448. So
watchOS layout numbers are not an analogy here — they transfer **directly**.

That gives us a unit system for free:

> ### 1 design unit = 2 physical pixels
>
> A watchOS **point** and a Wear OS **dp/sp** are the same size on this screen. Design in
> units, multiply by 2, get pixels.
>
> Logical canvas: **184 × 224 units**.

Viewing distance is ~30 cm — the distance from your eye to your wrist. Everything below
follows from that.

---

## Type scale

Apple and Google were consulted independently. **They agree to within 2 px at every tier**,
which is why these numbers are worth trusting:

| Role | Apple (watchOS) | Wear OS | **Use (px)** | Purpose |
| --- | --- | --- | --- | --- |
| Hero | Title 2 · 30 pt | Display Medium · 30 sp | **60** | One number that is the whole point of the app |
| Title | Title 3 · 20 pt | Title Large · 20 sp | **40** | Screen heading, app name |
| **Body** | Body · 17 pt | Body Large · 16 sp | **32** | **The default. Use this unless you have a reason.** |
| Caption | Footnote 2 · 13 pt | Label Small · 13 sp | **26** | Secondary/supporting text |
| Floor | minimum 12 pt | Body XS · 11 sp | **24** | Never go below this |

**Never below 24 px.** Apple states 12 pt as its sanctioned minimum; Google's smallest
published body token is 11 sp. Both land at 22–24 px here.

### The face is Lexend

The theme default is **Lexend Medium 32** — a face designed to reduce visual
stress and improve reading performance, compiled into the firmware at
**24/26/32/40/48/60/72/120** with LVGL's icon glyphs baked into every size. Ask
for a size with `lvgl.font(40)`; it cannot go missing with the SD card. (120 is
the watch-face hero face and carries digits and `.:` only.) (Honest caveat:
Lexend's measured reading gains are strongest in its *wider* variants, which a
368 px screen cannot spare — we inherit the design intent, not the measured
effect.)

**The built-ins go to 120 px**, so a hero numeral never needs a file: `lvgl.font(60)`
for a hero number, `lvgl.font(120)` for a watch face where the time *is* the screen.
`lvgl.font_load()` with a TTF on the card is only for a typeface we don't compile in —
and it raises if the file is missing, so it always needs a `pcall`.

Flash cost is not a concern: the app partition is 4 MB with ~76% free.

---

## Touch targets

Three independent sources, one answer:

| Source | Minimum | In pixels |
| --- | --- | --- |
| Apple watchOS | 44 × 44 pt | 88 px |
| Wear OS (standard) | 48 × 48 dp | 96 px |
| Wear OS (relaxed for watches) | 40 × 40 dp | 80 px |
| **Measured on this board** | 120 px works · 56 px drops ~half | — |

Our own measurement sits exactly where the guidelines predict. That is a good sign the
mapping is real and not numerology.

> ### Rule: no tappable thing smaller than 88 × 88 px. Aim for 200 × 100.
> 88 × 88 is the floor the platforms agree on. **200 × 100 is what we measured as
> comfortable here**, and it is what the app contract asks for.

Those are two different numbers doing two different jobs, so be clear which one applies:

| Size | Verdict |
| --- | --- |
| ≥ 200 × 100 | Aim here. A 240 × 120 button caught every tap. |
| 88 × 88 – 200 × 100 | Allowed when the layout forces it — two buttons side by side on a 368 px screen land near 164 × 104. Keep the height at 104+. |
| < 88 × 88 | Never. |

Be careful reading our own measurement: 240 × 120 worked and 180 × 56 dropped about half,
but those differ in **both** dimensions, so the data cannot tell us how much was width and
how much was height. The 56 px height is the likelier culprit — which is why the middle band
above holds height at 104 and only compromises on width.

Prefer **104 px** tall for list rows — Wear OS's standard `Chip` height, and comfortable here.

Neither Apple nor Google publishes a required *gap* between targets; both only say "don't
overlap." Use **16 px** (8 units) and rely on the size rule to do the real work.

## Layout and spacing

Apple and Google diverge here, and the reason matters.

**Apple: go full-bleed.** *"Design your content to extend from one edge of the screen to
the other. The Apple Watch bezel provides a natural visual padding."* Apple publishes no
margin number at all — the physical bezel *is* the margin.

**Google: percentage margins** — but explicitly to stop content clipping on **round**
screens. Ours is rectangular, so that rationale does not apply and their fixed dp values
are the transportable part.

For a rectangular panel with a bezel, take Apple's advice and Google's numbers:

| Element | Value | Origin |
| --- | --- | --- |
| Screen edge padding | 12–16 px | Minimal; the bezel does the work |
| Between list rows | 16 px | Wear OS 8 dp |
| Section padding | 32 px | Wear OS 16 dp |
| Title → content | 24 px | Wear OS 12 dp |
| Row internal padding | 16 px | — |

**Rows should be full width.** Side margins on a 368 px screen cost characters you cannot
spare.

Neither vendor publishes a corner radius. **12–16 px** reads well against this panel's
rounded glass.

---

## Colour

**Background is pure black — `0x000000`.** This is not a style preference:

- watchOS has **no light mode**; it is always black.
- Wear OS is **dark-theme only**: *"Watches are designed with a black background."*
- On OLED, a black pixel is an *off* pixel. On a battery device this is real power.
- Samsung adds that dark backgrounds keep content readable in direct sunlight.


**Contrast** — Apple cites WCAG AA, Google claims AAA. Design to the stricter one:

| Text | Ratio |
| --- | --- |
| Body (≤ 34 px) | **7:1** (AAA) |
| Large (≥ 36 px) or bold | **4.5:1** |

White `#FFFFFF` on black is 21:1. **`#A0A0AE` on black is 8.1:1** — the caption token, with
margin over the 7:1 rule.

`#8A8A99` is **6.2:1** — below the rule, and a tempting-looking grey. Don't invent one;
use the token. **Avoid grey body text** regardless; it is the most common way a watch UI
becomes unreadable outdoors.

**The palette.** These are the tokens the `ui` module and the launcher already use. Reach
for one of these before mixing your own, so six developers' apps look like one device:

| Token | Role |
| --- | --- |
| `#000000` | Background. Always. |
| `#FFFFFF` | Anything that matters |
| `#A0A0AE` | Captions and secondary text (8.1:1) |
| `#1E1E28` | Secondary button / raised surface |
| `#2F80ED` | Accent — the primary action |
| `#B3261E` | Destructive, and only ever behind `ui.confirm` |

**The 7:1 rule is a *text* rule.** Icons and glyphs are non-text under WCAG 1.4.11 and
need **3:1**, not 7:1 — so the accent `#2F80ED` checkmark in a `ui.select` row (5.43:1) is
correct as it stands, and does not want "fixing" to white. Audited every `text_color` in
`apps/` and the `ui` module against this: that glyph is the only thing below 7:1, and it
is the only one that should be.

**Never colour by itself.** Both platforms say this. Pair colour with text or shape. The
checkmark above obeys this too — what marks a row selected is the mark being *there*, not
its hue.

**Avoid full-screen colour on long-lived screens** — Apple calls this out specifically for
apps that stay up (a workout, a clock). Burn-in and battery.

---

## Content and interaction

**One screen, one job.** Apple: *"Support quick, glanceable, single-screen interactions."*
Interactions should take **under a minute**, often seconds. If your app needs a dashboard,
it needs to be two apps.

**At most 2–3 controls on screen.** Apple: no more than three icon buttons, or two text
buttons, in a row. Prefer one full-width button.

**Scroll vertically within a page; page horizontally between siblings.** Wear OS has to
reserve the left-edge swipe for back/dismiss, so it forbids horizontal paging. We don't:
Home is a hardware button, so no edge is reserved and `lvgl.tileview` + `ui.dots` is the
sanctioned pattern for sibling pages. See rule 6.

**Truncate with an ellipsis. Do not use marquee.** Google explicitly discourages
auto-scrolling text: it ignores reduce-motion preferences and hides content behind an
animation. Fit the label or trim it.

**Rows visible at once** — neither vendor prescribes a count. With 104 px rows and 16 px
gaps on a 448 px screen, expect **3–4 visible**, which is the right density for glancing.

---

## Physical inputs

Every wearable platform reserves the way home and hands apps at most one
button. We match: **BOOT (top right) is Home** — hardware, unconsumable, a
direct GPIO read that survives a wedged I²C bus. **PWR (bottom right) belongs
to apps** via `require("button")`.

| Platform | Exit to home | App-accessible buttons |
| --- | --- | --- |
| Apple Watch | Crown press — reserved | None (Ultra: Action Button, user-assigned) |
| Wear OS | Power — reserved | Stem buttons, with published rules |
| Samsung | Home key | Ultra's Quick Button |
| Garmin | — | All keys, but the root view always keeps back |

Wear OS is the only platform with written rules for that button, and all four
transfer (they are contract rules here): binary or eyes-free actions only; an
on-screen equivalent always exists; one press, immediate; **never destructive**
— PWR held ≥6 s powers the board off, so a destructive press sits one long
hold from data loss. The button earns its place in exactly two app categories:
health/fitness (start/stop, lap, rep) and media (play/pause).

Voice (`require("voice")`) follows the same doctrine: an accelerator with a
touch equivalent, never the only path. Commands want 2+ syllables — measured:
"start" and "stop" verify, "lap" does not; use "lap time".

## Navigation

One model, stated once:

| Level | Affordance | Owner |
| --- | --- | --- |
| Exit app → launcher | BOOT button | Launcher, unconsumable |
| Back one level | Corner control, top-left | The app (`ui.header`) |
| Between sibling pages | Horizontal swipe + `ui.dots` | The app |
| Within a page | Vertical scroll | LVGL |

Corner grammar: **`×` dismisses a sheet; `‹` pops a pushed screen; root
screens get neither** (nothing to close — BOOT is the exit). The glyph that
discards must never commit. Titles sit beside the corner control when one
exists, centred otherwise.

## Components

Patterns from the watchOS gallery (Stopwatch, Strava, Spotify, Translate,
Voice Memos, Home, NBA, Night Sky), as build-this-way guidance:

| Pattern | Build with |
| --- | --- |
| Single-select ("radio") | `ui.select` — ✓-rows, no radio circles |
| Dropdown / picker | `ui.picker` — a pushed page, never a popup |
| Toggle | `ui.row` kind="toggle" — the whole row toggles |
| Numeric value | `ui.stepper` — +/- with hold-to-repeat |
| Drag-to-set value | `ui.fill` — its own screen, never inside a tileview |
| Destructive action | `ui.confirm` — Cancel full-width, confirm armed after 400 ms |
| Text entry | `require("keyboard")` — never a hand-rolled QWERTY |
| Status feedback | `ui.toast` |
| Wait state | `ui.busy` |
| Attribute list | Bold label above, dim value below (recipe) |
| Transport row | Three round buttons, centre larger (recipe) |
| Big primary action | One large circular button + caption (recipe) |

Rules this hardware taught us, kept so nobody re-learns them:

- **Copy watchOS's arrangement, not its dimensions.** Apple's digitizer hits
  ~30 px keys; ours drops half of 180×56. Layouts transfer, sizes don't.
- **Visual size ≠ hit size.** Corner controls draw at ~72 px and hit at 88+
  (`ui.corner_button`). Icon-only corner controls are circles; text gets pills.
- **Coarse drags beat taps; fine drags lose to them.** The fill arc (coarse)
  works; a roller needing a controlled flick-and-snap was "very challenging"
  and became a dialer pad.
- **A drag surface and horizontal paging cannot share a screen** — same
  gesture domain. Fill-style controls open their own screen (Apple Home does
  exactly this).
- **A confirmation button must not sit where its trigger was**, and Cancel
  must be at least as easy to hit as the destructive action.
- **Silent no-ops read as broken** on a digitizer that drops taps — disabled
  controls acknowledge, then refuse.

## Where the platforms are silent

Stated so nobody mistakes a guess for a standard. Neither Apple nor Google publishes:

- a required gap between touch targets (only "don't overlap")
- a watchOS list row height, margin, or corner radius
- a target number of visible list items
- any measured OLED power saving, or a nit/lux threshold for outdoor legibility

Samsung publishes no numeric type scale, touch target, or contrast figure at all — its
guidance is qualitative, and it designs in raw device pixels rather than a density-
independent unit, so its numbers do not port cleanly.

Where this guide gives a number in those areas, it is our decision, derived from the
measured panel — not a platform requirement.

---

## Sources

- Apple, watchOS HIG: Layout, Typography, Buttons, Lists and Tables, Color, Accessibility
- Google, Wear OS: Accessibility, Type scale tokens, Screen shapes, Chips, Buttons, Lists, Color, Navigation, Clipping
- Google, `androidx.wear.compose.material3` `TypescaleTokens.kt` (the type scale as shipped, not as illustrated)
- Samsung, One UI Watch / Galaxy Watch Design: Touch, Typography, Colors, List
- W3C, WCAG 2.2 Contrast (Enhanced)
- Measured on this board: 322 PPI, and the 120 px / 56 px touch-target result
