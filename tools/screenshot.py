#!/usr/bin/env python3
"""Capture the device's active screen over serial as a PNG.

Usage: screenshot.py [out.png] [port]

Sends SHOT, decodes the base64 RGB565 stream, writes a PNG with no
dependencies beyond pyserial (PNG encoding is stdlib zlib). Log lines the
firmware interleaves into the console are filtered by charset.
"""
import sys, re, glob, base64, struct, zlib, time, serial

out_path = sys.argv[1] if len(sys.argv) > 1 else "shot.png"
ports = [sys.argv[2]] if len(sys.argv) > 2 else sorted(glob.glob("/dev/cu.usbmodem*"))
if not ports:
    sys.exit("no board found")

B64 = re.compile(r"^[A-Za-z0-9+/=]+$")

s = serial.Serial(ports[0], 115200, timeout=3)
s.reset_input_buffer()
s.write(b"SHOT\n")

w = h = stride = None
payload = bytearray()
deadline = time.time() + 90
while time.time() < deadline:
    line = s.readline().decode("utf-8", "replace").strip()
    if not line:
        continue
    if line.startswith("SHOT_ERR"):
        sys.exit(f"device: {line}")
    if line.startswith("SHOT "):
        _, w, h, stride = line.split()
        w, h, stride = int(w), int(h), int(stride)
        continue
    if line == "ENDSHOT":
        break
    if w is not None and B64.match(line):
        payload += base64.b64decode(line)
else:
    sys.exit("timed out")
s.close()

if w is None or len(payload) < h * stride:
    sys.exit(f"short read: {len(payload)} bytes, want {h * stride if w else '?'}")

# RGB565 little-endian -> RGB888 rows (dropping stride padding)
rows = []
for y in range(h):
    row = bytearray()
    base = y * stride
    for x in range(w):
        v = payload[base + 2 * x] | (payload[base + 2 * x + 1] << 8)
        row += bytes((
            (v >> 11) << 3 | (v >> 13),
            ((v >> 5) & 0x3F) << 2 | ((v >> 9) & 0x3),
            (v & 0x1F) << 3 | ((v >> 2) & 0x7),
        ))
    rows.append(bytes(row))

def png(w, h, rows):
    def chunk(tag, data):
        c = tag + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c))
    raw = b"".join(b"\x00" + r for r in rows)
    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 6))
            + chunk(b"IEND", b""))

open(out_path, "wb").write(png(w, h, rows))
print(f"wrote {out_path} ({w}x{h})")
