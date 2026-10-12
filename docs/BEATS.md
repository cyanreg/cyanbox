# The beat-grid analyser

How rxbox finds a track's beat grid, why each step is the way it is, and how
well it works. The code is in `src/analysis/` (`spectrum.c`, `onset.c`,
`tempo.c`, `grid_fit.c`, `downbeat.c`, `beats.c`, `analysis.c`, the private
interface `beats_internal.h`); the tests in `tests/analysis/test_beats.c`; the
evaluation tool in `tools/rxbox-eval.c` and `tests/analysis/py_beats.py`.

The result (`rxbox/analysis.h`, `RXBeatResult`) is what a DJ player wants: a
piecewise-constant grid (one section per tempo or phase, each a start beat, a
BPM and a beat number), a confidence, and flags saying what is uncertain.

## What it does, in one page

1. **Spectrum.** A centred 2048-point Hann STFT every 294 samples (150 frames
   a second at 44.1 kHz), 64 mel bands and a dedicated 30-150 Hz kick band.
2. **Onsets.** Spectral flux in four bands: SuperFlux over all bands, the
   kick band, 150-2500 Hz (snares, claps) and above 5 kHz (hats). Put on one
   time base.
3. **Tempo.** Autocorrelation candidates, rescored on their harmonic series,
   grouped into octave families; the strongest family, at its octave nearest
   125 BPM, is the *working* tempo. A tempogram over 12-second windows finds
   stretches at another tempo.
4. **Beats.** A dynamic-programming tracker follows the onsets at that tempo.
   Around every beat the audio is cut into three fine band envelopes, and the
   attack of each beat is measured to a third of a millisecond.
5. **Lines.** A robust line through the attacks; a change-point search
   proposes sections; evidence tests keep only the ones a DJ needs.
6. **Phase.** Each section is put on the start of its hits with searches
   that shrink step by step, and snapped to a whole BPM when that moves no
   beat by more than 5 ms.
7. **Family and octave, on the grid.** A close 3:2 or 4:3 family is fitted
   too and the grid whose beats land on hits wins. The octave (half, same,
   double) is judged by where the snares and kicks fall on the fitted beats.
8. **Off-beat and downbeat.** Kicks and structure decide whether the grid is
   on the beat or half a beat off; structure (novelty peaks at one to eight
   bars), bass-note changes and the music's start vote for beat 1.
9. **Confidence and flags.**

It costs about 290 ms of CPU for an average 4.5-minute track (383 ms per six
minutes of audio), single-threaded, after decoding. dj's Python analyser took
5 s for the same.

## 1. Spectral front end (`spectrum.c`)

- **2048-point frames, 294-sample hop.** 46 ms frames give 21.5 Hz bins, so
  30-150 Hz is six bins. rbxport measured that a low band over four and a
  half bins (1024-point frames) could not place an onset. 150 frames a
  second is dj's rate and the waveform column rate.
- **Mel bands on the power spectrum**, square-rooted back to an amplitude: a
  band's RMS amplitude, which is what a hit raises; it also avoids a square
  root per bin. Compression is dj's `log1p(1000 * amplitude / (N/4))`.
- **The kick band** is summed from the bins themselves (bins 1-7, the ends at
  half weight), finer than the lowest mel triangles, which straddle kick and
  bass.
- **av_tx everywhere.** Its fast codelets stop at 2^17 complex points (2^18
  real); above that it falls back to a naive O(n²) DFT (found the hard way: a
  2^20-point transform took minutes). Every transform here stays within
  that. The STFT is about two thirds of the analyser's time, nearly all of
  it in the transforms themselves (2.0 µs per 2048-point RDFT).
