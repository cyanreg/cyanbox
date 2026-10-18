# The beat-grid analyser

How cyanbox finds a track's beat grid, why each step is the way it is, and how
well it works. The grid is made in two stages: cyanbox's own analysis of the
audio (sections 1 to 11), then the grid made with Beat This!, a neural beat
and downbeat tracker run on the GPU, from that analysis and the network's
activations (section 12, "With Beat This!"). The code is in `src/analysis/`
(`spectrum.c`, `onset.c`, `start.c`, `tempo.c`, `grid_fit.c`, `walk.c`,
`downbeat.c`, `beats.c`, `hybrid.c`, `analysis.c`, the private interface
`beats_internal.h`), the network in `src/ml/beatthis.c` (docs/BEATTHIS.md);
the tests in `tests/analysis/test_beats.c` and `test_hybrid.c`; the
evaluation tool in `tools/cyanbox-eval.c` and `tests/analysis/py_beats.py`.

The result (`cyanbox/analysis.h`, `CBBeatResult`) is what a DJ player wants: a
piecewise-constant grid (one section per tempo or phase, each a start beat, a
BPM and a beat number) that begins where the music begins, a confidence,
flags saying what is uncertain, and where and how the music starts. Where
the tempo moves (a live drummer, rubato, a ramp) the grid follows the
music beat by beat instead: a *dynamic* grid, every beat with its own time
and local tempo, which the players play as they are (docs/anlz.md 4.3).

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
6. **Dynamic grids.** Where the attacks drift from a section's line beyond
   what a straight grid can hold, or bend around it, the stretch is
   tracked again at a period that follows the music, put on its attacks
   and walked: a robust local fit a bar either side of every beat gives
   the beat times a player would follow, its tempo moving by at most
   4 % a beat. Kept, window by window, only where they follow the hits
   better than the line; on a sequenced section only where it bends (a
   DAW's ramp), its runs off the line being another element (a fill),
   not drift.
7. **Phase.** Each section is put on the start of its hits with searches
   that shrink step by step, and snapped to a whole BPM when that moves no
   beat by more than 5 ms.
8. **Family and octave, on the grid.** A close 3:2 or 4:3 family is fitted
   too and the grid whose beats land on hits wins. The octave (half, same,
   double) is judged by where the snares and kicks fall on the fitted beats.
9. **Off-beat and downbeat.** Kicks, structure and the music's start decide
   whether the grid is on the beat or half a beat off; structure (novelty
   peaks at one to eight bars), bass-note changes and the music's start vote
   for beat 1. A dynamic stretch is judged on its own beats.
10. **Confidence and flags.**
11. **Where the music starts**, measured on the level: after silence or a
    noise floor, a hit or a rise. The grid's first beat is the first at or
    after it, and the automatic cue the first downbeat.
12. **With Beat This!** The network's beat activations choose the level
    and track the beats; cyanbox's grid, where it agrees or is only off in
    phase, is kept or moved onto them; its downbeat activation places the
    bar. Everything above is this stage's hints and arbiter.

It costs about 290 ms of CPU for an average 4.5-minute track (383 ms per six
minutes of audio), single-threaded, after decoding. dj's Python analyser took
5 s for the same. With a GPU the STFT and the envelopes run there (section 1)
and the CPU's share falls to about a third: 656 ms to 204 ms per file on the
58 acceptance tracks, on a busy machine ("Speed").

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
- **av_tx on the CPU.** Its fast codelets stop at 2^17 complex points (2^18
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

**On the GPU.** Given a compute context (`CBAnalysisOptions.compute`,
`cyanbox/compute.h`; the jobs pool, the CLI and `cyanbox-eval` make one when
there is a Vulkan 1.3 device, `--no-gpu` or `CYANBOX_ANALYSIS_GPU=0` says not
to, `--device` or `CYANBOX_GPU_DEVICE` picks one), the STFT and the envelopes
run as compute shaders (`src/gpu/shaders/`). One workgroup per frame
windows 2048 samples into shared memory, transforms them (a Stockham
radix-4 FFT of 1024 complex points; twiddles computed on the CPU in double
precision), takes the power, the 64 mel sums and the kick band, and
compresses them as above; one workgroup per window and band tapers 8192
samples, transforms them, applies the band's gains, transforms back and
takes the smoothed energies (compensated sums, where the CPU sums in
double). The signal goes up 4096 frames at a time and the windows in
batches of 256, two in flight, so the GPU never holds a whole track; the
envelopes are asked for in batches (every tracked beat, a shifted section,
the beats that strayed from the grid). Should the GPU fail, the CPU takes
over. Measured on one core before anything moved: the STFT was a third to
a half of the beat analyser's time, the envelopes around beats another
third on a track that needs 1700 windows (a quarter on one that needs
470); the bass chroma, 2 %, and the key's transforms (a small part of its
0.1 s) stay on the CPU, as do the waveforms (IIR filters, sequential).

Only this fixed, algorithm-neutral part runs there. The GPU is handed the
tables `spectrum.c` builds (the window, the mel and kick weights, the taper,
the band masks) and returns band energies; the profile sums, the flux and
everything after them (onsets, tempo, tracking, attacks, sections, phase,
downbeat, confidence, flags) stay plain C on both paths, exactly as they
were. Changing the analyser never means changing a shader; changing the
front end's tables changes both paths at once.

**The same results.** The two paths agree to float rounding, not bit for
bit: the transforms round differently in their last bits. `test_beats`
runs every synthetic track on both when there is a device: each must pass
on its own, and the two grids must be the same (sections, BPMs, beat
numbers, flags and where the music starts identical, the starts within
0.1 ms). Measured on the RX 6900 XT (RADV) and the Iris Xe (ANV): all 22
the same, the starts within 0.3 ns, the BPMs identical. The front end's
arrays are compared directly, within 1e-5 of each array's largest value:
an fp32 FFT of 2^11 points is accurate to about eleven float epsilons of a
frame's largest bin, and the log compression turns a relative error of an
amplitude into an absolute error no larger, so the bands, and the fluxes
summed from them, differ by about 1e-6 of their largest values; both GPUs
stay under 1e-6 (envelopes under 2e-7). Most of that is the CPU's: against
av_tx in double precision the GPU kernels are within 2e-7 at every size
from 64 to 8192 points (`tests/gpu/test_compute.c`). `CYANBOX_ANALYSIS_GPU=0` or `1`
forces either path in the test, the tools and the app;
`CYANBOX_GPU_VALIDATION=1` runs the GPU half under the Khronos validation
layer (with synchronisation checks; it reports nothing).

On the 58 acceptance tracks (`cyanbox-eval -j 2`, once on each path), 56
grids were identical to the CSV's precision, flags and starts of the music
included. Two first beats moved: AZKi's エントロピー by 10 µs (the fifth
decimal rounding the other way) and Orange Pekoe's Adagio, a live
recording, by 30 µs: its line before the on/off-beat decision sat on the
other half of the beat on the GPU (the tracker near a tie), and that
decision moved it back, so both grids end on the same beats at the same
BPM, with confidence 0.999 instead of 0.988.

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
  their cross-correlation (within ±8 frames, far short of an off-beat); a
  band with nothing in common with it at any lag (a silent one) stays.
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
on the fitted grid (section 8).

**The tempogram** (`cb_tempo_regions`): 12-second windows every 3 s, each
window's autocorrelation comb over the octave around the working tempo (the
comb sums the beat, two-beat and bar lags, so a half-time stretch still
reads as the main tempo). A *region* is a run of at least four windows
(21 s), each within 1.5 % of the run's first window, whose tempo is more
than 3 % from the main one and not 3:2, 2:3, 4:3 or 3:4 of it within 1 %
(rbxport's rule: triplet and dotted passages read at those ratios; 128 to
174 is 1.9 % from 4:3 and passes); the windows are rhythmic and support the
main tempo poorly there. The region's tempo is the run's median, and the
tracker follows it there. If most windows settle on another tempo that is
not one of those ratios, that becomes the main tempo (the ratios are left
to the family check of section 8, which fits both). In the final grids of the 150 evaluation tracks, four have
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

After the off-beat decisions (section 9) two neighbours whose lines end up
within the merge tolerance again are merged: one line holds the other's
beats within 15 ms at both of its ends (the lines are straight, so the ends
are the worst). That is a stretch tracked on the off-beat that its own
decision moved back, or a "re-entry" a few ms late. Such a cut is no cut a
DJ could hear, and on a stick it is two beats a few ms apart, every downbeat
after it a beat early (an acceptance trial found one at 2.2 ms on a Photon
Maiden remix, flagged `tempo-change`). The line kept is the one with more
attacks, if it holds both; otherwise the other one, if it does.

