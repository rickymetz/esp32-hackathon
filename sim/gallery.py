#!/usr/bin/env python3
"""Build a visual gallery of launcher apps, rendered by the simulator.

Runs each app to a representative state, screenshots it, and assembles a
single self-contained HTML page (screenshots embedded as data URIs). Handy for
a PR, a README, or just seeing every app at a glance -- all with no board.

    sim/gallery.py [out.html]     # default: sim/build/gallery.html

Requires the sim to be built (sim/build.sh).
"""
import base64, os, struct, subprocess, sys, zlib

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
SIM = os.path.join(HERE, "build", "sim")

# app, command tail (after `run apps/<app>.lua`), caption
SCENES = [
    ("counter",    ["tap","184","224",":","tap","184","224",":","tap","184","224"],
                   "The minimal app: tap to count."),
    ("tally",      ["tap","184","243",":","tap","184","243",":","tap","184","243",":","tap","184","243"],
                   "Wrist counter; PWR is a +1 accelerator."),
    ("stopwatch",  ["tap","184","233",":","sleep","1.4"],
                   "timer.every, big font, start/stop/reset."),
    ("countdown",  ["pwr",":","sleep","2"],
                   "Kitchen timer; the stepper reads out M:SS."),
    ("metronome",  ["_beat"],
                   "Visual metronome; the dot flashes on the beat."),
    ("dice",       ["tap","308","44",":","tap","308","44",":","tap","184","328"],
                   "Roll 1-4 d6; corner chip sets the count."),
    ("reaction",   ["tap","184","248",":","sleep","4.5"],
                   "Reaction game: tap when it turns green."),
    ("tip",        ["tap","184","130",":","sleep","0.4",":","tap","60","225",":","tap","184","140",
                    ":","tap","184","225",":","tap","184","400",":","tap","308","45",":","sleep","0.4"],
                   "Split a bill; number pad + steppers."),
    ("flashlight", ["sleep","0.2"],
                   "The whole screen is the light."),
    ("color",      ["swipe","200","236","320","236","300",":","swipe","200","364","66","364","300"],
                   "Mix an RGB color; hex flips for contrast."),
    ("simon",      ["_simon"],
                   "Memory game: repeat the flashing sequence."),
    ("breathe",    ["pwr",":","sleep","2"],
                   "Paced breathing; the circle guides the rhythm."),
    # Type "Hi" through the on-screen keyboard: Edit, GHIJKL group, H, I, OK.
    ("sign",       ["tap","184","380",":","tap","276","140",":","tap","276","140",
                    ":","tap","92","228",":","tap","320","48",":","sleep","0.3"],
                   "Type a message; it fills the glass."),
    ("tip_kbd",    ["@tip","tap","184","130",":","sleep","0.5"],
                   "The number keypad (require('keyboard'))."),
    ("settings",   ["sleep","0.3"],
                   "Set the UI font scale, with a live preview."),
    ("level",      ["sleep","0.3"],
                   "Bubble level from the accelerometer."),
    ("tone",       ["tap","106","244",":","sleep","0.3"],
                   "Tone generator; slider sets the pitch."),
    ("ui_test",    ["sleep","0.4"],
                   "Shared ui helpers: rows, toggle, page dots."),
]


def decode_png(path):
    d = open(path, "rb").read()
    i, w, h, idat = 8, None, None, b""
    while i < len(d):
        ln = struct.unpack(">I", d[i:i+4])[0]
        typ = d[i+4:i+8]
        data = d[i+8:i+8+ln]
        if typ == b"IHDR":
            w, h = struct.unpack(">II", data[:8])
        elif typ == b"IDAT":
            idat += data
        elif typ == b"IEND":
            break
        i += 12 + ln
    return w, h, zlib.decompress(idat)


def recompress(path):
    """Re-emit the sim's (uncompressed) PNG with real zlib compression."""
    w, h, raw = decode_png(path)
    def chunk(typ, data):
        return (struct.pack(">I", len(data)) + typ + data +
                struct.pack(">I", zlib.crc32(typ + data) & 0xffffffff))
    ihdr = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)
    png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) +
           chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))
    return png


