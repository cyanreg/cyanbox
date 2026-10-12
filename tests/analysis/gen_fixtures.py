# Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md.

"""Reference fixtures for tests/analysis/test_waveform.c and test_key.c, from dj.

Runs under dj's virtualenv, with dj importable:

    PYTHONPATH=~/projects/dj/src ~/projects/dj/.venv/bin/python tests/analysis/gen_fixtures.py

writes, into tests/data/analysis/, a few deterministic synthetic signals (closed
form, no random numbers) as raw little-endian float32 mono (`<name>.f32`) and
the waveform tags for each of them (`<name>.tags`): PWAV, PWV2, PWV3, PWV4,
PWV5 and PWVC exactly as dj.analysis.waveform.compute produced them (without
rxbox's firmware minimums; the C test applies those to this side before
comparing), PWV6 and PWV7 by rxbox's per-band recalibration of dj's rule
(three_band() below; dj's mid and high levels are several times too hot),
and keys.txt, the
expectations of test_key.c: the true key of each tonal() signal, with dj's
answer for information (rxbox fixes dj's tuning estimate, so they differ).

    gen_fixtures.py --real OUTDIR AUDIO...

does the same for real files (not committed: too large): each file is decoded
with the ffmpeg CLI to f32le mono at its native rate, and next to the .f32 and
.tags it writes `<name>.key`, dj's key analysis of the same samples on dj's own
path (resample_poly to 44.1 kHz, then analyse_key), as "rate name confidence
tuning", for an informational comparison (rxbox deliberately differs from dj
there, see src/analysis/key.c). The C side reads the same .f32, so both see
identical input.

The .tags layout, all integers little-endian:

    0   4  "DJWV"
    4   4  sample rate
    8   4  number of samples
    12  4  n, the number of detail columns
    16  6  PWVC gains, 3 x u16
    22  2  zero
    24     PWAV (400), PWV2 (100), PWV4 (1200 x 6), PWV6 (1200 x 3),
           PWV3 (n), PWV5 (2n, big-endian u16 as in the ANLZ file), PWV7 (3n)
"""

import math
import struct
import subprocess
import sys
from pathlib import Path

import numpy as np

from dj.analysis import waveform

DATA = Path(__file__).resolve().parent.parent / "data" / "analysis"


# ------------------------------------------------------------------- signals

def _kick(t):
    """dj's synthetic kick (tests/synth.py) in closed form: a 170 -> 50 Hz
    pitch drop, the phase integrated analytically, decaying over 0.25 s."""
    phase = 2 * np.pi * (50 * t + 120 * (1 - np.exp(-40 * t)) / 40)
    return np.where((t >= 0) & (t < 0.25), np.sin(phase) * np.exp(-12 * t), 0.0)


def _hat(t):
    """A metallic off-beat tick: two high partials ring-modulated, 40 ms."""
    return np.where((t >= 0) & (t < 0.04),
                    0.15 * np.sin(2 * np.pi * 7000 * t) * np.sin(2 * np.pi * 9100 * t)
                    * np.exp(-120 * t), 0.0)


def _music(t, bpm=128.0):
    """Bass, an A minor triad with a slow swell, a 5 kHz shimmer, kicks on
    every beat and hats on the off-beats."""
    period = 60.0 / bpm
    x = 0.30 * np.sin(2 * np.pi * 55.0 * t)
    for f in (220.0, 261.63, 329.63):
        x += 0.10 * np.sin(2 * np.pi * f * t) * (0.6 + 0.4 * np.sin(2 * np.pi * 0.25 * t))
    x += 0.05 * np.sin(2 * np.pi * 5000.0 * t) * (0.5 + 0.5 * np.cos(2 * np.pi * 2.0 * t))
    beat = np.mod(t, period)
    x += 0.8 * _kick(beat)
    x += _hat(np.mod(t - period / 2, period))
    return 0.6 * x


