#!/usr/bin/env python3
"""Drive the *simulator* with the same command chain as tools/drive.py.

Where drive.py talks to the board over serial, this runs the headless sim
binary -- no hardware needed. The verbs are identical, so a drive.py chain
works here unchanged:

  run <app.lua>                    launch an app (stops anything running first)
  stop                             stop the running app
  tap <x> <y>                      synthetic tap
  swipe <x0> <y0> <x1> <y1> [ms]   synthetic swipe/drag
  sleep <seconds>                  wait (UI settle, timers, animations)
  shot <out.png>                   capture the screen to a PNG

Example:
  sim/simctl.py run apps/counter.lua : sleep 1 : tap 184 224 : shot out.png

Options:
  --sdroot DIR   SD-card root that app/font paths resolve against
                 (default: repo root, so "apps/foo.ttf" works)

The sim binary is built on first use if missing (see sim/build.sh).
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
BINARY = os.path.join(HERE, "build", "sim")


def ensure_built():
    if not os.path.exists(BINARY):
        print("sim binary missing -- building ...", file=sys.stderr)
        subprocess.run([os.path.join(HERE, "build.sh")], check=True)


def main():
    args = sys.argv[1:]
    if not args or args[0] in ("-h", "--help"):
        sys.exit(__doc__)

    sdroot = REPO
    if args and args[0] == "--sdroot":
        sdroot = args[1]
        args = args[2:]

    ensure_built()
    # The binary parses the ':'-separated pipeline itself.
    cmd = [BINARY, "--sdroot", sdroot] + args
    sys.exit(subprocess.run(cmd).returncode)


if __name__ == "__main__":
    main()