def run_scene(app, tail):
    png = os.path.join(HERE, "build", f"gallery_{app}.png")
    real_app = app
    if tail and tail[0].startswith("@"):
        real_app = tail[0][1:]
        tail = tail[1:]
    if tail == ["_simon"]:
        # Start the game, capture across the sequence playback, and keep the
        # frame with the brightest pad (a lit flash reads better than dim).
        cmd = ["run", f"apps/{app}.lua", ":", "tap", "100", "175"]
        frames = []
        for k in range(40):
            f = os.path.join(HERE, "build", f"gallery_{app}_{k}.png")
            cmd += [":", "sleep", "0.04", ":", "shot", f]
            frames.append(f)
        subprocess.run([SIM, "--sdroot", REPO] + cmd, cwd=REPO, capture_output=True)
        best, best_bright = frames[0], -1
        for f in frames:
            w, h, raw = decode_png(f)
            # brightest pixel anywhere in the pad grid (y 96..416)
            mx = 0
            for y in range(110, 400, 12):
                base = y * (1 + w*3) + 1
                for x in range(30, 350, 12):
                    o = base + x*3
                    mx = max(mx, raw[o] + raw[o+1] + raw[o+2])
            if mx > best_bright:
                best_bright, best = mx, f
        os.replace(best, png)
        return png
    if tail == ["_beat"]:
        # Metronome: capture across a beat and keep the frame whose dot center
        # is bluest (the flash).
        cmd = ["run", f"apps/{app}.lua", ":", "tap", "184", "400"]
        frames = []
        for k in range(24):
            f = os.path.join(HERE, "build", f"gallery_{app}_{k}.png")
            cmd += [":", "sleep", "0.03", ":", "shot", f]
            frames.append(f)
        subprocess.run([SIM, "--sdroot", REPO] + cmd,
                       cwd=REPO, capture_output=True)
        best, best_blue = frames[0], -1
        for f in frames:
            w, h, raw = decode_png(f)
            off = 134 * (1 + w*3) + 1 + 184*3
            b = raw[off+2] - max(raw[off], raw[off+1])
            if b > best_blue:
                best_blue, best = b, f
        os.replace(best, png)
        return png
    cmd = ["run", f"apps/{real_app}.lua"]
    if tail:
        cmd += [":"] + tail
    cmd += [":", "shot", png]
    subprocess.run([SIM, "--sdroot", REPO] + cmd, cwd=REPO, capture_output=True)
    return png


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "build", "gallery.html")
    if not os.path.exists(SIM):
        sys.exit("sim not built -- run sim/build.sh")

    cards = []
    for app, tail, caption in SCENES:
        png = run_scene(app, tail)
        if not os.path.exists(png):
            print(f"  (skipped {app}: no frame)", file=sys.stderr)
            continue
        b64 = base64.b64encode(recompress(png)).decode()
        title = app.replace("_", " ")
        cards.append(f'''    <figure>
      <div class="device">
        <div class="glass"><img alt="{title}" src="data:image/png;base64,{b64}"></div>
      </div>
      <figcaption><span class="name">{title}</span><span class="desc">{caption}</span></figcaption>
    </figure>''')
        print(f"  rendered {app}")

    html = HTML_TEMPLATE.replace("__CARDS__", "\n".join(cards)).replace("__N__", str(len(cards)))
    with open(out, "w") as f:
        f.write(html)
    print(f"\nwrote {out} ({len(cards)} apps)")


