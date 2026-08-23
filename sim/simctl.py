#!/usr/bin/env python3
"""Drive the *simulator* with the same command chain as tools/drive.py.

Where drive.py talks to the board over serial, this runs the headless sim
binary -- no hardware needed. The verbs are identical, so a drive.py chain
works here unchanged:

  run <app.lua>                    launch an app (stops anything running first)
  stop                             stop the running app
  tap <x> <y>                      synthetic tap
  swipe <x0> <y0> <x1> <y1> [ms]   synthetic swipe/drag
  pwr [down|up|long]               inject the PWR button
  sleep <seconds>                  wait (UI settle, timers, animations)
  shot <out.png>                   capture the screen to a PNG
  check <out.png>                  capture and assert the frame is non-blank
  home [n]                         render the launcher's own home screen with a
                                   fake app list (n apps; 0 = empty state)
  type <text>                      drive require("keyboard") text entry, then OK
  typenum <digits>                 drive the number keypad, then OK

Fake-sensor injection (there is no such thing on the real board -- these set
the sim's stub readings so degraded/dynamic UI paths are testable). Issue them
BEFORE `run` to affect an app's load-time read, or after `run` for apps that
poll on a timer:

  accel <x> <y> <z>                set the IMU accel (g); e.g. 0.5 0 0.866 = 30deg
  gyro <x> <y> <z>                 set the IMU gyro (deg/s)
  battery <pct> [charging] [ext]   set the gauge; pct -1 => "gauge not ready"
  rtc unset                        make rtc.now() report "rtc not set"
  rtc set <y> <mo> <d> <h> <mi> <s> [wday]   set the wall clock
  wifi ok | wifi fail              how the next wifi.connect() resolves

`type`/`typenum` are simctl-side macros that expand to taps on the on-screen
keyboard (opened by the app first). They assume the sim's keyboard layout
(voice unavailable, so no mic key); `type` handles A-Z, space, and the
YZ.,-' punctuation, following the keyboard's auto-capitalisation.

Example:
  sim/simctl.py run apps/counter.lua : sleep 1 : tap 184 224 : shot out.png
  sim/simctl.py run apps/tip.lua : tap 184 130 : typenum 4250 : shot tip.png
  sim/simctl.py run apps/level.lua : accel 0.5 0 0.866 : sleep 0.2 : shot tilt.png
  sim/simctl.py rtc unset : run apps/clock.lua : shot noclock.png

Options:
  --sdroot DIR   SD-card root that app/font paths resolve against
                 (default: repo root, so "apps/foo.ttf" works)
  --timeout N    per-app watchdog seconds (forwarded to the binary)

The sim binary is built on first use if missing (see sim/build.sh).
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
BINARY = os.path.join(HERE, "build", "sim")

# --- keyboard geometry (require("keyboard"), a buttonmatrix at y=96, 4x88) ---
# Row centres, and column centres for 2- and 3-button rows (buttons split the
# 368px width evenly). Confirmed against rendered frames.
_ROWS = [140, 228, 316, 404]
_C2 = [92, 276]
_C3 = [61, 184, 307]
_OK = (320, 48)                       # top-right confirm corner button
_GROUPS = ["ABCDEF", "GHIJKL", "MNOPQR", "STUVWX", "YZ.,-'"]
# group cell in the groups view (rows 0-2 are 2-col; "123" sits at C2[1],row2)
_GROUP_XY = {
    "ABCDEF": (_C2[0], _ROWS[0]), "GHIJKL": (_C2[1], _ROWS[0]),
    "MNOPQR": (_C2[0], _ROWS[1]), "STUVWX": (_C2[1], _ROWS[1]),
    "YZ.,-'": (_C2[0], _ROWS[2]),
}
# letter cell within the letters view (6 letters, 2-col)
_LETTER_XY = [(_C2[0], _ROWS[0]), (_C2[1], _ROWS[0]),
              (_C2[0], _ROWS[1]), (_C2[1], _ROWS[1]),
              (_C2[0], _ROWS[2]), (_C2[1], _ROWS[2])]
_BACK = (_C3[0], _ROWS[3])            # "<" in the letters view
_SPACE = (_C3[1], _ROWS[3])          # space, bottom-middle (voice key absent)
_DIGIT_XY = {
    "1": (_C3[0], _ROWS[0]), "2": (_C3[1], _ROWS[0]), "3": (_C3[2], _ROWS[0]),
    "4": (_C3[0], _ROWS[1]), "5": (_C3[1], _ROWS[1]), "6": (_C3[2], _ROWS[1]),
    "7": (_C3[0], _ROWS[2]), "8": (_C3[1], _ROWS[2]), "9": (_C3[2], _ROWS[2]),
    "0": (_C3[1], _ROWS[3]),
}


def _tap(xy):
    return ["tap", str(xy[0]), str(xy[1])]


def _find_letter(ch):
    up = ch.upper()
    for g in _GROUPS:
        if up in g:
            return g, g.index(up)
    return None


def expand_type(text):
    """Expand a text string into taps on the letter keyboard, ending with OK."""
    out, view, cur = [], "groups", None
    for ch in text:
        if ch == " ":
            # Space stays in the current view (groups->groups, letters->letters
            # with the same group) -- see keyboard.lua -- so view/cur are left
            # as they are; the space key sits bottom-middle in both.
            out += _tap(_SPACE)
            continue
        found = _find_letter(ch)
        if not found:
            continue                  # unsupported char: skip
        g, idx = found
        if view == "letters" and cur == g:
            out += _tap(_LETTER_XY[idx])
        else:
            if view == "letters":
                out += _tap(_BACK)    # back to groups before switching group
            out += _tap(_GROUP_XY[g])
            out += _tap(_LETTER_XY[idx])
            view, cur = "letters", g
    out += _tap(_OK)
    return out


def expand_typenum(digits):
    """Expand digits into taps on the number keypad, ending with OK."""
    out = []
    for ch in digits:
        if ch in _DIGIT_XY:
            out += _tap(_DIGIT_XY[ch])
    out += _tap(_OK)
    return out


def expand_macros(args):
    """Rewrite `type`/`typenum` commands in the ':'-pipeline into tap runs."""
    out, i = [], 0
    while i < len(args):
        tok = args[i]
        if tok in ("type", "typenum") and i + 1 < len(args):
            expanded = (expand_type if tok == "type" else expand_typenum)(args[i + 1])
            # splice the tap run in, keeping ':' separators tidy
            if out and out[-1] != ":":
                out.append(":")
            for j in range(0, len(expanded), 3):
                if j:
                    out.append(":")
                out += expanded[j:j + 3]
            i += 2
        else:
            out.append(tok)
            i += 1
    return out


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
        if len(args) < 2:
            sys.exit("--sdroot needs a directory")
        sdroot = args[1]
        args = args[2:]

    ensure_built()
    args = expand_macros(args)
    # The binary parses the ':'-separated pipeline itself.
    cmd = [BINARY, "--sdroot", sdroot] + args
    sys.exit(subprocess.run(cmd).returncode)


if __name__ == "__main__":
    main()
