#!/usr/bin/env python3
"""Golden-frame regression for the simulator.

Where scenarios.py asserts a few hand-picked pixels, this checks the *whole*
frame against a committed reference. It is a net for STRUCTURAL / GROSS
regressions -- a frame that renders blank or wholly different, a background or
theme swap, a large fill recolouring, a major layout break. It is deliberately
NOT a pixel-diff: to stay committable and robust across machines, each golden is
the canonical frame downscaled 4x to a 92x112 thumbnail (box-averaged, which
smooths anti-aliasing), and a frame fails only if more than FRAC_TOL of its
cells move past CELL_TOL. That budget means a *small* change -- one caption
vanishing, a button nudged a few px, a 1px border recolouring -- can pass here.
Guard those specific elements with a scenarios.py probe instead; this catches
the big stuff a hand-picked pixel would miss. The committed thumbnails render in
GitHub diffs, so a golden update is reviewable by eye.

    sim/golden.py            # compare every frame to its golden; non-zero on drift
    sim/golden.py --update [name...]   # regenerate all goldens, or just the named ones

Coverage is each app's initial settled frame plus a set of driven ones -- a
post-tap value, the launcher error screen, an open keyboard, a tileview second
page, and sensor-injected degraded states (unset clock, low battery, tilted
level). Only deterministic frames qualify: apps driven by math.random or
mid-animation (dice, simon, reaction, breathe, metronome) are left to
scenarios.py. Requires the sim built (sim/build.sh).
"""
import os
import struct
import subprocess
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
SIM = os.path.join(HERE, "build", "sim")
GOLDEN_DIR = os.path.join(HERE, "golden")

SCALE = 4          # 368x448 -> 92x112 thumbnail
CELL_TOL = 28      # per-channel delta below which a cell is "unchanged"
# The sim renders deterministically (software rasterizer, compiled bitmap fonts,
# RGB565, pinned LVGL/Lua), so cross-machine drift on an unchanged frame is ~0 --
# leaving headroom to keep this tight. 1.2% (~124 of 10304 cells) still passes
# AA wobble but trips on a genuine structural change.
FRAC_TOL = 0.012

