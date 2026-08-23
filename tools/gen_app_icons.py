#!/usr/bin/env python3
"""Generate custom app-icon images for the launcher grid.

These are full-colour, lightly skeuomorphic icons (gradients, shading, a real
3D die, a watch face...) that fill the whole tile, not flat glyphs. Each is
rasterised to an LVGL RGB565 image (over black) and written to
launcher/main/launcher_icons.{c,h}; the launcher draws the image as the tile.

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
        rounded corners bake to black and still read as rounded -- at half the
        flash of ARGB8888."""
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


# ---- icon library -------------------------------------------------------
# Each icon is a function(Canvas) painting its layers over a rounded-tile bg.

TILE = rrect(0.0, 0.0, 1.0, 1.0, 0.22)


def bg(cv, c0, c1):
    cv.paint(TILE, vgrad(c0, c1))
    # soft top sheen
    cv.paint(rrect(0.06, 0.05, 0.94, 0.4, 0.16), solid(0xFFFFFF, 40), clip=TILE)


def ic_dice(cv):
    bg(cv, 0x3A4A6B, 0x1E2740)
    # isometric cube
    Tp, Lt, Rt = (0.5, 0.22), (0.24, 0.37), (0.76, 0.37)
    Cn, Lb, Rb, Bt = (0.5, 0.52), (0.24, 0.66), (0.76, 0.66), (0.5, 0.81)
    cv.paint(poly([Tp, Rt, Cn, Lt]), vgrad(0xFFFFFF, 0xEDEDE4, 0.22, 0.52))  # top
    cv.paint(poly([Lt, Cn, Bt, Lb]), vgrad(0xD3D3C8, 0xBEBEB2, 0.37, 0.81))  # left
    cv.paint(poly([Rt, Rb, Bt, Cn]), vgrad(0xB8B8AC, 0xA0A096, 0.37, 0.81))  # right
    pip = lambda x, y, r=0.032: cv.paint(disc(x, y, r), solid(0x2B2B2B))
    pip(0.5, 0.37)                                             # top: 1
    pip(0.34, 0.5); pip(0.40, 0.60)                            # left: 2
    pip(0.60, 0.47); pip(0.66, 0.57); pip(0.72, 0.67)          # right: 3


def ic_clock(cv):
    bg(cv, 0x2E6BE6, 0x1B45A8)
    cv.paint(disc(0.5, 0.5, 0.36), solid(0xC9D2DE))           # bezel
    cv.paint(disc(0.5, 0.5, 0.31), vgrad(0xFFFFFF, 0xE7ECF2, 0.2, 0.8))  # face
    for i in range(12):                                       # ticks
        a = i * math.pi / 6
        cv.paint(seg(0.5 + 0.27 * math.sin(a), 0.5 - 0.27 * math.cos(a),
                     0.5 + 0.30 * math.sin(a), 0.5 - 0.30 * math.cos(a), 0.02),
                 solid(0x8A94A2))
    cv.paint(seg(0.5, 0.5, 0.5, 0.32, 0.028), solid(0x2B3242))   # minute
    cv.paint(seg(0.5, 0.5, 0.64, 0.58, 0.032), solid(0x2B3242))  # hour
    cv.paint(seg(0.5, 0.5, 0.40, 0.36, 0.014), solid(0xE8434B))  # second
    cv.paint(disc(0.5, 0.5, 0.03), solid(0xE8434B))


def ic_faces(cv):
    ic_clock(cv)


def ic_stopwatch(cv):
    bg(cv, 0x2C3440, 0x171B22)
    cv.paint(rrect(0.44, 0.06, 0.56, 0.15, 0.02), solid(0xC0C6CE))  # button
    cv.paint(disc(0.5, 0.56, 0.35), solid(0xAEB6C0))               # chrome ring
    cv.paint(disc(0.5, 0.56, 0.30), vgrad(0xFFFFFF, 0xE7ECF2, 0.3, 0.85))
    cv.paint(seg(0.5, 0.56, 0.5, 0.32, 0.03), solid(0xE8434B))     # red hand
    cv.paint(disc(0.5, 0.56, 0.028), solid(0x2B3242))


def ic_counter(cv):
    bg(cv, 0x2F80ED, 0x1A5FBF)
    cv.paint(seg(0.5, 0.24, 0.5, 0.76, 0.14), solid(0xFFFFFF))
    cv.paint(seg(0.24, 0.5, 0.76, 0.5, 0.14), solid(0xFFFFFF))
    cv.paint(seg(0.5, 0.24, 0.5, 0.76, 0.14), solid(0xFFFFFF, 60))  # (kept simple)


def ic_tone(cv):
    bg(cv, 0xE0567A, 0xA82F52)
    cv.paint(disc(0.36, 0.72, 0.11), solid(0xFFFFFF))
    cv.paint(disc(0.66, 0.64, 0.11), solid(0xFFFFFF))
    cv.paint(seg(0.455, 0.72, 0.455, 0.26, 0.05), solid(0xFFFFFF))
    cv.paint(seg(0.755, 0.64, 0.755, 0.2, 0.05), solid(0xFFFFFF))
    cv.paint(poly([(0.455, 0.22), (0.755, 0.16), (0.755, 0.28), (0.455, 0.34)]),
             solid(0xFFFFFF))