- **Fine envelopes only around beats.** Attack timing needs sub-millisecond
  envelopes, but only near beats. dj band-passed the whole track with three
  zero-phase Butterworth filters (16 M samples, forwards and backwards),
  which alone costs more than this whole analyser. Here an 8192-sample
  window (186 ms) around each beat is transformed once and three bands are
  cut from it in the frequency domain with raised-cosine edges: zero phase
  by construction, no group delay to correct. Bands as dj: below 150 Hz
  (kick body), 150-2500 Hz (snare, clap, the kick's knock), 2.5-16 kHz (the
  kick's click, hats). Box-smoothed energy (10, 4, 2 ms), every 16 samples
  (0.36 ms), as an amplitude. Only the middle ±58 ms is read, so the filters'
  impulse responses never wrap into it.
- **The resampler** (libswresample, for input that is not 44.1 kHz) is
  time-aligned: an impulse at 1 s from 22.05, 32, 48 or 96 kHz comes out at
  1 s to a hundredth of a sample.
- **Bass chroma** for the downbeat: a 0.74 s window after each beat on the
  signal block-averaged to 2756 Hz, where a 2048-point transform resolves
  1.35 Hz, enough to tell bass notes apart, which 21.5 Hz bins cannot.

The front end is behind `rx_spectrum_compute` / `rx_env_window`; a Vulkan
version could replace it without touching the rest.

## 2. Onsets (`onset.c`)

- **SuperFlux** (Böck & Widmer 2013): the rise of each mel band against the
  maximum of it and its two neighbours two frames earlier. A note sliding or
  wobbling into the next band (vibrato, a filter sweep) is not an onset; a
  drum hit, which raises every band at once, is.
- **Bands.** Full (all mel bands), kick (the kick band), mid (150-2500 Hz)
  and high (above 5 kHz), each with its local mean (one second) removed,
  clipped at zero and scaled to unit variance (dj's normalisation).
- **Timestamps.** A frame's flux is stamped at the window's centre (rbxport:
  the window's start puts beats 23 ms early). But a *log* flux still peaks
  early: the log of the energy in a 46 ms window rises fastest while a hit
  has only just entered it. Measured as 13-17 ms on the synthetic kicks and
  on real tracks, so every flux is stamped two frames (13.3 ms) later.
- **Band alignment.** The kick band's flux peaks 15-40 ms before the full
  band's on sidechained house and techno: between kicks the band is nearly
  empty, so its log rises almost fully when a sliver of the kick enters the
  window. Each band is shifted onto the full band's time base by the lag of
  their cross-correlation (within ±8 frames, far short of an off-beat).
  After this the kick and full bands agree on where the hits are on 79 % of
  the evaluation tracks (64 % before).

## 3. Tempo (`tempo.c`)

**Candidates.** The autocorrelation of the full-band onsets, summed over one,
two and four beats (a comb), from 50 to 220 BPM; the six strongest peaks and
their relatives at ×2, ×½, ×3/2, ×2/3. The autocorrelation finds repetition
but cannot tell the beat from a pattern that also repeats (kick, hat, kick,
hat makes 1.5 beats strong), hence the relatives.

**Harmonic rescoring** (dj's idea, kept): an onset train with period P has DFT
lines at 1/P, 2/P, 3/P, 4/P; a 3/2 or 2/3 impostor hits far fewer of them.
Each candidate is refined to the peak of Σₕ |DFT(h·f)|/h on one zero-padded
transform of the whole track. Candidates are grouped into octave families;
the strongest family wins. Harmonic strength cannot choose *within* a
family (a kick on every beat with a hat between has as many lines at twice
the tempo), so the octave is not chosen here: the working octave is the
family member nearest the prior (125 BPM), and the octave is decided later
on the fitted grid (section 7).

**The tempogram** (`rx_tempo_regions`): 12-second windows every 3 s, each
window's autocorrelation comb over the octave around the working tempo (the
comb sums the beat, two-beat and bar lags, so a half-time stretch still
reads as the main tempo). A *region* is a run of at least four windows
(21 s) that agree within 1.5 % on a tempo more than 3 % from the main one,
are rhythmic, and support the main tempo poorly there; and that tempo is not
3:2, 2:3, 4:3 or 3:4 of the main one within 1 % (rbxport's rule: triplet and
dotted passages read at those ratios; 128 to 174 is 1.9 % from 4:3 and
passes). The tracker then follows each region's tempo. If most windows
settle on another tempo that is not one of those ratios, that becomes the
main tempo (the ratios are left to the family check of section 7, which
fits both). In the final grids of the 150 evaluation tracks, four have
regions, all live piano or jazz recordings.

## 4. Tracking and attacks (`grid_fit.c`)

**The tracker** is Ellis's (2007) dynamic programme over the accent signal
(full band plus the kick band in proportion to how much of the beat's
periodicity it carries): the best sequence of onsets about one period apart,
over the whole track at once, so a fill or a breakdown does not throw it,
and it follows a live drummer's drift by itself (tightness 100).

**Attacks.** The average beat around the tracked beats (a fold) gives the
remaining offset of the onset frames (a few ms) and the band that shows the
hits; then each beat's attack is the *nearest* strong rise of its envelope
(rbxport: reaching for the strongest rise lets a line chase a clap after the
kick), at least half as steep as the steepest in the window.

**Where a rise is.** dj took the steepest point of a compressed envelope;
the compression puts that at the very start of the rise, in the filters'
faint pre-ringing, 4-5 ms early on the synthetic kicks. Here a rise is timed
where the band's *energy* crosses halfway between its level before and the
level it reaches. For a step under symmetric smoothing that is exactly the
step; a click's few-millisecond rise puts it within a millisecond of the
first sample of the hit. On the synthetic tracks every grid is within 1.8 ms
of the true onsets (typically 1.3 ms early: a sine kick starting from zero
energy).

**Which band times the beat.** Not the one with the most contrast (dj). Below
150 Hz the kick shares the band with the bass, whose notes can start before
the kick (a rolling techno bass), and the kick's body needs most of a cycle
of its 50 Hz fundamental to build: on "Shine" (AD:HOUSE 10) the low band rose
5-15 ms after the click, measured with scipy's Butterworth filters, and a
low-band grid sat that much late. The click bands (mid, high) time the hit,
the earlier of the two when both show the beat clearly; the low band only
when neither does.

## 5. Lines and sections (`grid_fit.c`)

**The line.** A Theil-Sen start (the median slope over pairs half the stretch
apart: up to half the attacks can be wrong, say a hats-only intro the
tracker followed on the off-beat, without moving it), then weighted least
squares with Tukey's biweight. Six hundred attacks fix the tempo to ~0.0001
BPM; 0.05 BPM of error would drift a beat and a half over six minutes.

**Change points.** An optimal partition of the attacks into lines, each
segment costing its squared residuals (in units of the jitter, at least
2 ms) plus 3 ln(n): a little over BIC for two parameters. Isolated ghost
attacks (more than max(4 jitters, 8 ms) from the running median of their
neighbours) are dropped first, since a squared cost would give each its own
segment. This *proposes*; on a steady techno track it proposes twenty
segments. The evidence tests decide, repeated until nothing changes:

1. **One line is enough**: merge two neighbours when the merged line stays
   within 15 ms of the attacks everywhere (running median of 9).
2. **Hold through breakdowns**: when the sections either side of a weaker
   one (fewer attacks than both) are on one line, whatever happened between
   (an off-beat bass, a half-time bar) joins them. Only the weaker part is a
   breakdown: two scraps of junk either side of a good section once took it
   over and gave 130.76 BPM for a 129 track.
3. **Support**: each side of a boundary must be explained by its own line
   at least 1.25 times better than by its neighbour's carried across, on the
   arbiter band (the kicks, when the track has them; within 64 beats of the
   boundary); 1.5 times for a new phase at the same tempo (rbxport's "half
   again as much kick"), since an off-beat bass alone can make a half-beat
   line look nearly as good. A hats-only intro tracked on the off-beat has
   no kick on either line and joins the music after it.
4. **Length**: a section needs 16 attacks and 32 beats; a new *phase* at the
   same tempo 64 beats (rbxport: an outro's last bars never get a phase of
   their own), and 128 when it is half a beat away: an off-beat bass and
   hats carry intros, breakdowns and outros on their own (on nakanoshima's
   last 74 beats, all bands sit half a beat from the body's kicks).

Merging recounts beats: within a section the tracker's counts are
consistent, and only the offset across a boundary is uncertain, so the other
section's counts shift by the whole number of beats that matches its 16
attacks nearest the kept line. (Recounting every beat against an
extrapolated line was an early bug: a 0.1 % slope error became whole-beat
errors far away, and fits converged on 131.7 BPM for a 126 BPM track.) A
merged fit keeps the kept line's own scale (an MM-estimate): a scale
re-estimated from a mostly-disagreeing union grows until nothing is an
outlier.

After the off-beat decisions (section 8) two neighbours whose lines end up
within the merge tolerance again are merged: one line holds the other's
beats within 15 ms at both of its ends (the lines are straight, so the ends
are the worst). That is a stretch tracked on the off-beat that its own
decision moved back, or a "re-entry" a few ms late. Such a cut is no cut a
DJ could hear, and on a stick it is two beats a few ms apart, every downbeat
after it a beat early (an acceptance trial found one at 2.2 ms on a Photon
Maiden remix, flagged `tempo-change`). The line kept is the one with more
attacks, if it holds both; otherwise the other one, if it does.

**The exported beats.** A section's beats run up to the next section's
start, less the last one when it lies within half a period of that start
(`rx_grid_beats` in `src/core/grid.c`, rbxport's SAME_HIT): that beat and
the new section's first are the same hit. A section boundary that survives
therefore never puts two beats closer than half a period, and the beat count
across it stays right (a section that continues the count numbers its first
beat after the last beat kept). `test_beats` expands every synthetic grid
this way and fails on two beats closer than half a period or a section
downbeat not numbered 1; `rxbox-eval` counts such grids ("grids with two
beats closer than half a period": 0 of the 150 sample tracks and 0 of the 58
acceptance tracks).

**The boundary** sits on the first beat the new line explains: at a change
made on the beat (a DJ edit cut on the bar line), the beat at the join lies
on both lines and the search gives it to the old one, so it moves back while
the old section's last attacks fit the new line within 3 ms.

## 6. Phase and snapping (`beats.c`, `grid_fit.c`)

Per section: fold all bands on the line and move it onto the start of the
average hit (search ±30 ms), measure the attacks within 3× the section's
jitter (10-30 ms), refit, fold again (±5 ms), snap, fold again (±3 ms). The
search shrinks at every step so the line cannot walk from the kick onto a
hat or clap a little later, as a re-centred search of constant width lets
it, one step at a time. A drummer's scatter (jitter above 3 ms) smears the
fold and its rise comes early, so there the line through the attacks
(their mean) is the phase and the later folds are skipped.

**Snapping** to a whole or half BPM happens only when the drift it causes,
measured from the attacks' weighted centre to the section's ends, stays
within 5 ms. dj's rule (15 ms over the whole track from one end) with the
drift measured from the centre; rbxport's "within 0.1 BPM" would shift the
end of a six-minute track by 280 ms if the track really is at 128.1.

## 7. Family and octave on the grid (`beats.c`, `tempo.c`)

**Close families.** When the runner-up family's harmonic strength is within
25 % (3:2, 4:3 readings of a syncopated track), its grid is fitted too, and
it replaces the first only if its beats collect at least 10 % more of the
arbiter onset *and* it is as clean a grid (no more sections, no more
jitter): a 2-step garage track at 3/4 of its tempo collects hits, but only
with a ragged grid.

**The octave** is decided on the fitted grid, whose beats sit on the hits
(rhythm evidence needs that; a whole-track estimate does not have it). Half,
same and double are each judged by:

- **Too slow**: the snares and claps (mid band) fall on the half beats about
  as strongly as on the beats. At 87 for drum & bass they are its snares on
  2 and 4; at 64 for house its claps. Likelihood (1 − subdivision)/0.5.
- **Too slow, by the kick**: kicks on the half beats, but only when the next
  level up has a kick on every beat alike (UK hardcore at 175 read at 87.5)
  and is itself a plausible tempo (inside the range; at most 200 BPM without
  one), never for a syncopated kick (drum & bass's kick on the "and" of 3).
- **Too fast**: every other beat lacks snares and claps. Likelihood
  balance/0.8.
- **Four on the floor**: a kick on every beat alike and hardly between marks
  its level (×3), from 90 BPM up (a kick on 1 and 3 of a 125 track looks
  like that at 62.5). Without it a house track with a stab on the "and" of
  1 and 3 halves.
- **The prior**: log-normal around 125 BPM, 0.4 octave wide. It weighs 64
  against 128 by 18 to 1 and 87 against 174 hardly at all; the likelihoods
  are floored at 0.25 so the rhythm can overrule the prior only where it is
  nearly even.

- **The comfortable range**: without a requested range, a level above 200
  or below 60 BPM must win by a factor of four (its prior × 1/4). A pop song
  at 105 with sixteenths on the hats reads as a four-on-the-floor 210
  (Orange Pekoe's "Happy Valley" came out 210.24); drum & bass at 170-180
  is inside and unaffected. With `bpm_min`/`bpm_max` set, the range is the
  user's and nothing is folded.

Each level's beats are chosen from the finest down, because the line may
still sit on any eighth of the double level at this stage (a tracker held at
half the tempo can lock onto the "and"s): the double level's beats are the
line's beats and half beats or those a quarter beat later, whichever carry
more kick and snare; the line's level is every other one of those, again by
kick and snare; the half level every other one again, by kick alone (at
half tempo the beats would be the kicks; counting the snares there, drum &
bass's snares would win and hide the test).

The line's level used to take the parity with more kick. On a drum & bass
track whose kicks fall on the "and"s as often as on the beats ("Juggernaut.
– Determination", AD:Drum'n Bass 5) that put the 175 level on the "and"s:
its snares then sat on its own half beats (subdivision 1.48, "too slow"),
and the track came out 87.5 at confidence 1.00 while its seven album mates
were 172-180. With the snares counted, the 175 level's beats carry them, the
87.5 level has them on its half beats (subdivision 1.78), and 175 wins by
4.7. The kick veto ("too slow, by the kick") also applies only when the
level one up is inside the range (200 BPM without one): a busy drum & bass
kick, on the beats and the "and"s, is no four-on-the-floor at 344, and
had brought "Roundabout" (172) within 2 % of 86.

The kick band as a general veto was tried and made things worse (an off-beat
bass fills it between the kicks). The full band was tried and was worse
still (off-beat hats are as strong as kicks in flux). See "Evaluation".

## 8. Off-beat and downbeat (`beats.c`, `downbeat.c`)

**Structure** (rbxport's method): one 16-band log-mel profile per half beat;
novelty at every half beat is the distance between the mean profiles before
and after it at one, two, four and eight bars; only local maxima vote, each
scale's peaks normalised to sum to 1, by position among the 8 half beats of
a bar. Summing raw novelty gives every position nearly the same total (a
slow crescendo raises them all; rbxport measured a 3 % margin against 2.8×
from peaks).

**Weaker votes**: the bass's pitch-class change between the two beats
before and after each beat (peaks, ½ of a scale); and the beat where the
music starts, worth one scale. Tracks made for DJs start on a bar line: by
structure alone, 59 of the 66 electronic tracks whose music starts on a beat
had beat 1 there, and most of the other 7 were weak votes; with the start
worth one scale, all 68 such tracks do.

**On the beat or off it?** Two witnesses as log evidence: the arbiter band
(kicks on the beats against on the half beats, in a window reaching 30 ms
back and 10 ms forward, since a kick's low band leads its click) and the
structure (best beat position against best half-beat position, weighted 1.5:
rbxport found structure right on 153 of 155 reference grids, the kick alone
on 150). The grid moves half a beat only when the sum says so by a factor
1.25; the envelopes are cut again where the beats now are and the section is
refined again.

**Beat 1** is the best of the four beat positions, never a half beat. A later
section that starts on the previous one's grid continues its count (beat
field 0) unless its own vote clearly says otherwise; a section after a phase
cut numbers itself.

## 9. Confidence and flags

**Confidence** (dj's): the share of 16-beat windows (every 8 beats) whose
average beat starts its rise within 15 ms of the grid, weighted by how
clearly it rises (contrast less half the median contrast). Two changes. The
weight is capped at twice the median: in a breakdown the average beat's
level is tiny, its contrast explodes, and a few windows whose loudest rise
is a riser or a crash outweighed thirty bars of kicks. And the envelopes are
cut again at the grid's own beats wherever a tracked beat strayed more than
25 ms from it, so the confidence reads the grid as written rather than
where the tracker happened to be (empty windows had given one track 0.00).

Flags (`RXGridFlag`), and what they should mean to the DJ in the interface:

| Flag | Set when | Meaning for the user |
| --- | --- | --- |
| `tempo-change` | a section at another tempo than the one before it (more than 0.5 % apart) | The grid changes tempo; check each boundary. |
| `phase-cut` | a section at the same tempo as the one before it, re-entering shifted | An edit or a skip: the grid jumps phase at the boundary; check it. |
| `octave-uncertain` | the octave decision's margin < 1.5 | Half or double tempo was almost as likely: try ×2 / ÷2. |
| `phase-uncertain` | on-beat vs off-beat evidence < 1.5× | The grid may be half a beat off: listen to the first kick. |
| `downbeat-weak` | best beat position < 1.2× the next, or the section is too short to vote | Beat 1 may be another beat of the bar: use "set downbeat". |
| `no-kicks` | the kick band carries less than half the full band's periodicity at the beat rate and at twice it, and less than all of it at half the beat rate | Timed on clicks and claps; less precise. |
| `low-confidence` | confidence < 0.5 | The grid does not follow the hits in many places: check by ear. |
| `unsteady` | in some section, the attacks spread around the line by more than 6 ms *and* scatter from beat to beat by more than 4 ms | Live playing: the grid is the best straight line, beats scatter around it. |

**Phase cuts** were reported as `tempo-change` until an acceptance trial
(AZKi's "明けない夜があったなら": the music re-enters 71 ms late at 3:52, same
176 BPM). A DJ wants to know which: a phase cut is checked by ear at one
point, a tempo change needs the new tempo checked too. A grid can carry
both.

**No kicks** used the share of the beat-rate periodicity in the kick band
(at the beat rate or twice it). Drum & bass's two-step kick (on 1 and the
"and" of 3) lands half a period apart at the beat rate and cancels there:
"Neon" (174), whose kicks are plain on a bar pattern, read 0.14 and 0.31
and was flagged. At half the beat rate they do not cancel; the drum & bass
tracks of the sample read 1.7-4.9 there (against 0.1-0.5 at the beat rate).
That rate counts half, because a bass or a pianist's left hand moving twice
a bar reads there too (0.8 on an AD:PIANO VIVACE track). The flag is now
read at the tempo of the longest section. On the sample it went from 6
tracks to 1: two drum & bass tracks and a 133 BPM broken beat (kicks on 1,
3 and the "and" of 4) lost it, as did two multi-section tracks with a kick
drum once read at their main section's tempo (a piano-led dance track, an
Orange Pekoe song); the one left is AD:PIANO VIVACE "Cloud9" (0.33 at the
beat rate, 0.29 at twice it, 0.81 at half of it).

**Unsteady** used the spread around the line alone. A re-entry a few ms
late that is merged into one line (above) or a slow drift spreads the
attacks without scattering them, and a sequenced track was flagged. The
beat-to-beat scatter (the robust spread of the second differences of
consecutive attacks, over √6: white jitter σ gives σ) is what a drummer
shows; the flag wants both. Where a section has too few runs of three
consecutive attacks to measure the scatter, the spread alone decides.

On the evaluation sample, 78 of 89 electronic tracks carry no flag at all,
and 34 of the 61 others.

## Parameters

| Parameter | Value | Why |
| --- | --- | --- |
| STFT | 2048 / 294 at 44.1 kHz | kick band over 6 bins; 150 frames/s |
| Flux delay | 2 frames (13.3 ms) | log flux peaks early; measured |
| Band alignment | ±8 frames | per-track lag, far short of an off-beat |
| Tempo range | 50-220 BPM | dj's |
| Prior | 125 BPM, 0.4 octave, rhythm floored at 0.25 | 83 of 83 octave-labelled tracks |
| Comfortable range | 60-200 BPM without a requested range; ×1/4 outside | pop at 105 read as 210; drum & bass inside |
| Tracker tightness | 100 | follows a few % drift, walks through fills |
| Envelope window | 8192 samples, ±58 ms read | filter ringing never reaches the read part |
| Envelope bands | <150, 150-2500, 2500-16000 Hz | dj's |
| Attack search | ±20 ms (tracked), 10-30 ms (line) | a frame or two of tracker error; 3× jitter |
| Phase searches | 30, 5, 3 ms | shrinking: no walking |
| Snap tolerance | 5 ms at each end | inaudible |
| Section penalty | 3 ln(n), σ ≥ 2 ms | proposes generously; tests decide |
| Merge tolerance | 15 ms (running median of 9); after the off-beat decisions, 15 ms at both ends of a line | below what a DJ hears as drift |
| Support ratio | 1.25 on the kick band; 1.5 for a phase cut | rbxport: 1.5 for the new phase |
| Min section | 16 attacks, 32 beats; 64 for a phase, 128 half a beat away | rbxport's outro rule; off-beat basses |
| Tempogram | 12 s windows, 3 s hop, 4 windows | 21 s of a new tempo |
| Family check | runner-up within 25 %, 10 % more support | 3:2 and 4:3 |
| Unsteady | spread > 6 ms and beat-to-beat scatter > 4 ms | live drums scatter 5-15 ms, sequencers < 1 |
| No kicks | kick/full periodicity < 0.5 at 1× and 2× the beat rate, < 1 at ½× | two-step kicks cancel at the beat rate |
| Low confidence | < 0.5 | |

## Evaluation

There is no ground truth for the library, so the evaluation measures what
can be measured. The sample: 150 tracks from `~/Music`, chosen with a fixed
seed (`build-beats/eval/sample.txt` lists them): 90 electronic FLAC (AD:HOUSE
3-13, AD:HOUSE Winter, AD:TECHNO 4, AD:Garage, COLORS / Colors 2, AD:PIANO
VIVACE, ADvantage, AD-Progressive House 4), 30 J-pop and anime FLAC with live
drums (Orange Pekoe, yanaginagi, AZKi, D4DJ Photon Maiden, ReoNa, Yutaka
Yokokura) and 30 Opus from `music_temp` (10 from AD:Drum'n Bass 5).

### Synthetic tracks (`test_beats`, exact answers)

All pass: tempo within 0.05 BPM (all exact), phase within 3 ms (all within
1.8 ms), downbeat right, sections and boundaries right (boundaries within
1.3 ms), flags as required. Every grid is also expanded as a player gets it
(`rx_grid_beats`): no two beats closer than half a period, and each section's
first downbeat numbered 1.

| Case | Result |
| --- | --- |
| house 124, downbeat 0.3 s | 124.000, phase −1.3 ms, one section |
| house 128, 16-bar beatless intro (off-beat hats only), 8-bar breakdown | 128.000, −1.3 ms, one section |
| tech house 126, off-beat hats and bass louder than the kick | 126.000, −0.9 ms, on the kick |
| drum & bass 174 (two-step) | 174.000 (not 87), −1.0 ms |
| drum & bass 172 with 8 half-time bars | 172.000, +0.0 ms, one section |
| hip-hop 90 | 90.000 (not 180), −0.7 ms |
| live drums 118, 8 ms Gaussian jitter | 118.000, −1.7 ms, one section, `unsteady` |
| DJ edit 124 → 130 at bar 32 | two sections, boundary −1.2 ms, count continues, `tempo-change` |
| phase cut: 126, re-enters 0.2 s late at bar 32 | two sections, boundary −1.3 ms, new bar 1, `phase-cut` |
| DJ edit house 128 → drum & bass 174 at bar 32 | two sections, boundary +0.0 ms, `tempo-change` |
| re-entry 8 ms late at bar 32 (within the merge tolerance) | one section, no flags |
| determinism; 48 kHz input | identical; within 0.5 ms of 44.1 kHz |
| `rx_analyse` on stereo 48 kHz; auto cue | grid as mono, key "Am", cue on the first downbeat |

### Against dj's analyser (`py_beats.py`)

| | rxbox vs dj |
| --- | --- |
| BPM equal within 0.1 | 110 of 150 (73 %) |
| octave related | 11 |
| other | 29: 12 at 3:2 or 4:3, 11 within 3 % (live recordings), 6 unrelated (live, rubato) |
| phase within 15 ms (same or octave tempo) | 86 of 121 (71 %) |
| downbeat within 15 ms (same tempo) | 72 of 110 |
| same phase, downbeat a whole beat apart | 8 |

Where they disagree, independent evidence says:

- **Octave, on the 83 tracks whose octave is certain from their album**
  (house, techno, garage between 100 and 160; drum & bass between 150 and
  200): rxbox right on all 83, dj on 77. The prior alone would get 78.
- **Phase.** Of the 34 tracks where both agree on tempo and differ in phase
  by more than 10 ms, the click band (Butterworth above 2.5 kHz, scipy, folded
  over the whole track: `build-beats/eval/judge.py`) starts rising within 10 ms
  of rxbox's beats on 33, of dj's on 14.
- **Downbeat.** Of the electronic tracks whose music starts on a beat, all 74
  start on rxbox's beat 1 (dj: 52 of 52; dj's start vote decides nearly
  every track because its summed novelty is nearly flat).
- **BPM tags.** The whole library has 14 tagged files; 7 are one album with
  "135" on every track (acoustic songs between 117 and 170: the tag is
  wrong). Of the other 7, rxbox and dj both agree with the tag within 0.5 BPM
  on the same 5; on the other two both say 175 for a "170" and 139.000 for a
  "140" (rounded tags, most likely).

### Self-consistency

- Mean confidence 0.96 on electronic tracks (dj 0.73), 0.78 on the others
  (dj 0.67).
- Strong onsets locked to the grid (within 15 ms of their median offset from
  the nearest beat): kick band 43 %, full band 45 % of the strongest tenth of
  their peaks (the rest are off-beat hats, basses, fills).
- Kick band and full band put their hits at the same place of the beat on
  81 % of tracks.
- Sections on the 89 electronic tracks: 4 (three AD:PIANO VIVACE piano
  pieces and a garage track), flagged 2 `tempo-change` and 3 `phase-cut`;
  4 `unsteady`, 1 `low-confidence`. On the 61 others: 8 `tempo-change`, 5
  `phase-cut`, 22 `unsteady`, 14 `low-confidence`.

### Speed

Single-threaded, on the 150 tracks (mean 4.5 min): 290 ms CPU per track
(median 266, worst 827 ms when the grid is fitted three times), 64 ms per
minute of audio, with the machine otherwise quiet; 368 ms (median 274) with
other builds running beside it. dj: 4.98 s per track. Per six-minute track, roughly:
spectrum 165 ms (two thirds of it av_tx), onsets 15, tempo 14, tracking,
envelopes, attacks, sections and refinement 110, octave and family refits
when needed 100 each, phase, downbeat and bass 11, confidence 5.

### Memory

Peak resident memory above the decoded input (`rxbox-eval --mem`, which
resets the kernel's high-water mark after decoding; `--all` runs the whole
`rx_analyse`: waveforms, beats and key). Files made by concatenating
AD:HOUSE albums with ffmpeg; mono 44.1 kHz as the application decodes:

| Length | Input (mono float) | Beats | `rx_analyse` | Whole process, `rx_analyse` |
| --- | --- | --- | --- | --- |
| 5 min | 53 MB | 8 MB | 17 MB | 96 MB |
| 10 min | 106 MB | 14 MB | 32 MB | 162 MB |
| 30 min | 318 MB | 41 MB | 88 MB | 419 MB |
| 60 min | 635 MB | 81 MB | 176 MB | 810 MB |

The beat analyser grows by about 1.35 MB per minute: the onset signals, the
per-half-beat profiles and the tracked beats, but not the spectrum. The
largest stage of `rx_analyse` on an hour is the key analysis (151 MB above
the input), then the beats (82), then the waveforms (68).

At 48 kHz an hour costs 687 MB in the beat analyser and 781 MB in
`rx_analyse`, of which 606 MB is the one 44.1 kHz copy the analysers read;
stereo input at 44.1 kHz costs 781 MB in `rx_analyse`, of which 635 MB is
the mono mix that the waveform and key interfaces take. The decoder's own
transient peak (the audio module's) comes on top before the analysis starts.

Before this was measured, an hour cost 180 MB in the beat analyser and 790
MB in `rx_analyse` above a mono 44.1 kHz input, 1840 MB in the beat analyser
at 48 kHz and 1395 MB in `rx_analyse` for stereo. What changed, with every
result bit-identical (the synthetic suite, and `rxbox-eval --csv` over the
150 tracks with `--ffmpeg` decoding; with `rx_decode` too, except one file's
duration, which the decoder itself now reports differently):

- The mel frames are not kept. The STFT loop keeps three frames (SuperFlux
  compares each with the one two frames back), computes the three fluxes
  as it goes, and keeps only what later stages read: the kick band, and the
  16-band profiles (sums of four mel bands) that the downbeat's structure
  novelty reads per half beat. An hour was 580 000 frames of 64 floats.
- `rx_analyse` does not copy a 44.1 kHz input: the beat and key analysers
  read it in place. At other rates the waveforms are computed first, then
  one resampled copy is made and the own mono mix freed before beats and key.
- The resampler works in chunks of 65 536 samples, with each call's output
  bounded by `swr_get_out_samples`: given the whole hour's output size at
  once, libswresample allocated 1.2 GB internally.
- The family check frees the first fit's beat envelopes while the runner-up
  is fitted, and cuts them again only if the first fit is kept.

### Hand-checked cases

Checked by folding scipy band envelopes over the whole track on each grid
(`build-beats/eval/judge.py`, `foldcmp.py`), by bar patterns of the onset
bands in 16ths (`pattern.py`), and by musical reasoning about the genre.

| Track | rxbox | dj | Who is right, and why |
| --- | --- | --- | --- |
| AD:HOUSE 8 / Unclear | 127 | 63.5 | rxbox: kick on every beat at 127; at 63.5 the claps fall on the half beats. |
| AD:HOUSE 7 / groovin more groovin | 124 | 186 | rxbox: four-on-the-floor house; 186 is the 3:2 reading. |
| AD:Garage / 4:00 AM | 134 | 201 | rxbox: a garage tempo; 201 is 3:2. Confidence 1.00 vs 0.85. |
| AD:HOUSE 3 / Over the Azure | 130 | 195 | rxbox, probably: kick on every beat at 130. dj's confidence (0.91) is fooled by the kick on every third beat of a 3:2 grid. Forced to 180-210, rxbox fits 195 at 0.73. |
| AD:HOUSE 4 / Ocean Peridot | 132 | 198 | rxbox, probably: as above; a forced 180-210 fit is poor. |
| AD:Drum'n Bass 5 / Make You Better | 172 | 128.9 | rxbox: drum & bass; 128.9 is a 3:4 reading (dj's confidence 0.29). |
| AD:Drum'n Bass 5 / Divine Riptide | 177 | 177 | Same tempo; dj's grid is a sixteenth off (bar pattern: every hit on x.2 and x.4 of dj's beats). |
| AD:Drum'n Bass 5 / Firewall | 177 | 177 | Same. A busy breakbeat: on some versions rxbox halved it, its working grid at 88.5 sitting on the "and"s; fixed by choosing the levels' beats from the finest level down. |
| AD:HOUSE 10 / Shine | on the click | 35 ms early | rxbox: scipy fold has the click rising at 0 ms on rxbox's beats (dj's confidence 0.17). Before the click-band rule rxbox was 15 ms late (low-band body). |
| AD:TECHNO 4 / Blaster Weapon | 130 | 130 | Same grid. Ambiguous to the eye: a rolling 16th bass fills the kick band, open hats burst half a beat before a sharp mid-band click with a sidechain dip after it; both analysers put the beat on the click. |
| AD:HOUSE Winter 3 / White Lie | 127 | 127 | Same. Showed the need for the four-on-the-floor rule: a stab on the "and" of 1 and 3 made the snare test halve it. |
| ADvantage / Kneading | 104 | 138.66 | Undecided: 208 = 1.5 × 138.667 exactly, so one is a 3:2 reading; versions of rxbox have given 208, 138.667 and 104. |
| emome CASE01 / FVERTEX | 150 | 75 | rxbox, probably (dj's confidence 0.01). |
| COLORS / INTO THE LIGHT (Nhato remix) | 150, one section | 112.5 | rxbox, probably: a trance remix at 150. Earlier versions said 75 in three sections, flagged octave-uncertain and unsteady; choosing the line's beats by kick and snare (section 7) fixed it. |
| AD:Drum'n Bass 5 / Penglaishan | 190 | 190 | Same, flagged octave-uncertain: 95 and 190 were nearly tied. |
| AD:HOUSE 13 / When the Salsa Turns Orange | 123 | 123 | Same; an earlier version halved it (the octave test read the phase of the DFT's fundamental, 42 ms before the kick). |
| Taiyou no Kakera (Electric Sheep Remix) | 118.99 | 118.99 | Same. Showed that the tempogram must not override a 4:3 family (it gave 158.65). |
| AD:HOUSE 7 / Voyage with the Seagull | 125, one section | 125 | Same. An earlier version cut a half-beat "phase change" at 219.7 s on an off-beat bass, with confidence 0.00 from empty windows; both fixed. |
| AD:TECHNO 4 / nakanoshima | 125, one section | 125 | Same. The last 74 beats have only an off-beat element left; a 32-bar minimum for half-beat phase cuts keeps the body's grid. |
| Bootleg (eKiog UKHardcore) | 175 | 175 | Tag says 170; both say 175. Before the four-on-the-floor kick veto rxbox said 87.5. |
| TRY UNITE! (Uplifting Trance Mix) | 139 | 139 | Tag says 140; both independently 139.000: the tag is rounded. |
| Orange Pekoe / Taiyou no Flight (live) | 142.7, two sections | 140.3 | Neither is trustworthy: rubato live jazz; both have low confidence (0.27, 0.14). |

### Acceptance trial (58 library tracks)

A trial imported 58 tracks into a library and read the grids back
(`build-check/acceptance/`). Its findings, and what each change did there:

| Track | Trial | Now | Change |
| --- | --- | --- | --- |
| AD:Drum'n Bass 5 / Determination | 87.5, conf 1.00, `octave-uncertain` | 175 (margin 4.7) | the line's level chosen by kick and snare (section 7) |
| AD:Drum'n Bass 5 / Roundabout | 172 | 172 (margin 4.1) | the kick veto only below 200 BPM; it had fallen to a 2 % margin |
| AD:Drum'n Bass 5 / Hullbreaker, Keeps Me High | 175, 174, `octave-uncertain` | same, without it | the kick veto only below 200 BPM |
| AD:Drum'n Bass 5 / Neon | 174, `no-kicks` | 174 | kick presence also at half the beat rate |
| Orange Pekoe / Happy Valley | 210.24 | 105.13 | the comfortable range (60-200) |
| Photon Maiden / We never stop (DJ Shimamura Remix) | two sections, the second 2.2 ms off at 4:41.6, `tempo-change` | one section | lines within the merge tolerance merged after the off-beat decisions |
| AZKi / 明けない夜があったなら | `tempo-change`, `unsteady` | `phase-cut`, `unsteady` | the new flag |
| Orange Pekoe / Tonight: Hoshi No Furu Yoru Ni | 138.02, `octave-uncertain`, `unsteady` | 138.02, `unsteady` (margin 1.1 → 32) | the line's level by kick and snare: its snares had sat on its half beats |
| Orange Pekoe / Introduction (36 s, no steady beat) | 80.26, `octave-uncertain`, `low-confidence`, `unsteady` | 156.38, `phase-uncertain`, `downbeat-weak`, `unsteady` | the kick veto; neither reading means much |

All seven album mates of Determination are now 172-180, and none of the 58
grids has two beats closer than half a period. On the 150-track sample the
same changes left the agreement with dj as it was (110 equal, 11 octave, 29
other; downbeat 72), moved one phase into agreement (86), fixed the one
labelled octave rxbox had wrong (INTO THE LIGHT: 83 of 83), and otherwise
changed only flags.

### What was tried and dropped

- Octave by "balance × subdivision" on kick+snare onsets with a phase from
  the DFT's fundamental: the fundamental's phase is the hits' centroid and
  sat 42 ms before the kick on house; many house tracks halved.
- The same on the full band: off-beat hats are as strong as kicks in flux;
  every house track looked "too slow".
- The kick band as a general "too slow" veto: an off-beat bass fills it;
  drum & bass halved. Kept only for four-on-the-floor one level up.
- The earliest rise among all bands as the attack: a rolling bass in the low
  band starts before the kick.

## Known weaknesses

- **Octave and family remain the main errors** (none of 83 labelled; more on
  J-pop and hard dance, where the right answer is often a convention, and
  3:2 readings like Kneading that neither analyser settles). The
  `octave-uncertain` flag marks most of them; ×2 and ÷2 in the interface fix
  them in a click.
- **Live recordings.** Rubato jazz and live pop get sections or `unsteady`
  grids with low confidence. The grid is the best piecewise-straight line;
  a beat-by-beat grid (rbxport's walked ramps) is not implemented.
- **Ramps** are approximated by the sections the change-point search keeps
  (each at least 32 beats), not followed beat by beat.
- **DJ mixes** work but are not what it is tuned for: a 65-minute mix of
  pop songs comes out as one section per song where the tempogram finds the
  song's tempo settled (confidence 0.88), in 11 s of CPU. An hour at 44.1
  kHz costs 176 MB above the decoded audio (635 MB mono); see "Memory".
  Going lower needs a decoder that streams.
- **Downbeat** has no ground truth beyond "the music starts on beat 1"; the
  structure vote is rbxport's method, which they measured at 153 of 155.
- **Sub-millisecond phase** is relative to the half-energy point of the click
  band. rekordbox's own placement convention may differ by a millisecond or
  two; the interface's nudge covers it.
- **Speed**: the STFT dominates. A 100 frames/s front end would cut it by a
  third; a Vulkan front end could take it off the CPU.

## Reproducing the evaluation

```sh
meson setup build-beats
ninja -C build-beats src/analysis/test_beats tools/rxbox-eval
meson test -C build-beats --no-rebuild --suite analysis beats

# dj's analyser over the sample (re-executes itself under dj's venv)
tests/analysis/py_beats.py -j 6 -o build-beats/eval/dj.csv -l build-beats/eval/sample.txt

# rxbox over the sample, compared with dj
build-beats/tools/rxbox-eval -v -j 8 -l build-beats/eval/sample.txt \
    --csv build-beats/eval/rx.csv --compare build-beats/eval/dj.csv

# one track in detail: stage timings, sections, octave, the onset signals
build-beats/tools/rxbox-eval -vv --dump-onsets build-beats/dump/x FILE
```

`rxbox-eval` decodes with `rx_decode` (mono, the file's own rate, as
`rx_analyse` sees it); `--ffmpeg` pipes from the ffmpeg command line at
44.1 kHz exactly as dj decodes. `--bpm-range LO HI`, `--no-sections`,
`--from CSV` (compare without analysing) are also there. The scripts used
for the hand checks (`judge.py`, `foldcmp.py`, `pattern.py`, `octfit.py`,
`octacc.py`, `startbar.py`) were kept in `build-beats/eval/`, outside the
source tree.
