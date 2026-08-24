#!/usr/bin/env python3
"""Convert a PNG into an LVGL v9 RGB565 image (.bin) for a folder app's icon.

The launcher has no PNG decoder compiled in (that would cost PSRAM and only
works on the device), so a folder app ships its icon pre-converted to LVGL's
native binary image format, which the launcher streams straight off the card.

    tools/png2icon.py apps/mygame/icon.png            # -> apps/mygame/icon.bin (128x128)
    tools/png2icon.py logo.png icon.bin --size 96     # a different size

The output is a 12-byte lv_image_header_t (magic 0x19, RGB565 cf 0x12) followed
by width*height RGB565 little-endian pixels. Transparency is flattened onto a
background colour (--bg, default black) because RGB565 has no alpha channel and
the launcher's tiles sit on true black anyway.

The default size is 128 -- the launcher's grid tile size. That matters: LVGL's
file-image *upscale* clips a circle flat on its right and bottom edges, so an
icon smaller than the tile (e.g. 120) comes out visibly cut. Emitting at the
tile size means the launcher only ever downscales (in the list), which is clean.
Author the source PNG larger still (240-480) for the smoothest downscale.

PNG decoding uses Pillow when it is installed (any format, nice resampling) and
otherwise a small built-in decoder that handles 8-bit PNGs (grayscale, RGB,
palette, and their alpha variants); resizing is an antialiased box filter.
"""
import sys
import os
import struct
import zlib

LV_IMAGE_HEADER_MAGIC = 0x19
LV_COLOR_FORMAT_RGB565 = 0x12


def parse_bg(s):
    s = s.lstrip("#")
    if len(s) != 6:
        sys.exit(f"--bg must be a #RRGGBB colour, got {s!r}")
    return tuple(int(s[i:i + 2], 16) for i in (0, 2, 4))


# ---- PNG decode: Pillow if available, else a compact built-in decoder -------

def decode_rgba(path):
    """Decode a PNG to (w, h, RGBA bytes) at its native size."""
    try:
        from PIL import Image
        im = Image.open(path).convert("RGBA")
        return im.width, im.height, im.tobytes()
    except ImportError:
        return decode_builtin(path)


def _paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    return b if pb <= pc else c


def decode_builtin(path):
    """Minimal PNG -> (w, h, RGBA bytes). Handles bit depth 8, non-interlaced,
    colour types 0 (gray), 2 (RGB), 3 (palette), 4 (gray+A), 6 (RGBA)."""
    data = open(path, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        sys.exit(f"{path}: not a PNG")
    pos = 8
    width = height = bitdepth = colortype = interlace = None
    idat = bytearray()
    palette = None
    trans = None
    while pos < len(data):
        (length,) = struct.unpack(">I", data[pos:pos + 4])
        ctype = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        pos += 12 + length  # 4 len + 4 type + body + 4 crc
        if ctype == b"IHDR":
            width, height, bitdepth, colortype, _comp, _filt, interlace = \
                struct.unpack(">IIBBBBB", body)
        elif ctype == b"PLTE":
            palette = body
        elif ctype == b"tRNS":
            trans = body
        elif ctype == b"IDAT":
            idat += body
        elif ctype == b"IEND":
            break
    if bitdepth != 8:
        sys.exit(f"{path}: only 8-bit PNGs are supported without Pillow "
                 f"(this is bit depth {bitdepth}); install Pillow for others")
    if interlace:
        sys.exit(f"{path}: interlaced PNGs need Pillow")

    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}.get(colortype)
    if channels is None:
        sys.exit(f"{path}: unsupported colour type {colortype}")

    raw = zlib.decompress(bytes(idat))
    bpp = channels
    stride = width * bpp
    out = bytearray()
    prev = bytearray(stride)
    p = 0
    for _y in range(height):
        ftype = raw[p]
        p += 1
        line = bytearray(raw[p:p + stride])
        p += stride
        if ftype == 1:      # Sub
            for i in range(bpp, stride):
                line[i] = (line[i] + line[i - bpp]) & 0xFF
        elif ftype == 2:    # Up
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif ftype == 3:    # Average
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 0xFF
        elif ftype == 4:    # Paeth
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                c = prev[i - bpp] if i >= bpp else 0
                line[i] = (line[i] + _paeth(a, prev[i], c)) & 0xFF
        elif ftype != 0:
            sys.exit(f"{path}: bad PNG filter type {ftype}")
        out += line
        prev = line

    # Expand whatever channel layout we have into flat RGBA.
    rgba = bytearray(width * height * 4)
    px = width * height
    if colortype == 6:
        return width, height, bytes(out)
    for i in range(px):
        if colortype == 2:      # RGB
            r, g, b = out[i * 3], out[i * 3 + 1], out[i * 3 + 2]
            a = 255
        elif colortype == 0:    # gray
            r = g = b = out[i]
            a = 255
        elif colortype == 4:    # gray + alpha
            r = g = b = out[i * 2]
            a = out[i * 2 + 1]
        else:                   # palette
            idx = out[i]
            r, g, b = palette[idx * 3], palette[idx * 3 + 1], palette[idx * 3 + 2]
            a = trans[idx] if trans and idx < len(trans) else 255
        rgba[i * 4:i * 4 + 4] = bytes((r, g, b, a))
    return width, height, bytes(rgba)


