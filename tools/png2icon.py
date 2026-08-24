#!/usr/bin/env python3
"""Convert a PNG into an LVGL v9 RGB565 image (.bin) for a folder app's icon.

The launcher has no PNG decoder compiled in (that would cost PSRAM and only
works on the device), so a folder app ships its icon pre-converted to LVGL's
native binary image format, which the launcher streams straight off the card.

    tools/png2icon.py apps/mygame/icon.png            # -> apps/mygame/icon.bin
    tools/png2icon.py logo.png icon.bin --size 120    # resize to 120x120

The output is a 12-byte lv_image_header_t (magic 0x19, RGB565 cf 0x12) followed
by width*height RGB565 little-endian pixels. Transparency is flattened onto a
background colour (--bg, default black) because RGB565 has no alpha channel and
the launcher's tiles sit on true black anyway.

PNG decoding uses Pillow when it is installed (any format, nice resampling) and
otherwise a small built-in decoder that handles 8-bit PNGs (grayscale, RGB,
palette, and their alpha variants) with a nearest-neighbour resize.
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

def decode_with_pillow(path, size):
    from PIL import Image
    im = Image.open(path).convert("RGBA")
    if size:
        im = im.resize((size, size), Image.LANCZOS)
    return im.width, im.height, im.tobytes()


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


def nearest_resize(w, h, rgba, size):
    out = bytearray(size * size * 4)
    for y in range(size):
        sy = y * h // size
        for x in range(size):
            sx = x * w // size
            si = (sy * w + sx) * 4
            di = (y * size + x) * 4
            out[di:di + 4] = rgba[si:si + 4]
    return size, size, bytes(out)


def main():
    args = [a for a in sys.argv[1:]]
    size = None
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

    try:
        import PIL  # noqa: F401
        w, h, rgba = decode_with_pillow(src, size)
    except ImportError:
        w, h, rgba = decode_builtin(src)
        if size and (w, h) != (size, size):
            w, h, rgba = nearest_resize(w, h, rgba, size)

    # Flatten RGBA onto the background, pack RGB565 little-endian.
    br, bgc, bb = bg
    pixels = bytearray(w * h * 2)
    for i in range(w * h):
        r, g, b, a = rgba[i * 4], rgba[i * 4 + 1], rgba[i * 4 + 2], rgba[i * 4 + 3]
        if a != 255:
            r = (r * a + br * (255 - a)) // 255
            g = (g * a + bgc * (255 - a)) // 255
            b = (b * a + bb * (255 - a)) // 255
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