**The exported beats.** The first section's start is the grid's first beat:
there is none before it (`cyanbox/model.h`), and the analyser puts it at the
first beat at or after the start of the music (section 11). A section's
beats run up to the next section's
start, less the last one when it lies within half a period of that start
(`cb_grid_beats` in `src/core/grid.c`, rbxport's SAME_HIT): that beat and
the new section's first are the same hit. A section boundary that survives
therefore never puts two beats closer than half a period, and the beat count
across it stays right (a section that continues the count numbers its first
beat after the last beat kept). `test_beats` expands every synthetic grid
this way and fails on two beats closer than half a period or a section
downbeat not numbered 1; `cyanbox-eval` counts such grids ("grids with two
beats closer than half a period": 0 of the 150 sample tracks and 0 of the 58
acceptance tracks).

**The boundary** sits on the first beat the new line explains: at a change
made on the beat (a DJ edit cut on the bar line), the beat at the join lies
on both lines and the search gives it to the old one, so it moves back while
the old section's last attacks fit the new line within 3 ms.

## 6. Dynamic grids (`walk.c`, `beats.c`)

A live drummer, rubato, a band without a click or a wobbly tape move the
beat by far more than a straight line can hold: a 1 % wobble over sixteen
bars at 120 is 300 ms. The sections of section 5 can only cut such music
into straight pieces of 32 beats or more, each off the beat somewhere; the
old `unsteady` flag said as much and gave up. Now the measured beats
themselves become the grid there (`CBTempoSection.measured`,
`cyanbox/model.h`): one measured beat per beat, each with the local tempo
of its interval, which is what rekordbox's "dynamic" analysis writes and
every player plays (PQTZ, docs/anlz.md 4.3). A sequenced track keeps its
straight grid, bit for bit.

**What is read.** After the sections are fitted and put on their attacks,
each section's attacks are measured again around the *tracked* beats (the
line's own 35 ms search loses the beats a drift has carried furthest),
attacks lighter than a quarter of their median dropped as the
segmentation drops them, and the *strain* of the line on them read
(`cb_line_strain`): a robust local line through the attacks eight beats
either side of each (Tukey's biweight on the section's scatter), its
residual from the section's line, and the running median over nine of
that: the *deviation*, how far the music sits from the line along it; the
*drift* is its largest value. And the *bend*: a quadratic through the
attacks (weights from the line's own residuals, so a stretch half a beat
off does not pull it), read as its rise between the section's middle and
its ends; the bend white scatter alone would give is known (σ√(180/n)/4
for n attacks, the standard error of the quadratic term over uniform
abscissae, scaled to the ends), so a short live section is not called
bent by its own scatter.

**When the beats are kept.** A section is *bent* when the bend exceeds
10 ms and four times its scatter's, and the line is 8 ms off somewhere:
the tempo moves inside the section, and the whole section is dynamic (a
ramp cut by the change-point search into 32-beat pieces leaves every
piece bent). Otherwise, where the drift passes 15 ms, the merge tolerance
of the sections (below it two lines are one; above it a DJ hears the
flam), each run of beats off the line by more than 5 ms that reaches
15 ms is dynamic, with four beats of margin either side (the tracker's
counts, which the reading rests on, go wrong exactly where the music has
moved most), unless:

- the section is sequenced. Sequenced music does not drift, and a
  stretch of its attacks off the line is another element being timed (a
  breakdown's claps, an element swelling in, a fill's accents); the bend
  reads a DAW's ramp, and a run at the edge of a bent or dynamic
  neighbour counts all the same (a ramp the change-point search cut
  where it was still within a line's reach: the ramp case of
  `test_beats` ends its first section with fifteen beats curving away,
  39 ms by the last). Sequenced is either of two readings. The attacks
  spread around the line by under 3 ms (`refine_section`'s "steady").
  Or, whatever that spread, the attacks *on* the line sit there as a
  sequencer's do (`online_scatter`): over every three consecutive beats
  whose attacks lie within the merge tolerance of one point of the
  line's lattice of beats and half beats (the half of the beat is
  decided later, section 9), the second differences of the attacks,
  their lower quartile read as a scatter (over 0.3186 √6, which reads
  white scatter σ as σ, as the median over 0.6745 √6 does) under 1.5 ms,
  on 32 such triples at least. The spread is no proof of live playing:
  runs off the line inflate it, the very runs this rule is about
  ("Felicidade", below). The lower quartile because a fill's accents or
  a hand drum's slaps land near the beats among the sequencer's hits,
  and the median reads the mixture: a quarter of the triples spaced
  evenly within a millisecond is a sequencer, and a drummer's lower
  quartile is his scatter as his median is. The rubato case of
  `test_beats` (3 ms of jitter) reads 3.0 ms, the other sections of the
  acceptance list whose spread says live 2.5 to 16 ms (five have too
  few such triples to read), Felicidade 0.57 ms (its median 2.0 ms);
- the run's deviation sits still: its longest stretch within 3 ms (or
  the section's scatter) of one level covers two fifths of the run and
  eight beats. A breakdown tracked on its off-beat sits half a beat from
  the line for sixty bars on AD:HOUSE 11's "Hot Vacation", every attack
  within a millisecond of −233 ms, flat but for the smoothing's eight
  beats at each end; drift keeps moving;
- the run's attacks scatter beat to beat three times more than the
  section's (and 3 ms): the attacks jumping between elements (a
  breakbeat's kicks and snares), not the tempo moving.

A straight remnant shorter than 32 beats (64 between two dynamic
stretches) joins the stretch beside it; the others keep the parent's
line, which held them within 5 ms and is snapped and on the fold.

**The walk.** Ellis's tracker held to one period puts extra beats on the
hats when the music slows by a fifth (the rubato case of `test_beats`:
two inserted in eight bars, the count wrong from there on), so a dynamic
stretch is tracked again first at a period that follows the music: at
every tracked beat the median of the intervals between consecutive
attacks within eight beats of it, those within 40 % of the line's period
(an inserted beat makes a half interval, a skipped one a double), and the
same tracker over the stretch's frames with that period, from a quarter
period before its first beat to a quarter past where its last belongs
(the tracker ends on the best score within its last period: a range
reaching further lets a hat end it half a beat short). The first beat
kept sits on the old count within three tenths of a period, so the
numbering through the stretch holds; the sections after move by the
difference in count. Then the attacks within 25 ms of the new beats, and
the walk (`cb_walk_beats`): a robust local *quadratic* through the
attacks a bar either side of every beat, evaluated at the beat. A local
line sits off a tempo that moves by four times its curvature per beat
squared (the window's second moment), 17 ms at the steepest point of the
rubato case; the quadratic has no bias. It follows a tempo that moves
inside a bar or two, as a player following a drummer does, and averages
the drummer's scatter rather than reproducing it (a grid that copied
every early and late hit would make the deck's beat counter and the
player's sync jitter with it). An attack not where its neighbours say
gets Tukey's weight of nothing; a beat without an attack, or a breakdown
without any, is interpolated linearly between the nearest beats that
have them; the ends are extrapolated on the last local slope; no two
beats closer than a third of the median interval survive. And the tempo
keeps its continuity: the local tempo (60 over the interval to the next
beat) changes by at most 4 % from one beat to the next and stays within
a factor 1.2 of the stretch's median. While some interval breaks that,
of the beats with attacks around it the one furthest from where its
neighbours put it is no beat, and the walk goes through it by
interpolation; the ends continue the first and last interval within
4 %. A drummer's rubato stays well inside: the random walk of
`test_beats` walks at 3.0 % a beat at most, the rubato dip at 2.1 %, the
ramp at 0.3 %, and all three keep their grids; the walk that followed
Felicidade's fill went 128, 123, 117, 105, 120, 129 and 140 BPM from
beat to beat. Each beat's tempo is 60 over its interval; the last's that
of the interval to the section after.

The re-tracking runs on the accent, and moves the tracker's frames by the
fold's delay, of the grid kept. Until these rules came, a runner-up
family fitted and dropped (section 8) left its own behind: the stretches
were tracked on its accent weighting and every re-tracked beat moved by
the difference of the delays (1.2 ms on Felicidade, whose family check
tries 97.5 BPM).

**The judgment**, window by window. The walked beats are kept only where
they follow the hits better than the line they replace, read in the
confidence's 16-beat windows (every 8 beats): in each, the distance of
the average beat's rise from the beats, on envelopes cut at the beats
themselves and at the line's beats alike (judged on envelopes cut around
the tracked beats, a line far from the music has no data where it is
wrong and scores on the windows where it is right), a window whose
average beat rises by less than half the walked beats' median contrast
counted at the search width (`cb_fold` reports no rise as an offset of
0). The confidence's own yes-or-no at 15 ms is too coarse here: a line
20 ms off scores as the beats do.

- A window is *lost* where the line holds its hits within the merge
  tolerance and the walked beats lie further from them than the line
  by as much again and 3 ms: twice as far, audibly so where the hits
  are clear. A lost window goes back to the line: the classification
  loses its beats, the section is split anew from the state before (so
  the straight remnants share the parent's line and join it as
  before), the shorter stretches are re-tracked, walked and judged
  again, until none loses a window; what is left of a stretch shorter
  than a window goes back too.
- A stretch hands over to a straight piece of its line beside it on the
  same beat: its end beat within the merge tolerance of that line's
  beat of the same count, or the window at that end is lost. A stretch
  that ended off the line's phase or count put two beats too close at
  the join, or a gap: on the acceptance list the walked grids of
  "Taiyo no Kakera", "Chaotic inner world", "明けない夜があったなら"
  and "Fortune Land E" had intervals of half a period or one and a half
  at their joins (Taiyo no Kakera at 120 BPM: 742 ms into its stretch,
  250 ms out of it).
- A stretch that loses nothing is kept when it brings the hits a
  millisecond closer than the line on average; the line is the simpler
  grid and a tie is its, and the tracker's beats then come back as they
  were.

Why not the stricter rule, every window won? A fold of 16 beats is noisy
in live playing, and blind to a drift that covers less than half of its
window: on the random-walk case the line crosses the drifting music and
five windows tie within 2 ms, and on the rubato case the first window
ties while the line slips 50 ms behind the music in its second half.
Given back to the line, such windows cut the stretches into pieces whose
joins lost beats: the rubato case went from 1.1 to 28 ms RMS and missed
6 of its 48 downbeats, the random walk from 3.3 to 7.4 ms and 18 of 64.
With the rule above the three cases keep their grids, and a walk that
wins on average while it loses a bar by 300 ms, as Felicidade's did, is
kept nowhere near that bar.

**Phase, downbeat, the rest.** A dynamic stretch is a section like any
other to what follows, on its beats' own times: the half beats are the
beats and the midpoints after them, the kicks are weighed on those
(`support_at`), the structure votes over them (`cb_downbeat_vote_times`,
the line version a wrapper that builds the same arrays), a move by half
a beat puts every beat on the midpoint after it and walks again, and the
downbeat is counted from the stretch's first beat as from a line's t0.
But a stretch cut from a line and handing over to its straight pieces on
the same beat takes their half of the beat (that of the one with the
stronger evidence, should the two disagree; the straight sections decide
first): deciding its own, it moved half a beat on "Taiyo no Kakera" and
"Chaotic inner world" and put both joins half a beat apart. The
confidence folds a dynamic section on its beats. A boundary between a
dynamic stretch and a straight section is no tempo change or phase cut
(the `dynamic` flag covers it); `unsteady` on a dynamic section is the
scatter around the walked beats. The grid lists a measured beat per beat,
the first with the section's number, the rest continuing the count; a
dynamic first section reaches back to time 0 at its first interval as a
line does, before the music's start moves the grid on. The last measured
beat of a track runs on at its tempo. Nothing of this runs with
`allow_dynamic` off (`cyanbox-eval --no-dynamic`), and nothing of it touches
the GPU front end: the decisions stay in C on both paths, and the two
agree on every synthetic grid, dynamic ones included.

**Felicidade.** AD:HOUSE 11's "Felicidade" (2.01, latin house) is
sequenced at exactly 130.000 BPM: the segmentation's merges report
130.000 BPM with 0.6 ms of jitter and 129.998 with 1.0 for many of its
segments, and the straight fit is one section, beats 0-620 at 129.9935
BPM (snapped to 130.000 later), confidence 0.50. Percussion is played
over the sequencer, and at the dynamic pass the line stood on the
off-beat hats (the half of the beat is decided after it): the tracked
beats sat half a beat from it for long stretches and followed a
syncopated fill's accents at 39-47 s, and the attacks spread 6.9 ms
around the line, 7.3 ms beat to beat, so the section read as live. The
pass read 375 ms of drift, runs at beats 56-114, 120-170, 173-259 and
328-592 (scatter 11.1, 14.6, 15.2 and 4.6 ms: none a plateau, none
ragged), joined them with their margins and the remnant rule into one
stretch, beats 52-621 (570 measured beats, local tempo 105.3 to 155.3
BPM), and kept it because the walked beats put the hits 8.7 ms from the
beats against 9.9 for the line, over the whole track. By ear (the
user's) it is wrong from 39.3 to 47.2 s, a fill over a steady pulse that
leaves 4/4, not a tempo change. The strong onsets fall at 44.113,
44.580, 45.113, 45.733, 46.200, 46.733 and 47.193 s (467, 533, 620, 467,
533 and 460 ms apart), the last two on the straight line's beats (46.732,
47.193). The walk went 105 BPM at 39.9 s and 140 at 41.4, put beats 98
and 99 (45.059, 45.524) 57 and 59 ms before their attacks and beat 102 at
46.893 against the pulse's 47.193, and sat half a beat off from 50 to
56 s; before 39 s the walked and straight grids agree within 3 ms.

Now its attacks on the line read 0.57 ms beat to beat from their lower
quartile (the median reads 2.0), the section is sequenced, no run
counts, and the grid is the straight one, bit for bit the `--no-dynamic`
result: 130.000 BPM from 0.578 s, beats at 46.732 and 47.193 s. Without
that rule the other two bring it back to the same straight grid as
well, in eight rounds of lost windows and joins (550 ms of decisions
against 200). `test_beats` has the case: a sequenced 130 BPM click track
with a two-bar fill at Felicidade's gaps must stay one straight section,
its beats through the fill within 3 ms of the pulse (1.2 ms); and a walk
through attacks that leave a 130 BPM pulse for a dozen beats must step
its tempo by 4 % at most (3.8 %; 5.5 % without the rule). The click
track does not reproduce the failure itself: its attacks spread 0.8 ms
and the old rule already called it sequenced; what made Felicidade's
spread read as live (the hand percussion, the line on the off-beat at
the time) a synthetic track did not reproduce without wrecking its line.

**What it costs.** On the 58 acceptance tracks from the harness's cache
(the front end on the RX 6900 XT, `-j 2`, the machine otherwise quiet):
108 ms of CPU per file with the dynamic pass against 103 without it in
the same binary (the strain of every section; the re-tracking, attacks,
envelope cuts and walk of the dynamic stretches only), 126 against 123 ms
of wall time for the decisions; the baseline binary of the day took 115
and 133 in the same session. About 5 % on the decisions, well inside the
doubling the feature was allowed. The window-by-window judgment walks a
section again for every round of lost windows (up to five on the
acceptance list, on "Happy Valley"), only where there is a stretch to
judge: with the machine busy (load 20 to 30), the 58 tracks took 132 and
157 ms of CPU per file before these rules and 135 and 143 after, in
alternating runs, the same within the noise.

## 7. Phase and snapping (`beats.c`, `grid_fit.c`)

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

## 8. Family and octave on the grid (`beats.c`, `tempo.c`)

**Close families.** When the runner-up family's harmonic strength is within
25 % (3:2, 4:3 readings of a syncopated track), its grid is fitted too, and
it replaces the first only if its beats collect at least 10 % more of the
arbiter onset *and* it is as clean a grid (no more sections, no more
jitter): a 2-step garage track at 3/4 of its tempo collects hits, but only
with a ragged grid. Both grids are read on one band: the kick band when
either fit has it as its arbiter (kicks carrying the beat at its tempo),
the full band otherwise. Each grid used to be read on its own fit's
arbiter, and the kick share can cross a half between two tempos a 3:2 or
4:3 apart: one grid was then scored on the kicks and the other on the
full band, unlike quantities. The kicks mark the beat where a track has
them, while the full band, hats on every subdivision, favours neither
grid. On the 58 acceptance tracks the check runs on three, each with
kicks at both tempos, and on 18 synthetic cases, two of them with a kick
share under a half at the runner-up (0.44, 0.21) whose decision stands
either way: no grid changed.

The runner-up also wins, whatever its grid's support, where the full
band repeats at least twice as strongly at its beat, two-beat and bar
lags (the comb `cb_tempo_estimate` finds its peaks with, whole track):
a grid fitted to the off-beat hats rather than the kicks can collect
fewer kicks than the impostor's, and a ragged one can fail the
cleanliness test, but neither repeats. "plan goes on" and "In My House"
(128 by ear), read at 96, the 3:4 family whose fourth harmonic is the
eighth-note triplets at 128: combs 1.07 at 128 against 0.12 and 0.46 at
96 (`CB_BEATS_VERSION` 8). Where the check runs on the lists checked and
the runner-up is not the beat, its comb is the weaker (0.22-0.76 of the
first) or within a third (1.29, 1.36).

**The octave** is decided on the fitted grid, whose beats sit on the hits
(rhythm evidence needs that; a whole-track estimate does not have it). Half,
same and double are each judged by:

- **Too slow**: the snares and claps (mid band) fall on the half beats about
  as strongly as on the beats. At 87 for drum & bass they are its snares on
  2 and 4; at 64 for house its claps. Likelihood (1 − subdivision)/0.5.
  Not where the next level up has a *lone snare* (below): its half beats
  then hold eighth-note syncopation, not the next level's backbeat.
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

"A kick on every beat alike" is read two ways: as many kicks on the even
beats as on the odd ones (min/max 0.7 or more, dj's alternation), and on
each of the four beats of a bar (the weakest at least 0.4 of the
strongest). The parity alone hides a beat in four: a pop kick on 1, the
"and" of 3 and the "and" of 4 at 100 is, at 200, 0.68, 1.73, 2.42 and 2.31
by the bar ("Love me, myself", STYLEY NiGHT ii), 0.77 by parity, and it
vetoed 100; on Clean Tears' "Skyscraper" eighths in the low band did the
same and marked 196 a four-on-the-floor (0.77 by parity, 0.27 by the bar).

The **lone snare**: per position of a bar (four beats, four half beats),
the snare is the mid band above its mean where the low band is not above
its own (mid/mean − low/mean; a kick's body reaches into the mid band, and
on drum & bass the strongest mid-band beat can be the kick on 1). A level
whose strongest snare beat (0.5 or more) has nothing like it two beats on
(1 − that/it ≥ 0.8) is the half-time reading of the level below, whose
backbeat it is: "Love me, myself" at 200 has +1.85 on one beat and −0.17
two beats on, against +1.13 and +0.90 at 100 (snares on 2 and 4). The
lower level is then not "too slow" by its snares (subdivision 0.78 at
100, likelihood 0.44: hats, the kick's "and"s and other eighths fill its
half beats). Drum & bass at 172-180 pairs its snares (lone 0.08-0.59 on the eight
acceptance tracks, or a snare under 0.5, no evidence), so its 87 reading
stays too slow. The kick veto is not touched: the kick band speaks for
itself.

When another level wins, the grid is fitted again there. A track too short
for a grid at that level (16 beats) keeps the grid it had, flagged
`octave-uncertain`: its octave margin is then the reciprocal of the
decision's, under 1. A kick and clap loop at 87 of 9 or 10 seconds has 27
or 30 beats at 174, where it is fitted first, and the octave decision
chooses 87 (margin 3.0), where the tracker finds fewer than 16: such a
loop used to get no grid at all, flagged `low-confidence`.

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

## 9. Off-beat and downbeat (`beats.c`, `downbeat.c`)

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
worth one scale, all 68 such tracks do. "Where the music starts" was dj's
first loud onset; where the music starts after silence with a hit carrying
a kick, or fades in on a beat, the start is now a witness of its own,
outside the structure (section 11).

**On the beat or off it?** Two witnesses as log evidence: the arbiter band
(kicks on the beats against on the half beats, in a window reaching 30 ms
back and 10 ms forward, since a kick's low band leads its click) and the
structure (best beat position against best half-beat position, weighted 1.5:
rbxport found structure right on 153 of 155 reference grids, the kick alone
on 150); and, where the music starts after silence, the start (section 11).
The grid moves half a beat only when the sum says so by a factor 1.25; the
envelopes are cut again where the beats now are and the section is refined
again.

**Beat 1** is the best of the four beat positions, never a half beat. A later
section that starts on the previous one's grid continues its count (beat
field 0) unless its own vote clearly says otherwise; a section after a phase
cut numbers itself.

## 10. Confidence and flags

**Confidence** (dj's): the share of 16-beat windows (every 8 beats) whose
average beat starts its rise within 15 ms of the grid, weighted by how
clearly it rises (contrast less half the median contrast). Three changes.
The weight is capped at twice the median: in a breakdown the average beat's
level is tiny, its contrast explodes, and a few windows whose loudest rise
is a riser or a crash outweighed thirty bars of kicks. The envelopes are
cut again at the grid's own beats wherever a tracked beat strayed more than
25 ms from it, so the confidence reads the grid as written rather than
where the tracker happened to be (empty windows had given one track 0.00).
And the rise must be one: at least half the window's contrast, or half the
median contrast if that is less, from the lowest level within the search
(an eighth of a beat, at most 50 ms) before its steepest point to the
highest within it after. The steepest point of the average beat is taken
however small, and the contrast spans the whole fold (±50 ms): beats 60 ms
after their hits fold the hits' decaying tails, a contrast of 13 to 33 on
a kick and click loop, and find a ripple near the beats, a rise under 0.01,
which counted as on the grid (0.87 on that loop's mid band, 0.20 on its
high band; now 0). The less of the two halves, because a quiet window's
hit rises by half of its own contrast but not of the median's, and a hit
after something louder in the same window by half the median's but not of
its own. Read only 10 ms either side (where the attack is timed), the
rise of a drummer's scattered hits falls short: `test_beats`' live drums
(8 ms of jitter) would read 0.85 instead of 1.00 on a grid that is right.
The weight is still the contrast, not the rise: a window whose
beats sit in a tail has a beat, not where the grid is, and weighted by
the rise it found it counts for nothing (a grid half on that loop's hits
and half 60 ms late read 1.0 so; it reads 0.30 to 0.39 now, 0.39 to 0.90
before). On the 58 acceptance tracks two confidences moved, each by one
window whose rise at the grid is a third of its contrast and under half
the median's: "CYBERPROTOCOL" 0.976 to 0.972 and "ag - Weary" 1.000 to
0.970 (a window at 4:17); no flag changed, so `low-confidence` means what
it did. Still
out of its reach: a grid off by more than the fold reaches everywhere,
whose windows hold no hit at all and so set their own median (one 100 ms
late on that loop reads up to 0.24, as before).

Flags (`CBGridFlag`), and what they should mean to the DJ in the interface:

| Flag | Set when | Meaning for the user |
| --- | --- | --- |
| `tempo-change` | a section at another tempo than the one before it (more than 0.5 % apart) | The grid changes tempo; check each boundary. |
| `phase-cut` | a section at the same tempo as the one before it, re-entering shifted | An edit or a skip: the grid jumps phase at the boundary; check it. |
| `octave-uncertain` | the octave decision's margin < 1.5, or the octave it chose has no grid (section 8) | Half or double tempo was almost as likely: try ×2 / ÷2. |
| `phase-uncertain` | on-beat vs off-beat evidence < 1.5× | The grid may be half a beat off: listen to the first kick. |
| `downbeat-weak` | best beat position < 1.2× the next, or the section is too short to vote | Beat 1 may be another beat of the bar: use "set downbeat". |
| `no-kicks` | the kick band carries less than half the full band's periodicity at the beat rate and at twice it, and less than all of it at half the beat rate | Timed on clicks and claps; less precise. |
| `low-confidence` | confidence < 0.5 | The grid does not follow the hits in many places: check by ear. |
| `unsteady` | in some section, the attacks spread around the line (or the walked beats) by more than 6 ms *and* scatter from beat to beat by more than 4 ms | Live playing: beats scatter around the grid. |
| `dynamic` | some stretch keeps its measured beats (section 6) | The grid follows the beats one by one; the BPM readout is the local tempo. |

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

## 11. Where the music starts (`start.c`, `beats.c`)

Nearly every track in the library starts after some silence: of the 150
sample tracks, 142 have at least 60 ms of digital silence or a noise floor
before the music, mostly 0.1 to 1.4 s (a CD's pregap, an encoder's
padding). Where the music starts, and how, now decides three things: where
the grid begins, a witness for the half of the beat and for beat 1, and the
automatic memory cue.

**Measured on the level.** The onset signals are normalised to the whole
track (section 2), so a fade-in's first seconds are nothing to them, and
dj's rule for the start (the first frame of full-band flux above a quarter
of its 99th percentile, `cb_onsets_first`) finds the first *loud* onset: on
pushover's "PASSIVE feat. Shiina", a hit 3.6 s in and two seconds into the
fade, half a beat off the grid. `start.c` reads the level of the 6.7 ms
frames, smoothed over three (20 ms, so that a low note's cycle does not
make a frame look silent):

- the loud level: the 90th percentile of the frame levels;
- the floor: the commonest level (1 dB bins) of the frames before the music
  is surely playing (within 45 dB of the loud level), digital silence a
  level of its own. A floor lies at least 60 dB under the loud level and
  lasts at least 60 ms, or the music starts at once: an MP3's encoder delay
  (25 ms with LAME) is no silence, and a track cut out of a mix would
  "start" there with whatever is playing;
- the threshold: after digital silence, 65 dB under the loud level, so
  that a few ms of dither before the music (-90 to -100 dB) is not the
  music while a fade from -60 dB is, from its first hit; over a noise floor
  (dither, hiss), 20 dB over it but never lower than 50 dB under the loud
  level (-60 dBFS on a loud master), where a fade becomes audible over the
  floor.
  The level stays over it for a third of the next 200 ms: a click in a
  pregap is no start;
- **a hit**: two frames (13 ms) after the level crosses the threshold it is
  25 dB over it. Its time is the first sample at 3 % of its peak, above the
  floor's noise; a sine kick starting at zero phase is there within two
  samples;
- anything slower starts, out of digital silence, at its first sound (or
  where it rises 6 dB out of a short dither before it); over a noise floor,
  at the threshold. **A fade** takes a second or more to come within 15 dB
  of the loud level, **a swell** (a riser, a reversed cymbal, a pad's
  attack) less.

Over a noise floor the start is where the fade is heard, not where it
leaves the floor. "PASSIVE" leaves its -96 dB dither at 1.03 s, 23 ms before
a beat of its grid, and a first version started it there: its bar 1 and
cue stayed 0.7 s before anything can be heard, on the bar line the
structure had preferred (1.10 against 0.84). Its kick drum enters at
44.45 s, on beat 3 of those bars and on beat 1 of the bars from 1.979 s.
The fade is over -60 dBFS (50 dB under the loud level) for good at 1.87 s.
But the fade is only the way in, and the user heard at once what a first
version missed: the music starts at the hit at 3.60 s, 22 dB over the pad
within 5 ms, half a beat from the grid that version had fitted. So **a
hit after a fade** is a start like a hit after silence: the first
transient after the fade's origin (12 dB over four frames, at the music's
level) that stands at least 10 dB over the loudest of the half second
before it. It is a beat and beat 1 with the same weight (and once the grid
is on that half of the beat the structure agrees, +0.93 against -0.18 on
the other half). It needs no kick: after a fade the first loud thing is
where a DJ expects the track to start, whatever it is made of; after
silence the kick still tells an opening hit from a pickup. A transient at the fade's origin is a fade
with the beat under it (drums fading in, the synthetic fades), and a later
one that is not clearly over what came before is a louder kick in such a
fade; both stay a fade. The synthetic case: a pad fading in for two
seconds, then the hit on beat 1.

**The grid** begins at the first beat at or after the start, less 50 ms (a
line a few ms early on a hit): `cb_grid_start_at` moves the first section's
start forward by whole beats, every later beat keeping its time and number,
so bar 1 is the first downbeat at or after the start. dj, rbxport
(`first_beat_secs`: "the first grid position at or after time zero, as
rekordbox does") and rekordbox's own analysis extend the first tempo back to
the start of the file, beats and a bar 1 in the silence; cyanbox's grids no
longer do (`cyanbox/model.h`, `src/core/grid.c`). **The automatic cue** is the
first downbeat at or after the start (it was the first after dj's first
loud onset).

**Never before time 0.** Where the music starts at once there is no slack:
the first beat is the line's first at or after 0, so a track whose first
hit is its very first sample, on a line a few ms early, begins its grid on
the second hit (a kick loop at 128 whose line sits 2.3 ms early: its
first beat at 0.4665 s). On the acceptance list AD:Drum'n Bass 5's "Keeps
Me High" and AD:HOUSE 7's "A Small Happiness", which start at once, have
their line's beat 21 and 16 ms before 0. A beat before 0 is no beat on
the players' timeline: `cb_grid_beats` lists none (`src/core/grid.c`), so
the stick's PQTZ (its times unsigned milliseconds, docs/anlz.md 4.3), the
automatic cue and the waveform would never see it, and rekordbox's own
grids begin at the first position at or after zero (rbxport's
`first_beat_secs`). Putting it at 0 instead would move the whole first
section, every beat of it, by those few ms.

**Witnesses.** Section 8 had the start as one vote of one scale inside the
structure, at dj's first loud onset, and so only as a half-beat position.
On AD:Garage "Feel Alive" (134 BPM, digital silence until the hit at
0.7214 s) the hit sat half a beat from the fitted line; the kicks (log
evidence +0.70: a two-step kick, on 1 and the "and" of 3 in the first bars,
on several "and"s later) and the structure without the start (+0.79, times
1.5) outweighed the vote; the grid stayed half a beat off (margin 1.12) with
beat 1 in the silence, and for beat 1 the vote was wasted on a half-beat
position. Now, for the section the start is in:

- **An opening hit with a kick**, within a tenth of a beat (40 ms at most)
  of a beat or a half beat of the line, where the kick band does not
  itself mark one half of the beat by a factor e (log evidence under 1.0),
  is log evidence of 2.5 for its half of the beat and, when on a beat after
  the decision, a vote of two scales for beat 1. "With a kick": the kick
  band, from 15 ms before the hit to 60 ms after it (a kick's body builds
  after its click), rises at least twice as much as on the beats (or half
  beats) that carry the kicks: an opening downbeat, kick, crash and bass at
  once, not one more kick. Feel Alive's hit has 3.5 times the kick of its
  kick beats; the grid moves half a beat (margin 1.86), its first beat and
  beat 1 at 0.7230 s, 1.6 ms after the hit's first sample (the click band's
  half-energy point, section 4).
- **A hit without a kick does not count**: a stab or a vocal on the "and"
  before bar 1 has none. AD:HOUSE 12 "Need Your Groove" opens with a chord
  stab, 0.42 of its kick beats' kick, half a beat before a four-on-the-floor
  kick (+1.34). Counted, it put the grid on the off-beat of every kick
  (its strongest kick onsets 218 ms from the beats).
- **Nor one with an ordinary kick**: counted from the kick beats' level up,
  the witness moved three grids of the first 800 library tracks whose
  opening kick is 1.10 to 1.28 of their kick beats': two for the worse (on
  ADvantage "Encore" the strongest kicks went from 1.7 to 13.9 ms off the
  beats, 69 % of them locked to the grid, then 25 %, and the beat-to-beat
  scatter from 0.5 to 3.1 ms; on "Seimitsukensa" from Dance with Me the
  confidence fell from 0.89 to 0.31), one unclear (Chromosomal
  Reprogramming "Fragmentation": confidence up from 0.61 to 0.91, its
  strongest kicks from the beats to the half beats). The opening hits
  behind the other changes, Feel Alive and two Decade Archives tracks
  whose phase cut the move removed ("Universe", "Ink Drops"), have 2.3 to
  3.5 times their kick beats' kick.
