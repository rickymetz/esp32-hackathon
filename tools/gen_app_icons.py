#!/usr/bin/env python3
"""Generate custom app-icon images for the launcher grid.

Flat, two-tone icons: each is a circular solid-colour tile (a distinct per-app
hue) carrying a clean flat symbol -- a white primary shape plus, where it aids
recognition, one accent colour. No gradients, no sheen, no drop shadows; the
look is Material/iOS-glyph flat, edge-to-edge in the disc.

Each icon is rasterised to an LVGL RGB565 image (composited over black) and
written to launcher/main/launcher_icons.{c,h}; the launcher draws the image as
the tile. On the true-black OLED the disc's transparent corners bake to black,
so the circle reads clean at half the flash of ARGB8888.

Pure Python (no Pillow): a tiny painter composites flat-filled shapes into a
supersampled RGBA buffer, box-downsampled for anti-aliasing.

    tools/gen_app_icons.py        # regenerate launcher/main/launcher_icons.{c,h}
"""
import math
import os

SIZE = 128
SS = 3
HERE = os.path.dirname(os.path.abspath(__file__))
OUT_C = os.path.join(HERE, "..", "launcher", "main", "launcher_icons.c")
OUT_H = os.path.join(HERE, "..", "launcher", "main", "launcher_icons.h")


# ---- colour + fill ------------------------------------------------------

def hx(c):
    return ((c >> 16) & 255, (c >> 8) & 255, c & 255)


def solid(c, a=255):
    r, g, b = hx(c)
    return lambda u, v: (r, g, b, a)


# ---- shapes (return (inside_fn, (x0,y0,x1,y1) bbox)) --------------------

def disc(cx, cy, r):
    return (lambda u, v: (u - cx) ** 2 + (v - cy) ** 2 <= r * r,
            (cx - r, cy - r, cx + r, cy + r))


def ring(cx, cy, r, t):
    return (lambda u, v: abs(math.hypot(u - cx, v - cy) - r) <= t / 2,
            (cx - r - t, cy - r - t, cx + r + t, cy + r + t))


def seg(ax, ay, bx, by, t):
    dx, dy = bx - ax, by - ay
    L2 = dx * dx + dy * dy or 1e-9

    def f(u, v):
        s = max(0.0, min(1.0, ((u - ax) * dx + (v - ay) * dy) / L2))
        return (u - (ax + s * dx)) ** 2 + (v - (ay + s * dy)) ** 2 <= (t / 2) ** 2
    return (f, (min(ax, bx) - t, min(ay, by) - t, max(ax, bx) + t, max(ay, by) + t))


def rrect(x0, y0, x1, y1, rad):
    def f(u, v):
        if not (x0 <= u <= x1 and y0 <= v <= y1):
            return False
        cx = min(max(u, x0 + rad), x1 - rad)
        cy = min(max(v, y0 + rad), y1 - rad)
        return (u - cx) ** 2 + (v - cy) ** 2 <= rad * rad
    return (f, (x0, y0, x1, y1))


def poly(pts):
    xs = [p[0] for p in pts]
    ys = [p[1] for p in pts]

    def f(u, v):
        inside = False
        j = len(pts) - 1
        for i in range(len(pts)):
            xi, yi = pts[i]
            xj, yj = pts[j]
            if ((yi > v) != (yj > v)) and \
               (u < (xj - xi) * (v - yi) / ((yj - yi) or 1e-9) + xi):
                inside = not inside
            j = i
        return inside
    return (f, (min(xs), min(ys), max(xs), max(ys)))


def arc_stroke(cv, cx, cy, r, a0, a1, t, c, clip=None):
    """Stroke a circular arc (degrees, screen coords) by walking overlapping
    discs -- smooth flat curves the polygon rasteriser can't do."""
    steps = max(8, int(abs(a1 - a0) / 8))
    for i in range(steps + 1):
        a = math.radians(a0 + (a1 - a0) * i / steps)
        cv.paint(disc(cx + r * math.cos(a), cy + r * math.sin(a), t / 2),
                 solid(c), clip=clip)


