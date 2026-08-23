#!/usr/bin/env python3
"""Generate custom app-icon images for the launcher grid.

A cohesive family of *circular*, edge-to-edge icons in a modern-gradient style:
every tile is a full disc sharing one indigo-dusk sky gradient with a gentle top
light and a bottom vignette for depth, and each carries a small illustration
(a die, a watch face, an hourglass, a coin...) drawn in warm cream plus a
restrained accent set (amber / gold / teal / coral) with soft drop shadows.
No heavy gloss -- the depth comes from the gradients and the shadows.

Each icon is rasterised to an LVGL RGB565 image (composited over black) and
written to launcher/main/launcher_icons.{c,h}; the launcher draws the image as
the tile. On the true-black OLED the disc's transparent corners bake to black,
so the circle reads clean at half the flash of ARGB8888.

Pure Python (no Pillow): a tiny painter composites layers (each a shape + a
solid/gradient/radial fill) into a supersampled RGBA buffer, box-downsampled for
anti-aliasing.

    tools/gen_app_icons.py        # regenerate launcher/main/launcher_icons.{c,h}
"""
import math
import os

SIZE = 120
SS = 3
HERE = os.path.dirname(os.path.abspath(__file__))
OUT_C = os.path.join(HERE, "..", "launcher", "main", "launcher_icons.c")
OUT_H = os.path.join(HERE, "..", "launcher", "main", "launcher_icons.h")


# ---- colour + fills -----------------------------------------------------

def hx(c):
    return ((c >> 16) & 255, (c >> 8) & 255, c & 255)


def solid(c, a=255):
    r, g, b = hx(c)
    return lambda u, v: (r, g, b, a)


def vgrad(c0, c1, y0=0.0, y1=1.0):
    r0, g0, b0 = hx(c0)
    r1, g1, b1 = hx(c1)

    def f(u, v):
        t = 0.0 if y1 == y0 else max(0.0, min(1.0, (v - y0) / (y1 - y0)))
        return (int(r0 + (r1 - r0) * t), int(g0 + (g1 - g0) * t),
                int(b0 + (b1 - b0) * t), 255)
    return f


def radial(cx, cy, r, c0, c1, a0=255, a1=255):
    r0, g0, b0 = hx(c0)
    r1, g1, b1 = hx(c1)

    def f(u, v):
        t = min(1.0, math.hypot(u - cx, v - cy) / r)
        return (int(r0 + (r1 - r0) * t), int(g0 + (g1 - g0) * t),
                int(b0 + (b1 - b0) * t), int(a0 + (a1 - a0) * t))
    return f


def esoft(c, cx, cy, rx, ry, a0, a1):
    """Elliptical alpha falloff -- used for soft drop shadows and glows."""
    r0, g0, b0 = hx(c)

    def f(u, v):
        t = min(1.0, math.hypot((u - cx) / rx, (v - cy) / ry))
        return (r0, g0, b0, int(a0 + (a1 - a0) * t))
    return f


# ---- shapes (return (inside_fn, (x0,y0,x1,y1) bbox)) --------------------

def disc(cx, cy, r):
    return (lambda u, v: (u - cx) ** 2 + (v - cy) ** 2 <= r * r,
            (cx - r, cy - r, cx + r, cy + r))


def edisc(cx, cy, rx, ry):
    return (lambda u, v: ((u - cx) / rx) ** 2 + ((v - cy) / ry) ** 2 <= 1.0,
            (cx - rx, cy - ry, cx + rx, cy + ry))


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
                        # premultiply by alpha == composite over black
                        r += pr * pa; g += pg * pa; b += pb * pa
                r = r // (n * 255); g = g // (n * 255); b = b // (n * 255)
                val = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
                o = (y * SIZE + x) * 2
                out[o] = val & 0xFF
                out[o + 1] = (val >> 8) & 0xFF
        return out


# ---- the shared family --------------------------------------------------
# One circular sky, one restrained palette. Identity comes from the little
# illustration on top, not from a per-app background colour.

DISC = disc(0.5, 0.5, 0.5)

SKY_TOP = 0x3A5390      # indigo dusk, lit at the top
SKY_BOT = 0x151C33      # deep navy at the bottom

CREAM = 0xF3EEE4
CREAM_LO = 0xDED8CC     # shaded cream (volume)
STEEL = 0xC7C1B4
INK = 0x28304A          # near-navy "black" for marks/hands
AMBER = 0xE0863C
AMBER_HI = 0xFBE38A
GOLD = 0xF6C544
CORAL = 0xEC6A5E
TEAL = 0x53B7A9
MINT = 0xCFF3E6
SLATE = 0x8790A6