- **Nor where the kicks are clear**: a kick and a crash on the "and" before
  bar 1 is a pickup. On AD:HOUSE 10 "Get It", AD:HOUSE 3 "KKE" and AD:HOUSE
  11 "Kiss Like This" the opening hit carries a kick half a beat from a
  kick band at +1.34, +1.40 and +1.14; counted, it moved their grids half a
  beat, and their strongest kicks went from within 10 ms of the beats to
  87 to 221 ms from them.
- **A fade**: its first beat, the grid's first, is beat 1 unless the
  structure prefers another by more than half a scale. dj's first loud
  onset keeps its vote too, being more often than not where the drums come
  in: as a full scale for the beat nearest the fade's first sound, the
  fade's vote moved beat 1 on 7 of the first 400 library tracks, and where
  the kick drum enters after the fade (the first four kicks a beat apart)
  was a bar line on four of them: the old beat 1 on three, the fade's on
  one. As log evidence of 1.0 for its half of the beat, the fade moved the
  grids of six of the first 800 tracks:
  better on two (on AD:HOUSE Winter 2 "nomophobia" the kick drum now
  enters on a beat, on "プラチナ・パスポート" the strongest kicks moved from
  89 ms to 5 ms off the beats), worse on two (the confidence of ADvantage
  "Whiteout" fell from 1.00 to 0.89, of "Light & Shadow" from 0.35 to
  0.00), unclear on two; it has no say on the phase.