# name -> commands that drive the app to its canonical, settled frame.
# Keep these deterministic: no randomness, no mid-animation capture. clock and
# faces render seconds (a :SS field / a second hand) and are only stable because
# the sim RTC is frozen at sec=0 (sim_sensors.c); if that clock ever advances,
# re-capture those two or mask the seconds region.
FRAMES = [
    ("counter",      ["run", "apps/counter.lua", ":", "sleep", "0.4"]),
    ("color",        ["run", "apps/color.lua", ":", "sleep", "0.4"]),
    ("flashlight",   ["run", "apps/flashlight.lua", ":", "sleep", "0.4"]),
    ("hello_world",  ["run", "apps/hello_world.lua", ":", "sleep", "0.4"]),
    ("sensors_test", ["run", "tests/fixtures/sensors_test.lua", ":", "sleep", "0.4"]),
    ("settings",     ["run", "apps/settings.lua", ":", "sleep", "0.4"]),
    ("tally",        ["run", "apps/tally.lua", ":", "sleep", "0.4"]),
    ("sign",         ["run", "apps/sign.lua", ":", "sleep", "0.4"]),
    ("countdown",    ["run", "apps/countdown.lua", ":", "sleep", "0.4"]),
    ("stopwatch",    ["run", "apps/stopwatch.lua", ":", "sleep", "0.4"]),
    ("tip",          ["run", "apps/tip.lua", ":", "sleep", "0.4"]),
    ("level",        ["run", "apps/level.lua", ":", "sleep", "0.4"]),
    ("tone",         ["run", "apps/tone.lua", ":", "sleep", "0.4"]),
    ("calculator",   ["run", "apps/calculator.lua", ":", "sleep", "0.4"]),

    # --- Interaction / injected states (beyond the initial settled frame) ---
    # A post-tap value, the launcher error screen, an open keyboard, a tileview
    # second page, and the sensor-injected degraded clock/level states -- the
    # dynamic surfaces a first-frame snapshot can't reach.
    ("counter_tapped", ["run", "apps/counter.lua",
                        ":", "tap", "184", "224", ":", "tap", "184", "224",
                        ":", "tap", "184", "224", ":", "sleep", "0.2"]),
    ("error_screen", ["run", "tests/fixtures/broken.lua", ":", "sleep", "0.3"]),
    ("keyboard",     ["run", "apps/sign.lua", ":", "tap", "184", "380", ":", "sleep", "0.3"]),
    ("level_tilted", ["run", "apps/level.lua",
                        ":", "accel", "0.5", "0", "0.866", ":", "sleep", "0.3"]),
    # Calculator keypad + logic, three discriminating results so an operator
    # swap, a format regression (3 vs 3.0), or a broken divide-by-zero guard
    # each change a distinct frame -- not just one addition that a loose
    # tolerance could wave through.
    ("calc_sum",     ["run", "apps/calculator.lua",       # 2 + 3 = 5
                        ":", "tap", "138", "314", ":", "tap", "322", "403",
                        ":", "tap", "230", "314", ":", "tap", "230", "403",
                        ":", "sleep", "0.2"]),
    ("calc_div",     ["run", "apps/calculator.lua",       # 8 / 2 = 4 (integer format)
                        ":", "tap", "138", "135", ":", "tap", "322", "135",
                        ":", "tap", "138", "314", ":", "tap", "230", "403",
                        ":", "sleep", "0.2"]),
    ("calc_err",     ["run", "apps/calculator.lua",       # 1 / 0 = Err (sticky red guard)
                        ":", "tap", "46", "314", ":", "tap", "322", "135",
                        ":", "tap", "46", "403", ":", "tap", "230", "403",
                        ":", "sleep", "0.2"]),

    # The launcher's OWN home screen (the app list), built by the shared
    # launcher_home_build() with a fake app list -- the most-seen surface, and
    # the one the sim couldn't reach until it was factored out of launcher_main.
    ("home",         ["home"]),
    ("home_empty",   ["home", "0"]),
    ("home_grid",    ["home", "grid", ":", "sleep", "0.3"]),
    ("home_grid_p2", ["home", "grid",
                        ":", "swipe", "300", "224", "60", "224", "250",
                        ":", "sleep", "0.6"]),
    # The last grid page carries the custom metronome image alongside the
    # glyph-avatar fallback tiles (Settings, Color) -- covers both icon paths.
    ("home_grid_last", ["home", "grid",
                        ":", "swipe", "300", "224", "60", "224", "250", ":", "sleep", "0.4",
                        ":", "swipe", "300", "224", "60", "224", "250", ":", "sleep", "0.4",
                        ":", "swipe", "300", "224", "60", "224", "250", ":", "sleep", "0.6"]),
    # One more page reaches the letter-avatar fallback tile (an unmapped app,
    # "Zebra"), the icon path neither an image nor a glyph covers.
    ("home_grid_p5", ["home", "grid",
                        ":", "swipe", "300", "224", "60", "224", "250", ":", "sleep", "0.4",
                        ":", "swipe", "300", "224", "60", "224", "250", ":", "sleep", "0.4",
                        ":", "swipe", "300", "224", "60", "224", "250", ":", "sleep", "0.4",
                        ":", "swipe", "300", "224", "60", "224", "250", ":", "sleep", "0.6"]),

    # The app-info sheet (long-press-to-delete). "Metronome" exercises the
    # compiled-image icon in the sheet; the card variant points at a committed
    # RGB565 .bin, so it also covers the card-icon decode path deterministically
    # (the fixture the removed home-preview probe used to reach).
    ("sheet",        ["sheet", "Metronome"]),
    ("sheet_card",   ["sheet", "Quicktap", "D:/apps/quicktap/icon.bin"]),
    # A built-in has no file to delete, so the sheet drops Delete entirely and
    # Cancel takes its place -- a control that must refuse is worse than none.
    ("sheet_builtin", ["sheet", "Settings", "builtin"]),
    # The card-less home: built-ins only, the "No SD card" note, no view
    # toggle. Untestable until the sim could render it -- sd_mounted was
    # hardcoded true -- and the first version of this screen had the note
    # painted over by the first row's opaque background.
    ("home_nocard",  ["home", "nocard"]),

    # The built-in watch face -- the shell's home screen, so the most-seen
    # surface on the device. Three cases because its degraded states are where
    # the bugs are: "unset" is the fresh-board/dead-cell path (and the reason
    # the placeholder is not drawn in the 120px face, whose charset is digits
    # and ".:" only -- a "--:--" there renders as empty boxes), and the low
    # battery case covers the warning colour and the stepped battery glyph.
    # Settings: the menu plus the two pages most likely to regress -- the
    # timezone page (which writes the NVS offset the C face reads) and Wi-Fi
    # (the absorbed wifi_setup).
    ("settings_time", ["run", "apps/settings.lua", ":", "sleep", "0.3",
                       ":", "tap", "184", "384", ":", "sleep", "0.4"]),
    # 1.5s, not the usual 0.4: the scan stub resolves after ~3 polls of the
    # page's 250ms timer (~0.75s), so 0.4 captures "scanning..." -- the least
    # informative state, and only ~2x from flipping to the populated list if
    # timings ever shift. 1.5s is clear of both boundaries.
    ("settings_wifi", ["run", "apps/settings.lua", ":", "sleep", "0.3",
                       ":", "tap", "184", "268", ":", "sleep", "1.5"]),

    # The same two pages at the 1.3 font scale -- the accessibility ceiling, and
    # the state the simulator could not reach until --scale existed. Everything
    # here rendered correctly at 1.0 while being broken at 1.3: the header title
    # wrapped and was clipped by the list, row labels left their 104px cards,
    # the stepper readouts were eaten by their own +/- slabs, and the footer
    # note ran off the bottom of the screen. Cheap to keep, and the only thing
    # standing between that class of bug and a release.
    ("settings_time_13", ["--scale", "1.3",
                          "run", "apps/settings.lua", ":", "sleep", "0.3",
                          ":", "tap", "184", "384", ":", "sleep", "0.4"]),
    # The same page at the DEFAULT scale. There was no 1.0 golden for it, only
    # the 1.3 one below, which is how "Display & sound" wrapped and was clipped
    # by its own list -- at the default scale, on a shipped page -- without any
    # test noticing. Long strings overflow independently of the font scale.
    ("settings_disp", ["run", "apps/settings.lua", ":", "sleep", "0.3",
                       ":", "swipe", "184", "380", "184", "120", "400",
                       ":", "sleep", "0.4",
                       ":", "tap", "184", "265", ":", "sleep", "0.4"]),
    ("settings_disp_13", ["--scale", "1.3",
                          "run", "apps/settings.lua", ":", "sleep", "0.3",
                          ":", "swipe", "184", "380", "184", "120", "400",
                          ":", "sleep", "0.4",
                          ":", "tap", "184", "265", ":", "sleep", "0.4"]),

    ("face",         ["face", "digital", "10:09:30", "72"]),
    ("face_analog",  ["face", "analog",  "10:09:30", "72"]),
    ("face_rings",   ["face", "rings",   "10:09:30", "72"]),
    ("face_words",   ["face", "words",   "10:09:30", "72"]),
    # The two times the words face used to contradict itself. "twenty to
    # twelve" names the NEXT hour, but the AM/PM label read the raw hour --
    # so 11:40 said AM while naming noon, and 23:40 said PM while naming
    # midnight. Twelve hours wrong, twice a day.
    #
    # These are a visual RECORD, not the guard: measured against the buggy
    # build they still passed at 0.2% drift, because two letters of text is
    # far under the threshold. The actual regression test is
    # test_face_words() in sim/test/test_units.c, which checks the
    # arithmetic over all 1440 minutes.
    ("face_words_to_noon",     ["face", "words", "11:40:00", "72"]),
    ("face_words_to_midnight", ["face", "words", "23:40:00", "72"]),
    ("face_minimal", ["face", "minimal", "10:09:30", "72"]),
    ("face_unset",   ["face", "unset"]),
    # The unset state on a face that has hands and a pinion, not just a label.
    # The digital case alone accepted a screen full of missing-glyph boxes for
    # as long as it existed, because nobody looked at the golden it wrote.
    ("face_unset_analog", ["face", "analog", "unset"]),
    ("face_low",     ["face", "07:45", "12"]),
]


