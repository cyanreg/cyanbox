# The model: genre, hue and comment

How rxbox tags a track with a genre, a colour (a hue) and a comment, what
the model behind it can and cannot tell, and how it runs. The model is
MOSS-Audio-4B-Instruct (OpenMOSS, Apache-2.0) from a GGUF file: its audio
encoder in F16, its decoder (Qwen3-4B) in Q4_K. The code is `src/ml/`
(`rxbox/ml.h`), the shaders `src/ml/shaders/`, the kernel tests
`tests/ml/test_kernels.c`, the check against the reference implementation
`tools/ml_check.py`, and the tool for trying prompts `tools/rxbox-ml.c`.

## In short

| | per track (4–5 min) |
| --- | --- |
| Loading the model, once | 3.5 s; 4.1 GB of weights onto the GPU, none kept in RAM |
| Listening (the whole track) | ~4 s on a 6900 XT, ~70x real time |
| The ten questions of `rx_ml_tag` | ~1.1 s |
| Memory | 174 MB of RAM at most (64 MB of it a read buffer while loading); 7 GB of VRAM at the peak (4.1 GB weights, 1.2 GB cache, the encoder's scratch for a 6-minute track) |

What it gives, measured on the author's library (some 150 tracks: house,
garage, techno, trance, J-core, electro, bossa nova, jazz, AOR, J-pop,
ambient, orchestral):

- **Mood and intensity: yes.** Asked yes or no, the probability of "Yes"
  orders tracks sensibly and consistently: calm (bossa nova, ballads,
  ambient) to intense (hardstyle, J-core, big room), dark (techno) to bright
  (uplifting house). Questions that should agree do (|r| 0.9 among "intense",
  "calm", "chill-out"). The two measures of a whole track and of 150 s of
  its middle correlate at 0.95.
- **Vocals: yes, in words.** "Are there vocals? If yes, who sings and in what
  language?" gets instrumental albums right and names the language and the
  singer's sex plausibly.
