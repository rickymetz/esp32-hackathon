#!/usr/bin/env python3
"""Generate custom app-icon images for the launcher grid.

Rasterises a small set of hand-defined vector icons to LVGL A8 (alpha-mask)
image descriptors and writes launcher/main/launcher_icons.c (+ .h). The icons
are white shapes on transparency; the launcher draws them recoloured white on
each app's coloured tile. A8 keeps them tiny (w*h bytes).

Pure Python (no Pillow): each icon is defined as add/subtract membership shapes
over the unit square, rendered at SSx supersampling and box-downsampled for
anti-aliasing.

    tools/gen_app_icons.py        # regenerate launcher/main/launcher_icons.{c,h}
"""
import math
import os

SIZE = 64          # icon px
SS = 4             # supersample factor
HERE = os.path.dirname(os.path.abspath(__file__))
OUT_C = os.path.join(HERE, "..", "launcher", "main", "launcher_icons.c")
OUT_H = os.path.join(HERE, "..", "launcher", "main", "launcher_icons.h")


# ---- membership shape factories (unit square [0,1]) ---------------------

def disc(cx, cy, r):
    return lambda u, v: (u - cx) ** 2 + (v - cy) ** 2 <= r * r


def ring(cx, cy, r, t):
    return lambda u, v: abs(math.hypot(u - cx, v - cy) - r) <= t / 2


def seg(ax, ay, bx, by, t):
    dx, dy = bx - ax, by - ay
    L2 = dx * dx + dy * dy or 1e-9

    def f(u, v):
        s = ((u - ax) * dx + (v - ay) * dy) / L2
        s = max(0.0, min(1.0, s))
        px, py = ax + s * dx, ay + s * dy
        return (u - px) ** 2 + (v - py) ** 2 <= (t / 2) ** 2
    return f


def rrect(x0, y0, x1, y1, rad):
    def f(u, v):
        if not (x0 <= u <= x1 and y0 <= v <= y1):
            return False
        cx = min(max(u, x0 + rad), x1 - rad)
        cy = min(max(v, y0 + rad), y1 - rad)
        return (u - cx) ** 2 + (v - cy) ** 2 <= rad * rad
    return f


def poly(pts):
    def f(u, v):
        inside = False
        n = len(pts)
        j = n - 1
        for i in range(n):
            xi, yi = pts[i]
            xj, yj = pts[j]
            if ((yi > v) != (yj > v)) and \
               (u < (xj - xi) * (v - yi) / ((yj - yi) or 1e-9) + xi):
                inside = not inside
            j = i
        return inside
    return f


# ---- icon definitions: list of (op, shape) -----------------------------
# op 'a' adds, 's' subtracts (draw order matters).

def rring(x0, y0, x1, y1, rad, t):
    """A rounded-rect outline, as fill minus inner fill."""
    return [('a', rrect(x0, y0, x1, y1, rad)),
            ('s', rrect(x0 + t, y0 + t, x1 - t, y1 - t, max(0.01, rad - t)))]


def clock_hands(cx=0.5, cy=0.52):
    return [('a', seg(cx, cy, cx, 0.30, 0.055)),      # minute up
            ('a', seg(cx, cy, 0.68, 0.60, 0.055))]    # hour


