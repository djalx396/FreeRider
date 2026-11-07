#!/usr/bin/env python3
"""Generate the Rider capture kit: a synthetic vocal and a music sidechain.

Every section is placed at a known time, so a render of any gain rider can be
turned into its exact gain curve (output / input) and compared section by section.

    python3 gen_signals.py OUTDIR [--sr 48000]

Writes OUTDIR/rider_vocal.wav (mono), OUTDIR/rider_music.wav (stereo),
and OUTDIR/rider_sections.json (section timing / levels).
"""
import json, sys, argparse
import numpy as np
from scipy.signal import lfilter, butter
from scipy.io import wavfile
try:
    import soundfile as sf
except ImportError:
    sf = None

ap = argparse.ArgumentParser()
ap.add_argument("outdir")
ap.add_argument("--sr", type=int, default=48000)
args = ap.parse_args()
SR = args.sr
rng = np.random.default_rng(1234)

VOWELS = {  # formant centre frequencies (Hz) and relative gains
    "a": [(800, 1.0), (1200, 0.5), (2600, 0.25)],
    "e": [(500, 1.0), (1900, 0.45), (2600, 0.3)],
    "i": [(300, 1.0), (2300, 0.35), (3000, 0.3)],
    "o": [(500, 1.0), (900, 0.5), (2500, 0.15)],
    "u": [(330, 1.0), (800, 0.35), (2300, 0.1)],
}

def resonator(x, f, bw):
    r = np.exp(-np.pi * bw / SR)
    th = 2 * np.pi * f / SR
    return lfilter([1 - r], [1, -2 * r * np.cos(th), r * r], x)

def level_db(x):
    """RMS level +3.01 dB (a sine reads its peak) — the same convention the engine uses."""
    return 10 * np.log10(np.mean(x ** 2) + 1e-30) + 3.0103

def set_level(x, db):
    return x * 10 ** ((db - level_db(x)) / 20)

def syllable(dur, db, f0=None, vowel=None, att=0.015, rel=0.05):
    n = int(dur * SR)
    t = np.arange(n) / SR
    f0 = f0 or rng.uniform(170, 260)
    vib = 1 + 0.012 * np.sin(2 * np.pi * 5.2 * t + rng.uniform(0, 6.28))
    ph = 2 * np.pi * np.cumsum(f0 * vib) / SR
    phs = rng.uniform(0, 2 * np.pi, 40)  # random harmonic phases keep the crest factor vocal-like
    src = sum(np.sin(k * ph + phs[k]) / k for k in range(1, 40) if k * f0 < SR / 2.2)
    v = VOWELS[vowel or rng.choice(list(VOWELS))]
    y = sum(g * resonator(src, f, 80 + f * 0.06) for f, g in v)
    y += 0.003 * rng.standard_normal(n)  # a little breath
    env = np.ones(n)
    a, r = int(att * SR), int(rel * SR)
    env[:a] = np.linspace(0, 1, a) ** 2
    env[-r:] *= np.linspace(1, 0, r) ** 1.5
    return set_level(y * env, db)

def pink(n, db):
    w = rng.standard_normal(n)
    b = [0.049922035, -0.095993537, 0.050612699, -0.004408786]
    a = [1, -2.494956002, 2.017265875, -0.522189400]
    return set_level(lfilter(b, a, w), db)

def tone(dur, f, db):
    t = np.arange(int(dur * SR)) / SR
    y = np.sin(2 * np.pi * f * t)
    r = int(0.005 * SR)
    y[:r] *= np.linspace(0, 1, r); y[-r:] *= np.linspace(1, 0, r)
    return y * 10 ** (db / 20)

def music_bed(dur, db):
    """Pink noise + kick/snare pulses at 120 BPM, stereo, set to `db`."""
    n = int(dur * SR)
    t = np.arange(n) / SR
    beat = (t % 0.5)
    kick = np.sin(2 * np.pi * (50 + 80 * np.exp(-beat * 30)) * beat) * np.exp(-beat * 12)
    nl, nr = pink(n, -20), pink(n, -20)
    hp = butter(2, 2000 / (SR / 2), "high")
    sn = lfilter(*hp, rng.standard_normal(n)) * np.exp(-((t - 0.25) % 0.5) * 25) * (((t + 0.25) % 1.0) < 0.5)
    l = 0.6 * nl + kick + 0.3 * sn
    r = 0.6 * nr + kick + 0.3 * sn
    st = np.stack([l, r])
    g = 10 ** ((db - level_db(st.mean(0))) / 20)
    return st * g

voc, mus, sections = [], [], []
cursor = [0.0]

def add(name, v, m=None, **info):
    n = len(v)
    if m is None: m = np.zeros((2, n))
    sections.append(dict(name=name, start=round(cursor[0], 6), dur=round(n / SR, 6), **info))
    voc.append(v); mus.append(m)
    cursor[0] += n / SR

def silence(d, name="silence"):
    add(name, np.zeros(int(d * SR)))

