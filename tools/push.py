#!/usr/bin/env python3
"""Push a .lua app to the board over USB. Usage: push.py <file.lua> [port]"""
import sys, os, glob, base64, zlib, time, serial

if len(sys.argv) < 2:
    sys.exit("usage: push.py <file.lua> [port]")

path = sys.argv[1]
if not path.endswith(".lua"):
    sys.exit("only .lua files can be pushed")

name = os.path.basename(path)
data = open(path, "rb").read()
crc = zlib.crc32(data) & 0xFFFFFFFF

ports = [sys.argv[2]] if len(sys.argv) > 2 else sorted(glob.glob("/dev/cu.usbmodem*"))
if not ports:
    sys.exit("no board found")

try:
    s = serial.Serial(ports[0], 115200, timeout=2)
except serial.SerialException as e:
    sys.exit(f"cannot open {ports[0]}: {e}\nIs a serial monitor holding the port?")

s.write(f"PUSH {name} {len(data)} {crc:08x}\n".encode())
for i in range(0, len(data), 57):          # 57 bytes -> 76 base64 chars
    s.write(base64.b64encode(data[i:i + 57]) + b"\n")
    s.flush()
s.write(b"ENDPUSH\n")

deadline = time.time() + 10
while time.time() < deadline:
    line = s.readline().decode("utf-8", "replace").strip()
    if line.startswith("PUSH_OK"):
        print(f"pushed {name} ({len(data)} bytes) — tap Refresh on the device")
        sys.exit(0)
    if line.startswith("PUSH_ERR"):
        sys.exit(f"device rejected: {line}")
s.close()
sys.exit("timed out waiting for the device")
