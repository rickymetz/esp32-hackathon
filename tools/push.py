#!/usr/bin/env python3
"""Push a .lua app to the board over USB. Usage: push.py <file.lua> [port]"""
import sys, os, glob, base64, zlib, time, subprocess

if len(sys.argv) < 2:
    sys.exit("usage: push.py <file.lua> [port]  |  push.py --list [port]  |  push.py --delete <name.lua> [port]")


def _serial():
    """Import pyserial lazily so the icon-build and path logic run (and are
    testable) on a machine without it -- only talking to the board needs it."""
    try:
        import serial
        return serial
    except ImportError:
        sys.exit("pyserial is required to talk to the board: pip install -r tools/requirements.txt")


def open_port(argv_idx):
    serial = _serial()
    ports = [sys.argv[argv_idx]] if len(sys.argv) > argv_idx else sorted(glob.glob("/dev/cu.usbmodem*"))
    if not ports:
        sys.exit("no board found")
    try:
        return serial.Serial(ports[0], 115200, timeout=2)
    except serial.SerialException as e:
        sys.exit(f"cannot open {ports[0]}: {e}\nIs a serial monitor holding the port?")


if sys.argv[1] == "--list":
    s = open_port(2)
    s.write(b"LIST\n")
    deadline = time.time() + 5
    while time.time() < deadline:
        line = s.readline().decode("utf-8", "replace").strip()
        if line.startswith("APP "):
            print(line[4:])
        elif line.startswith("LIST_OK"):
            print(f"-- {line.split()[1]} app(s)")
            sys.exit(0)
    sys.exit("timed out")

if sys.argv[1] == "--delete":
    if len(sys.argv) < 3:
        sys.exit("usage: push.py --delete <name.lua> [port]")
    s = open_port(3)
    s.write(f"DELETE {sys.argv[2]}\n".encode())
    deadline = time.time() + 5
    while time.time() < deadline:
        line = s.readline().decode("utf-8", "replace").strip()
        if line.startswith("DELETE_OK"):
            print(f"deleted {sys.argv[2]}")
            sys.exit(0)
        if line.startswith("DELETE_ERR"):
            sys.exit(f"device rejected: {line}")
    sys.exit("timed out")

path = sys.argv[1]


def remote_name_for(file_path):
    """The name to PUSH a single file under, mirroring how the launcher scans
    apps/: a file directly in apps/ (or anywhere else) is flat and keeps its
    basename; a file inside apps/<folder>/ is a folder-app file and is sent as
    <folder>/<basename> so it lands in that folder on the card."""
    ap = os.path.abspath(file_path)
    parent = os.path.dirname(ap)
    grandparent = os.path.dirname(parent)
    if os.path.basename(parent) != "apps" and os.path.basename(grandparent) == "apps":
        return f"{os.path.basename(parent)}/{os.path.basename(ap)}"
    return os.path.basename(ap)


def maybe_build_icon(folder):
    """Convert icon.png -> icon.bin (LVGL RGB565) when the PNG is present and the
    .bin is missing or stale, so a dev keeps a source PNG in the app folder and
    the launcher-ready binary is regenerated on push. The device has no PNG
    decoder, so only the .bin is sent."""
    png = os.path.join(folder, "icon.png")
    binp = os.path.join(folder, "icon.bin")
    if not os.path.isfile(png):
        return
    if os.path.isfile(binp) and os.path.getmtime(binp) >= os.path.getmtime(png):
        return
    conv = os.path.join(os.path.dirname(os.path.abspath(__file__)), "png2icon.py")
    subprocess.run([sys.executable, conv, png, binp], check=True)   # default 128px tile


# Build the list of (local_path, remote_name) files to send. A directory is a
# folder app: push every regular file in it as <folder>/<file>. A single file
# is pushed on its own (flat, or as a folder-app file when it sits in apps/<x>/).
jobs = []
if os.path.isdir(path):
    maybe_build_icon(path)
    folder = os.path.basename(os.path.abspath(path))
    for entry in sorted(os.listdir(path)):
        fp = os.path.join(path, entry)
        # Skip dotfiles and source PNGs -- the device can't decode a PNG, only
        # the generated icon.bin ships.
        if entry.startswith(".") or entry.endswith(".png") or not os.path.isfile(fp):
            continue
        jobs.append((fp, f"{folder}/{entry}"))
    if not jobs:
        sys.exit(f"{path}: no files to push")
    if not any(r.endswith("/main.lua") for _, r in jobs):
        print(f"warning: {path} has no main.lua — the launcher won't list it as an app")
else:
    if not (path.endswith(".lua") or path.endswith(".bin")):
        sys.exit("only .lua and .bin files (or a folder app) can be pushed")
    jobs.append((path, remote_name_for(path)))

serial = _serial()
ports = [sys.argv[2]] if len(sys.argv) > 2 else sorted(glob.glob("/dev/cu.usbmodem*"))
if not ports:
    sys.exit("no board found")

try:
    s = serial.Serial(ports[0], 115200, timeout=2)
except serial.SerialException as e:
    sys.exit(f"cannot open {ports[0]}: {e}\nIs a serial monitor holding the port?")


def push_one(local_path, remote):
    data = open(local_path, "rb").read()
    crc = zlib.crc32(data) & 0xFFFFFFFF
    s.write(f"PUSH {remote} {len(data)} {crc:08x}\n".encode())
    for i in range(0, len(data), 57):          # 57 bytes -> 76 base64 chars
        s.write(base64.b64encode(data[i:i + 57]) + b"\n")
        s.flush()
    s.write(b"ENDPUSH\n")

    deadline = time.time() + 10
    while time.time() < deadline:
        line = s.readline().decode("utf-8", "replace").strip()
        if line.startswith("PUSH_OK"):
            print(f"pushed {remote} ({len(data)} bytes)")
            return
        if line.startswith("PUSH_ERR"):
            sys.exit(f"device rejected {remote}: {line}")
    sys.exit(f"timed out waiting for the device on {remote}")


for local_path, remote in jobs:
    push_one(local_path, remote)
s.close()
print("done — the launcher list refreshes itself (deferred until it is on screen)")