def phrase(level, n_syl=8, spread=0.0, gap=(0.04, 0.12)):
    parts = []
    for k in range(n_syl):
        parts.append(syllable(rng.uniform(0.18, 0.42), level + rng.uniform(-spread, spread)))
        parts.append(np.zeros(int(rng.uniform(*gap) * SR)))
    return np.concatenate(parts)

silence(1.0, "lead-in")

# A. Static curve: phrases at fixed levels → fader position vs input level
for L in [-42, -36, -30, -27, -24, -21, -18, -15, -12, -9]:
    add(f"A_static_{L}", phrase(L), level=L)
    silence(1.5)

# B. Step response: a held vowel that jumps ±6 and ±12 dB (attack / release speed)
for jump in [+6, -6, +12, -12]:
    base = -21 - jump / 2
    s1 = syllable(2.0, base, f0=220, vowel="a", rel=0.002)
    s2 = syllable(2.0, base + jump, f0=220, vowel="a", att=0.002)
    add(f"B_step_{jump:+d}", np.concatenate([s1, s2]), jump=jump, step_at=2.0)
    silence(1.5)

# C. Gaps: two words separated by growing gaps (hold time, idle return)
for g in [0.05, 0.1, 0.2, 0.4, 0.8, 1.6, 3.2]:
    w1 = syllable(1.0, -12, f0=200, vowel="o")   # loud word pulls the fader down
    w2 = syllable(1.0, -30, f0=200, vowel="e")   # quiet word pushes it up
    add(f"C_gap_{int(g*1000)}ms", np.concatenate([w1, np.zeros(int(g * SR)), w2]), gap=g)
    silence(1.5)

# D. Noise floor / spill: noise alone, then words over it (Vocal Sensitivity)
for nf in [-66, -54, -48, -42, -36]:
    bed = pink(int(5.0 * SR), nf)
    words = np.zeros_like(bed)
    p = phrase(-21, n_syl=6)
    words[int(2.0 * SR):int(2.0 * SR) + len(p)] = p[: len(bed) - int(2.0 * SR)]
    add(f"D_noise_{nf}", bed + words, noise=nf, words_at=2.0)
    silence(1.5)

# E. Weighting: 1 s tones at -21 dBFS at several frequencies (detector EQ)
for f in [60, 100, 200, 500, 1000, 2000, 4000, 8000]:
    add(f"E_tone_{f}Hz", tone(1.0, f, -21), freq=f)
    silence(1.5)

# F. A natural performance: 30 s of phrases with ±9 dB level wander
parts = []
for k in range(8):
    parts.append(phrase(-21 + rng.uniform(-9, 9), n_syl=int(rng.integers(5, 10)), spread=4))
    parts.append(np.zeros(int(rng.uniform(0.3, 1.2) * SR)))
add("F_performance", np.concatenate(parts))
silence(1.5)

# G. Music sidechain: steady vocal at -21, music bed stepping in level
for md in [-60, -36, -30, -24, -18, -15, -12]:
    v = phrase(-21, n_syl=14, gap=(0.04, 0.08))
    v = v[: int(5.0 * SR)] if len(v) >= int(5.0 * SR) else np.pad(v, (0, int(5.0 * SR) - len(v)))
    add(f"G_music_{md}", v, music_bed(5.0, md), music=md)
# music swell under a steady vocal: -30 → -12 → -30 over 16 s
n = int(16 * SR)
env_db = -30 + 18 * np.sin(np.linspace(0, np.pi, n))
bed = music_bed(16, -20) * 10 ** ((env_db + 20) / 20)
v = np.concatenate([phrase(-21, n_syl=10, gap=(0.04, 0.08)) for _ in range(4)])
v = v[:n] if len(v) >= n else np.pad(v, (0, n - len(v)))
add("G_music_swell", v, bed)
silence(2.0, "tail")

V = np.concatenate(voc).astype(np.float32)
M = np.concatenate(mus, axis=1).T.astype(np.float32)
peak = max(np.abs(V).max(), np.abs(M).max())
if peak >= 1.0:
    t0 = 0
    for sec, v, m in zip(sections, voc, mus):
        pk = max(np.abs(v).max(initial=0), np.abs(m).max(initial=0))
        if pk >= 1.0: print("clips:", sec["name"], 20 * np.log10(pk))
    sys.exit(1)
import os
os.makedirs(args.outdir, exist_ok=True)
if sf is not None:   # 24-bit keeps the kit small; the analysis uses this exact file as reference
    sf.write(os.path.join(args.outdir, "rider_vocal.wav"), V, SR, subtype="PCM_24")
    sf.write(os.path.join(args.outdir, "rider_music.wav"), M, SR, subtype="PCM_24")
else:
    wavfile.write(os.path.join(args.outdir, "rider_vocal.wav"), SR, V)
    wavfile.write(os.path.join(args.outdir, "rider_music.wav"), SR, M)
json.dump(dict(sr=SR, length_s=len(V) / SR, sections=sections),
          open(os.path.join(args.outdir, "rider_sections.json"), "w"), indent=1)
print(f"{len(V)/SR:.1f} s, {len(sections)} sections, peak {20*np.log10(peak):.1f} dBFS")