def decode(path):
    """Return (w, h, rgb_bytes) for a PNG (stored-or-deflated, filter 0-4)."""
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
    prev = bytearray(w * 3)
    for y in range(h):
        ft = raw[y * stride]
        line = bytearray(raw[y * stride + 1:y * stride + 1 + w * 3])
        if ft == 0:
            pass
        elif ft == 1:  # Sub
            for x in range(3, w * 3):
                line[x] = (line[x] + line[x - 3]) & 0xFF
        elif ft == 2:  # Up
            for x in range(w * 3):
                line[x] = (line[x] + prev[x]) & 0xFF
        elif ft == 3:  # Average
            for x in range(w * 3):
                a = line[x - 3] if x >= 3 else 0
                line[x] = (line[x] + ((a + prev[x]) >> 1)) & 0xFF
        elif ft == 4:  # Paeth
            for x in range(w * 3):
                a = line[x - 3] if x >= 3 else 0
                b = prev[x]
                c = prev[x - 3] if x >= 3 else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[x] = (line[x] + pr) & 0xFF
        else:
            raise ValueError(f"bad PNG filter {ft}")
        rgb[y * w * 3:(y + 1) * w * 3] = line
        prev = line
    return w, h, bytes(rgb)


def thumbnail(w, h, rgb):
    """Box-average down by SCALE -> (tw, th, rgb)."""
    tw, th = w // SCALE, h // SCALE
    out = bytearray(tw * th * 3)
    for ty in range(th):
        for tx in range(tw):
            r = g = b = 0
            for dy in range(SCALE):
                row = (ty * SCALE + dy) * w * 3
                for dx in range(SCALE):
                    o = row + (tx * SCALE + dx) * 3
                    r += rgb[o]; g += rgb[o + 1]; b += rgb[o + 2]
            n = SCALE * SCALE
            o = (ty * tw + tx) * 3
            out[o] = r // n; out[o + 1] = g // n; out[o + 2] = b // n
    return tw, th, bytes(out)


