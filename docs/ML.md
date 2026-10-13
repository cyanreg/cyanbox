# The model: genre, colour and comment

How rxbox tags a track with a genre, a colour (one of rekordbox's eight,
picked from a legend) and a comment, what the model behind it can and
cannot tell, and how it runs. The model is MOSS-Audio-4B-Instruct
(OpenMOSS, Apache-2.0) from a GGUF file: its audio
encoder in F16, its decoder (Qwen3-4B) in Q4_K. The code is `src/ml/`
(`rxbox/ml.h`), the shaders `src/ml/shaders/`, the kernel tests
`tests/ml/test_kernels.c`, the check against the reference implementation
`tools/ml_check.py`, and the tool for trying prompts `tools/rxbox-ml.c`.

In rxbox it runs only when asked: the track menu's Analyse tags, Analysis →
Analyse tags of selection, or `rxbox tag TRACK...`. The tracks join one
batch that loads the model once and unloads it after the last
(`src/app/jobs.c`); a result writes the colour and the comment, and the
genre unless the user typed one (README.md, Tags).

## In short

| | per track (4–5 min) |
| --- | --- |
| Loading the model, once | 3.5 s; 4.1 GB of weights onto the GPU, none kept in RAM |
| Listening (the whole track) | ~4 s on a 6900 XT, ~70x real time |
| The other questions of `rx_ml_tag` (seven yes/no, three in words) | ~1.1 s |
| The colour question, in eight orders | ~1.7 s (1.3–2.1; 2.5 s with three renders sharing the GPU) |
| Memory | 174 MB of RAM at most (64 MB of it a read buffer while loading); 5.4 GB of VRAM in buffers from one listen to the next (4.1 GB weights, 1.2 GB cache, the decoder's scratch), 6.2 GB at the peak for a 4-minute track and 6.7 GB for one of 9.5 minutes or more (the encoder's scratch, freed after listening) |

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
- **Multiple choice: yes, in every order.** Which of eight descriptions
  fits a track it answers with a letter, but with a strong pull towards
  the letters D and E: one order of the list alone picks what all eight
  orders together pick a third of the time (The colour, below).

## The tags

`rx_ml_tag` listens to the whole track (up to what fits in the context
beside the questions, about nine and a half minutes; longer tracks are cut
at the end) and asks:

1. Seven yes/no questions, each read as the log-odds of "Yes", centred and
   scaled by its mean and spread over the library above (each question has
   a bias of its own; the scaling cancels it): the measures the library
   keeps.
   - intensity: "intense, high-energy?", minus "calm?", minus "chill-out?"
   - mood: "happy and uplifting?", minus "dark?", minus "melancholic?"
   - club: "made for dancing in a club?"
2. The colour: which of the legend's eight descriptions fits best, asked
   in eight orders (below).
3. The genre, "with the genre name only, as specific as possible", cut at
   the first comma or line and capitalised ("Progressive House", "J-Pop").
4. The vocals, in a few words: "Female vocals, Japanese", "Instrumental".
5. The mood, in at most three words.

The comment is the vocals and the mood: "Female vocals, English.
Melancholic, introspective, atmospheric."

### The colour

The colour is one of rekordbox's eight, the one whose words in the legend
fit the track best (README.md, Tags; the settings `color.pink` ...
`color.purple`). After the yes/no questions the model is asked, against
the same audio in its cache, with the default legend:

```
Which of these descriptions fits this music best?
A. dark and intense, hard
B. intense, peak time
C. energetic and bright, driving
D. bright, uplifting
E. groovy and relaxed, warm
F. calm, smooth
G. deep, melancholic
H. calm and dark, ambient
Answer with the letter only.
```

The answer is scored, not sampled (`rx_ml_pick`, on `rx_ml_choose`): one
forward pass over the question, then the logits of the eight letters, each
a single token, at the answer's first token, softmaxed over the eight. The
letters hold 99.9 % of the whole distribution there (99.7 % at least, on
every ask below), so the model does answer with a letter; its probability
for each is how well it hears that description in the track. The colour is
the most probable, so the division into eight is the model's; the eight
probabilities are kept with the track, with a hash of the legend's words
(`rx_ml_legend_hash`), so a track tagged under other words shows as such.
Should the letters ever hold less than half of it, the tag fails: the
model is not answering the question, and eight renormalised scraps would
make a colour of nothing.

