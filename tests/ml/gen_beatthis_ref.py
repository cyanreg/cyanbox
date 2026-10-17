#!/usr/bin/env python3
# Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md.

"""Writes tests/ml/beatthis_ref.h: Beat This!'s logits, from its reference code
(beat_this, PyTorch), for the audio test_beatthis.c makes, which this makes the
same way (the same LCG, the same double-precision sin and exp, then float).

usage: gen_beatthis_ref.py GGUF OUT.h

Needs torch and beat_this (github.com/CPJKU/beat_this); the weights come from
the GGUF file, its batch norms folded back into identity ones, as cyanbox
reads them.
"""
import math
import struct
import sys

import numpy as np
import torch
from beat_this.inference import Audio2Frames
from beat_this.model.beat_tracker import BeatThis

RATE = 22050
SECONDS = 40
STEP = 2  # every second frame goes in the header


def make_audio(n):
    """test_beatthis.c's make_audio, sample for sample."""
    out = np.empty(n, dtype=np.float32)
    r = 12345
    beat = 60.0 / 128.0
    for i in range(n):
        t = i / RATE
        tb = math.fmod(t, beat)
        b = int(t / beat)
        r = (r * 1664525 + 1013904223) & 0xFFFFFFFF
        noise = (r >> 8) / (1 << 24) * 2.0 - 1.0
        accent = 1.0 if b % 4 == 0 else 0.6
        kick = (math.sin(2 * math.pi * (50.0 + 80.0 * math.exp(-tb * 30.0)) * tb) *
                math.exp(-tb * 8.0))
        snare = noise * math.exp(-tb * 20.0) * 0.4 if b % 2 == 1 else 0.0
        toff = math.fmod(t + beat / 2, beat)
        hat = noise * math.exp(-toff * 60.0) * 0.2
        bass = math.sin(2 * math.pi * (55.0 if (b // 4) % 2 == 0 else 65.41) * t) * 0.25
        out[i] = 0.5 * accent * kick + snare + hat + bass
    return out


def read_gguf(path):
    f = open(path, "rb")

    def rd(fmt):
        return struct.unpack("<" + fmt, f.read(struct.calcsize("<" + fmt)))
    assert f.read(4) == b"GGUF"
    rd("I")
    nt, nk = rd("QQ")

    def s():
        n, = rd("Q")
        return f.read(n).decode()
    T = {0: "B", 1: "b", 2: "H", 3: "h", 4: "I", 5: "i", 6: "f", 7: "?", 10: "Q", 11: "q", 12: "d"}

    def val(t):
        if t == 8:
            return s()
        if t == 9:
            et, n = rd("I")[0], rd("Q")[0]
            return [val(et) for _ in range(n)]
        return rd(T[t])[0]
    meta = {}
    for _ in range(nk):
        k = s()
        meta[k] = val(rd("I")[0])
    infos = []
    for _ in range(nt):
        name = s()
        nd, = rd("I")
        dims = rd("Q" * nd)
        ty, = rd("I")
        off, = rd("Q")
        infos.append((name, dims, ty, off))
    align = meta.get("general.alignment", 32)
    base = (f.tell() + align - 1) // align * align
    w = {}
    for name, dims, ty, off in infos:
        dt = {0: np.float32, 1: np.float16}[ty]
        f.seek(base + off)
        a = np.frombuffer(f.read(int(np.prod(dims)) * np.dtype(dt).itemsize), dtype=dt)
        w[name] = a.astype(np.float32).reshape(tuple(reversed(dims)))
    return w


def model_from_gguf(path):
    w = read_gguf(path)
    m = BeatThis().eval()
    sd = m.state_dict()
    for k, a in w.items():
        if k in sd:
            sd[k] = torch.from_numpy(a.copy())
    eps = 1e-5
    sd["frontend.stem.bn1d.weight"] = torch.from_numpy(w["frontend.stem.bn1d.scale"].copy())
    sd["frontend.stem.bn1d.bias"] = torch.from_numpy(w["frontend.stem.bn1d.offset"].copy())
    sd["frontend.stem.bn1d.running_mean"].zero_()
    sd["frontend.stem.bn1d.running_var"].fill_(1 - eps)
    for bn in ["frontend.stem.bn2d"] + [f"frontend.blocks.{i}.norm" for i in range(3)]:
        sd[bn + ".weight"].fill_(1)
        sd[bn + ".bias"].zero_()
        sd[bn + ".running_mean"].zero_()
        sd[bn + ".running_var"].fill_(1 - eps)
    m.load_state_dict(sd)
    for conv in ["frontend.stem.conv2d"] + [f"frontend.blocks.{i}.conv2d" for i in range(3)]:
        m.get_submodule(conv).bias = torch.nn.Parameter(torch.from_numpy(w[conv + ".bias"].copy()))
    return m


def main():
    gguf, out = sys.argv[1], sys.argv[2]
    a2f = Audio2Frames(checkpoint_path=None)
    a2f.model = model_from_gguf(gguf)
    pcm = make_audio(SECONDS * RATE)
    beat, down = a2f.spect2frames(a2f.spect(torch.tensor(pcm)))
    beat, down = beat.numpy(), down.numpy()
    with open(out, "w") as f:
        f.write("/* Made by gen_beatthis_ref.py: Beat This!'s logits (PyTorch, beat_this) for\n"
                f" * test_beatthis.c's audio, one frame in {STEP} of {len(beat)}. */\n\n")
        f.write(f"#define REF_FRAMES {len(beat)}\n#define REF_STEP {STEP}\n\n")
        for name, a in (("ref_beat", beat), ("ref_down", down)):
            f.write(f"static const float {name}[] = {{\n")
            vals = [f"{v:.5f}f" for v in a[::STEP]]
            for i in range(0, len(vals), 8):
                f.write("    " + ", ".join(vals[i:i + 8]) + ",\n")
            f.write("};\n\n")


if __name__ == "__main__":
    main()