ICONS = {
    "plus": [('a', seg(0.5, 0.22, 0.5, 0.78, 0.13)),
             ('a', seg(0.22, 0.5, 0.78, 0.5, 0.13))],
    "clock": [('a', ring(0.5, 0.5, 0.34, 0.07))] + clock_hands(),
    "stopwatch": [('a', seg(0.42, 0.1, 0.58, 0.1, 0.06)),   # top bar
                  ('a', seg(0.5, 0.08, 0.5, 0.17, 0.06)),   # stem
                  ('a', ring(0.5, 0.56, 0.32, 0.07)),
                  ('a', seg(0.5, 0.56, 0.5, 0.34, 0.05)),   # hand
                  ('a', seg(0.5, 0.56, 0.66, 0.62, 0.05))],
    "audio": [('a', disc(0.34, 0.7, 0.11)),                 # note head
              ('a', disc(0.64, 0.62, 0.11)),
              ('a', seg(0.44, 0.7, 0.44, 0.26, 0.05)),      # stems
              ('a', seg(0.74, 0.62, 0.74, 0.2, 0.05)),
              ('a', seg(0.44, 0.26, 0.74, 0.2, 0.06))],     # beam
    "list": [('a', rrect(0.2, 0.24, 0.8, 0.32, 0.02)),
             ('a', rrect(0.2, 0.46, 0.8, 0.54, 0.02)),
             ('a', rrect(0.2, 0.68, 0.8, 0.76, 0.02))],
    "sun": [('a', disc(0.5, 0.5, 0.2))] +
           [('a', seg(0.5 + 0.30 * math.cos(a), 0.5 + 0.30 * math.sin(a),
                      0.5 + 0.42 * math.cos(a), 0.5 + 0.42 * math.sin(a), 0.06))
            for a in [i * math.pi / 4 for i in range(8)]],
    "fire": [('a', poly([(0.5, 0.12), (0.72, 0.42), (0.75, 0.66),
                          (0.62, 0.86), (0.38, 0.86), (0.25, 0.62),
                          (0.34, 0.4), (0.44, 0.52), (0.42, 0.3)]))],
    "tint": [('a', poly([(0.5, 0.14), (0.74, 0.54), (0.72, 0.72),
                         (0.5, 0.86), (0.28, 0.72), (0.26, 0.54)]))],
    "settings": [('a', ring(0.5, 0.5, 0.22, 0.14))] +
                [('a', seg(0.5 + 0.22 * math.cos(a), 0.5 + 0.22 * math.sin(a),
                           0.5 + 0.40 * math.cos(a), 0.5 + 0.40 * math.sin(a), 0.12))
                 for a in [i * math.pi / 4 for i in range(8)]] +
                [('s', disc(0.5, 0.5, 0.1))],
    "wifi": [('a', ring(0.5, 0.66, 0.4, 0.07)),
             ('a', ring(0.5, 0.66, 0.26, 0.07)),
             ('a', disc(0.5, 0.66, 0.06)),
             ('s', poly([(0.5, 0.66), (0.02, 0.66), (0.02, 0.98), (0.98, 0.98), (0.98, 0.66)]))],
    "thermometer": [('a', ring(0.5, 0.4, 0.1, 0.05)),
                    ('a', seg(0.5, 0.4, 0.5, 0.66, 0.14)),
                    ('a', disc(0.5, 0.74, 0.13)),
                    ('a', seg(0.5, 0.5, 0.5, 0.7, 0.055))],  # mercury (drawn over)
    "keyboard": rring(0.14, 0.32, 0.86, 0.68, 0.06, 0.05) +
                [('a', disc(0.28, 0.44, 0.03)), ('a', disc(0.4, 0.44, 0.03)),
                 ('a', disc(0.52, 0.44, 0.03)), ('a', disc(0.64, 0.44, 0.03)),
                 ('a', disc(0.72, 0.44, 0.03)),
                 ('a', rrect(0.36, 0.55, 0.64, 0.6, 0.02))],
    "heart": [('a', disc(0.34, 0.38, 0.16)), ('a', disc(0.66, 0.38, 0.16)),
              ('a', poly([(0.19, 0.44), (0.81, 0.44), (0.5, 0.84)]))],
    "dollar": [('a', ring(0.5, 0.5, 0.32, 0.07)),
               ('a', seg(0.5, 0.24, 0.5, 0.76, 0.05)),
               ('a', poly([(0.62, 0.36), (0.4, 0.36), (0.38, 0.5), (0.62, 0.5),
                           (0.6, 0.64), (0.38, 0.64)])),
               ('s', poly([(0.44, 0.4), (0.58, 0.4), (0.58, 0.46), (0.44, 0.46)])),
               ('s', poly([(0.42, 0.54), (0.56, 0.54), (0.56, 0.6), (0.42, 0.6)]))],
    "dice": rring(0.2, 0.2, 0.8, 0.8, 0.1, 0.055) +
            [('a', disc(0.34, 0.34, 0.05)), ('a', disc(0.66, 0.34, 0.05)),
             ('a', disc(0.5, 0.5, 0.05)),
             ('a', disc(0.34, 0.66, 0.05)), ('a', disc(0.66, 0.66, 0.05))],
    "level": rring(0.12, 0.4, 0.88, 0.6, 0.1, 0.045) +
             [('a', disc(0.5, 0.5, 0.06)),
              ('a', seg(0.42, 0.4, 0.42, 0.6, 0.02)),
              ('a', seg(0.58, 0.4, 0.58, 0.6, 0.02))],
    "simon": [('a', rrect(0.16, 0.16, 0.47, 0.47, 0.05)),
              ('a', rrect(0.53, 0.16, 0.84, 0.47, 0.05)),
              ('a', rrect(0.16, 0.53, 0.47, 0.84, 0.05)),
              ('a', rrect(0.53, 0.53, 0.84, 0.84, 0.05))],
    "flashlight": [('a', poly([(0.36, 0.16), (0.64, 0.16), (0.58, 0.34), (0.42, 0.34)])),
                   ('a', rrect(0.42, 0.34, 0.58, 0.82, 0.03)),
                   ('a', seg(0.5, 0.16, 0.5, 0.06, 0.04)),
                   ('a', seg(0.36, 0.2, 0.28, 0.12, 0.04)),
                   ('a', seg(0.64, 0.2, 0.72, 0.12, 0.04))],
    "metronome": rring(0.5, 0.16, 0.5, 0.16, 0.01, 0.01) +   # placeholder no-op
                 [('a', poly([(0.5, 0.14), (0.72, 0.84), (0.28, 0.84)])),
                  ('s', poly([(0.5, 0.3), (0.63, 0.75), (0.37, 0.75)])),
                  ('a', seg(0.5, 0.8, 0.62, 0.3, 0.035)),
                  ('a', disc(0.62, 0.3, 0.05))],
    "microphone": [('a', rrect(0.4, 0.16, 0.6, 0.56, 0.1)),
                   ('a', ring(0.5, 0.5, 0.16, 0.045)),
                   ('s', rrect(0.34, 0.16, 0.66, 0.5, 0.0)),  # keep only lower arc
                   ('a', seg(0.5, 0.66, 0.5, 0.8, 0.045)),
                   ('a', seg(0.36, 0.8, 0.64, 0.8, 0.045))],
}


