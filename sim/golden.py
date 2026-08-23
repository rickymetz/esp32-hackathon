#!/usr/bin/env python3
"""Golden-frame regression for the simulator.

Where scenarios.py asserts a few hand-picked pixels, this checks the *whole*
frame against a committed reference -- catching layout shifts, a widget that
moved or vanished, a font/theme change, a colour regression anywhere on screen.

To stay robust across machines (the CI runner renders on different hardware
than wherever a golden was captured) and small enough to commit, each golden is
the canonical frame **downscaled 4x** to a 92x116 thumbnail (box-averaged, which
smooths anti-aliasing noise) and stored as a PNG under sim/golden/. Comparison
is perceptual, not exact: a thumbnail cell "differs" only if a channel is off by
more than CELL_TOL, and a frame fails only if more than FRAC_TOL of its cells
differ -- so AA/FP wobble passes but a real change (which moves many cells)
fails. The committed thumbnails render in GitHub diffs, so a golden update is
reviewable by eye.

    sim/golden.py            # compare every frame to its golden; non-zero on drift
    sim/golden.py --update   # (re)generate goldens from the current build

Only settled, deterministic frames are covered -- apps driven by math.random or
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
FRAC_TOL = 0.03    # fail if >3% of cells differ

# name -> commands that drive the app to its canonical, settled frame.
# Keep these deterministic: no randomness, no mid-animation capture.
FRAMES = [
    ("counter",      ["run", "apps/counter.lua", ":", "sleep", "0.4"]),
    ("clock",        ["run", "apps/clock.lua", ":", "sleep", "0.4"]),
    ("faces",        ["run", "apps/faces.lua", ":", "sleep", "0.4"]),
    ("color",        ["run", "apps/color.lua", ":", "sleep", "0.4"]),
    ("flashlight",   ["run", "apps/flashlight.lua", ":", "sleep", "0.4"]),
    ("hello_world",  ["run", "apps/hello_world.lua", ":", "sleep", "0.4"]),
    ("sensors_test", ["run", "apps/sensors_test.lua", ":", "sleep", "0.4"]),
    ("settings",     ["run", "apps/settings.lua", ":", "sleep", "0.4"]),
    ("tally",        ["run", "apps/tally.lua", ":", "sleep", "0.4"]),
    ("sign",         ["run", "apps/sign.lua", ":", "sleep", "0.4"]),
    ("countdown",    ["run", "apps/countdown.lua", ":", "sleep", "0.4"]),
    ("stopwatch",    ["run", "apps/stopwatch.lua", ":", "sleep", "0.4"]),
    ("tip",          ["run", "apps/tip.lua", ":", "sleep", "0.4"]),
    ("level",        ["run", "apps/level.lua", ":", "sleep", "0.4"]),
    ("tone",         ["run", "apps/tone.lua", ":", "sleep", "0.4"]),
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
    update = "--update" in sys.argv

    failures = 0
    for name, cmds in FRAMES:
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
            print(f"  FAIL  {name}  -- {frac*100:.1f}% of cells changed (>{FRAC_TOL*100:.0f}%)")
            failures += 1
        else:
            print(f"  ok    {name}  ({frac*100:.1f}% drift)")

    if update:
        print(f"\ngoldens: wrote {len(FRAMES)} to sim/golden/")
        return
    print(f"\ngolden: {len(FRAMES) - failures} ok, {failures} failed")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
