#!/usr/bin/env python3
"""Scenario assertions for the simulator.

Where test.sh only asks "did the app draw *something*", these drive an app
through an interaction and assert a specific screen region has a specific
colour -- catching regressions a blank-check can't: a broken slider->swatch
binding, a dead toggle, a lost theme, a value that stops updating.

Assertions are colour/brightness on fixed pixels (no OCR), with generous
tolerances so legitimate anti-aliasing wobble doesn't trip them.

    sim/scenarios.py            # exits non-zero if any scenario fails

Requires the sim built (sim/build.sh).
"""
import os
import struct
import subprocess
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
SIM = os.path.join(HERE, "build", "sim")


def decode(path):
    """Return (w, h, rgb_bytes) for a PNG the sim wrote."""
    d = open(path, "rb").read()
    i, w, h, idat = 8, 0, 0, b""
    while i + 8 <= len(d):
        ln = struct.unpack(">I", d[i:i + 4])[0]
        typ = d[i + 4:i + 8]
        data = d[i + 8:i + 8 + ln]
        if typ == b"IHDR":
            w, h = struct.unpack(">II", data[:8])
        elif typ == b"IDAT":
            idat += data
        elif typ == b"IEND":
            break
        i += 12 + ln
    raw = zlib.decompress(idat)
    stride = 1 + w * 3
    rgb = bytearray(w * h * 3)
    for y in range(h):
        rgb[y * w * 3:(y + 1) * w * 3] = raw[y * stride + 1:y * stride + 1 + w * 3]
    return w, h, bytes(rgb)


def px(w, rgb, x, y):
    o = (y * w + x) * 3
    return rgb[o], rgb[o + 1], rgb[o + 2]


def bright_count(w, rgb, x0, y0, x1, y1, thresh=170):
    """Count pixels in a box whose channels are all >= thresh -- a
    text-agnostic 'something bright rendered here' probe, for asserting a
    readout drew (e.g. the clock's big digits) without pinning a stroke pixel."""
    n = 0
    for y in range(y0, y1):
        base = y * w * 3
        for x in range(x0, x1):
            o = base + x * 3
            if rgb[o] >= thresh and rgb[o + 1] >= thresh and rgb[o + 2] >= thresh:
                n += 1
    return n


def run(cmds):
    subprocess.run([SIM, "--sdroot", REPO] + cmds, cwd=REPO, capture_output=True)


# --- assertions (return (ok, message)) -----------------------------------

def flashlight_on(w, rgb):
    r, g, b = px(w, rgb, 184, 224)
    return (r > 200 and g > 200 and b > 200), f"centre not white: {r},{g},{b}"


def flashlight_off(w, rgb):
    r, g, b = px(w, rgb, 184, 224)
    return (r < 40 and g < 40 and b < 40), f"centre not black: {r},{g},{b}"


# NB: sample away from centred text/readouts -- the swatch and reaction pad
# both draw a centred label, so probe a corner of the fill instead.
def color_default_blue(w, rgb):
    r, g, b = px(w, rgb, 60, 110)
    return (b > 150 and b > r and b > g), f"swatch not blue: {r},{g},{b}"


def color_mixed_orange(w, rgb):
    r, g, b = px(w, rgb, 60, 110)
    return (r > 180 and b < 90 and r > b), f"swatch not orange: {r},{g},{b}"


def reaction_green(w, rgb):
    r, g, b = px(w, rgb, 80, 320)
    return (g > 120 and g > r and g > b), f"pad not green: {r},{g},{b}"


# The sim IMU reads flat, so level.lua's bubble sits centred and green. A
# regression (imu returns nil, or the level threshold breaks) would leave the
# centre amber or empty. Probe the vial centre (CX=184, CY=214).
def level_centred_green(w, rgb):
    r, g, b = px(w, rgb, 184, 214)
    return (g > 150 and g > r and g > b), f"bubble not green at centre: {r},{g},{b}"