def sky(cv):
    """The shared circular background: dusk gradient + top light + vignette."""
    cv.paint(DISC, vgrad(SKY_TOP, SKY_BOT, 0.05, 0.98))
    cv.paint(DISC, radial(0.5, 0.26, 0.60, 0xFFFFFF, 0xFFFFFF, 34, 0), clip=DISC)
    cv.paint(DISC, radial(0.5, 1.05, 0.85, 0x000000, 0x000000, 80, 0), clip=DISC)


def shadow(cv, cx, cy, rx, ry, a=110):
    """A soft elliptical drop shadow, clipped to the disc, for grounded depth."""
    cv.paint(edisc(cx, cy, rx, ry), esoft(0x000000, cx, cy, rx, ry, a, 0), clip=DISC)


def arc_stroke(cv, cx, cy, r, a0, a1, t, c, clip=None):
    """Stroke a circular arc (degrees, screen coords) by walking overlapping
    discs -- gives smooth curves the polygon rasteriser can't."""
    steps = max(8, int(abs(a1 - a0) / 8))
    for i in range(steps + 1):
        a = math.radians(a0 + (a1 - a0) * i / steps)
        cv.paint(disc(cx + r * math.cos(a), cy + r * math.sin(a), t / 2),
                 solid(c), clip=clip)


# ---- icon library -------------------------------------------------------

def ic_dice(cv):
    sky(cv)
    shadow(cv, 0.5, 0.83, 0.25, 0.06, 130)
    Tp, Lt, Rt = (0.5, 0.20), (0.23, 0.35), (0.77, 0.35)
    Cn, Lb, Rb, Bt = (0.5, 0.50), (0.23, 0.66), (0.77, 0.66), (0.5, 0.81)
    cv.paint(poly([Tp, Rt, Cn, Lt]), vgrad(0xFBF7EE, 0xEFE9DC, 0.20, 0.50))  # top
    cv.paint(poly([Lt, Cn, Bt, Lb]), vgrad(0xD9D3C6, 0xC7C1B4, 0.35, 0.81))  # left
    cv.paint(poly([Rt, Rb, Bt, Cn]), vgrad(0xC2BCAF, 0xAAA498, 0.35, 0.81))  # right
    pip = lambda x, y, c=AMBER, r=0.030: cv.paint(disc(x, y, r), solid(c))
    pip(0.5, 0.35)                                        # top face: 1
    pip(0.335, 0.49); pip(0.40, 0.60)                     # left face: 2
    pip(0.60, 0.47); pip(0.665, 0.565); pip(0.73, 0.66)   # right face: 3


def ic_clock(cv):
    sky(cv)
    shadow(cv, 0.5, 0.87, 0.30, 0.055, 120)
    cv.paint(disc(0.5, 0.5, 0.36), solid(0xE8E2D6))                     # rim
    cv.paint(disc(0.5, 0.5, 0.325), vgrad(0xFBF8F1, 0xEAE4D8, 0.18, 0.82))  # face
    for i in range(12):
        a = i * math.pi / 6
        r_in = 0.275 if i % 3 == 0 else 0.285
        w = 0.022 if i % 3 == 0 else 0.013
        cv.paint(seg(0.5 + r_in * math.sin(a), 0.5 - r_in * math.cos(a),
                     0.5 + 0.30 * math.sin(a), 0.5 - 0.30 * math.cos(a), w),
                 solid(SLATE))
    cv.paint(seg(0.5, 0.5, 0.5, 0.30, 0.026), solid(INK))     # minute
    cv.paint(seg(0.5, 0.5, 0.63, 0.57, 0.030), solid(INK))    # hour
    cv.paint(seg(0.5, 0.5, 0.395, 0.355, 0.013), solid(CORAL))  # second
    cv.paint(disc(0.5, 0.5, 0.028), solid(CORAL))


def ic_faces(cv):
    ic_clock(cv)


def ic_stopwatch(cv):
    sky(cv)
    shadow(cv, 0.5, 0.90, 0.28, 0.05, 120)
    cv.paint(rrect(0.44, 0.07, 0.56, 0.155, 0.02), solid(0xD9D3C6))    # top button
    cv.paint(seg(0.5, 0.14, 0.5, 0.20, 0.02), solid(0xD9D3C6))         # stem
    cv.paint(disc(0.5, 0.56, 0.36), solid(TEAL))                       # teal bezel
    cv.paint(disc(0.5, 0.56, 0.315), vgrad(0xFBF8F1, 0xEAE4D8, 0.28, 0.86))  # face
    cv.paint(seg(0.5, 0.56, 0.5, 0.30, 0.026), solid(CORAL))           # red hand
    cv.paint(disc(0.5, 0.56, 0.028), solid(INK))