- Everything else (the music starts at once, a swell, a start between the
  half beats of the line) votes as before: dj's first loud onset, one
  scale inside the structure. A swell leads into the downbeat rather than
  starting on it: on the sample, the old beat 1 is on the swell's start for
  only 7 of 43 swells after 0.25 s of silence, and the first loud onset
  after it (the downbeat it leads into) is what the old vote already took.

**How strong, and what was measured.** The thresholds were set on the
sample, the acceptance list and the first 800 tracks of the library (in
path order), against the analyser before this section, with the kick entry
(where the first four kicks a beat apart come in) and the locking of the
strongest kick onsets to the grid as checks where nothing else could tell.
On those 800 tracks the first version of the witnesses changed 17 grids
(2 %): the tempo of none except a low-confidence live track (160.078 to
160.093 after its half-beat move), two phase cuts that the move removed
(Decade Archives "Universe", "Ink Drops"), the rest phase or beat 1, each
listed above with what decided it. On the sample (150) and the acceptance
list (58) that version left every tempo and section as it was; the sample
changed beat 1 on three fades, the acceptance list nothing. Among the
placed opening hits with a kick in those 800 tracks, the rest of the
evidence (kicks and structure) agreed with the hit's half of the beat on
about nine in ten; where it disagreed, the old analyser had mostly put the
grid on the hit already through dj's start vote, and the witness changed
the phase only where it is described above. A whole-library comparison of
the final version was not finished (see "Known weaknesses").

## 12. With Beat This! (`hybrid.c`)

Beat This! (Foscarin, Schlüter and Widmer, ISMIR 2024) is a transformer
that turns a log-mel spectrogram into a beat and a downbeat activation for
every frame, 50 a second; cyanbox runs it on the GPU (docs/BEATTHIS.md,
0.2 s for a 4-minute track). It hears the beat where onsets mislead
cyanbox's own analysis: an off-beat bass taken for the beat, a half tempo,
the bar. But its own postprocessing picks peaks, so its tempo wanders and
changes level inside a track, and its beats wobble by 10-20 ms where a
line through attacks does not. So the grid is made from both
(`cb_beats_hybrid`), the network's activations and cyanbox's result for
the same audio (sections 1-11), which are its hints and its arbiter:

1. **The level.** For every 20 s window (every 10 s), the beat period whose
   grid best explains Beat This!'s detections (its local maxima over 0.5):
   the F-measure of grid points with a detection within a frame against
   detections hit, at its best phase, for every tempo from 60 to 240 BPM by
   0.25; summed over the windows, so drift between them does not matter.
   Where an octave of the best scores within 15 % (the activations
   alternate between the two), the level whose bar of 4 (or 3) Beat
   This!'s downbeats keep wins, else the one cyanbox found. Then folded
   into 80-201 BPM, and a reading of 88.5-100 that cyanbox hears at the
   double is doubled: J-core, hardcore and drum and bass, which DJs count at
   the double (below 88.5 the slow ones, chill-out, deep house, nu-disco,
   are as many).