- **Mood words: yes.** Three words, apt if a little repetitive ("uplifting,
  energetic, euphoric" is its favourite).
- **Genre: the family, not the subgenre.** Its labels cluster in about
  fifteen: tech house, progressive trance, J-pop, J-core, progressive house,
  hardstyle, big room house, smooth jazz ... It hears TOHO BOSSA NOVA's
  bossa nova as smooth jazz, salsa or J-pop, and Shibayan's electro house as
  J-core, hardstyle or Eurodance. The same track gives the same genre from
  its whole and from 150 s of its middle 61 times in 84. Giving it examples
  ("for example: uplifting progressive house, bass house") only makes it
  repeat the first example.
- **Tempo, key, chords: no.** It answers 111 BPM and C major for nearly
  everything (rxbox's own analysis has both, measured).
- **Numbers on a scale: no.** "On a scale of 0 to 9, how energetic?" is 8 or
  9 for anything with a beat. Its probabilities move, but less than a
  yes/no question's.
- **Where in a set: no, if asked.** "Opener, warm-up, peak time or closing?"
  is "peak time" for every track, a bossa nova included.

## The tags

`rx_ml_tag` listens to the whole track (up to what fits in the context,
about ten and a half minutes; longer tracks are cut at the end) and asks:

1. Seven yes/no questions, each read as the log-odds of "Yes", centred and
   scaled by its mean and spread over the library above (each question has
   a bias of its own; the scaling cancels it):
   - intensity: "intense, high-energy?", minus "calm?", minus "chill-out?"
   - mood: "happy and uplifting?", minus "dark?", minus "melancholic?"
   - club: "made for dancing in a club?"
2. The genre, "with the genre name only, as specific as possible", cut at
   the first comma or line and capitalised ("Progressive House", "J-Pop").
3. The vocals, in a few words: "Female vocals, Japanese", "Instrumental".
4. The mood, in at most three words.

The comment is the vocals and the mood: "Female vocals, English.
Melancholic, introspective, atmospheric."

### The hue

The hue is the direction of the track on the plane of mood and intensity
(Russell's circumplex of affect), laid on the colour wheel with its usual
associations:

| hue | | example |
| --- | --- | --- |
| 0 red | intense | Get On The Floor, Somebody Like Me (AD:HOUSE) |
| 45 orange | intense and bright | Soleil, Best wishes, (AD:HOUSE), スイングバイ |
| 90 yellow | bright | Blue Horizon, 夏恋 |
| 135 green | bright and relaxed | Summer Twilight, Love Light (Yutaka Yokokura), TOHO BOSSA NOVA |
| 180 cyan | calm | A Guiding Star, Glide High |
| 225 blue | calm and sad | Flow (Calm Time), 風舞う春に, Imagination (AD:Garage) |
| 270 violet | dark | Awakening, nakanoshima (AD:TECHNO 4) |
| 315 magenta | dark and intense | Hyperspark, 荊の城, Blood Sugar (Voodoo remix) |

It is continuous, so similar tracks get similar hues. Within 0.6 standard
deviations of the middle of the plane, where the direction is undefined
and would turn round on a small change, it fades into orange, the hue of
the average track of that library. Between a whole track and 150 s of its
middle, half of the hues are within 15 degrees and nine in ten within 40;
the rest are tracks whose middle is more intense than their intro and
outro, which is why the whole track is used. Whether a track is club music
is measured and kept (`RXMLTags.club`) but left out of the hue.

A first mapping placed tracks by the DJ's set roles (pink an opener, orange
a warm-up, red a banger, cyan chill-out, green the rest). The model cannot
tell set roles apart when asked, and the roles do not lie on a line of the
colour wheel (orange, between an opener and a banger, is on the far side
of red from pink), so that mapping had to jump; it was dropped for this one.

## How it runs

Everything from the samples on runs on the GPU, in Vulkan compute with
GLSL shaders and no cooperative matrices; the CPU tokenizes the prompt,
reads back a token or a row of logits, and does nothing else.

- **Weights.** The GGUF's tensors are streamed into device buffers that keep
  the file's layout (a segment of up to 1 GB is a copy of a run of the
  file), 64 MB at a time, read with O_DIRECT so they stay out of the page
  cache. The conv weights are reordered once on the GPU for the GEMM's
  gather.
- **Features.** Whisper's log-mel (128 bins, 25 ms frames every 10 ms) from a
  DFT table computed in double precision: `mel.comp`, `mel_norm.comp`.
- **Encoder.** Chunks of 4 s; three 3x3 stride-2 convolutions (the first
  direct, `conv1.comp`, the others an implicit GEMM), the stem projection,
  32 Whisper layers with attention inside each chunk (`enc_attn.comp`), the
  final norm, the adapter and three deepstack mergers (SwiGLU MLPs).
- **Decoder.** Qwen3-4B with a 8192-token f16 cache: RMS norms, the q/k
  norms and RoPE from a double-precision table (`qk_rope.comp`), flash
  attention for prefills (`attn.comp`, split over the keys when there are
  few queries), flash decoding for single tokens (`attn_dec.comp`).
  The audio's rows replace their tokens' embeddings, and the deepstack's
  rows are added after layers 0 to 2. The prompt is MOSS-Audio's default
  with its time markers (the seconds every 2 s); the cache holds it up to
  the end of the audio, and every question starts again from there.
- **Products.** `gemm.comp`: 128x128 tiles, 8x8 per invocation, f32 FMAs,
  shared memory swizzled against bank conflicts (Q4_K dequantised on the
  way in; a 32-row variant for questions). `gemv.comp` for one to eight
  rows, at the memory's speed.

| kernel (6900 XT) | |
| --- | --- |
| GEMM, F16 weights, 2048 tokens | 12.3 TFLOPS (53 % of the f32 peak) |
| GEMM, Q4_K, 512 tokens | 10.2–12.9 TFLOPS |
| GEMV, Q4_K, one token | 400–445 GB/s (80–87 % of the memory's) |
| Attention, 512 queries after 3584 keys | 3.3 ms a layer, 9.8 TFLOPS |
| Attention, one query after 4000 keys | 64 µs a layer |

A 5-minute track is ~4,000 tokens through the decoder: ~2.5 s of GEMM,
~0.6 s of attention, ~0.7 s of encoder.

**One model per GPU.** Two processes running the model on the 6900 XT at
once made the driver reset the GPU (both contexts lost) within minutes;
one alone ran over 250 tracks without fault.

## Checking it

- `test_ml_kernels` (meson suite `ml`) runs every kernel on random inputs of
  awkward sizes against plain C in double precision; `-b` benchmarks them.
- `tools/ml_check.py` recomputes the encoder stage by stage in PyTorch from
  the reference code (transformers' Whisper feature extractor and encoder
  layer, MOSS-Audio's conv stem and gated MLPs), each stage from the GPU's
  own input to it, reading only the tensors it needs from the GGUF. Run
  `RXBOX_ML_DUMP=DIR rxbox-ml ...` first. On a 37 s excerpt every stage
  agrees to 2e-5 or better (relative to its largest value).
- The decoder answers "What is the capital of France?" with "The capital of
  France is Paris.", and the encoder and decoder together transcribe
  MOSS-Audio's English test clip correctly.

## Trying prompts

```sh
rxbox-ml -m MODEL -t FILE...                       # the tags
rxbox-ml -m MODEL -q "Describe this music." FILE   # an answer in words
rxbox-ml -m MODEL -y "Is this track dark? Answer yes or no." FILE
rxbox-ml -m MODEL -c "... Answer with one digit." FILE
rxbox-ml -m MODEL -r "Genre? Answer with the genre name only.|House|Techno" FILE
```

`-s` and `-l` choose an excerpt (`-s -1`: from the middle);
`RXBOX_ML_PROFILE=1` times every kernel.