def ic_counter(cv):
    sky(cv)
    shadow(cv, 0.5, 0.81, 0.24, 0.05, 120)
    cv.paint(disc(0.5, 0.5, 0.30), vgrad(0xFBF8F1, 0xEAE4D8, 0.2, 0.8))  # cream chip
    cv.paint(seg(0.5, 0.34, 0.5, 0.66, 0.075), solid(AMBER))            # amber plus
    cv.paint(seg(0.34, 0.5, 0.66, 0.5, 0.075), solid(AMBER))


def ic_tone(cv):
    sky(cv)
    shadow(cv, 0.37, 0.80, 0.11, 0.035, 120)
    shadow(cv, 0.63, 0.72, 0.11, 0.035, 120)
    cv.paint(disc(0.37, 0.70, 0.095), solid(CREAM))
    cv.paint(disc(0.63, 0.62, 0.095), solid(CREAM))
    cv.paint(seg(0.455, 0.70, 0.455, 0.26, 0.045), solid(CREAM))
    cv.paint(seg(0.715, 0.62, 0.715, 0.20, 0.045), solid(CREAM))
    cv.paint(poly([(0.455, 0.22), (0.715, 0.16), (0.715, 0.28), (0.455, 0.34)]),
             solid(AMBER))                                              # beam accent


def ic_tally(cv):
    sky(cv)
    shadow(cv, 0.5, 0.82, 0.23, 0.05, 120)
    cv.paint(rrect(0.28, 0.20, 0.72, 0.80, 0.06),
             vgrad(0xFBF8F1, 0xEAE4D8, 0.2, 0.8))                       # card
    for y in (0.34, 0.50, 0.66):
        cv.paint(rrect(0.335, y - 0.048, 0.425, y + 0.038, 0.018), solid(TEAL))  # box
        cv.paint(seg(0.355, y - 0.004, 0.378, y + 0.022, 0.014), solid(0xFBF8F1))
        cv.paint(seg(0.378, y + 0.022, 0.412, y - 0.030, 0.014), solid(0xFBF8F1))
        cv.paint(rrect(0.46, y - 0.018, 0.66, y + 0.014, 0.016), solid(0xC3C0BA))  # line


def ic_countdown(cv):
    sky(cv)
    shadow(cv, 0.5, 0.87, 0.20, 0.045, 120)
    cv.paint(seg(0.30, 0.20, 0.70, 0.20, 0.045), solid(0xE8E2D6))       # top cap
    cv.paint(seg(0.30, 0.80, 0.70, 0.80, 0.045), solid(0xE8E2D6))       # bottom cap
    cv.paint(poly([(0.32, 0.22), (0.68, 0.22), (0.5, 0.5)]), solid(0xEAE4D8, 80))
    cv.paint(poly([(0.5, 0.5), (0.68, 0.78), (0.32, 0.78)]), solid(0xEAE4D8, 80))
    cv.paint(poly([(0.40, 0.34), (0.60, 0.34), (0.5, 0.47)]), solid(AMBER))    # top sand
    cv.paint(poly([(0.5, 0.53), (0.635, 0.76), (0.365, 0.76)]), solid(AMBER))  # low sand
    cv.paint(seg(0.5, 0.47, 0.5, 0.62, 0.012), solid(AMBER))            # falling stream


def ic_reaction(cv):
    sky(cv)
    cv.paint(disc(0.52, 0.5, 0.34), radial(0.52, 0.5, 0.34, GOLD, GOLD, 80, 0),
             clip=DISC)                                                 # energy glow
    cv.paint(poly([(0.58, 0.14), (0.34, 0.54), (0.49, 0.54),
                   (0.42, 0.86), (0.70, 0.44), (0.53, 0.44)]),
             vgrad(AMBER_HI, AMBER, 0.14, 0.86))                        # bolt


def ic_tip(cv):
    sky(cv)
    shadow(cv, 0.5, 0.81, 0.24, 0.05, 130)
    cv.paint(disc(0.5, 0.5, 0.32), radial(0.42, 0.40, 0.5, 0xFCE08A, 0xD79A2E))  # coin
    cv.paint(ring(0.5, 0.5, 0.285, 0.028), solid(0xB9842A))            # rim
    cv.paint(edisc(0.40, 0.36, 0.09, 0.05), solid(0xFFFFFF, 80),
             clip=disc(0.5, 0.5, 0.30))                                 # shine
    coin = disc(0.5, 0.5, 0.30)
    arc_stroke(cv, 0.5, 0.42, 0.082, -30, -270, 0.028, 0x8A6320, clip=coin)  # top bowl
    arc_stroke(cv, 0.5, 0.58, 0.082, -90, 150, 0.028, 0x8A6320, clip=coin)   # bot bowl
    cv.paint(seg(0.5, 0.27, 0.5, 0.73, 0.024), solid(0x8A6320))        # $ stem (on top)