The question is as long as the legend's words, which the user writes: the
context left beside the audio is measured from the questions (the colour
question in every rotation, the others with the longest answers they are
allowed), 256 tokens at least. The default legend's question is under
that; eight Japanese descriptions of the 127 bytes the interface's fields
hold take 310 tokens, and a track of 9.5 minutes or more is cut 3 s
shorter for them. A legend whose question would take more than a quarter
of the context (2048 tokens) is refused.

**The order of the list matters, a lot.** Asked in one order, the model
pulls towards the letters D and E whatever stands there. Over the 58
tracks of the acceptance library and the eight rotations of the list, in
which every description stands under every letter once, the letters drew
on average

| A | B | C | D | E | F | G | H |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 0.022 | 0.015 | 0.096 | 0.276 | 0.267 | 0.123 | 0.113 | 0.087 |

of the probability, where an unbiased model would give each 0.125. In the
order above it names yellow (D) for 57 of the 58 tracks, drum'n bass and
jazz alike; the same list reversed names orange (F there) for 52, and the
two orders agree on none. One order alone picks the colour all eight
together pick for 34 % of the asks.

So the question is asked in all eight rotations (rotation r lists the
description r + k under the k-th letter) and the eight answers are combined
by their geometric mean, renormalised (`rx_ml_color`). A letter's pull
multiplies the probability of whatever stands under it, and each
description has stood under each letter once, so in the product the pulls
cancel and what is left is the model's preference among the words. The
plain mean of the eight leaves the pull of the favoured letters in, and
picks less steadily: the colour from the even rotations (0, 2, 4, 6) and
from the odd ones agree for 49 tracks in 58 with the geometric mean, 29
with the plain mean, and the geometric mean's top probability is higher
(0.50 on average against 0.35). Four rotations would pick what eight pick
for 53 tracks in 58, for half the time; rxbox asks all eight.

Other wordings did not help (on 11 of the tracks): digits for labels move
the pull to 5 and 6 and pick less steadily (9 in 11 even against odd,
against 11 in 11 with letters); "(A) ..." labels let the letters' share of
the answer fall to 0.92; "Which of these describes this track best?" gives
the same picks.

**The words' own pull, and why it stays in.** The rotations cancel the
letters' pull, not the model's taste for some of the words. Asked of no
audio at all (the prompt without its audio block, which is the nearest to
an input with no content: silence is not one, the model hears 10 s of it
as pink 0.39 and purple 0.16, dark and hard), the eight rotations'
geometric mean is purple 0.30, red 0.21, orange 0.19, pink 0.16, yellow
0.08, green 0.05, blue 0.013, aqua 0.011. Dividing a track's colour
probabilities by this prior and renormalising (a contextual calibration)
was tried on the 58 tracks and held against what the model itself says of
them: the rank correlation of the colour's intensity (purple, aqua, blue,
green, yellow, orange, red, pink, calm to hard) with its intensity
measure and of the colour's mood (pink, purple, blue, red, aqua, green,
orange, yellow, dark to bright) with its mood measure; the picks its mood
words contradict (an intense colour for "calm, peaceful", a dark one for
"uplifting, joyful"); and whether the six tracks its mood words call calm
("Calm, reflective, peaceful", "Melancholic, introspective, tender") get
green, aqua, blue or purple.

| | as asked | divided by the prior |
| --- | --- | --- |
| colours | orange 26, yellow 20, red 7, purple 3, green 2 | yellow 34, orange 12, red 6, green 3, purple 2, blue 1 |
| spread (entropy; the largest colour) | 1.81 bits; 26 | 1.75 bits; 34 |
| top probability; runner-up within 0.1 | 0.50; 20 | 0.51; 17 |
| colour against the intensity measure | 0.65 | 0.64 |
| colour against the mood measure | 0.61 | 0.67 |
| picks the mood words contradict | 0 | 0 |
| calm tracks with a cool colour | 5 of 6 | 6 of 6 |

