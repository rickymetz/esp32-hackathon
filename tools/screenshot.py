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
        # Validate before trusting. These numbers come off the wire, and `w` in
        # particular is used to size a struct.Struct below -- CPython allocates
        # one formatcode per repeat, so "SHOT 500000000 1 2" from a corrupted
        # line asks the host for gigabytes before a single pixel is read. The
        # old per-pixel loop hit IndexError almost immediately on a bad `w`;
        # the faster decode traded a quick failure for an unbounded one, so
        # the bound has to be explicit now.
        try:
            _, w, h, stride = line.split()
            w, h, stride = int(w), int(h), int(stride)
        except ValueError:
            sys.exit(f"malformed SHOT header: {line!r}")
        if not (0 < w <= 4096 and 0 < h <= 4096 and 2 * w <= stride <= 2 * 4096):
            sys.exit(f"implausible SHOT header: {w}x{h} stride {stride}")
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

# RGB565 little-endian -> RGB888 rows (dropping stride padding).
#
# Done through a 65,536-entry lookup rather than per-pixel arithmetic. A
# 368x448 frame is 164,864 pixels and the old inner loop did ~10 Python
# operations on each; this builds the table once (65,536 iterations) and then
# each row is one struct.unpack plus a map/join that run in C. Measured:
# 0.016 s to build the table, 0.005 s to convert the whole frame.
#
# Do not expect that to show up as a faster screenshot. The transfer, not the
# decode, is what a capture costs: 1.445 s of the 1.54 s round trip is getting
# the base64 over the wire, for reasons documented above serial_push_start()
# in launcher/main/serial_push.c. This just stops the host adding to it.
# Still no dependency beyond pyserial.
TABLE = [
    bytes((
        (v >> 11) << 3 | (v >> 13),
        ((v >> 5) & 0x3F) << 2 | ((v >> 9) & 0x3),
        (v & 0x1F) << 3 | ((v >> 2) & 0x7),
    ))
    for v in range(65536)
]

rows = []
unpack_row = struct.Struct(f"<{w}H").unpack_from
for y in range(h):
    rows.append(b"".join(map(TABLE.__getitem__, unpack_row(payload, y * stride))))

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