def encode_png(w, h, rgb):
    """Minimal RGB8 PNG (filter 0), for writing thumbnail goldens."""
    def chunk(typ, data):
        return (struct.pack(">I", len(data)) + typ + data
                + struct.pack(">I", zlib.crc32(typ + data) & 0xFFFFFFFF))
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        raw += rgb[y * w * 3:(y + 1) * w * 3]
    ihdr = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr)
            + chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b""))


def diff_cells(a, b):
    """Fraction of thumbnail cells where any channel differs by > CELL_TOL."""
    n = len(a) // 3
    bad = 0
    for i in range(n):
        o = i * 3
        if (abs(a[o] - b[o]) > CELL_TOL or abs(a[o + 1] - b[o + 1]) > CELL_TOL
                or abs(a[o + 2] - b[o + 2]) > CELL_TOL):
            bad += 1
    return bad / n if n else 0.0


def render_thumb(name, cmds):
    out = os.path.join(HERE, "build", f"gold_{name}.png")
    subprocess.run([SIM, "--sdroot", REPO] + cmds + [":", "shot", out],
                   cwd=REPO, capture_output=True)
    w, h, rgb = decode(out)
    return thumbnail(w, h, rgb)


def main():
    if not os.path.exists(SIM):
        sys.exit("sim not built -- run sim/build.sh")
    os.makedirs(os.path.join(HERE, "build"), exist_ok=True)
    os.makedirs(GOLDEN_DIR, exist_ok=True)
    args = sys.argv[1:]
    update = "--update" in args
    # names after --update limit the (re)generation to just those apps, so an
    # intended single-app change doesn't churn the other goldens' binaries.
    only = set(a for a in args if not a.startswith("-"))

    failures = 0
    for name, cmds in FRAMES:
        if only and name not in only:
            continue
        gpath = os.path.join(GOLDEN_DIR, f"{name}.png")
        try:
            tw, th, thumb = render_thumb(name, cmds)
        except Exception as e:
            print(f"  FAIL  {name}  -- render/decode error: {e}")
            failures += 1
            continue
        if update:
            open(gpath, "wb").write(encode_png(tw, th, thumb))
            print(f"  wrote {name}")
            continue
        if not os.path.exists(gpath):
            print(f"  FAIL  {name}  -- no golden (run --update)")
            failures += 1
            continue
        gw, gh, gold = decode(gpath)
        if (gw, gh) != (tw, th):
            print(f"  FAIL  {name}  -- size {tw}x{th} vs golden {gw}x{gh}")
            failures += 1
            continue
        frac = diff_cells(thumb, gold)
        if frac > FRAC_TOL:
            print(f"  FAIL  {name}  -- {frac*100:.1f}% of cells changed (>{FRAC_TOL*100:.1f}%)")
            failures += 1
        else:
            print(f"  ok    {name}  ({frac*100:.1f}% drift)")

    processed = len(only) if only else len(FRAMES)
    if update:
        print(f"\ngoldens: wrote {processed} to sim/golden/")
        return
    print(f"\ngolden: {processed - failures} ok, {failures} failed")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
