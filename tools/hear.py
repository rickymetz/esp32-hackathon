#!/usr/bin/env python3
"""Hear the board: verify it actually makes sound, without ears.

Records from the Mac microphone and reports the energy at given
frequencies, so an agent can close the loop on audio the same way
tools/screenshot.py closes it on the display.

    tools/hear.py 880              # record 3s, look for 880Hz
    tools/hear.py 880 1568         # several at once
    tools/hear.py --file x.wav 880 # analyse an existing recording

A ratio near 1 means "not present"; a tone reads tens to hundreds.
Goertzel rather than an FFT: we only care about known frequencies, and
it needs no numpy. Requires microphone permission for your terminal
(System Settings -> Privacy & Security -> Microphone) -- without it
ffmpeg hangs rather than failing.

Note it records the ROOM, not just the board.
"""
import sys, wave, math, subprocess, tempfile, os

def read_wav(path):
    with wave.open(path, 'rb') as w:
        assert w.getsampwidth() == 2 and w.getnchannels() == 1
        rate = w.getframerate()
        raw = w.readframes(w.getnframes())
    n = len(raw) // 2
    s = [int.from_bytes(raw[2*i:2*i+2], 'little', signed=True) for i in range(n)]
    return rate, s

def goertzel(samples, rate, freq):
    n = len(samples)
    k = int(0.5 + (n * freq) / rate)
    w = (2.0 * math.pi * k) / n
    cosine, coeff = math.cos(w), 2.0 * math.cos(w)
    s1 = s2 = 0.0
    for x in samples:
        s0 = x + coeff * s1 - s2
        s2, s1 = s1, s0
    power = s1*s1 + s2*s2 - coeff*s1*s2
    return math.sqrt(max(power, 0)) / n

def rms(samples):
    if not samples: return 0.0
    return math.sqrt(sum(x*x for x in samples) / len(samples))

def record(seconds, path):
    subprocess.run(['ffmpeg', '-y', '-f', 'avfoundation', '-i', ':0',
                    '-t', str(seconds), '-ar', '16000', '-ac', '1',
                    path, '-loglevel', 'error'], check=True, timeout=seconds + 20)


if __name__ == '__main__':
    args = sys.argv[1:]
    path = None
    if args and args[0] == '--file':
        path, args = args[1], args[2:]
    targets = [float(a) for a in args] or [880.0]
    if path is None:
        path = os.path.join(tempfile.gettempdir(), 'hear.wav')
        print("recording 3s...")
        record(3, path)
    rate, s = read_wav(path)
    print(f"{path}: {len(s)/rate:.2f}s @ {rate}Hz  rms={rms(s):.1f}")
    for f in targets:
        mag = goertzel(s, rate, f)
        # neighbours as a noise floor: broadband sound lifts all three,
        # a tone lifts only the target
        lo = goertzel(s, rate, f * 0.75)
        hi = goertzel(s, rate, f * 1.33)
        floor = (lo + hi) / 2 or 1e-9
        print(f"  {f:7.1f}Hz  mag={mag:9.2f}  floor={floor:8.2f}  ratio={mag/floor:7.2f}x")