def ic_level(cv):
    sky(cv)
    shadow(cv, 0.5, 0.64, 0.33, 0.045, 120)
    cv.paint(rrect(0.13, 0.41, 0.87, 0.59, 0.05),
             vgrad(CREAM, CREAM_LO, 0.41, 0.59))                        # body
    cv.paint(rrect(0.34, 0.445, 0.66, 0.555, 0.04), solid(0x2E5E56))    # vial well
    cv.paint(rrect(0.35, 0.45, 0.65, 0.55, 0.035), solid(TEAL))        # vial fluid
    cv.paint(disc(0.5, 0.5, 0.042), solid(MINT))                       # bubble
    cv.paint(seg(0.44, 0.455, 0.44, 0.545, 0.010), solid(0xEAF6F1))    # guide lines
    cv.paint(seg(0.56, 0.455, 0.56, 0.545, 0.010), solid(0xEAF6F1))


def ic_metronome(cv):
    sky(cv)
    shadow(cv, 0.5, 0.84, 0.27, 0.05, 130)
    cv.paint(rrect(0.20, 0.79, 0.80, 0.85, 0.02), solid(0xC8B084))       # base plinth
    body = poly([(0.40, 0.19), (0.60, 0.19), (0.75, 0.81), (0.25, 0.81)])
    cv.paint(body, vgrad(0xF3EEE4, 0xD8C29A, 0.19, 0.81))               # wooden body
    cv.paint(poly([(0.44, 0.25), (0.56, 0.25), (0.655, 0.74), (0.345, 0.74)]),
             vgrad(0x2A3252, 0x1A2038, 0.25, 0.74))                     # scale recess
    cv.paint(seg(0.5, 0.75, 0.60, 0.14, 0.020), solid(INK))            # pendulum rod
    cv.paint(rrect(0.512, 0.445, 0.582, 0.500, 0.010), solid(AMBER))   # sliding weight
    cv.paint(disc(0.60, 0.14, 0.024), solid(AMBER_HI))                  # top finial
    cv.paint(disc(0.5, 0.75, 0.022), solid(0x8A6320))                  # pivot


def ic_flashlight(cv):
    sky(cv)
    cv.paint(poly([(0.52, 0.30), (0.92, 0.10), (0.92, 0.50)]),
             radial(0.52, 0.30, 0.55, AMBER_HI, AMBER_HI, 150, 0), clip=DISC)  # beam
    shadow(cv, 0.34, 0.66, 0.13, 0.035, 120)
    cv.paint(rrect(0.12, 0.42, 0.30, 0.58, 0.03),
             vgrad(0xE8E2D6, 0xB9B3A6, 0.42, 0.58))                     # body
    cv.paint(poly([(0.30, 0.37), (0.50, 0.44), (0.50, 0.56), (0.30, 0.63)]),
             vgrad(CREAM, STEEL, 0.37, 0.63))                           # head
    cv.paint(rrect(0.47, 0.44, 0.53, 0.56, 0.02), solid(AMBER_HI))     # lens


ICONS = {
    "dice": ic_dice, "clock": ic_clock, "faces": ic_faces,
    "stopwatch": ic_stopwatch, "plus": ic_counter, "audio": ic_tone,
    "list": ic_tally, "hourglass": ic_countdown, "fire": ic_reaction,
    "dollar": ic_tip, "level": ic_level, "flashlight": ic_flashlight,
    "metronome": ic_metronome,
}


def main():
    names = sorted(ICONS.keys())
    with open(OUT_C, "w") as c:
        c.write('/* Generated by tools/gen_app_icons.py -- do not edit by hand.\n'
                ' * Circular, edge-to-edge app-icon images (LVGL RGB565, over black)\n'
                ' * for the launcher grid: one shared dusk-gradient family. */\n')
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
                '/* Circular custom app-icon image for an icon-name key, or NULL. */\n'
                'const lv_image_dsc_t *launcher_app_image(const char *key);\n')
    print(f"wrote {len(names)} circular icons ({SIZE}x{SIZE} RGB565) to launcher_icons.{{c,h}}")


if __name__ == "__main__":
    main()
