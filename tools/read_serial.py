#!/usr/bin/env python3
"""Reset the board and capture console output.

Opening the port and reading returns 0 bytes on this board; an RTS pulse is
required first. Usage: read_serial.py [seconds] [--no-reset]
"""
import sys, time, glob, serial

# Parse arguments in any order (Finding 2: flag-order-independent parsing)
secs = 10.0
reset = True
for arg in sys.argv[1:]:
    if arg == "--no-reset":
        reset = False
    else:
        try:
            secs = float(arg)
        except ValueError:
            pass

ports = sorted(glob.glob("/dev/cu.usbmodem*"))
if not ports:
    sys.exit("no /dev/cu.usbmodem* found — is the board powered and attached?")

# Finding 3: Report which port was chosen and list alternatives to stderr
sys.stderr.write(f"using {ports[0]}")
if len(ports) > 1:
    sys.stderr.write(f" (alternatives: {', '.join(ports[1:])})")
sys.stderr.write("\n")
sys.stderr.flush()

# Finding 1: Protect serial.Serial() call with error handling
try:
    s = serial.Serial(ports[0], 115200, timeout=1)
except (OSError, serial.SerialException) as e:
    sys.exit(f"failed to open {ports[0]}: {e} (is a serial monitor holding the port?)")

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