# tone.lua's Play button is a solid accent-blue slab at the bottom. Probe an
# off-centre fill pixel -- NOT the horizontal centre, which lands on the
# "> Play" label (per the sample-away-from-centred-text rule above).
def tone_play_blue(w, rgb):
    r, g, b = px(w, rgb, 250, 360)
    return (b > 150 and b > r and b > g), f"play button not blue: {r},{g},{b}"


# Inject a 30deg tilt (accel 0.5,0,0.866): the bubble must LEAVE the centre, so
# the vial centre goes dark. This exercises the `accel` injection command and
# the tilt path that a flat sim could never reach.
def level_tilted_off_centre(w, rgb):
    r, g, b = px(w, rgb, 184, 214)
    return (g < 90 and r < 90), f"centre still lit after tilt: {r},{g},{b}"


# The launcher home (shared launcher_home_build) draws the white "Apps" header
# at top centre. A bright band there proves the launcher's own screen builds.
def home_header_lit(w, rgb):
    n = bright_count(w, rgb, 120, 25, 250, 70)
    return n > 200, f"Apps header not rendered ({n} bright px)"


# Tapping the header toggle from the list switches to the 2x2 grid, whose tiles
# carry a saturated colour icon (the list has none). Probe the first tile's icon
# and require it be strongly coloured -- proving the toggle rebuilt as a grid,
# independent of which palette colour the name hashes to.
def home_toggled_to_grid(w, rgb):
    # Two-point check so a broken toggle (list stays up) can't false-pass: the
    # first grid tile is bright at its centre AND the point just left of it is
    # pure black (a tile gap). In list view that left-edge point sits inside a
    # navy row, so the black-gap test is what actually proves we switched.
    cr, cg, cb = px(w, rgb, 101, 161)      # tile 1 centre
    gr, gg, gb = px(w, rgb, 30, 161)       # gap left of tile 1
    tile_lit = max(cr, cg, cb) > 170
    gap_black = max(gr, gg, gb) < 16
    return tile_lit and gap_black, \
        f"not grid (tile max={max(cr, cg, cb)}, gap max={max(gr, gg, gb)})"


def home_back_to_list(w, rgb):
    # After toggling twice we're back to the list: the point left of where tile 1
    # sat is now the navy row background (~41), not the black grid gap (0) and not
    # a bright tile -- so the second toggle actually rebuilt the list.
    r, g, b = px(w, rgb, 30, 161)
    m = max(r, g, b)
    return 16 <= m <= 90, f"not list after round-trip (left-edge max={m})"


def _row_bands(w, rgb, y0=140, y1=340):
    """Count ui.row cards in the list band.

    Two things this has to survive, both found by measuring the real frame
    rather than assuming:

    - The card colour is (25, 28, 41), not the #1E1E28 the source sets --
      LVGL composites it against the screen.
    - A row's label is white text, so sampling a single column counts one row
      as two bands where the text interrupts it. Instead a scanline counts as
      "row" when MOST of its width is card-coloured, which text cannot break.

    Counting rows and not raw pixels is what makes this discriminate: the
    pre-scan app drew 2 rows and a scan list draws 4, but both produce "lots
    of card pixels".
    """
    def is_row_line(y):
        n = 0
        for x in range(20, w - 20, 2):
            r, g, b = px(w, rgb, x, y)
            if abs(r - 25) < 10 and abs(g - 28) < 10 and abs(b - 41) < 10:
                n += 1
        # Measured: a full-width row samples 164, a row where the label's
        # text interrupts drops to ~70, a gap is 0. 50 sits below the text
        # minimum so a label cannot split one row into two bands, and far
        # above zero so a gap cannot join two rows into one.
        return n > 50

    bands, run = 0, False
    for y in range(y0, y1):
        line = is_row_line(y)
        if line and not run:
            bands += 1
        run = line
    return bands


