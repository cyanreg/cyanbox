#!/usr/bin/env python3
# Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md.
"""Checks the model's encoder (src/ml/moss.c) against a reference written
from MOSS-Audio's own code (modeling_moss_audio.py, processing_moss_audio.py
and transformers' WhisperFeatureExtractor and WhisperEncoderLayer), stage by
stage: each stage is computed here from the GPU's own input to it, so an
error shows where it is made rather than downstream.

    RXBOX_ML_DUMP=DIR rxbox-ml -m MODEL -l 30 -q "..." FILE
    python3 ml_check.py MODEL DIR

Only the tensors a stage needs are read from the GGUF (memory-mapped, a few
tens of MB each); the model is never loaded whole. Needs numpy and torch
(CPU)."""
import math
import struct
import sys

import numpy as np
import torch
import torch.nn.functional as F

GGML_F32, GGML_F16 = 0, 1


class Gguf:
    def __init__(self, path):
        self.path = path
        with open(path, "rb") as f:
            def rd(fmt):
                return struct.unpack("<" + fmt, f.read(struct.calcsize("<" + fmt)))

            def rstr():
                (n,) = rd("Q")
                return f.read(n).decode("utf-8", "replace")

            sizes = {0: "B", 1: "b", 2: "H", 3: "h", 4: "I", 5: "i", 6: "f", 7: "?",
                     10: "Q", 11: "q", 12: "d"}

            def skip(t):
                if t == 8:
                    rstr()
                elif t == 9:
                    et, n = rd("IQ")
                    if et == 8:
                        for _ in range(n):
                            rstr()
                    else:
                        f.seek(n * struct.calcsize(sizes[et]), 1)
                else:
                    rd(sizes[t])

            assert f.read(4) == b"GGUF"
            rd("I")
            nt, nkv = rd("QQ")
            for _ in range(nkv):
                rstr()
                (t,) = rd("I")
                skip(t)
            self.tensors = {}
            for _ in range(nt):
                name = rstr()
                (nd,) = rd("I")
                dims = rd("Q" * nd)
                ty, off = rd("IQ")
                self.tensors[name] = (dims, ty, off)
            self.data = (f.tell() + 31) // 32 * 32

    def get(self, name):
        """The tensor in PyTorch's order (dims reversed), as f32."""
        dims, ty, off = self.tensors[name]
        n = int(np.prod(dims))
        dt = {GGML_F32: np.float32, GGML_F16: np.float16}[ty]
        a = np.memmap(self.path, dtype=dt, mode="r", offset=self.data + off, shape=(n,))
        return torch.from_numpy(np.array(a, dtype=np.float32)).reshape(tuple(reversed(dims)))


def load(d, name, shape=None):
    a = np.fromfile(f"{d}/{name}.f32", dtype=np.float32)
    return torch.from_numpy(a if shape is None else a.reshape(shape))


def report(name, got, want, tol):
    got, want = got.double(), want.double()
    err = (got - want).abs().max().item()
    ref = want.abs().max().item()
    rel = err / max(ref, 1e-30)
    print(f"{name:34s} {'ok  ' if rel <= tol else 'FAIL'} max err {rel:.2e} of {ref:.3g}")
    return rel <= tol


def whisper_mel(wav, filters, window):
    """WhisperFeatureExtractor._np_extract_fbank_features: centred frames,
    reflect padding, |rfft|^2, mel, log10, the last frame dropped, clamped
    to max - 8, (x + 4) / 4."""
    wav = np.pad(wav.astype(np.float64), (200, 200), mode="reflect")
    nb = 1 + (wav.size - 400) // 160
    frames = np.lib.stride_tricks.sliding_window_view(wav, 400)[::160][:nb]
    spec = np.abs(np.fft.rfft(frames * window, n=400)) ** 2
    mel = np.maximum(1e-10, spec @ filters)
    log = np.log10(mel)[:-1]
    log = np.maximum(log, log.max() - 8.0)
    return (log + 4.0) / 4.0


