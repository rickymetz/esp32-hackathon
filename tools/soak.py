#!/usr/bin/env python3
"""Soak the device: run an app for a long time and watch memory.

The launch/exit cycle is known leak-free; SUSTAINED running is not, and
a watch face ticking every second with per-minute repaints is exactly
the shape of thing that leaks slowly.

    tools/soak.py faces.lua 120     # minutes

Samples free PSRAM and internal DRAM via a MEM serial command, appending
CSV so a run survives interruption.
"""
import sys, glob, time, serial, datetime

app = sys.argv[1] if len(sys.argv) > 1 else 'faces.lua'
minutes = float(sys.argv[2]) if len(sys.argv) > 2 else 60.0
out = sys.argv[3] if len(sys.argv) > 3 else 'soak.csv'

port = sorted(glob.glob('/dev/cu.usbmodem*'))[0]
s = serial.Serial(port, 115200, timeout=2)

s.write(b'STOP\n'); time.sleep(1.5); s.read(8000)
s.write(f'RUN {app}\n'.encode()); time.sleep(3); s.read(8000)

end = time.time() + minutes * 60
n = 0
with open(out, 'a') as f:
    f.write(f"# soak {app} started {datetime.datetime.now().isoformat()}\n")
    f.write("elapsed_s,psram_free,internal_free,largest_internal\n")
    f.flush()
    while time.time() < end:
        s.reset_input_buffer()
        s.write(b'MEM\n')
        time.sleep(1.0)
        line = s.read(4000).decode(errors='replace')
        for l in line.splitlines():
            if l.startswith('MEM '):
                p = l.split()
                if len(p) >= 4:
                    n += 1
                    row = f"{int(time.time()-(end-minutes*60))},{p[1]},{p[2]},{p[3]}"
                    f.write(row + "\n"); f.flush()
                    if n % 10 == 1:
                        print(row, flush=True)
                break
        time.sleep(29)
s.close()
print(f"soak complete: {n} samples -> {out}")