# ---- painter ------------------------------------------------------------

class Canvas:
    def __init__(self):
        self.W = SIZE * SS
        self.buf = [(0, 0, 0, 0)] * (self.W * self.W)

    def paint(self, shape, fill, clip=None):
        inside, (bx0, by0, bx1, by1) = shape
        W = self.W
        x0 = max(0, int(bx0 * W)); x1 = min(W, int(bx1 * W) + 1)
        y0 = max(0, int(by0 * W)); y1 = min(W, int(by1 * W) + 1)
        cin = clip[0] if clip else None
        for yy in range(y0, y1):
            v = (yy + 0.5) / W
            row = yy * W
            for xx in range(x0, x1):
                u = (xx + 0.5) / W
                if not inside(u, v):
                    continue
                if cin and not cin(u, v):
                    continue
                sr, sg, sb, sa = fill(u, v)
                if sa <= 0:
                    continue
                if sa >= 255:
                    self.buf[row + xx] = (sr, sg, sb, 255)
                else:
                    dr, dg, db, da = self.buf[row + xx]
                    a = sa / 255.0
                    self.buf[row + xx] = (
                        int(sr * a + dr * (1 - a)), int(sg * a + dg * (1 - a)),
                        int(sb * a + db * (1 - a)), max(sa, da))

    def downsample(self):
        """Box-downsample to RGB565 (little-endian) composited over black. The
        launcher grid sits on a true-black screen, so the icons' transparent
        corners bake to black and still read as circular -- at half the flash of
        ARGB8888."""
        W = self.W
        out = bytearray(SIZE * SIZE * 2)
        n = SS * SS
        for y in range(SIZE):
            for x in range(SIZE):
                r = g = b = 0
                for dy in range(SS):
                    base = (y * SS + dy) * W + x * SS
                    for dx in range(SS):
                        pr, pg, pb, pa = self.buf[base + dx]
                        r += pr * pa; g += pg * pa; b += pb * pa
                r = r // (n * 255); g = g // (n * 255); b = b // (n * 255)
                val = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
                o = (y * SIZE + x) * 2
                out[o] = val & 0xFF
                out[o + 1] = (val >> 8) & 0xFF
        return out


# ---- the flat palette ---------------------------------------------------
# Each app owns a distinct tile hue; symbols are flat white plus, where useful,
# one accent. INK is the dark mark colour used on white faces.

WHITE = 0xFFFFFF
INK = 0x25304A
A_RED = 0xEB5757
A_AMBER = 0xF2C94C
A_YELLOW = 0xFFD23F

TILE = disc(0.5, 0.5, 0.5)


def tile(cv, c):
    cv.paint(TILE, solid(c))


# ---- icon library -------------------------------------------------------

def ic_counter(cv):
    # A stepper (-- | +) reads as "increment a count", where a bare plus reads
    # as generic add. White pill, dark controls.
    tile(cv, 0x27AE60)
    cv.paint(rrect(0.14, 0.39, 0.86, 0.61, 0.11), solid(WHITE))          # pill
    cv.paint(seg(0.22, 0.5, 0.34, 0.5, 0.05), solid(INK))               # minus
    cv.paint(seg(0.5, 0.42, 0.5, 0.58, 0.022), solid(0xB6C6BC))         # divider
    cv.paint(seg(0.66, 0.5, 0.78, 0.5, 0.05), solid(INK))              # plus
    cv.paint(seg(0.72, 0.44, 0.72, 0.56, 0.05), solid(INK))