2. **The beats.** Ellis' dynamic programming on the beat activation, with
   a period per frame (each window's best within 25 % of the level, a
   median of three windows); each beat on its activation's peak,
   parabolic between frames; the beats numbered by the periods between
   them, so a skipped one leaves its number out. A penalised least-squares
   segmentation (60 a section, the squared error in the beats' own jitter)
   splits them into constant-tempo sections; sections either side of a
   breakdown whose beats fall on the grid carried over are one; one line
   through the whole track when 90 % of the strong beats lie on it.
   Each section is fitted through cyanbox's beats where 60 % of it has one
   within 25 ms (they are a few times steadier), else moved by the 9 ms
   that Beat This!'s beats come later than cyanbox's on the same audio.
3. **The arbiters**, on cyanbox's grid at this level (its octave above or
   below made this one's):
   - *agree*: where three quarters of each grid's beats have one of the
     other's within 35 ms, cyanbox's grid;
   - *phase*: where cyanbox's grid has Beat This!'s tempo but another
     phase (an off-beat bass taken for the beat), its own tempo at the
     phase Beat This!'s beats keep on it (their circular mean, 80 %
     concentrated, in 75 % of 30 s windows), section by section after a
     phase cut. Moved only from 25 ms off when cyanbox was unsure of its
     phase (a flag, or a phase margin under 10), from a quarter of a beat
     when it was sure: Beat This!'s own beats stay within 20 ms of a right
     grid on 95 % of tracks, but were 49 ms late on one that cyanbox had
     right (MK's "Good Time (Extended Mix)", by ear). Not on a grid with a
     passage cyanbox measured beat by beat;
   - *evidence*: cyanbox's grid where it explains Beat This!'s detections
     as well (within 0.02 of the F-measure).
4. **The tempo family.** Where cyanbox's tempo is no octave of the grid's
   but explains Beat This!'s detections as well (within 0.05), the beats
   are tracked again at cyanbox's tempo (held within 4 % where its grid is
   one steady line): the fitting can settle on a tempo that is not the
   network's best.
5. **The bar.** Per section, the beat place where the downbeat activation
   adds up most. (Beat by beat, with a Viterbi path that can insert a short
   bar, was tried: it moved the bar in intros of drums alone, where Beat
   This!'s downbeats drift by half a bar.)

The grid is then sections: each run of eight or more beats on one line is
a straight section, any other beat a measured one (section 6), and, as
cyanbox's, it begins where the music begins (section 11): the first
straight section reaches back to the start of the music by whole beats
before the start cuts it, also where the grid is cyanbox's moved to Beat
This!'s phase, whose beat before its own first fell to cyanbox's start
(Clean Tears' "Spiral (Lily Remix)": its opening hit and downbeat at
0.156 s, cyanbox's line 77 ms early there, the grid from the beat after;
six of the 174 tracks of the sample began a beat or more after the music
so). Where Beat This! hears no beat (no 20 s window with four detections)
cyanbox's grid stands.
The confidence is how well the grid explains Beat This!'s detections (the
F-measure within 70 ms: within the arbiter's 50 ms it fell to 0.40 on
"Good Time (Extended Mix)", heard right, whose detections are 49 ms late;
within 70 ms no right grid of the osu! maps is under 0.6, and that one is
0.75); the flags are the grid's shape, `low-confidence` under 0.5, and
cyanbox's `unsteady`. It costs about 50 ms of CPU a track
after the network.

This was worked out in Python (`build-beats/eval/osu/hybrid.py`, frozen as
"fix16") and ported; the port gives the same beats as the script, to the
5 decimals they were printed with, on 1293 of 1294 tracks (the other
20 µs off), given the same logits and cyanbox results.

### How well