def sinusoids(length, channels):
    inc = math.log(10000.0) / (channels // 2 - 1)
    inv = torch.exp(-inc * torch.arange(channels // 2).double())
    t = torch.arange(length).double()[:, None] * inv[None, :]
    return torch.cat([torch.sin(t), torch.cos(t)], dim=1)


def layer_norm(x, g, prefix, eps=1e-5):
    return F.layer_norm(x, (x.shape[-1],), g.get(prefix + ".weight"), g.get(prefix + ".bias"), eps)


def whisper_layer(x, g, i, lens):
    """WhisperEncoderLayer over chunks [B, 50, 1280], keys past lens[b]
    masked."""
    p = f"enc.blk.{i}."
    B, T, D = x.shape
    H = 20
    h = layer_norm(x, g, p + "attn_norm")
    q = (h @ g.get(p + "attn.q.weight").T + g.get(p + "attn.q.bias")) * (64 ** -0.5)
    k = h @ g.get(p + "attn.k.weight").T
    v = h @ g.get(p + "attn.v.weight").T + g.get(p + "attn.v.bias")
    q, k, v = (t.view(B, T, H, 64).transpose(1, 2) for t in (q, k, v))
    s = q @ k.transpose(-1, -2)
    mask = torch.arange(T)[None, :] >= torch.tensor(lens)[:, None]
    s = s.masked_fill(mask[:, None, None, :], float("-inf"))
    o = (s.softmax(-1) @ v).transpose(1, 2).reshape(B, T, D)
    x = x + o @ g.get(p + "attn.o.weight").T + g.get(p + "attn.o.bias")
    h = layer_norm(x, g, p + "ffn_norm")
    h = F.gelu(h @ g.get(p + "ffn.fc1.weight").T + g.get(p + "ffn.fc1.bias"))
    return x + h @ g.get(p + "ffn.fc2.weight").T + g.get(p + "ffn.fc2.bias")


def gated_mlp(x, g, prefix):
    a = x @ g.get(prefix + ".gate.weight").T
    b = x @ g.get(prefix + ".up.weight").T
    return (F.silu(a) * b) @ g.get(prefix + ".down.weight").T


def main():
    g = Gguf(sys.argv[1])
    d = sys.argv[2]
    torch.set_grad_enabled(False)
    ok = True

    # Features: from the samples the GPU had.
    pcm = load(d, "pcm").numpy()
    filters = g.get("audio.mel_filters").numpy()            # [201][128]
    window = g.get("audio.mel_window").numpy()
    hann = np.hanning(401)[:-1]
    print(f"{'mel window vs periodic Hann':34s} max diff {np.abs(window - hann).max():.2e}")
    mel = whisper_mel(pcm, filters.astype(np.float64), window.astype(np.float64))
    frames = mel.shape[0]
    gmel = load(d, "mel", (frames, 128))
    ok &= report("mel", gmel, torch.from_numpy(mel), 1e-4)

    # The stem: chunks of 400 frames, the last one zero-padded, three
    # convolutions with GELU, permute(0, 3, 1, 2).flatten(2), projection,
    # positions from 0 in each chunk. The first batch of 8 chunks.
    nch = (frames + 399) // 400
    B = min(8, nch)
    chunks = torch.zeros(B, 1, 128, 400)
    lens = []
    for b in range(B):
        seg = gmel[b * 400:(b + 1) * 400]
        chunks[b, 0, :, :seg.shape[0]] = seg.T.float()
        lens.append(seg.shape[0])
    x = F.gelu(F.conv2d(chunks, g.get("enc.conv1.weight"), g.get("enc.conv1.bias"), 2, 1))
    x = F.gelu(F.conv2d(x, g.get("enc.conv2.weight"), g.get("enc.conv2.bias"), 2, 1))
    x = F.gelu(F.conv2d(x, g.get("enc.conv3.weight"), g.get("enc.conv3.bias"), 2, 1))
    x = x.permute(0, 3, 1, 2).flatten(2)
    x = x @ g.get("enc.stem_proj.weight").T + g.get("enc.stem_proj.bias")
    x = x + sinusoids(50, 1280).float()
    rows = nch * 50
    stem = load(d, "stem", (rows, 1280))
    tok = [((n - 1) // 2 + 1 - 1) // 2 + 1 for n in lens]
    tok = [(t - 1) // 2 + 1 for t in tok]
    for b in range(B):
        ok &= report(f"stem chunk {b} ({tok[b]} tokens)", stem[b * 50:b * 50 + tok[b]],
                     x[b, :tok[b]], 1e-3)

    # Layer 0, from the GPU's stem.
    all_lens = [50] * (nch - 1) + [((((frames - (nch - 1) * 400) - 1) // 2 + 1 - 1) // 2 + 1 - 1) // 2 + 1]
    xin = stem.view(nch, 50, 1280).float()
    y = whisper_layer(xin, g, 0, all_lens)
    enc0 = load(d, "enc0", (nch, 50, 1280))
    for b in range(nch):
        ok &= report(f"encoder layer 0, chunk {b}", enc0[b, :all_lens[b]], y[b, :all_lens[b]], 1e-3) \
              if b in (0, nch - 1) else True

    # The final norm and the adapter, from the GPU's last layer; the first
    # deepstack merger, from its layer-8 output.
    nb_audio = (nch - 1) * 50 + all_lens[-1]
    enc31 = load(d, "enc31", (rows, 1280))[:nb_audio].float()
    ad = gated_mlp(layer_norm(enc31, g, "enc.norm"), g, "adapter")
    ok &= report("final norm + adapter", load(d, "adapter", (nb_audio, 2560)), ad, 1e-3)
    ds8 = load(d, "ds8", (rows, 1280))[:nb_audio].float()
    ok &= report("deepstack merger 0", load(d, "deepstack0", (nb_audio, 2560)),
                 gated_mlp(ds8, g, "deepstack.0"), 1e-3)

    print("all ok" if ok else "FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