def ic_clock(cv):
    tile(cv, 0x2F80ED)
    cv.paint(disc(0.5, 0.5, 0.34), solid(WHITE))
    for i in range(12):
        a = i * math.pi / 6
        w = 0.028 if i % 3 == 0 else 0.016
        cv.paint(seg(0.5 + 0.265 * math.sin(a), 0.5 - 0.265 * math.cos(a),
                     0.5 + 0.30 * math.sin(a), 0.5 - 0.30 * math.cos(a), w),
                 solid(INK))
    cv.paint(seg(0.5, 0.5, 0.5, 0.31, 0.030), solid(INK))     # minute
    cv.paint(seg(0.5, 0.5, 0.63, 0.57, 0.034), solid(INK))    # hour
    cv.paint(seg(0.5, 0.5, 0.395, 0.36, 0.016), solid(A_RED))  # second
    cv.paint(disc(0.5, 0.5, 0.030), solid(A_RED))


def ic_faces(cv):
    # "Watch faces" picker: a 2x2 of mini dials on a warm coral tile -- reads as
    # "choose a face", and stays clearly distinct from Clock's single blue dial.
    tile(cv, 0xF2775A)
    for fx, fy in ((0.33, 0.33), (0.67, 0.33), (0.33, 0.67), (0.67, 0.67)):
        cv.paint(disc(fx, fy, 0.16), solid(WHITE))
        # Bolder hands + centre so the dials survive the downsample at ~100px.
        cv.paint(seg(fx, fy, fx, fy - 0.10, 0.028), solid(INK))       # minute
        cv.paint(seg(fx, fy, fx + 0.08, fy + 0.035, 0.028), solid(INK))  # hour
        cv.paint(disc(fx, fy, 0.026), solid(INK))


def ic_stopwatch(cv):
    tile(cv, 0x34495E)
    cv.paint(rrect(0.44, 0.10, 0.56, 0.18, 0.02), solid(WHITE))   # top button
    cv.paint(seg(0.5, 0.16, 0.5, 0.28, 0.02), solid(WHITE))       # stem (reaches the face)
    cv.paint(disc(0.5, 0.57, 0.31), solid(WHITE))                 # face
    cv.paint(seg(0.5, 0.57, 0.5, 0.32, 0.028), solid(A_RED))      # red hand
    cv.paint(disc(0.5, 0.57, 0.030), solid(INK))


def ic_level(cv):
    tile(cv, 0x9B51E0)   # magenta-violet -- opens the gap from Reaction's blue-violet
    cv.paint(rrect(0.12, 0.37, 0.88, 0.63, 0.06), solid(WHITE))       # body (taller)
    cv.paint(rrect(0.33, 0.44, 0.67, 0.56, 0.04), solid(0x53286F))    # vial window
    cv.paint(disc(0.5, 0.5, 0.055), solid(0x9BE7C0))                  # bubble (mint, larger)
    cv.paint(seg(0.42, 0.45, 0.42, 0.55, 0.020), solid(WHITE))        # guide lines (thicker)
    cv.paint(seg(0.58, 0.45, 0.58, 0.55, 0.020), solid(WHITE))


def ic_tone(cv):
    tile(cv, 0xE0567A)
    cv.paint(disc(0.37, 0.70, 0.095), solid(WHITE))
    cv.paint(disc(0.63, 0.62, 0.095), solid(WHITE))
    cv.paint(seg(0.455, 0.70, 0.455, 0.26, 0.045), solid(WHITE))
    cv.paint(seg(0.715, 0.62, 0.715, 0.20, 0.045), solid(WHITE))
    cv.paint(poly([(0.455, 0.22), (0.715, 0.16), (0.715, 0.28), (0.455, 0.34)]),
             solid(WHITE))


def ic_tally(cv):
    tile(cv, 0x22B8C9)   # cyan -- opens the gap from Clock's royal blue
    for x in (0.28, 0.42, 0.56, 0.70):
        cv.paint(seg(x, 0.24, x, 0.76, 0.040), solid(WHITE))
    cv.paint(seg(0.24, 0.76, 0.74, 0.24, 0.040), solid(WHITE))   # the cross-stroke


