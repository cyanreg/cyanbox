# Beat This! on the GPU

Beat This! (Foscarin, Schlüter and Widmer, ISMIR 2024; CPJKU, code and
weights MIT) is a beat and downbeat tracker: a transformer that turns a
log-mel spectrogram into a beat and a downbeat activation for every frame,
50 a second. cyanbox runs its network with Vulkan compute on the code the
tagging model uses (`src/ml/`, docs/ML.md, How it runs): `cyanbox/beatthis.h`,
`src/ml/beatthis.c`, the kernels `src/ml/shaders/bt_*.comp`. The weights
come from `models/beat-this-f16.gguf` (the final0 checkpoint in F16, its
batch norms folded), about 40 MB on the GPU.

This is the network, not a beat grid: `cb_beatthis_run` returns the logits
as the reference's `Audio2Frames` does, and what to make of them is being
worked out (its own "minimal" postprocessor picks peaks, and its tempo
wanders and changes level inside a track). Nothing in the analysis uses it
yet.

## What it does

As `beat_this` (`inference.py`, `model/beat_tracker.py`, `model/roformer.py`):

- 22050 Hz mono: frames of 1024 every 441 samples, centred, periodic Hann,
  the magnitude over sqrt(1024), 128 slaney filters, log1p(1000 x)
  (`bt_mel.comp`, a direct DFT from a table made in double precision).
- Chunks of 1500 frames, 6 at each end overlapping the next, the last
  moved to end at the piece's end; a piece of no more than 1488 frames is
  one chunk of its own length and the borders. Each chunk's frames but its
  six at either end are kept, an earlier chunk's where two overlap
  ("keep_first").
- The network on 4 chunks at a time (about 90 MB of scratch each, `batch`
  in the options): the stem (`bt_stem.comp`), three frontend blocks of
  attention and feed-forward across frequency then across time and a 2x3
  convolution (`bt_im2col.comp` and `gemm.comp`), a projection to 512, six
  roformer layers, the sum head. Activations are channel-last, so every
  Linear is one product over contiguous rows; one attention kernel
  (`bt_attn.comp`, RoPE and the heads' gates inside) serves the three
  directions.

## How close and how fast

On a 6900 XT, against the reference in PyTorch on the same samples:

| | |
| --- | --- |
| logits | within 3e-4 of PyTorch's (of ±16); none on the other side of 0 in 1.25 million frames of 117 tracks |
| 4-minute track | 0.2 s: the spectrogram 35 ms, the network 0.18 s (45–55 ms a minute of audio) |
| PyTorch on 8 CPU threads | 11 s |

Decoding (and resampling to 22050 Hz) costs more than the network.

## Checking it

`test_ml_beatthis` (meson suite `ml`, skipped without the model or a GPU)
compares the logits of 40 s of a made beat, two overlapping chunks, with
what `beat_this` makes of the same samples (`tests/ml/beatthis_ref.h`,
written by `tests/ml/gen_beatthis_ref.py`); runs it again for the same bits;
and runs one second, a chunk shorter than the network's. It fails for a
RoPE turned the wrong way, for keep_last instead of keep_first and for the
stem's padding taken before its batch norm.

## The tool

```sh
cyanbox-beatthis -m models/beat-this-f16.gguf -o DIR [-l LIST] [--ffmpeg] FILE...
```

writes input i's logits to `DIR/NNNNN.npy`, float32 of shape (2, frames),
beat and downbeat, for numpy. Files are decoded on `-j` threads ahead of
the GPU; by default with cyanbox's decoder (the players' timeline),
`--ffmpeg` as the reference's harness does, and a `.f32` file is raw
22050 Hz mono as it is.
