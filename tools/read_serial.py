#!/usr/bin/env python3
"""Reset the board and capture console output.

Opening the port and reading returns 0 bytes on this board; an RTS pulse is
required first. Usage: read_serial.py [seconds] [--no-reset]
"""
import sys, time, glob, serial

secs = float(sys.argv[1]) if len(sys.argv) > 1 and not sys.argv[1].startswith("-") else 10.0
reset = "--no-reset" not in sys.argv

ports = sorted(glob.glob("/dev/cu.usbmodem*"))
if not ports:
    sys.exit("no /dev/cu.usbmodem* found — is the board powered and attached?")

s = serial.Serial(ports[0], 115200, timeout=1)
if reset:
    s.dtr = False
    s.rts = False
    time.sleep(0.1)
    s.rts = True
    time.sleep(0.2)
    s.rts = False

end = time.time() + secs
buf = b""
while time.time() < end:
    buf += s.read(8192)
s.close()
sys.stdout.write(buf.decode("utf-8", "replace"))