def mix44k():
    """20 s at 44.1 kHz: 1 s of digital silence, 8 s of music at 128 BPM, a
    4 s logarithmic sweep 30 Hz -> 15 kHz, 5 s of music fading out, 1 s of a
    -80 dB tone, then silence; 123 samples past a whole column."""
    rate = 44100
    n = 20 * rate + 123
    t = np.arange(n) / rate
    x = np.zeros(n)
    m = (t >= 1) & (t < 9)
    x[m] = _music(t[m] - 1)
    m = (t >= 9) & (t < 13)
    ts = t[m] - 9
    k = math.log(15000 / 30) / 4
    x[m] = 0.5 * np.sin(2 * np.pi * 30 * (np.exp(k * ts) - 1) / k)
    m = (t >= 13) & (t < 18)
    x[m] = _music(t[m] - 13) * (18 - t[m]) / 5
    m = (t >= 18) & (t < 19)
    x[m] = 1e-4 * np.sin(2 * np.pi * 1000 * t[m])
    return rate, x


def cols48k():
    """dj's test_waveform_columns signal: 2 s of 60 Hz then 2 s of 5 kHz at 48 kHz."""
    rate = 48000
    t = np.arange(rate * 2) / rate
    return rate, np.concatenate([0.9 * np.sin(2 * np.pi * 60 * t), 0.9 * np.sin(2 * np.pi * 5000 * t)])


def odd11k():
    """11025 Hz, so columns are 73.5 samples and alternate 73/74 long, and
    the length is chosen so the last column is empty (601 * 73.5 rounds to
    44174): 602 columns, between the 400 and 1200 preview columns. A decaying
    chord on beats at 128 BPM over a 40 Hz drone."""
    rate = 11025
    n = 44174
    t = np.arange(n) / rate
    beat = np.mod(t, 60 / 128)
    x = 0.2 * np.sin(2 * np.pi * 40 * t)
    for f in (392.0, 493.88, 587.33, 2793.8):
        x += 0.15 * np.sin(2 * np.pi * f * t) * np.exp(-6 * beat)
    return rate, x