The calibration does not spread the colours. The audio splits the bright
tracks between orange and yellow, and the prior is lower for yellow (0.08
against 0.19), so dividing it out tips most of them to yellow. It moves
"Adagio" (melancholic, introspective) from purple to blue and "Shizuka No
Office" (calm, peaceful) from yellow to green, and its colours follow the
mood measure a little better, but the intensity measure no better. The
prior measured with silence instead spreads them less still (yellow 36 and
38 of 58 for 10 and 240 s). So rxbox keeps the probabilities as the model
gives them. The pull is the words' as much as the model's: a description
the model favours wins the close calls, and the user can reword it.

One other wording was tried for the cool colours: "Which of these
describes the mood of this music best?". It gives two more tracks purple,
its colours follow the intensity measure better (0.76), and its spread is
a little wider (1.87 bits), but it gives no track aqua or blue, and it
puts purple on a track the model calls "uplifting, energetic, euphoric".
Divided by its own prior it uses all eight colours but its colours follow
the intensity measure much worse (0.51), and still a yellow 35 of 58. On
58 tracks of mostly bright dance music neither is a clear gain; the
question stays as it is.

**On the acceptance library** (58 tracks: AD:HOUSE 7 and 11, AD:Drum'n
Bass 5, Orange Pekoe's jazz pop, AZKi's J-pop, a film soundtrack, ag's
J-core, D4DJ remixes), with the default legend: orange 26, yellow 20, red
7, purple 3, green 2, and no pink, aqua or blue. The top probability is
0.50 on average (0.26 to 0.88); for 20 tracks the runner-up is within 0.1
of it, for 17 of those a neighbour in the same warm family (orange with red
or yellow). Drum'n bass is red for five of its eight tracks, the brighter
rest orange or yellow; the house albums orange and yellow; Orange Pekoe's
"Adagio" and two pieces of the soundtrack ("Saimin De Tonight", "Martin
No Yakata") purple, the soundtrack's "Fortune Land E" and Orange Pekoe's
"Introduction" green. "Best wishes," (AD:HOUSE 11), orange in the hue's
table of the first tagging (intense and bright), is orange (0.61, then red
0.17). With three Blender renders sharing the 6900 XT, a track took 9.9 s
on average (4.4 to 14.3) from decoding to the last answer, the colour's
eight asks 2.5 s of it. Alone on the GPU, the eight asks take 1.7 s (1.3
to 2.1, the longer tracks' cache the slower) and listening 3.3 s.

Before tagging version 2 the colour was a hue computed from the measures:
the direction of the track on the plane of mood and intensity (Russell's
circumplex of affect) laid on the colour wheel, red intense, yellow
bright, cyan calm, violet dark, continuous so that similar tracks got
similar hues, and the stick took the nearest of its eight. The model had
no say in where the eight were cut; now it has all of it, and the user
says what each colour means. A track tagged then keeps its hue until it
is tagged again. A first mapping before that placed tracks by the DJ's set
roles (pink an opener, red a banger); the model cannot tell set roles
apart when asked (above), which is why the legend's words are about what
can be heard.

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
rxbox-ml -m MODEL -t FILE...                       # the tags and the colours
rxbox-ml -m MODEL -t -L "a|b|c|d|e|f|g|h" FILE     # another legend, pink to purple
rxbox-ml -m MODEL -b FILE...                       # the colour in each order
rxbox-ml -m MODEL -b                               # the same of no audio: the words' prior
rxbox-ml -m MODEL -q "Describe this music." FILE   # an answer in words
rxbox-ml -m MODEL -y "Is this track dark? Answer yes or no." FILE
rxbox-ml -m MODEL -c "... Answer with one digit." FILE
rxbox-ml -m MODEL -r "Genre? Answer with the genre name only.|House|Techno" FILE
```

`-s` and `-l` choose an excerpt (`-s -1`: from the middle);
`RXBOX_ML_PROFILE=1` times every kernel.