def ic_tally(cv):
    bg(cv, 0x3EA36B, 0x217A48)
    for i, x in enumerate((0.3, 0.42, 0.54, 0.66)):
        cv.paint(seg(x, 0.28, x, 0.72, 0.035), solid(0xFFFFFF))
    cv.paint(seg(0.26, 0.72, 0.7, 0.28, 0.035), solid(0xFFFFFF))


def ic_countdown(cv):
    bg(cv, 0xE59A3C, 0xB56E1C)
    # hourglass
    cv.paint(poly([(0.3, 0.2), (0.7, 0.2), (0.5, 0.5)]), solid(0xFFFFFF))
    cv.paint(poly([(0.5, 0.5), (0.7, 0.8), (0.3, 0.8)]), solid(0xFFFFFF))
    cv.paint(seg(0.28, 0.18, 0.72, 0.18, 0.04), solid(0xFFF0D8))
    cv.paint(seg(0.28, 0.82, 0.72, 0.82, 0.04), solid(0xFFF0D8))
    cv.paint(poly([(0.42, 0.34), (0.58, 0.34), (0.5, 0.46)]), solid(0xE59A3C))  # sand gap


def ic_reaction(cv):
    bg(cv, 0xF2994A, 0xC26320)
    cv.paint(poly([(0.56, 0.14), (0.34, 0.54), (0.48, 0.54),
                   (0.42, 0.86), (0.68, 0.44), (0.52, 0.44)]),
             vgrad(0xFFF3C4, 0xFFD34E, 0.14, 0.86))


def ic_tip(cv):
    bg(cv, 0x3EA36B, 0x217A48)
    cv.paint(disc(0.5, 0.5, 0.32), vgrad(0xFFE98A, 0xE7B84A, 0.2, 0.8))   # coin
    cv.paint(ring(0.5, 0.5, 0.28, 0.03), solid(0xC79A34))
    cv.paint(seg(0.5, 0.26, 0.5, 0.74, 0.035), solid(0x8A6A1E))          # $
    cv.paint(poly([(0.62, 0.36), (0.4, 0.36), (0.38, 0.5), (0.62, 0.5),
                   (0.6, 0.64), (0.38, 0.64)]), solid(0x8A6A1E))
    cv.paint(poly([(0.44, 0.4), (0.58, 0.4), (0.58, 0.455), (0.44, 0.455)]), solid(0xF3D77A))
    cv.paint(poly([(0.42, 0.545), (0.56, 0.545), (0.56, 0.6), (0.42, 0.6)]), solid(0xF3D77A))


def ic_level(cv):
    bg(cv, 0x8E5AD6, 0x5E33A0)
    cv.paint(rrect(0.14, 0.4, 0.86, 0.6, 0.08), vgrad(0xF7D774, 0xD9A93C, 0.4, 0.6))  # body
    cv.paint(rrect(0.32, 0.44, 0.68, 0.56, 0.05), solid(0x2E7D4F))       # vial
    cv.paint(disc(0.5, 0.5, 0.045), solid(0xBFF3C9))                     # bubble
    cv.paint(seg(0.42, 0.44, 0.42, 0.56, 0.012), solid(0xBFF3C9))
    cv.paint(seg(0.58, 0.44, 0.58, 0.56, 0.012), solid(0xBFF3C9))


def ic_flashlight(cv):
    bg(cv, 0x30363E, 0x171A1F)
    # beam
    cv.paint(poly([(0.5, 0.34), (0.86, 0.14), (0.86, 0.54)]), solid(0xFFE27A, 90))
    cv.paint(rrect(0.14, 0.4, 0.34, 0.6, 0.04), vgrad(0xD6DBE2, 0x9099A3, 0.4, 0.6))  # body
    cv.paint(poly([(0.34, 0.36), (0.5, 0.42), (0.5, 0.58), (0.34, 0.64)]),
             vgrad(0xEDF1F6, 0xB9C0C9, 0.36, 0.64))                       # head
    cv.paint(rrect(0.46, 0.42, 0.52, 0.58, 0.02), solid(0xFFE27A))       # lens


ICONS = {
    "dice": ic_dice, "clock": ic_clock, "faces": ic_faces,
    "stopwatch": ic_stopwatch, "plus": ic_counter, "audio": ic_tone,
    "list": ic_tally, "hourglass": ic_countdown, "fire": ic_reaction,
    "dollar": ic_tip, "level": ic_level, "flashlight": ic_flashlight,
}


def main():
    names = sorted(ICONS.keys())
    with open(OUT_C, "w") as c:
        c.write('/* Generated by tools/gen_app_icons.py -- do not edit by hand.\n'
                ' * Full-colour app-icon images (LVGL RGB565, over black) for the launcher grid. */\n')
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
                '/* Full-colour custom app-icon image for an icon-name key, or NULL. */\n'
                'const lv_image_dsc_t *launcher_app_image(const char *key);\n')
    print(f"wrote {len(names)} colour icons ({SIZE}x{SIZE} RGB565) to launcher_icons.{{c,h}}")


if __name__ == "__main__":
    main()