def flatten_rgb(w, h, rgba, bg):
    """Composite RGBA over the background to opaque RGB. Done *before* any
    resize so a resampled edge blends the icon colour into the background --
    the alpha is baked in at full resolution, not averaged separately."""
    br, bgc, bb = bg
    rgb = bytearray(w * h * 3)
    for i in range(w * h):
        r, g, b, a = rgba[i * 4], rgba[i * 4 + 1], rgba[i * 4 + 2], rgba[i * 4 + 3]
        if a != 255:
            r = (r * a + br * (255 - a)) // 255
            g = (g * a + bgc * (255 - a)) // 255
            b = (b * a + bb * (255 - a)) // 255
        rgb[i * 3:i * 3 + 3] = bytes((r, g, b))
    return rgb


def resample_rgb(w, h, rgb, size):
    """Resize opaque RGB to size*size. Downscaling averages every source pixel
    that falls in a destination cell (a box filter -- real antialiasing, so a
    hard-edged source comes out smooth); the common case, since icons are drawn
    larger than the tile. Upscaling falls back to nearest, so author at >= the
    target size for crisp results."""
    if w >= size:  # Pillow gives nicer filtering when present; use it if we can.
        try:
            from PIL import Image
            im = Image.frombytes("RGB", (w, h), bytes(rgb)).resize((size, size), Image.LANCZOS)
            return bytearray(im.tobytes())
        except ImportError:
            pass
    out = bytearray(size * size * 3)
    for dy in range(size):
        sy0 = dy * h // size
        sy1 = max(sy0 + 1, (dy + 1) * h // size)
        for dx in range(size):
            sx0 = dx * w // size
            sx1 = max(sx0 + 1, (dx + 1) * w // size)
            r = g = b = n = 0
            for sy in range(sy0, sy1):
                row = sy * w
                for sx in range(sx0, sx1):
                    i = (row + sx) * 3
                    r += rgb[i]; g += rgb[i + 1]; b += rgb[i + 2]; n += 1
            di = (dy * size + dx) * 3
            out[di] = r // n; out[di + 1] = g // n; out[di + 2] = b // n
    return out


def main():
    args = [a for a in sys.argv[1:]]
    size = 128           # the launcher's grid tile size; see the module docstring
    bg = (0, 0, 0)
    positional = []
    i = 0
    while i < len(args):
        if args[i] == "--size":
            size = int(args[i + 1]); i += 2
        elif args[i] == "--bg":
            bg = parse_bg(args[i + 1]); i += 2
        else:
            positional.append(args[i]); i += 1

    if not positional:
        sys.exit("usage: png2icon.py <in.png> [out.bin] [--size N] [--bg #RRGGBB]")
    src = positional[0]
    dst = positional[1] if len(positional) > 1 else os.path.splitext(src)[0] + ".bin"

    w, h, rgba = decode_rgba(src)
    rgb = flatten_rgb(w, h, rgba, bg)          # bake alpha at full resolution
    if size and (w, h) != (size, size):        # then resample (antialiased)
        rgb = resample_rgb(w, h, rgb, size)
        w = h = size

    # Pack RGB565 little-endian.
    pixels = bytearray(w * h * 2)
    for i in range(w * h):
        r, g, b = rgb[i * 3], rgb[i * 3 + 1], rgb[i * 3 + 2]
        v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        pixels[i * 2] = v & 0xFF
        pixels[i * 2 + 1] = (v >> 8) & 0xFF

    stride = w * 2
    header = struct.pack("<BBHHHHH",
                         LV_IMAGE_HEADER_MAGIC, LV_COLOR_FORMAT_RGB565,
                         0, w, h, stride, 0)
    with open(dst, "wb") as f:
        f.write(header)
        f.write(pixels)
    print(f"wrote {dst} ({w}x{h}, {len(header) + len(pixels)} bytes)")


if __name__ == "__main__":
    main()