# These two run as a PAIR and it is the difference between them that means
# something. Only 2 rows fit on this panel (104px touch floor, 448px screen),
# so a populated list and the old fixed two-row app both show 2 bands -- a
# count alone cannot tell them apart. The empty case can: scan-driven
# rendering collapses to the single manual-entry row, while a fixed list does
# not change at all.
def wifi_lists_networks(w, rgb):
    """A resolved scan fills the visible list; the rest scrolls."""
    n = _row_bands(w, rgb)
    return n >= 2, f"only {n} row bands -- scan results never rendered as rows"


def wifi_empty_state(w, rgb):
    """`wifi scan 0` must collapse the list to just 'Other network...'.

    This is the discriminating half: the pre-scan app drew 2 rows regardless
    of any scan, so <=1 here is only reachable if the list really is built
    from scan results.
    """
    n = _row_bands(w, rgb)
    return n <= 1, f"{n} row bands -- expected only the manual-entry row"


SCENARIOS = [
    # #9's scan coverage, re-pointed: apps/wifi_setup.lua was folded into
    # Settings, so these drive Settings -> Wi-Fi (tap 184 268 from the menu)
    # rather than a top-level app. The sleep is 1.5s, not the usual 0.4: the
    # scan stub resolves after ~3 polls of the page's 250ms timer, so 0.4
    # captures "scanning..." -- the least informative state.
    ("wifi-lists-networks", ["run", "apps/settings.lua", ":", "sleep", "0.3",
                             ":", "tap", "184", "268", ":", "sleep", "1.5"],
     wifi_lists_networks),
    ("wifi-empty-state",    ["wifi", "scan", "0",
                             ":", "run", "apps/settings.lua", ":", "sleep", "0.3",
                             ":", "tap", "184", "268", ":", "sleep", "1.5"],
     wifi_empty_state),
    ("flashlight-on",  ["run", "apps/flashlight.lua"], flashlight_on),
    ("flashlight-off", ["run", "apps/flashlight.lua", ":", "tap", "184", "224"], flashlight_off),
    ("color-default",  ["run", "apps/color.lua"], color_default_blue),
    ("color-mixed",    ["run", "apps/color.lua",
                        ":", "swipe", "150", "250", "256", "250", "300",   # R -> max
                        ":", "swipe", "200", "394", "56", "394", "300"],   # B -> min
                       color_mixed_orange),
    ("reaction-green", ["run", "apps/reaction.lua",
                        ":", "tap", "184", "248", ":", "sleep", "4.5"], reaction_green),
    ("level-centred",  ["run", "apps/level.lua", ":", "sleep", "0.3"], level_centred_green),
    ("level-tilted",   ["run", "apps/level.lua", ":", "accel", "0.5", "0", "0.866",
                        ":", "sleep", "0.3"], level_tilted_off_centre),
    ("tone-play-blue", ["run", "apps/tone.lua", ":", "sleep", "0.3"], tone_play_blue),
    ("home-header",    ["home"], home_header_lit),
    ("home-toggle-grid", ["home", ":", "tap", "324", "48", ":", "sleep", "0.3"], home_toggled_to_grid),
    ("home-toggle-roundtrip", ["home", ":", "tap", "324", "48", ":", "sleep", "0.3",
                                        ":", "tap", "324", "48", ":", "sleep", "0.3"], home_back_to_list),
]


def main():
    if not os.path.exists(SIM):
        sys.exit("sim not built -- run sim/build.sh")
    os.makedirs(os.path.join(HERE, "build"), exist_ok=True)

    failures = 0
    for name, cmds, check in SCENARIOS:
        out = os.path.join(HERE, "build", f"scen_{name}.png")
        run(cmds + [":", "shot", out])
        try:
            w, _, rgb = decode(out)
            ok, msg = check(w, rgb)
        except Exception as e:  # a crash/blank frame is a failure, not a traceback
            ok, msg = False, f"decode error: {e}"
        if ok:
            print(f"  ok    {name}")
        else:
            print(f"  FAIL  {name}  -- {msg}")
            failures += 1

    print(f"\nscenarios: {len(SCENARIOS) - failures} ok, {failures} failed")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
