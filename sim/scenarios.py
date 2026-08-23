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


# The rtc stub feeds a fixed 14:30; clock.lua renders it as big white digits.
# If the stub regressed to nil ("rtc not set"), the app degrades to a small
# message and the big-digit band goes nearly dark -- so a bright-pixel floor
# in that band proves the clock got a real time.
def clock_shows_time(w, rgb):
    n = bright_count(w, rgb, 110, 155, 260, 225)
    return n > 300, f"time band too dark ({n} bright px) -- rtc not feeding clock"


# The mirror of clock_shows_time: with `rtc unset` the hero HH:MM is replaced by
# a small "--:--", so the big-digit band's ink drops sharply (measured ~630 px
# vs ~2477 for a real time). Proves the rtc injection reaches the degraded path.
def clock_unset_dim(w, rgb):
    n = bright_count(w, rgb, 110, 155, 260, 225)
    return n < 1400, f"time band still lit ({n} bright px) -- rtc unset not honoured"


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
    # After toggling to grid, the first cell (counter) draws its cream chip and
    # amber plus at centre -- a bright illustration that stands well clear of the
    # muted dusk-gradient tile background, so brightness there proves the custom
    # icon rendered, not just that a coloured well appeared.
    r, g, b = px(w, rgb, 101, 161)
    bright = max(r, g, b)
    return bright > 170, f"first grid icon illustration not lit ({r},{g},{b}, max={bright})"


SCENARIOS = [
    ("flashlight-on",  ["run", "apps/flashlight.lua"], flashlight_on),
    ("flashlight-off", ["run", "apps/flashlight.lua", ":", "tap", "184", "224"], flashlight_off),
    ("color-default",  ["run", "apps/color.lua"], color_default_blue),
    ("color-mixed",    ["run", "apps/color.lua",
                        ":", "swipe", "200", "236", "320", "236", "300",
                        ":", "swipe", "200", "364", "66", "364", "300"], color_mixed_orange),
    ("reaction-green", ["run", "apps/reaction.lua",
                        ":", "tap", "184", "248", ":", "sleep", "4.5"], reaction_green),
    ("clock-shows-time", ["run", "apps/clock.lua", ":", "sleep", "0.3"], clock_shows_time),
    ("clock-unset-dim",  ["rtc", "unset", ":", "run", "apps/clock.lua", ":", "sleep", "0.3"], clock_unset_dim),
    ("level-centred",  ["run", "apps/level.lua", ":", "sleep", "0.3"], level_centred_green),
    ("level-tilted",   ["run", "apps/level.lua", ":", "accel", "0.5", "0", "0.866",
                        ":", "sleep", "0.3"], level_tilted_off_centre),
    ("tone-play-blue", ["run", "apps/tone.lua", ":", "sleep", "0.3"], tone_play_blue),
    ("home-header",    ["home"], home_header_lit),
    ("home-toggle-grid", ["home", ":", "tap", "324", "48", ":", "sleep", "0.3"], home_toggled_to_grid),
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
