#!/usr/bin/env python3
"""Recover a gain rider's fader curve from a render: gain = output / input.

    python3 extract_gain.py rider_vocal.wav render.wav out.npz [--offset auto|N]

Works for any plugin that only changes level (Vocal Rider, Rider, ...).
Gain is measured in 1 ms windows as <out·in>/<in·in> wherever the input has
signal; silent windows are NaN. If the render is shifted (DAW latency, a
render that started early) the offset is found by cross-correlation.
"""
import argparse, numpy as np
from scipy.io import wavfile
from scipy.signal import fftconvolve

ap = argparse.ArgumentParser()
ap.add_argument("vocal"); ap.add_argument("render"); ap.add_argument("out")
ap.add_argument("--offset", default="auto")
a = ap.parse_args()

def mono(path):
    sr, raw = wavfile.read(path)
    x = raw.astype(np.float64)
    if raw.dtype.kind == "i":                     # integer PCM → ±1.0
        x /= float(np.iinfo(raw.dtype).max) + 1.0
    elif raw.dtype.kind == "u":                   # 8-bit unsigned
        x = (x - 128.0) / 128.0
    return sr, (x.mean(axis=1) if x.ndim > 1 else x)

sr, vin = mono(a.vocal)
sr2, vout = mono(a.render)
assert sr == sr2, f"sample rates differ: {sr} vs {sr2}"

if a.offset == "auto":
    seg = slice(int(1.0 * sr), int(30 * sr))          # use the first ~30 s
    c = fftconvolve(vout[seg], vin[seg][::-1], mode="full")
    lag = int(np.argmax(np.abs(c))) - (len(vin[seg]) - 1)
else:
    lag = int(a.offset)
if lag > 0: vout = vout[lag:]
elif lag < 0: vout = np.concatenate([np.zeros(-lag), vout])
n = min(len(vin), len(vout)); vin, vout = vin[:n], vout[:n]

w = int(sr / 1000)
m = n // w
xi = vin[: m * w].reshape(m, w); yo = vout[: m * w].reshape(m, w)
num = (xi * yo).sum(1); den = (xi * xi).sum(1)
ok = den > (10 ** (-80 / 10)) * w
g = np.full(m, np.nan)
g[ok] = num[ok] / den[ok]
gdb = np.full(m, np.nan)
pos = ok & (g > 0)
gdb[pos] = 20 * np.log10(g[pos])
t = (np.arange(m) + 1) * w / sr
np.savez(a.out, t=t, gain_db=gdb, lag=lag, sr=sr)
print(f"lag {lag} samples, {np.isfinite(gdb).mean()*100:.0f}% of windows measurable, "
      f"gain {np.nanmin(gdb):+.2f} .. {np.nanmax(gdb):+.2f} dB")