def short44k():
    """0.5 s, fewer columns (75) than every preview has: each preview column
    repeats a detail column."""
    rate = 44100
    t = np.arange(rate // 2) / rate
    return rate, 0.7 * np.sin(2 * np.pi * 110 * t) * np.exp(-4 * t) + 0.2 * np.sin(2 * np.pi * 3520 * t) * t


def silence44k():
    """A quarter second of digital silence: every column empty of signal."""
    return 44100, np.zeros(44100 // 4)


def tonal(rate, tonic, minor, cents=0.0, bpm=124.0, bars=16):
    """A chord progression in a known key, after dj's tests/synth.py without
    the (random) hats: i - iv - V - i (minor) or I - V - vi - IV (major),
    each chord tone a sine plus 0.3 of its octave, a bass on the chord root
    from bar 4, and the kick on every beat; every pitch detuned by `cents`.
    test_key.c computes the same closed form."""
    period = 60.0 / bpm
    bar = 4 * period
    n = int(round(bars * bar * rate))
    t = np.arange(n) / rate
    b = np.floor(t / bar).astype(np.int64)
    u = t / bar - b
    if minor:
        prog = [(0, 3, 7), (5, 8, 12), (7, 11, 14), (0, 3, 7)]
    else:
        prog = [(0, 4, 7), (7, 11, 14), (9, 12, 16), (5, 9, 12)]
    detune = 2.0 ** (cents / 1200)
    x = np.zeros(n)
    for k in range(4):
        m = (b % 4) == k
        swell = np.sin(np.pi * u[m]) ** 0.1
        for i in prog[k]:
            f = 440.0 * 2.0 ** ((48 + tonic + i - 69) / 12) * detune
            x[m] += 0.25 * (np.sin(2 * np.pi * f * t[m]) + 0.3 * np.sin(4 * np.pi * f * t[m])) / 3 * swell
        f = 440.0 * 2.0 ** ((36 + tonic + prog[k][0] - 69) / 12) * detune
        mb = m & (b >= 4)
        x[mb] += 0.3 * (np.sin(2 * np.pi * f * t[mb]) + 0.3 * np.sin(4 * np.pi * f * t[mb]))
    x += 0.8 * _kick(np.mod(t, period))
    return x.astype(np.float32)


# The expectations of test_key.c, written to keys.txt: name, rate, tonic,
# minor, detune in cents, and the key rxbox is known to give instead of the
# true one (None: it must find the true key).
#
# The two known misses are major I - V - vi - IV progressions: the true key,
# its IV and its relative minor correlate within ~0.02 with the Temperley
# profiles, and the kick tips the balance. Its 170 -> 50 Hz sweep sounds on
# every beat for 0.25 s of 0.48 s, longer than half of the 9-frame median, so
# percussion removal keeps it, and it adds a chroma tilt peaking at C/Db/D
# (C2..D2, where the sweep spends most of its energy). G major reads as C
# (C 0.757, G 0.737) and E major as its relative Dbm (0.733 vs 0.647).
# Without the kick both come out right (checked with dj's corrected code).
#
# dj itself, with its tuning bug (see src/analysis/key.c), names only 5 of
# these 9 correctly: bb44k as Eb, g96k as C, e22k as Dbm and am44k+30 as F.
# Its answers are kept in keys.txt for information only.
KEYS = [
    ("am44k", 44100, 9, True, 0.0, None),
    ("d44k", 44100, 2, False, 0.0, None),
    ("cm44k", 44100, 0, True, 0.0, None),
    ("dbm44k", 44100, 1, True, 0.0, None),
    ("bb44k", 44100, 10, False, 0.0, None),
    ("fsm48k", 48000, 6, True, 0.0, None),
    ("g96k", 96000, 7, False, 0.0, "C"),
    ("e22k", 22050, 4, False, -20.0, "Dbm"),
    ("am44k+30", 44100, 9, True, 30.0, None),
]


def keys():
    """The expectations for test_key.c, plus dj's key analysis of the same
    tonal signals on dj's path (resample_poly to 44.1 kHz, then analyse_key)
    for information."""
    from math import gcd

    from scipy.signal import resample_poly

    from dj.analysis.key import NAMES, MINOR_NAMES, analyse_key

    lines = ["# name rate tonic minor cents true known-miss | dj (information only): key confidence tuning(semitones)"]
    for name, rate, tonic, minor, cents, known in KEYS:
        x = tonal(rate, tonic, minor, cents)
        if rate != 44100:
            g = gcd(44100, rate)
            x = resample_poly(x, 44100 // g, rate // g).astype(np.float32)
        k = analyse_key(x, 44100)
        truth = MINOR_NAMES[tonic] if minor else NAMES[tonic]
        lines.append(f"{name} {rate} {tonic} {int(minor)} {cents:g} {truth} {known or '-'} "
                     f"{k.key.name} {k.confidence:.6f} {k.tuning:.6f}")
        print(lines[-1])
    (DATA / "keys.txt").write_text("\n".join(lines) + "\n")


SIGNALS = {
    "mix44k": mix44k,
    "cols48k": cols48k,
    "odd11k": odd11k,
    "short44k": short44k,
    "silence44k": silence44k,
}


# ------------------------------------------------------------------- output

def three_band(x: np.ndarray, rate: int, gains: np.ndarray):
    """rxbox's PWV7 and PWV6, which replace dj's (src/analysis/waveform.c has
    the measurements against rekordbox's own bytes): the same band envelopes
    and gains as dj, x = min(1, u / (GAIN_TARGET * BAND_SCALE)) of dj's
    level u, PWV7 = 127 * x^1.5 (low, mid) and 127 * x^2.5 (high), PWV6 =
    half the range mean of that (low, mid) and 0.5 * sqrt(127 * range mean)
    (high). Mirrors the C step for step so the bytes agree exactly."""
    from scipy.signal import lfilter

    W = waveform
    x = x.astype(np.float64)
    edges = W.column_edges(len(x), rate)
    n = len(edges) - 1
    starts = edges[:-1]
    full = edges[1:] > starts
    fs = starts[full]

    def colmax(v):
        out = np.zeros(n)
        if len(fs):
            out[full] = np.maximum.reduceat(np.abs(v), fs)
        return out

    low = colmax(lfilter(*W._rbj("lp", W.LOW_HZ, rate), x))
    mid = colmax(lfilter(*W._rbj("lp", W.MID_HIGH_HZ, rate), lfilter(*W._rbj("hp", W.MID_LOW_HZ, rate), x)))
    high = colmax(lfilter(*W._rbj("hp", W.HIGH_HZ, rate), x))
    band = np.stack([low, mid, high], axis=1)
    env = np.empty_like(band)
    for j in range(3):
        r, prev, col, out = W.RELEASE[j], 0.0, band[:, j].tolist(), env[:, j]
        for i, v in enumerate(col):
            prev = v if v > prev * r else prev * r
            out[i] = prev
    u = np.minimum(127, W.BAND_SCALE * gains * env)
    full = W.GAIN_TARGET * W.BAND_SCALE
    x = np.minimum(1.0, u / full)
    sx = np.sqrt(x)
    v = np.empty_like(x)
    v[:, :2] = x[:, :2] * sx[:, :2]
    v[:, 2] = x[:, 2] * x[:, 2] * sx[:, 2]
    pwv7 = np.floor(127 * v + 0.5).astype(np.uint8)
    mean = W._range_mean(pwv7.astype(np.float64), *W._split(n, 1200))
    p6 = np.empty_like(mean)
    p6[:, :2] = 0.5 * mean[:, :2]
    p6[:, 2] = 0.5 * np.sqrt(127 * mean[:, 2])
    pwv6 = np.minimum(127, np.floor(p6 + 0.5))
    return pwv7, pwv6.astype(np.uint8)


def tags_bytes(x: np.ndarray, rate: int) -> bytes:
    wf = waveform.compute(x, rate)
    wf.pwv7, wf.pwv6 = three_band(x, rate, wf.gains)
    n = len(wf.pwv3)
    head = b"DJWV" + struct.pack("<IIIHHHH", rate, len(x), n, *(int(g) for g in wf.gains), 0)
    body = [
        wf.pwav.astype(np.uint8).tobytes(),
        wf.pwv2.astype(np.uint8).tobytes(),
        wf.pwv4.astype(np.uint8).tobytes(),
        wf.pwv6.astype(np.uint8).tobytes(),
        wf.pwv3.astype(np.uint8).tobytes(),
        wf.pwv5.astype(">u2").tobytes(),
        wf.pwv7.astype(np.uint8).tobytes(),
    ]
    return head + b"".join(body)


def write_signal(out: Path, name: str, rate: int, x: np.ndarray):
    x = x.astype(np.float32)
    (out / f"{name}.f32").write_bytes(x.astype("<f4").tobytes())
    (out / f"{name}.tags").write_bytes(tags_bytes(x, rate))
    print(f"{name}: {rate} Hz, {len(x)} samples, {len(x) * 4} bytes")
    return x


def synthetic():
    DATA.mkdir(parents=True, exist_ok=True)
    for name, gen in SIGNALS.items():
        rate, x = gen()
        write_signal(DATA, name, rate, x)
    keys()


def real(out: Path, paths: list[str]):
    from math import gcd

    from scipy.signal import resample_poly

    from dj.analysis.key import analyse_key

    out.mkdir(parents=True, exist_ok=True)
    for i, p in enumerate(paths):
        probe = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "a:0",
                                "-show_entries", "stream=sample_rate", "-of", "default=nw=1:nk=1",
                                f"file:{p}"], capture_output=True, check=True)
        rate = int(probe.stdout.split()[0])
        raw = subprocess.run(["ffmpeg", "-v", "error", "-nostdin", "-i", f"file:{p}",
                              "-map", "0:a:0", "-ac", "1", "-f", "f32le", "-"],
                             capture_output=True, check=True).stdout
        x = np.frombuffer(raw, dtype=np.float32)
        name = f"real{i}"
        write_signal(out, name, rate, x)
        # dj's own analysis path (export.prepare): native -> 44.1 kHz with
        # resample_poly in float32, then analyse_key
        y = x
        if rate != 44100:
            g = gcd(44100, rate)
            y = resample_poly(x, 44100 // g, rate // g).astype(np.float32)
        k = analyse_key(y, 44100)
        (out / f"{name}.key").write_text(f"{rate} {k.key.name} {k.confidence:.6f} {k.tuning:.6f}\n")
        print(f"{name}: {p}: key {k.key.name} confidence {k.confidence:.4f} tuning {k.tuning:+.4f}")


if __name__ == "__main__":
    if len(sys.argv) > 2 and sys.argv[1] == "--real":
        real(Path(sys.argv[2]), sys.argv[3:])
    else:
        synthetic()
