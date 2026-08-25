#!/usr/bin/env python3
"""Soak the device: run an app for a long time and watch memory.

The launch/exit cycle is known leak-free; SUSTAINED running is not, and
a watch face ticking every second with per-minute repaints is exactly
the shape of thing that leaks slowly.

    tools/soak.py stopwatch.lua 120     # minutes

Samples free PSRAM and internal DRAM via a MEM serial command, appending
CSV so a run survives interruption.
"""
import sys, glob, time, serial, datetime

app = sys.argv[1] if len(sys.argv) > 1 else 'stopwatch.lua'
minutes = float(sys.argv[2]) if len(sys.argv) > 2 else 60.0
out = sys.argv[3] if len(sys.argv) > 3 else 'soak.csv'

# This board uses the S3's native USB, and the link re-enumerates on its own
# during a long run -- the port even comes back under a different name
# (usbmodem101 -> usbmodem1101). A soak that holds one handle open dies at the
# first blip, which is exactly when a long run is getting interesting. So:
# re-glob the port and reopen on every failure, and treat a dead sample as a
# gap rather than the end of the run.
def open_port():
    ports = sorted(glob.glob('/dev/cu.usbmodem*'))
    if not ports:
        return None
    try:
        return serial.Serial(ports[0], 115200, timeout=2)
    except serial.SerialException:
        return None


def reconnect(timeout=90):
    """Wait for the device to come back. Returns None if it never does."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        s = open_port()
        if s is not None:
            return s
        time.sleep(5)
    return None


s = reconnect()
if s is None:
    sys.exit('no board found')

s.write(b'STOP\n'); time.sleep(1.5); s.read(8000)
s.write(f'RUN {app}\n'.encode()); time.sleep(3); s.read(8000)

end = time.time() + minutes * 60
n = 0
with open(out, 'a') as f:
    f.write(f"# soak {app} started {datetime.datetime.now().isoformat()}\n")
    f.write("elapsed_s,psram_free,internal_free,largest_internal\n")
    f.flush()
    start = end - minutes * 60
    gaps = 0
    while time.time() < end:
        try:
            s.reset_input_buffer()
            s.write(b'MEM\n')
            time.sleep(1.0)
            line = s.read(4000).decode(errors='replace')
        except (serial.SerialException, OSError):
            line = ''

        got = False
        for l in line.splitlines():
            if l.startswith('MEM '):
                p = l.split()
                if len(p) >= 4:
                    n += 1
                    got = True
                    row = f"{int(time.time()-start)},{p[1]},{p[2]},{p[3]}"
                    f.write(row + "\n"); f.flush()
                    if n % 10 == 1:
                        print(row, flush=True)
                break

        if not got:
            # Lost the link. Reopen, relaunch the app, and carry on -- a
            # recorded gap is far more useful than a truncated run.
            gaps += 1
            f.write(f"# link lost at {int(time.time()-start)}s, reconnecting\n")
            f.flush()
            print(f"link lost at {int(time.time()-start)}s, reconnecting", flush=True)
            try:
                s.close()
            except Exception:
                pass
            s = reconnect()
            if s is None:
                f.write("# device never returned; soak aborted\n")
                print("device never returned; soak aborted", flush=True)
                break
            s.write(b'STOP\n'); time.sleep(1.0); s.read(8000)
            s.write(f'RUN {app}\n'.encode()); time.sleep(2.5); s.read(8000)

        time.sleep(29)

try:
    s.close()
except Exception:
    pass
print(f"soak complete: {n} samples, {gaps} link drops -> {out}")