HTML_TEMPLATE = """<meta charset="utf-8">
<title>Apps on Glass</title>
<meta name="viewport" content="width=device-width, initial-scale=1">
<link rel="preconnect" href="https://fonts.googleapis.com">
<link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
<link rel="stylesheet" href="https://fonts.googleapis.com/css2?family=Chakra+Petch:wght@500;600;700&family=IBM+Plex+Mono:wght@400;500&family=IBM+Plex+Sans:wght@400;500;600&display=swap">
<style>
  /* Dark-only by design: the panel is a true-black AMOLED, so this commits to
     one instrument-lit visual world. Every color is painted explicitly. */
  :root {
    --ink:#0a0b0d; --panel:#14161a; --panel2:#1b1e24; --edge:#262a32;
    --fg:#eef1f5; --mut:#8b93a1; --faint:#5b626e;
    --accent:#2f80ed; --accent-dim:#1f5fb0; --pwr:#c0392b;
    --disp:"Chakra Petch",system-ui,sans-serif;
    --body:"IBM Plex Sans",system-ui,sans-serif;
    --mono:"IBM Plex Mono",ui-monospace,monospace;
  }
  * { box-sizing:border-box; }
  body {
    margin:0; background:var(--ink); color:var(--fg); font-family:var(--body);
    background-image:
      radial-gradient(1200px 600px at 50% -10%, rgba(47,128,237,.10), transparent 70%),
      repeating-linear-gradient(0deg, rgba(255,255,255,.014) 0 1px, transparent 1px 3px);
  }
  .wrap { max-width:1160px; margin:0 auto; padding:0 24px; }

  header { padding:64px 24px 20px; text-align:center; }
  .eyebrow {
    font-family:var(--mono); font-size:12px; letter-spacing:.28em;
    text-transform:uppercase; color:var(--accent); margin:0 0 18px;
  }
  header h1 {
    font-family:var(--disp); font-weight:700; font-size:clamp(34px,6vw,60px);
    letter-spacing:-.01em; margin:0; text-wrap:balance; line-height:1.02;
  }
  header p {
    max-width:60ch; margin:18px auto 0; color:var(--mut); font-size:17px;
  }
  .chips { display:flex; flex-wrap:wrap; gap:8px; justify-content:center; margin:26px 0 4px; }
  .chip {
    font-family:var(--mono); font-size:12px; color:var(--fg);
    background:var(--panel); border:1px solid var(--edge); border-radius:999px;
    padding:6px 12px; letter-spacing:.02em;
  }
  .chip b { color:var(--accent); font-weight:500; }

  main {
    display:grid; gap:30px 26px; padding:40px 0 16px;
    grid-template-columns:repeat(auto-fill,minmax(220px,1fr));
  }
  figure { margin:0; display:flex; flex-direction:column; align-items:center; }

  /* The watch body: a bezel around the glass, with the two real side buttons
     from the hardware -- BOOT (top-right) and PWR (lower-right). */
  .device {
    position:relative; padding:13px; border-radius:34px;
    background:linear-gradient(150deg,#2b303a 0%,#141619 55%,#0d0f12 100%);
    box-shadow:inset 0 1px 0 rgba(255,255,255,.10),
               inset 0 0 0 1px rgba(0,0,0,.6), 0 16px 34px rgba(0,0,0,.55);
  }
  .device::before, .device::after {
    content:""; position:absolute; right:-3px; width:4px; border-radius:2px;
    background:linear-gradient(90deg,#3c414c,#22262e);
  }
  .device::before { top:64px; height:30px; }             /* BOOT */
  .device::after  { bottom:78px; height:44px; background:linear-gradient(90deg,#4a3033,#241315); } /* PWR */
  .glass {
    border-radius:23px; overflow:hidden; background:#000;
    box-shadow:inset 0 0 0 1px rgba(255,255,255,.05), 0 0 40px rgba(47,128,237,.06);
    line-height:0;
  }
  .glass img { display:block; width:200px; height:auto; }

  figcaption { text-align:center; margin-top:16px; max-width:220px; }
  .name {
    display:block; font-family:var(--disp); font-weight:600; font-size:17px;
    text-transform:capitalize; letter-spacing:.01em;
  }
  .desc { display:block; color:var(--mut); font-size:13.5px; margin-top:4px; line-height:1.45; }

  .legend {
    display:flex; gap:22px; justify-content:center; flex-wrap:wrap;
    color:var(--faint); font-family:var(--mono); font-size:12px;
    padding:6px 0 4px; letter-spacing:.04em;
  }
  .legend span b { color:var(--mut); font-weight:500; }
  footer {
    text-align:center; color:var(--faint); padding:26px 24px 60px; font-size:13px;
    font-family:var(--mono); letter-spacing:.04em;
  }
  footer a { color:var(--mut); }
</style>
<div class="wrap">
  <header>
    <p class="eyebrow">Waveshare ESP32-S3 &middot; Touch AMOLED 1.8</p>
    <h1>Apps on Glass</h1>
    <p>__N__ launcher apps for a wrist-sized AMOLED &mdash; every frame here was
       rendered by the headless simulator, running the real Lua&#8202;&#8594;&#8202;LVGL
       firmware bindings. No board attached.</p>
    <div class="chips">
      <span class="chip"><b>368&#8202;&times;&#8202;448</b> panel</span>
      <span class="chip">RGB565</span>
      <span class="chip">LVGL 9.5</span>
      <span class="chip">Lua 5.5</span>
      <span class="chip"><b>headless</b> render</span>
    </div>
  </header>
  <p class="legend">
    <span><b>&#9679;</b> BOOT &mdash; top-right, always Home</span>
    <span><b>&#9679;</b> PWR &mdash; lower-right, app button</span>
  </p>
  <main>
__CARDS__
  </main>
  <footer>Rendered by <a href="#">sim/gallery.py</a> &mdash; build once, screenshot every app, no hardware.</footer>
</div>
"""


if __name__ == "__main__":
    main()