def render(shapes):
    W = SIZE * SS
    grid = [0] * (W * W)
    for op, f in shapes:
        add = (op == 'a')
        for yy in range(W):
            v = (yy + 0.5) / W
            row = yy * W
            for xx in range(W):
                u = (xx + 0.5) / W
                if f(u, v):
                    grid[row + xx] = 1 if add else 0
    # box downsample SSxSS -> alpha 0..255
    out = bytearray(SIZE * SIZE)
    for y in range(SIZE):
        for x in range(SIZE):
            s = 0
            for dy in range(SS):
                base = (y * SS + dy) * W + x * SS
                for dx in range(SS):
                    s += grid[base + dx]
            out[y * SIZE + x] = (s * 255) // (SS * SS)
    return out


def main():
    names = sorted(ICONS.keys())
    with open(OUT_C, "w") as c:
        c.write('/* Generated by tools/gen_app_icons.py -- do not edit by hand.\n'
                ' * Custom app-icon images (LVGL A8 alpha masks) for the launcher grid. */\n')
        c.write('#include "launcher_icons.h"\n#include <string.h>\n\n')
        for n in names:
            data = render(ICONS[n])
            c.write(f"static const uint8_t {n}_map[{len(data)}] = {{\n")
            for i in range(0, len(data), 16):
                c.write("    " + ",".join(str(b) for b in data[i:i + 16]) + ",\n")
            c.write("};\n")
            c.write(f"static const lv_image_dsc_t {n}_img = {{\n"
                    f"    .header = {{ .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_A8,\n"
                    f"                 .w = {SIZE}, .h = {SIZE}, .stride = {SIZE} }},\n"
                    f"    .data_size = {len(data)}, .data = {n}_map,\n}};\n\n")
        c.write("const lv_image_dsc_t *launcher_app_image(const char *key)\n{\n")
        c.write("    if (!key) return NULL;\n")
        for n in names:
            c.write(f'    if (!strcmp(key, "{n}")) return &{n}_img;\n')
        c.write("    return NULL;\n}\n")

    with open(OUT_H, "w") as h:
        h.write('/* Generated by tools/gen_app_icons.py -- do not edit by hand. */\n'
                '#pragma once\n#include "lvgl.h"\n\n'
                '/* Custom app-icon image for an icon-name key, or NULL if none. */\n'
                'const lv_image_dsc_t *launcher_app_image(const char *key);\n')
    print(f"wrote {len(names)} icons to launcher/main/launcher_icons.{{c,h}}")


if __name__ == "__main__":
    main()