def ic_dice(cv):
    tile(cv, 0xE74C3C)
    cv.paint(rrect(0.26, 0.26, 0.74, 0.74, 0.12), solid(WHITE))   # die face
    pip = lambda x, y: cv.paint(disc(x, y, 0.052), solid(0xE74C3C))
    pip(0.38, 0.38); pip(0.62, 0.38)                              # 5 pips
    pip(0.50, 0.50)
    pip(0.38, 0.62); pip(0.62, 0.62)


def ic_countdown(cv):
    tile(cv, 0xF2994A)
    cv.paint(seg(0.30, 0.21, 0.70, 0.21, 0.045), solid(WHITE))    # top cap
    cv.paint(seg(0.30, 0.79, 0.70, 0.79, 0.045), solid(WHITE))    # bottom cap
    cv.paint(poly([(0.34, 0.23), (0.66, 0.23), (0.5, 0.5)]), solid(WHITE))  # upper
    cv.paint(poly([(0.5, 0.5), (0.66, 0.77), (0.34, 0.77)]), solid(WHITE))  # lower


def ic_reaction(cv):
    tile(cv, 0x7B61FF)
    cv.paint(poly([(0.56, 0.15), (0.32, 0.54), (0.47, 0.54),
                   (0.40, 0.85), (0.68, 0.44), (0.51, 0.44)]),
             solid(A_YELLOW))   # bolt, centred in the disc


def ic_tip(cv):
    tile(cv, 0x159957)   # deep emerald -- distinct from Counter's grass green
    cv.paint(disc(0.5, 0.5, 0.30), solid(WHITE))                  # coin
    coin = disc(0.5, 0.5, 0.30)
    # $ in INK, matching every other dark-on-white mark; thicker so it reads.
    arc_stroke(cv, 0.5, 0.42, 0.082, -30, -270, 0.036, INK, clip=coin)  # $ top
    arc_stroke(cv, 0.5, 0.58, 0.082, -90, 150, 0.036, INK, clip=coin)   # $ bottom
    cv.paint(seg(0.5, 0.26, 0.5, 0.74, 0.030), solid(INK))       # $ stem


def ic_flashlight(cv):
    tile(cv, 0x2C3E50)
    # A torch pointing right: barrel -> flared head -> lens -> beam, each part
    # overlapping the next so it reads as one connected object. The beam is
    # clipped to the disc so it can't spill into the (black) corners.
    cv.paint(rrect(0.13, 0.44, 0.42, 0.56, 0.03), solid(WHITE))                   # barrel
    cv.paint(poly([(0.40, 0.42), (0.60, 0.35), (0.60, 0.65), (0.40, 0.58)]),
             solid(WHITE))                                                        # head (flares out)
    cv.paint(rrect(0.575, 0.35, 0.625, 0.65, 0.02), solid(A_YELLOW))            # lens
    cv.paint(poly([(0.615, 0.40), (0.90, 0.23), (0.90, 0.77), (0.615, 0.60)]),
             solid(0xFFE066), clip=TILE)                                          # beam (overlaps lens)


def ic_calculator(cv):
    tile(cv, 0x12B886)
    cv.paint(rrect(0.27, 0.15, 0.73, 0.85, 0.07), solid(WHITE))       # body
    cv.paint(rrect(0.32, 0.21, 0.68, 0.35, 0.03), solid(0x0E3B33))    # screen
    cv.paint(rrect(0.55, 0.265, 0.65, 0.305, 0.012), solid(0x53E0CC)) # readout digits
    for cy in (0.475, 0.61, 0.745):                                   # 3x3 keypad
        for ci, cx in enumerate((0.375, 0.5, 0.625)):
            col = A_AMBER if ci == 2 else 0x12B886   # operator column pops amber
            cv.paint(rrect(cx - 0.05, cy - 0.05, cx + 0.05, cy + 0.05, 0.02), solid(col))