**osu! beatmaps.** A map's timing points are a human's grid, every beat
and the bar, made to play along to: references for beats and downbeats
that DJ music otherwise lacks. Maps with one tempo throughout, paired with
their audio (`build-beats/osu/`); beats right within 70 ms (F-measure,
each algorithm's constant latency removed), and "fully right" when that
and the downbeats are both at 0.9 or more:

| | cyanbox alone | with Beat This! |
| --- | --- | --- |
| **239 maps** (where it was developed) | | |
| beats and bar right | 137 | 175 |
| beat F, 70 ms / 20 ms | 0.838 / 0.813 | 0.909 / 0.894 |
| downbeat F | 0.702 | 0.892 |
| **623 maps held out** (never looked at while working on it) | | |
| beats and bar right | 344 (55 %) | 462 (74 %) |
| beats right (F 0.9 or more) | 66.3 % | 76.2 % |
| right up to the octave | 91.0 % | 96.1 % |
| beat F, 70 ms / 20 ms | 0.831 / 0.794 | 0.916 / 0.873 |
| downbeat F | 0.698 | 0.895 |
| wrong: the bar / off-beat / other | 69 / 23 / 52 | 13 / 2 / 24 |

The rest are octaves: 122 of the held-out maps are charted above 201 BPM
(or below 80), which the convention halves (doubles) by design.

**GiantSteps tempo** (414 tracks held out): Acc1 88.6 % (cyanbox alone
82.3 %, Beat This!'s own peaks 83.7 %, TempoCNN 87.9 %), Acc2 97.1 %.

**By ear.** AD:HOUSE Winter's ten tracks, whose grids were confirmed by
listening (two corrected so, in blind click tests): all ten beats and all
ten bars. Eight library tracks named as wrong in cyanbox's grid (MK's "Good
Time", ケンカするほど仲がいい！？ at half, nomophobia and Cruising off the
beat, Slowly's tempo and bar, 灯火's first 85 s): each one steady grid now,
at the tempo heard; "Good Time (Extended Mix)", right before, stays.

The whole pipeline in C, as the app runs it (`cyanbox-eval`: decoding, the
network on the GPU, cyanbox's analysis, this stage), scores the same on
the three sets, to the digits above. (`cyanbox-eval --ffmpeg` gives the
network the mean of the channels, as the app's decoder mixes them:
ffmpeg's own mono is 3 dB louder, and the network hears it, enough to move
nine of the 623.)

## Parameters

| Parameter | Value | Why |
| --- | --- | --- |
| STFT | 2048 / 294 at 44.1 kHz | kick band over 6 bins; 150 frames/s |
| Flux delay | 2 frames (13.3 ms) | log flux peaks early; measured |
| Band alignment | ±8 frames | per-track lag, far short of an off-beat |
| Tempo range | 50-220 BPM | dj's |
| Prior | 125 BPM, 0.4 octave, rhythm floored at 0.25 | 83 of 83 octave-labelled tracks |
| Comfortable range | 60-200 BPM without a requested range; ×1/4 outside | pop at 105 read as 210; drum & bass inside |
| Kick on every beat | parity ≥ 0.7 and the bar's weakest beat ≥ 0.4 of its strongest | pop at 98-100 read at double: 0.27-0.30; acceptance levels 0.45 up, but one that is no four-on-the-floor |
| Lone snare | strongest snare beat ≥ 0.5, the one two beats on under 0.2 of it (lone ≥ 0.8) | 200 of "Love me, myself" 1.00, 196 of "Skyscraper" 0.85; drum & bass at its tempo 0.08-0.59 |
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
| Family check | runner-up within 25 %, 10 % more support, both on the kick band if either fit's arbiter is it | 3:2 and 4:3; like with like |
| Unsteady | spread > 6 ms and beat-to-beat scatter > 4 ms | live drums scatter 5-15 ms, sequencers < 1 |
| Dynamic: drift | deviation > 15 ms (runs from 5 ms, 4 beats of margin); live (spread > 3 ms and the attacks on the line not sequenced), or at a dynamic neighbour | the merge tolerance; sequenced music does not drift |
| Dynamic: sequenced | the attacks on the line (triples within 15 ms of a beat or half beat), lower quartile of the second differences as a scatter, < 1.5 ms on 32 triples | runs off the line inflate the spread; Felicidade 0.57, the rubato case 3.0 |
| Dynamic: bend | > 10 ms and 4× the scatter's, with 8 ms of drift | a ramp's 32-beat pieces bend 20-40 ms |
| Dynamic: plateau | longest flat stretch (3 ms or the scatter) ≥ 2/5 of the run and 8 beats | an off-beat breakdown is flat at half a beat |
| Dynamic: ragged | the run's scatter > 3× the section's and 3 ms | attacks jumping between elements |
| Dynamic: remnants | a straight remnant < 32 beats (64 between two) joins | a section's minimum |
| Walk | local quadratic ±4 beats; strain ±8, running median of 9; tempo steps ≤ 4 % a beat, within ×1.2 of the median | a bar either side; no bias under a moving tempo; a drummer walks 3 % a beat at most |
| Re-track | the local period from attack intervals within 40 % of the line's, ±8 beats; the range from ¼ period before the first beat to ¼ past the last | the tracker inserts beats at one period |
| Judgment | per 16-beat window: lost where the line is within 15 ms of the hits and the walk twice as far and 3 ms more, or an end beat 15 ms off a straight neighbour's line; lost windows back to the line, then the mean must beat the line's by 1 ms | the line is the simpler grid; a fold is noisy, and blind to half a window |
| No kicks | kick/full periodicity < 0.5 at 1× and 2× the beat rate, < 1 at ½× | two-step kicks cancel at the beat rate |
| Confidence | 16-beat windows every 8; on the grid: a rise within 15 ms, at least half the window's contrast or the median's (the less), read over the search either side; weight: contrast less half the median, at most 1.5 medians | dj's; a hit's tail is no rise |
| Low confidence | < 0.5 | |
| Music start | floor: commonest level before the music is within 45 dB of the loud level (90th percentile); at least 60 dB under it and 60 ms long | dither and hiss are floors; an encoder delay is not |
| Start threshold | after digital silence 65 dB under the loud level; over a noise floor 20 dB over it, never lower than 50 dB under the loud level; held for a third of 200 ms | a -60 dB fade counts from its first hit; a fade over dither from where it is heard; no clicks |
| Hit | 25 dB over the threshold two frames on; time at 3 % of its peak | a jump within a frame or two |
| Fade, swell | a rise to 15 dB under the loud level in a second or more, or less; out of digital silence from the first sound | a swell leads into a bar line |
| Start witness | within 0.1 beat (40 ms) of a beat or half beat; a hit whose kick band is at least twice the kick beats', where the kicks leave the half of the beat open (log under 1.0): log 2.5 and 2 scales for beat 1; a fade's first beat: half a scale for beat 1 | measured on the library, section 11 |
| Grid start | first beat at or after the start, less 50 ms, never before 0 | a line a few ms early on the hit; no beat before the file |

## Evaluation

There is no ground truth for the library, so the evaluation measures what
can be measured. The sample: 150 tracks from `~/Music`, chosen with a fixed
seed (`build-beats/eval/sample.txt` lists them): 90 electronic FLAC (AD:HOUSE
3-13, AD:HOUSE Winter, AD:TECHNO 4, AD:Garage, COLORS / Colors 2, AD:PIANO
VIVACE, ADvantage, AD-Progressive House 4), 30 J-pop and anime FLAC with live
drums (Orange Pekoe, yanaginagi, AZKi, D4DJ Photon Maiden, ReoNa, Yutaka
Yokokura) and 30 Opus from `music_temp` (10 from AD:Drum'n Bass 5).

To iterate on the decisions without decoding and transforming every file
on every run, give `cyanbox-eval` a cache on a disk: `--cache DIR` keeps each
file's decoded audio and front end, and later runs only analyse
("Reproducing the evaluation").

### Synthetic tracks (`test_beats`, exact answers)

Every case runs on the CPU's front end and, when there is a device, on the
GPU's, with the same answers and the same grids (section 1).

All pass: tempo within 0.05 BPM (all exact), phase within 3 ms (all within
1.8 ms), downbeat right, sections and boundaries right (boundaries within
1.3 ms), flags as required. Every grid is also expanded as a player gets it
(`cb_grid_beats`): no two beats closer than half a period, and each section's
first downbeat numbered 1. Every track starts after digital silence with a
hit on beat 1; the cases on where the music starts also check the grid's
first beat (within 5 ms of the first sound on a beat), its number, that no
beat, bar or snap lies before it, and the automatic cue.

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
| `cb_analyse` on stereo 48 kHz; auto cue | grid as mono, key "Am", cue on the first downbeat |
| 0.5, 1.7, 3.3 s of silence, a hit on beat 1 | music starts at the hit's sample; first beat 1.3, 1.3, 0.9 ms before it, beat 1 |
| fade-in over 2, 4, 8 s, linear in dB from -60 dB | a fade from its first hit; first beat on it, beat 1 |
| a -75 dB noise floor, then a hit | the floor is no music; first beat on the hit, beat 1 |
| a click in the silence, then a hit | the click is no start (it does not last) |
| two-step garage 134 (kick on 1, the "and" of 2 and of 3) after 0.72 s | on the hit, beat 1 |
| a chord stab on the "and" before bar 1 | music starts at the stab; the grid stays on the kicks, first beat bar 1 |
| a snare pickup on beat 4 before bar 1 | first beat the pickup, numbered 4; bar 1 and the cue on the next |
| ramp 120 → 132 over bars 8-40, then held (house) | dynamic over 130 beats: beats within 1.2 ms RMS, 1.6 ms worst, 56 of 56 bar lines downbeats; the best straight grid (4 sections) 59 ms RMS, 263 ms worst |
| a drummer's random walk (0.4 % a beat, within 10 %), 6 ms jitter, 122 | dynamic throughout (262 beats): 3.3 ms RMS, 11.1 ms worst, 64 of 64 downbeats; straight (5 sections) 51 ms RMS, 256 worst |
| rubato: hip-hop 120 dipping to 100 over bars 16-24, 3 ms jitter | dynamic over the dip (42 beats), straight either side: 1.1 ms RMS, 4.6 worst, 48 of 48 downbeats; straight (one section) 54 ms RMS, 247 worst |
| sequenced house 130, a two-bar syncopated fill at Felicidade's gaps (467, 533, 620, 467, 533, 460 ms, the last two on the beats) | one straight section, 130.000, not dynamic; the beats through the fill and a bar either side within 1.2 ms of the pulse, none missing or extra |
| a kick and clap loop at 87, 9 and 10 s long | 174.000, `octave-uncertain` (margin 0.33): 87 won the octave, but has too few beats for a grid; no grid before |
| the same loop, 12 s long | 87.000, −1.3 ms |
| pop 100: kick on 1 and the "and"s of 3 and 4, a high-passed snare on 2 and 4, hats and a legato bass in eighths | 100.000 (margin 2.30; 200 by 5.2 before the lone snare and the bar's kick), −1.0 ms |
| UK hardcore 175: a kick on every beat, claps on 2 and 4 | 175.000 (not 87.5; margin 9.0), −1.4 ms |
| (not a track) a walk through attacks that leave a 130 BPM pulse for a dozen beats, 160 ms off | tempo steps of 3.8 % at most (5.5 % without the continuity), the beats away from it on the pulse |
| (not a track) a kick and a click on every beat at 128, sixteen beats folded on their hits and 60 ms after them (the confidence's windows) | on the hits a rise of 7.20, its whole contrast, at −1.3 ms; 60 ms late a contrast of 12.83 from the tails and a rise of 0.002 at +4.6 ms: not on the grid (it counted as on it) |

The three tracks with a tempo that moves are judged beat by beat against
their true beat times (the RMS and the largest distance of each true
beat from the nearest grid beat, from the grid's first beat on, and that
every bar line has a downbeat), and against the best straight grid of the
same analyser with `allow_dynamic` off, which the dynamic grid must beat.
The errors are against the smooth beat times the tempo curve gives, not
the jittered hits: a grid that reproduced the drummer's scatter would
score worse, as it should.

### Against dj's analyser (`py_beats.py`)

| | cyanbox vs dj |
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
  200): cyanbox right on all 83, dj on 77. The prior alone would get 78.
- **Phase.** Of the 34 tracks where both agree on tempo and differ in phase
  by more than 10 ms, the click band (Butterworth above 2.5 kHz, scipy, folded
  over the whole track: `build-beats/eval/judge.py`) starts rising within 10 ms
  of cyanbox's beats on 33, of dj's on 14.
- **Downbeat.** Of the electronic tracks whose music starts on a beat, all 74
  start on cyanbox's beat 1 (dj: 52 of 52; dj's start vote decides nearly
  every track because its summed novelty is nearly flat).
- **BPM tags.** The whole library has 14 tagged files; 7 are one album with
  "135" on every track (acoustic songs between 117 and 170: the tag is
  wrong). Of the other 7, cyanbox and dj both agree with the tag within 0.5 BPM
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

**On the CPU**, single-threaded, on the 150 tracks (mean 4.5 min): 290 ms CPU per track
(median 266, worst 827 ms when the grid is fitted three times), 64 ms per
minute of audio, with the machine otherwise quiet; 368 ms (median 274) with
other builds running beside it. dj: 4.98 s per track. Per six-minute track, roughly:
spectrum 165 ms (two thirds of it av_tx), onsets 15, tempo 14, tracking,
envelopes, attacks, sections and refinement 110, octave and family refits
when needed 100 each, phase, downbeat and bass 11, confidence 5.

**With the GPU** (section 1). On two 3.3-minute FLAC tracks, one core for
everything, the fastest of seven runs with the two paths alternating (the
machine was busy with other builds, so the CPU path is given for each
device's runs; the RX 6900 XT sits behind Thunderbolt here, so its time is
mostly the transfers, while the Iris Xe shares the CPU's memory and power):

| Beat analyser | Feel Alive (garage, 1726 windows) | PASSIVE (470 windows) |
| --- | --- | --- |
| CPU path / RX 6900 XT | 322 / 135 ms (98 ms CPU) | 208 / 80 ms (63 ms CPU) |
| CPU path / Iris Xe | 371 / 236 ms (152 ms CPU) | 171 / 100 ms (59 ms CPU) |
| GPU time (timestamps), STFT + envelopes, 6900 XT | 18.8 + 27.1 ms | 17.6 + 7.4 ms |
| GPU time, STFT + envelopes, Iris Xe | 48.6 + 43.2 ms | 36.7 + 12.0 ms |
| Decoding, then `cb_analyse` (all stages), CPU path / 6900 XT | 1013 / 815 ms | 791 / 650 ms |
| the same, CPU path / Iris Xe | 1256 / 1137 ms | 624 / 589 ms |

What is left on the CPU is the decisions (and copying the signal up);
decoding (250-350 ms) and the waveforms now take longer than the beats. On
the 58 acceptance tracks (`-j 2`, the RX 6900 XT, a busier machine): 204
ms CPU per file against 656 on the CPU path (54 against 172 ms per minute
of audio), the front end 47 ms of wall time per file against 377. A
context costs 30-190 ms to make, and its pipelines 130-570 ms to compile
the first time they are used (once per process; Mesa caches them on disk).

### Memory

Peak resident memory above the decoded input (`cyanbox-eval --mem`, which
resets the kernel's high-water mark after decoding; `--all` runs the whole
`cb_analyse`: waveforms, beats and key). Files made by concatenating
AD:HOUSE albums with ffmpeg; mono 44.1 kHz as the application decodes:

| Length | Input (mono float) | Beats | `cb_analyse` | `cb_analyse` before 2026-10-14 | Whole process, before |
| --- | --- | --- | --- | --- | --- |
| 5 min | 53 MB | 8 MB | 9 MB | 17 MB | 96 MB |
| 10 min | 106 MB | 14 MB | 17 MB | 32 MB | 162 MB |
| 30 min | 318 MB | 41 MB | 44 MB | 88 MB | 419 MB |
| 60 min | 635 MB | 81 MB | 86 MB | 176 MB | 810 MB |

The beat analyser grows by about 1.35 MB per minute: the onset signals, the
per-half-beat profiles and the tracked beats, but not the spectrum. The
largest stage of `cb_analyse` on an hour is the beats (82 MB above the
input), then the waveforms (68); the key takes 2 MB. Until 2026-10-14 the
key kept a decimated copy of the track (a quarter of the input at 44.1
kHz) and took 151 MB, the largest stage; it now decimates as it reads,
once per pass (docs/KEY.md), and the key and its confidence came out the
same to the bit (225 synthetic tracks, the 58 acceptance tracks). The
`cb_analyse` column was measured again then on a synthetic loop of each
length (mono 16-bit FLAC, the GPU front end, three runs alike to 1 MB):
the analyser before the change took 16, 33, 89 and 177 MB above the input
there, as on the albums, and the whole process peaked at 150, 218, 477
and 866 MB (the Vulkan context included), now 144, 203, 431 and 776.

At 48 kHz an hour costs 687 MB in the beat analyser and 781 MB in
`cb_analyse`, of which 606 MB is the one 44.1 kHz copy the analysers read;
stereo input at 44.1 kHz costs 781 MB in `cb_analyse`, of which 635 MB is
the mono mix that the waveform and key interfaces take. The decoder's own
transient peak (the audio module's) comes on top before the analysis starts.

Before this was measured, an hour cost 180 MB in the beat analyser and 790
MB in `cb_analyse` above a mono 44.1 kHz input, 1840 MB in the beat analyser
at 48 kHz and 1395 MB in `cb_analyse` for stereo. What changed, with every
result bit-identical (the synthetic suite, and `cyanbox-eval --csv` over the
150 tracks with `--ffmpeg` decoding; with `cb_decode` too, except one file's
duration, which the decoder itself now reports differently):

- The mel frames are not kept. The STFT loop keeps three frames (SuperFlux
  compares each with the one two frames back), computes the three fluxes
  as it goes, and keeps only what later stages read: the kick band, and the
  16-band profiles (sums of four mel bands) that the downbeat's structure
  novelty reads per half beat. An hour was 580 000 frames of 64 floats.
- `cb_analyse` does not copy a 44.1 kHz input: the beat and key analysers
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

| Track | cyanbox | dj | Who is right, and why |
| --- | --- | --- | --- |
| AD:HOUSE 8 / Unclear | 127 | 63.5 | cyanbox: kick on every beat at 127; at 63.5 the claps fall on the half beats. |
| AD:HOUSE 7 / groovin more groovin | 124 | 186 | cyanbox: four-on-the-floor house; 186 is the 3:2 reading. |
| AD:Garage / 4:00 AM | 134 | 201 | cyanbox: a garage tempo; 201 is 3:2. Confidence 1.00 vs 0.85. |
| AD:HOUSE 3 / Over the Azure | 130 | 195 | cyanbox, probably: kick on every beat at 130. dj's confidence (0.91) is fooled by the kick on every third beat of a 3:2 grid. Forced to 180-210, cyanbox fits 195 at 0.73. |
| AD:HOUSE 4 / Ocean Peridot | 132 | 198 | cyanbox, probably: as above; a forced 180-210 fit is poor. |
| AD:Drum'n Bass 5 / Make You Better | 172 | 128.9 | cyanbox: drum & bass; 128.9 is a 3:4 reading (dj's confidence 0.29). |
| AD:Drum'n Bass 5 / Divine Riptide | 177 | 177 | Same tempo; dj's grid is a sixteenth off (bar pattern: every hit on x.2 and x.4 of dj's beats). |
| AD:Drum'n Bass 5 / Firewall | 177 | 177 | Same. A busy breakbeat: on some versions cyanbox halved it, its working grid at 88.5 sitting on the "and"s; fixed by choosing the levels' beats from the finest level down. |
| AD:HOUSE 10 / Shine | on the click | 35 ms early | cyanbox: scipy fold has the click rising at 0 ms on cyanbox's beats (dj's confidence 0.17). Before the click-band rule cyanbox was 15 ms late (low-band body). |
| AD:TECHNO 4 / Blaster Weapon | 130 | 130 | Same grid. Ambiguous to the eye: a rolling 16th bass fills the kick band, open hats burst half a beat before a sharp mid-band click with a sidechain dip after it; both analysers put the beat on the click. |
| AD:HOUSE Winter 3 / White Lie | 127 | 127 | Same. Showed the need for the four-on-the-floor rule: a stab on the "and" of 1 and 3 made the snare test halve it. |
| ADvantage / Kneading | 104 | 138.66 | Undecided: 208 = 1.5 × 138.667 exactly, so one is a 3:2 reading; versions of cyanbox have given 208, 138.667 and 104. |
| emome CASE01 / FVERTEX | 150 | 75 | cyanbox, probably (dj's confidence 0.01). |
| COLORS / INTO THE LIGHT (Nhato remix) | 150, one section | 112.5 | cyanbox, probably: a trance remix at 150. Earlier versions said 75 in three sections, flagged octave-uncertain and unsteady; choosing the line's beats by kick and snare (section 8) fixed it. |
| AD:Drum'n Bass 5 / Penglaishan | 190 | 190 | Same, flagged octave-uncertain: 95 and 190 were nearly tied. |
| AD:HOUSE 13 / When the Salsa Turns Orange | 123 | 123 | Same; an earlier version halved it (the octave test read the phase of the DFT's fundamental, 42 ms before the kick). |
| Taiyou no Kakera (Electric Sheep Remix) | 118.99 | 118.99 | Same. Showed that the tempogram must not override a 4:3 family (it gave 158.65). |
| AD:HOUSE 7 / Voyage with the Seagull | 125, one section | 125 | Same. An earlier version cut a half-beat "phase change" at 219.7 s on an off-beat bass, with confidence 0.00 from empty windows; both fixed. |
| AD:TECHNO 4 / nakanoshima | 125, one section | 125 | Same. The last 74 beats have only an off-beat element left; a 32-bar minimum for half-beat phase cuts keeps the body's grid. |
| STYLEY NiGHT ii / Love me, myself, and its instrumental | 100 (margin 2.4, 2.7) | not run | 100 by ear. Was 200 (1.66; the instrumental 1.50, `octave-uncertain`): a snare on 2 and 4, a kick on 1, the "and" of 3 and late in 4, eighths on the hats; the kick veto took the parity's 0.77 at 200 for a four-on-the-floor. Section 8. |
| Clean Tears / Skyscraper (Heat Haze) | 98 (margin 1.9) | not run | About 98 by ear. Was 196 (6.3): four-on-the-floor at 196 by parity (0.77; by the bar 0.27: eighths in the low band of its louder sections), and 98 too slow by its snares, which are alone at 196. Its pattern varies by section and was not settled by eye. |
| Bootleg (eKiog UKHardcore) | 175 | 175 | Tag says 170; both say 175. Before the four-on-the-floor kick veto cyanbox said 87.5. |
| TRY UNITE! (Uplifting Trance Mix) | 139 | 139 | Tag says 140; both independently 139.000: the tag is rounded. |
| Orange Pekoe / Taiyou no Flight (live) | 142.7, two sections | 140.3 | Neither is trustworthy: rubato live jazz; both have low confidence (0.27, 0.14). |

### Acceptance trial (58 library tracks)

A trial imported 58 tracks into a library and read the grids back
(`build-check/acceptance/`). Its findings, and what each change did there:

| Track | Trial | Now | Change |
| --- | --- | --- | --- |
| AD:Drum'n Bass 5 / Determination | 87.5, conf 1.00, `octave-uncertain` | 175 (margin 4.7) | the line's level chosen by kick and snare (section 8) |
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
labelled octave cyanbox had wrong (INTO THE LIGHT: 83 of 83), and otherwise
changed only flags.

**Dynamic grids** (section 6), the same 58 tracks analysed from the
harness's cache before and after, every column of the CSV compared
(`build-grid/acc/` in the worktree the feature was made in; `--no-dynamic`
reproduces the baseline on all 58 to the last digit, sections, flags,
confidence and starts of the music). 48 grids are the baseline's, bit for
bit: every sequenced track (AD:HOUSE 11, AD:HOUSE 7, Drum'n Bass 5, the
MP3s), and Orange Pekoe's "Ai no Izumi" and "Adagio", AZKi's
"エントロピー", "Lazy" and Yokokura's "Tonight", on which stretches were
walked and judged for the line. Ten grids are dynamic, all on tracks the
baseline had flagged `unsteady` or `low-confidence`, or at 0.77 and
below; the confidence is the whole-track share of 16-beat windows on the
beat (section 10), the same measure before and after:

| Track | Before | Now |
| --- | --- | --- |
| AD:HOUSE 11 / Felicidade (latin house, live percussion) | 130.000, conf 0.50, no flag | 570 measured beats from 24 s (105-155 BPM local, 130.1 median), conf 0.91, `downbeat-weak` |
| Yokokura / Shizuka No Office | 124.29, 0.31, `low-confidence` | 75 beats, 0.77 |
| Yokokura / Martin No Yakata | 98.0, 0.09, `low-confidence` | 66 beats, 0.47 |
| Yokokura / Saimin De Tonight | 98.0, 0.34, `low-confidence` | 49 beats, 0.51 |
| Yokokura / Fortune Land E | 124.0, 0.47, `low-confidence` | 31 beats at the start, 0.62 |
| AZKi / Chaotic inner world | 155, 0.93, `phase-uncertain`, `no-kicks` | two stretches (136 beats), 0.94 |
| Orange Pekoe / Happy Valley | 105.13, 0.77 | 143 beats from the start, 0.78, `phase-uncertain` |
| Orange Pekoe / Introduction (36 s, no steady beat) | 126.63, 0.65, `unsteady` | 78 beats, 0.67: as meaningless as before |
| Orange Pekoe / Taiyo no Kakera | 120, 0.36, `low-confidence` | 111 beats in the middle, 0.37, still `low-confidence` |
| AZKi / 明けない夜があったなら | 176, two sections (`phase-cut` at 3:52), 0.84 | 38 beats at the start, 107 from 3:52 (the 71 ms re-entry one short interval), 0.84; the `phase-cut` flag goes with the cut |

The Yokokura soundtrack recordings, live and loose, gain the most: a
straight grid followed half their windows or fewer, the walked beats two
thirds and more. Felicidade's attacks spread 7 ms beat to beat around
the line (the baseline's 0.50 says the line held half of it), and its
local tempo wanders a quarter around 130: whether the band or the
analyser is loose was not checked by ear. Nothing a straight grid held
got worse: the judgment keeps the line wherever the walked beats do not
bring the hits closer, which on this list was every sequenced stretch
that passed the earlier tests and three live ones.

**The sequencer, the windows and the continuity** (section 6, after
Felicidade was heard to be wrong), the same 58 tracks from the cache,
before (the grids of the table above) and after, every column compared
(`build-fix/acc/` in the worktree they were made in). `--no-dynamic`
gives the same 58 grids as before to the last digit, and 49 grids are
before's, bit for bit: every steady track, and Yokokura's "Martin No
Yakata". Of the ten dynamic grids:

| Track | Before | Now | Straight (`--no-dynamic`) |
| --- | --- | --- | --- |
| AD:HOUSE 11 / Felicidade | 570 measured beats, 105-155 BPM, 0.91 | straight 130.000, 0.50, no flag: sequenced (0.57 ms on the line) | the same |
| Yokokura / Shizuka No Office | 75 beats, 0.77 | 75 beats, 125.2 BPM average, 0.73 (the walk's continuity) | 124.29, 0.31 |
| Yokokura / Martin No Yakata | 66 beats, 0.47 | the same | 98.0, 0.09 |
| Yokokura / Saimin De Tonight | 49 beats, 0.51 | 39 beats, then the line from 24.4 s, 0.86 | 98.0, 0.34 |
| Yokokura / Fortune Land E | 31 beats at the start, 0.62; a beat 252 ms after another at the join | straight, 0.47: the walk does not meet the line at its end | 124.0, 0.47 |
| AZKi / Chaotic inner world | two stretches, 136 beats, 0.94; joins 1.5 periods apart | straight, 0.93: windows the line holds within 2 ms lost by the walk | 155, 0.93 |
| Orange Pekoe / Happy Valley | 143 beats, 0.78 | 15 beats at the start, 0.77 | 105.13, 0.77 |
| Orange Pekoe / Introduction | 78 beats, 0.67 | straight, 0.65: the walk, held to its continuity, follows the hits no better than the line (26 against 17 ms) | 126.63, 0.65 |
| Orange Pekoe / Taiyo no Kakera | 111 beats, 0.37; joins of 742 and 250 ms | 32 beats at 31-47 s, 0.43, 116-122 BPM, joined on the beat | 120, 0.36 |
| AZKi / 明けない夜があったなら | 38 + 107 beats, 0.84; a join of 197 ms | straight, 0.84, `phase-cut` at 3:52 again | the same |

No grid on the list has two beats closer than half a period, and none a
step of its local tempo over 10 % but the straight 71 ms re-entry of
"明けない夜があったなら" at 3:52, which is flagged. Five grids are
dynamic now, all on live recordings with `unsteady`; the two AZKi tracks
went back to their straight grids. Whether AZKi's recordings are played
or sequenced was not checked by ear; their attacks on the line read 3.5
and 3.7-5.1 ms beat to beat, live by the rule.

**The bar's kick and the lone snare** (section 8; `CB_BEATS_VERSION` 6),
the 57 acceptance tracks still on disk analysed from `cyanbox-eval`'s cache
before and after, every column of the CSV compared: 56 grids are the
same to the last digit, sections, flags, confidence and starts. One
changed, a fix:

| Track | Before | Now |
| --- | --- | --- |
| AD:Drum'n Bass 5 / Run Away | 175, `octave-uncertain` (margin 1.42) | 175 (margin 5.7): at 350 its snare is alone (one beat in four), so 175 is not too slow by its snares (subdivision 0.88) |

No octave changed; the margins moved on 19 others, none below 2.7: the
drum & bass rose (Keeps Me High 1.65 to 5.2, Determination 4.7 to 7.1,
Hullbreaker 4.7 to 5.3), as did エントロピー (2.0 to 7.9), Change
Everything (9.2 to 21) and Ai no Izumi (12 to 49); house fell where its
half tempo is no longer too slow by the claps and the prior holds it
(Hot Vacation 91 to 23, Float 313 to 78, 琥珀 68 to 27, Linked Ring
8.3 to 5.9). The three tracks heard as 98-100 are right (above), and the
synthetic pop case at 100 was 200 before.

**Eighths that pass for a four-on-the-floor** (section 8;
`CB_BEATS_VERSION` 7). Fifteen tracks the user named, with their tempos
by ear (2026-10-17), ten of them read at twice that: a kick or a bass on
every eighth at 94-102 is, one level up, a kick on every beat alike, the
four-on-the-floor that tripled that level's prior and made the true one
too slow by its kicks on the half beats. Two things now say otherwise:

- A level of 150 BPM or more with nothing in the mid band on its half
  beats (subdivision under 0.15) is the eighths of half its tempo: too
  fast, and no four-on-the-floor. "Burping", "Defragmenter", "night
  walk" (and its instrumental) and "Heure bleue" read 0.01-0.05 at
  188-200; every fast level chosen on the lists checked has 0.42 or more.
- A level whose snare is lone (once in four beats) has it on 2 and 4 of
  half its tempo: no four-on-the-floor there either, and the slower
  level is not too slow by its kicks between the beats, its own eighths
  ("Super-Slow-Internet-san" at 200: snare 0.55, lone 1.00; "ALONE" at
  196: 1.24, 1.00). This was already so for its snares (version 6).

| Track | By ear | Before | Now |
| --- | --- | --- | --- |
| Burping | 100 | 200 | 100 |
| Defragmenter | 100 | 200 | 100 |
| インターネットが遅いさん (Super-Slow-Internet-san) | 100 | 200 | 100 |
| ALONE (ReoNa) | 98 | 196 | 98 |
| night walk, night walk (Instrumental) | 97 | 192 | 96 |
| Heure bleue | 92 | 188 | 94 |
| Lucky☆Honey☆Pancake | 190 | 190 | 190 |

On the 57 acceptance tracks and the 60 of "ADvantage" (a hardcore
compilation, kept as a guard for fast tracks), from `cyanbox-eval`'s
cache before and after: every tempo, flag and confidence the same. Two
synthetic cases at 100, one per rule, came out 200 before.

Version 8 adds the family by the comb (above): "plan goes on" and "In
My House" are 128 now, from 192; again no tempo, flag or confidence
changed on the two lists, and a synthetic case at 128 (kicks on the
sixteenths 1.3, 2.4, 3.3 and 4.4, triplets on 2 and 4), read at 96
before, is 128.

Not fixed: "LIFE GOES ON" (98, read 196) has its snare on 2 and 4 with
next to nothing on 1 and 3 in the mid band (balance 0.16 at 98), which
the "too fast" test takes for 16ths, and its kick follows the snare's
parity, so neither band tells the backbeat from a double tempo. "Egret
and Willow" (98 by the grid; heard as about 110, which holds no grid)
is 196 by a margin of 1.2, its lone snare at 196 just under the
threshold (0.48). "Yay Yeah (Long Ver.)" changes tempo: 102 for its
first 80 seconds, then 82 to the end (16-second windows: nothing at 102
after 80 s); it is read 204 and 164, both twice, because the octave is
decided on the longest section, where the rhythm leans to 82 (0.85
against 0.68) but the prior to 164. "In My House" is flagged
`phase-uncertain`, and "plan goes on"'s kicks sit a sixteenth before its
beats: where the beat lies in those two syncopated patterns has not been
checked by ear. Two the user named do not hold the tempo given:
"Peaky Pumpin" (180 by ear) is 199-200 in every 16-second window of the
track and 180 in none, and "Reminiscence of Sea" (170) likewise 197.

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
- The snare as the strongest mid-band beat, for the lone snare: a kick's
  body is in the mid band too. Drum & bass's 175 ("Desolate") had its kick
  on 1 alone and the half tempo would have lost "too slow"; house at 128
  looked lone as often. Hence mid over its mean less low over its own.
- The slower level's phase, for "Skyscraper": both of its 98 level's
  phases read too slow by the snares (subdivision 1.15 and 0.87), so the
  phase was not the cause.

## Known weaknesses

- **The octave is a convention** (section 12): 80-201 BPM, with 88.5-100
  doubled where cyanbox hears the double. Maps charted at 202 BPM and up
  (speedcore, some J-core) are halved by design; ×2 on the deck is one
  click.
- **Octave and family remain the main errors** (none of 83 labelled; more on
  J-pop and hard dance, where the right answer is often a convention, and
  3:2 readings like Kneading that neither analyser settles). The
  `octave-uncertain` flag marks most of them; ×2 and ÷2 in the interface fix
  them in a click.
- **Pop at 95-105** with eighths in the mid band (hats, guitars, a kick on
  the "and"s) was read at double tempo; three tracks heard as 98-100 are
  right now (section 8), but by thresholds with little room: the bar's
  kick is 0.27-0.30 on them at double tempo against 0.4, and
  "Skyscraper"'s margin is 1.9 with a pattern that changes by section.
  Seven more with a kick or a bass on every eighth are right since
  version 7; "LIFE GOES ON" and "Egret and Willow" are not (section 8).
- **Half-time genres**: a lone snare at the faster level is read as the
  half-time of the slower, which is what a listener hears in pop. Dubstep
  and trap at 140 (snare on 3) keep 140 by the prior (8 to 1 over 70);
  half-time drum & bass at 170 would have little but the kick to keep it
  there (none on the lists checked).
- **The snare measure** (mid band over its mean less the low band over its
  own) is no backbeat detector for house: with a kick on every beat, the
  claps on 2 and 4 often read unlike and "lone" at the right tempo. It
  costs nothing there (the half tempo's prior is 18 to 1 against), but it
  is why the rule only ever lifts "too slow" and never scores a level.
- **Live recordings** now get dynamic grids where the line does not hold
  (section 6), and the confidence rises with them on the acceptance list
  (three of the Yokokura recordings from 0.09-0.34 to 0.47-0.86). What a
  dynamic grid cannot fix is music without a clear beat: Orange Pekoe's
  "Introduction" (36 s, no steady beat) is as meaningless walked as
  straight, and a drummer's scatter stays scatter (`unsteady` is still
  set, now around the walked beats). A drift of under 15 ms, or of a
  sequenced section, stays straight by design: a band playing to a click
  with a loose percussionist on top reads as sequenced where a quarter
  of its attacks on the line are evenly spaced within a millisecond.
- **The judgment is by the hits**, the same envelopes the confidence
  reads: where a live recording's attacks are poor (brushes, a soft
  kick), the walked beats and the line are judged on the same poor
  evidence, and the line, the simpler grid, wins ties. Window by window
  the fold is noisier still: a window is only lost where the line holds
  its hits within 15 ms and the walk is twice as far, so a walk a few
  milliseconds worse than the line on clear hits (or 10 ms on poor ones)
  is kept there when the stretch wins as a whole. A walk that misses the
  line at its ends is not kept at all near them: "Fortune Land E" went
  back to its straight grid (0.62 to 0.47) rather than keep a beat 252
  ms after another at the join, where the walk and the line are half a
  beat apart.
- **Each round of lost windows walks the section again**, and the
  rounds are not bounded but by the beats (each takes some from the
  stretch): five on "Happy Valley", eight on Felicidade without the
  sequencer's rule (550 ms of decisions against 200).
- **Ramps** are followed beat by beat where the change-point search left
  bent pieces; a ramp slower than about 2 BPM a minute at 125 leaves
  32-beat pieces bent by under 10 ms, a staircase of straight sections
  within 8 ms of the music.
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
- **Where the music starts** after silence is measured well; what the
  start says about the beat is weaker evidence than it looks. A fade's
  first beat decides beat 1 only within half a scale of the structure, a
  hit without a strong kick says nothing, and a hit with one is outvoted
  where the kicks are clear. Two-step garage, broken beats and any track
  whose kick band leaves the half of the beat open now follow their
  opening hit; a track that opens on an off-beat kick as heavy as a
  downbeat, with an equally unclear kick band, would be moved wrongly.
  Where the drums enter after a fade (the first kicks a beat apart) placed
  beat 1 right more often than the fade or the structure on the tracks
  checked by hand, but is not a witness yet.
- **Not evaluated on the whole library.** The final thresholds were
  measured on the first 800 library tracks and the sample; a full
  before/after scan of the library (3192 files under 20 minutes) was
  started and stopped unfinished.
- **Grids before the music** are gone: a cue set in a track's leading
  silence still has a beat index (the first section's line runs on for the
  arithmetic), but no beat to snap to.
- **Speed**: on the CPU the STFT dominates; a 100 frames/s front end would
  cut it by a third. With a GPU it does not: the decisions are what is
  left, and decoding and the waveforms take longer than the whole beat
  analysis.

## Reproducing the evaluation

```sh
meson setup build-beats
ninja -C build-beats src/analysis/test_beats tools/cyanbox-eval
meson test -C build-beats --no-rebuild --suite analysis beats

# dj's analyser over the sample (re-executes itself under dj's venv)
tests/analysis/py_beats.py -j 6 -o build-beats/eval/dj.csv -l build-beats/eval/sample.txt

# cyanbox over the sample, compared with dj
build-beats/tools/cyanbox-eval -v -j 8 -l build-beats/eval/sample.txt \
    --csv build-beats/eval/cb.csv --compare build-beats/eval/dj.csv

# one track in detail: stage timings, sections, octave, the onset signals
# (-v prints each octave level's evidence, by parity and by the bar, and
# the levels' first beats, for pattern.py on BASE.0.onsets.csv)
build-beats/tools/cyanbox-eval -vv --dump-onsets build-beats/dump/x FILE

# iterating on the decisions: the first run fills the cache, later ones read it
build-beats/tools/cyanbox-eval -j 8 -l build-beats/eval/sample.txt --cache ~/cyanbox-cache \
    --csv build-beats/eval/cb.csv
```

`cyanbox-eval` makes the app's grid: cyanbox's analysis, then the grid with
Beat This! (section 12; the network on the GPU from `models/`, or `-m
FILE`); `--first-stage` stops at cyanbox's own analysis, the hints. It
decodes with `cb_decode` (mono, the file's own rate, as `cb_analyse` sees
it); `--ffmpeg` pipes from the ffmpeg command line at 44.1 kHz exactly as dj
decodes (and the mean of its two channels for the network). Its front end runs on the GPU when there is
one (`--device`, `--no-gpu`); each file's wall time is split into decoding,
the front end and the decisions (`-v`, and summed at the end).

**Iterating without decoding again**: `--cache DIR` keeps, per file, the
decoded mono 44.1 kHz audio as 16-bit PCM and the front end's arrays
(exactly), found by the file's path, size, modification time, the decoder
and `CB_FRONTEND_VERSION`, so that a second run over the same list skips
decoding and the STFT and only runs the decisions; it prints how many files
came from it. About 6 MB a minute of audio (21 MB of PCM and 3 MB of arrays
for a four-minute track), in whatever directory is named: put it on a disk,
never under /tmp (a tmpfs is RAM; the tool warns). A run that fills the
cache analyses the 16-bit copy it stores, so every run with the cache sees
the same samples; against a run without it only levels below -96 dB move
(on a test track the same grid, flags and start, confidence 0.964 against
0.965). Bump `CB_FRONTEND_VERSION` when the front end's output changes. `--bpm-range LO HI`, `--no-sections`,
`--from CSV` (compare without analysing) and `--no-dynamic` (the best
straight sections everywhere) are also there, and `--starts`
measures only where the music starts (no beat analysis), one line per file.
Its CSV writes a dynamic passage as one `D:` entry in `sections` (its
first beat) with the count and tempo range of the measured beats in
`dynamic_beats`, `dynamic_bpm_lo` and `dynamic_bpm_hi`, and has the start
(`music_start`, `start_kind`, the threshold crossing,
floor, loud level, rise), the grid's first beat and its number, its first
downbeat and the automatic cue, and the start's section's evidence before
its on/off-beat decision (kicks, structure, the start's witness, its half
beat and offset, the structure's eight scores, the hit's kick). The scripts used
for the hand checks (`judge.py`, `foldcmp.py`, `pattern.py`, `octfit.py`,
`octacc.py`, `startbar.py`) were kept in `build-beats/eval/`, outside the
source tree.