def ic_color(cv):
    tile(cv, 0x3E4657)   # slate light enough to keep a visible disc edge on black
    cv.paint(rrect(0.23, 0.23, 0.48, 0.48, 0.05), solid(0xEB5757))   # red
    cv.paint(rrect(0.52, 0.23, 0.77, 0.48, 0.05), solid(0x27AE60))   # green
    cv.paint(rrect(0.23, 0.52, 0.48, 0.77, 0.05), solid(0x2F80ED))   # blue
    cv.paint(rrect(0.52, 0.52, 0.77, 0.77, 0.05), solid(0xF2C94C))   # yellow


def ic_metronome(cv):
    tile(cv, 0xC0693F)   # burnt orange -- more saturated, joins the vivid family
    cv.paint(rrect(0.20, 0.79, 0.80, 0.85, 0.02), solid(WHITE))       # base plinth
    cv.paint(poly([(0.40, 0.19), (0.60, 0.19), (0.75, 0.81), (0.25, 0.81)]),
             solid(WHITE))                                            # body
    cv.paint(poly([(0.44, 0.25), (0.56, 0.25), (0.655, 0.74), (0.345, 0.74)]),
             solid(0x8A3F22))                                         # scale recess
    cv.paint(seg(0.5, 0.75, 0.60, 0.14, 0.022), solid(WHITE))        # pendulum rod
    cv.paint(rrect(0.505, 0.435, 0.590, 0.505, 0.012), solid(A_AMBER))  # weight (larger)
    cv.paint(disc(0.60, 0.14, 0.028), solid(A_AMBER))                # finial
    cv.paint(disc(0.5, 0.75, 0.024), solid(0x8A3F22))              # pivot


ICONS = {
    "dice": ic_dice, "clock": ic_clock, "faces": ic_faces,
    "stopwatch": ic_stopwatch, "plus": ic_counter, "audio": ic_tone,
    "list": ic_tally, "hourglass": ic_countdown, "fire": ic_reaction,
    "dollar": ic_tip, "level": ic_level, "flashlight": ic_flashlight,
    "metronome": ic_metronome, "calculator": ic_calculator, "color": ic_color,
}


def main():
    names = sorted(ICONS.keys())
    with open(OUT_C, "w") as c:
        c.write('/* Generated by tools/gen_app_icons.py -- do not edit by hand.\n'
                ' * Flat, two-tone app-icon images (LVGL RGB565, over black) for the\n'
                ' * launcher grid: a distinct solid-colour disc per app. */\n')
        c.write('#include "launcher_icons.h"\n#include <string.h>\n\n')
        for n in names:
            cv = Canvas()
            ICONS[n](cv)
            data = cv.downsample()
            c.write(f"static const uint8_t {n}_map[{len(data)}] = {{\n")
            for i in range(0, len(data), 20):
                c.write("    " + ",".join(str(b) for b in data[i:i + 20]) + ",\n")
            c.write("};\n")
            c.write(f"static const lv_image_dsc_t {n}_img = {{\n"
                    f"    .header = {{ .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565,\n"
                    f"                 .w = {SIZE}, .h = {SIZE}, .stride = {SIZE * 2} }},\n"
                    f"    .data_size = {len(data)}, .data = {n}_map,\n}};\n\n")
        c.write("const lv_image_dsc_t *launcher_app_image(const char *key)\n{\n")
        c.write("    if (!key) return NULL;\n")
        for n in names:
            c.write(f'    if (!strcmp(key, "{n}")) return &{n}_img;\n')
        c.write("    return NULL;\n}\n")

    with open(OUT_H, "w") as h:
        h.write('/* Generated by tools/gen_app_icons.py -- do not edit by hand. */\n'
                '#pragma once\n#include "lvgl.h"\n\n'
                '/* Flat custom app-icon image for an icon-name key, or NULL. */\n'
                'const lv_image_dsc_t *launcher_app_image(const char *key);\n')
    print(f"wrote {len(names)} flat icons ({SIZE}x{SIZE} RGB565) to launcher_icons.{{c,h}}")


if __name__ == "__main__":
    main()
