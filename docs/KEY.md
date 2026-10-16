# The key detector

How cyanbox names a track's key, why each step is the way it is, and how well
it works. The code is `src/analysis/key.c`; the tests
`tests/analysis/test_key.c`; the evaluation and the template fitting
`tests/analysis/gen_key_fixtures.py`; the case lists
`tests/data/analysis/keys.txt` and `keys_synth.txt`.

The result (`cyanbox/analysis.h`, `CBKeyResult`) is a classic key name in dj's
spelling (C Db D Eb E F F# G Ab A Bb B, "m" for minor), a confidence and the
tuning offset from A440 in cents.

## In short

| | GiantSteps key (604, held out) | GiantSteps MTG (1159, development) | Synthetic (225) | 5 min of audio |
| --- | --- | --- | --- | --- |
| Before (dj's port) | 30.3 %, MIREX 0.404 | 28.8 %, 0.389 | 216 of 225 | ~550 ms |
| libKeyFinder 2 | 60.4 %, 0.674 | 54.7 %, 0.632 | | ~700 ms |
| Now | **64.7 %, 0.704** | **63.2 %, 0.696** (cross-validated) | **225 of 225** | **~110 ms** |

Times are the best of several runs on one core of a shared machine (the
before and KeyFinder figures include reading the 50 MB file). Accuracy is
the share of tracks named exactly; the MIREX score counts a
fifth above as 0.5, the relative key as 0.3 and the parallel as 0.2. The
confidence is now a probability: on the held-out set the answers at 0.9 or
more are right 89 % of the time, at 0.4-0.6 46 %; before, every answer had
0.0-0.4 and the number meant nothing. On the user's 58 acceptance tracks
the old detector named 48 major (49 in the run that prompted this work,
with two exact ties), with confidences of 0.01-0.32; now 37 major,
confidences 0.26-0.95 (median 0.60), and ties are broken by rule.

## What it does

1. **Decimation** to about 11 kHz by an integer factor (4 at 44.1 kHz, 12 kHz
   from 48 kHz) through a 16d + 1-tap Kaiser-windowed sinc. Any rate works;
   `cb_analyse` hands the detector 44.1 kHz.
2. **STFT**: 8192-point Hann frames (0.74 s) every 4096 samples, av_tx.
3. **Tuning**: the spectral peaks between 150 Hz and 4 kHz, within 40 dB of
   their frame's loudest, in non-overlapping frames, each located by a
   parabola through the log magnitudes; the power-weighted circular mean of
   their offsets from the equal-tempered semitones.
4. **Semitone bands C1..B6** on the tuned semitones, with KeyFinder's spectral
   kernel: a raised-cosine window 0.8 semitone wide, scaled so a sinusoid
   reads the same at any pitch.
5. **Whitening**: every frame divided by its own moving average over an
   octave of bands.
6. **Three chroma vectors** summed over the track, from C2..B2 (the bass),
   C3..B4 and C5..B6, each scaled to unit length.
7. **Scores**: for each of the 24 keys, the dot product of the three vectors,
   rotated to the key's tonic, with the template of its mode: 36 weights per
   mode, fitted on the development set.
8. **A major bias** added to the 12 major keys, so that the detector names as
   many keys major as the development set's annotators did.
9. **Ties** (impossible in practice, possible on a perfectly symmetric chroma)
   go to Sha'ath's profile, then to minor, then to the lower tonic.
10. **Confidence**: a softmax over the 24 scores; its top probability and the
    share of 20-second segments whose own best key is the same go through a
    logistic fitted on cross-validated answers of the development set.

Memory is 288 bytes per frame (2.8 MB an hour) and a ring of 8192
decimated samples: each pass decimates the signal again as it reads it.
A copy of the decimated track (a quarter of the input at 44.1 kHz, 159 MB
an hour) made the key the largest stage of `cb_analyse` until 2026-10-14,
for a decimation less (~80 ms for five minutes). Time, five minutes at
44.1 kHz on one core: 30 ms for each of the two decimations, 30 ms tuning
pass, 20 ms band pass.

## The evaluation

Everything is reproducible with the private build and the script:

```sh
meson setup build-key && ninja -C build-key src/analysis/test_key
G=tests/analysis/gen_key_fixtures.py
TK=build-key/src/analysis/test_key
python3 -I $G fetch build-key/giantsteps                      # 2.9 GB, md5-checked
python3 -I $G gs build-key/giantsteps --set key --prog $TK --out key.tsv
python3 -I $G gs build-key/giantsteps --set mtg --prog $TK --out mtg.tsv
python3 -I $G features build-key/giantsteps --set mtg --prog $TK --out feat-mtg.npz
python3 -I $G train feat-mtg.npz                              # the constants of key.c
CYANBOX_KEY_FULL=1 $TK                                          # all synthetic tracks
python3 -I $G real build-check/acceptance/list.txt --prog $TK
```

`gs` takes any program that prints a key name (and optionally a
confidence) for `PROG - RATE` on raw float32 mono from stdin, which is how
the old detector and libKeyFinder were measured on the same decodes.

### The sets

- **GiantSteps key** (Knees et al., "Two data sets for tempo estimation and
  key detection in electronic dance music annotated from user
  corrections", ISMIR 2015): 604 two-minute Beatport previews of EDM with
  keys from user corrections. The standard benchmark, and here the
  **held-out test**: nothing was tuned on it. 85 % minor.
- **GiantSteps MTG key** (Faraldo, 1486 previews annotated by an expert, with
  a confidence): the 1159 with a single key annotated with confidence 2, the
  usual filter. The **development set**: every choice below was measured on
  it, and the templates, bias, temperature and calibration were fitted on
  it. Its numbers for the final detector are five-fold cross-validated
  (each track answered by templates fitted without it). 67 % minor.
  Both sets download from the JKU mirror the datasets' own scripts use; all
  2090 files arrived.
- **Synthetic** (`test_key.c`, all synthesised in closed form): dj's nine
  sine loops (`keys.txt`) and 216 short dance tracks (`keys_synth.txt`):
  every key with six progressions (major I-V-vi-IV, ii-V-I-I and the J-pop
  "royal road" IV-V-iii-vi; minor i-VI-III-VII, i-iv-v-i and the Andalusian
  i-VII-VI-V), each plain, detuned by 10-40 cents and with a sparse bass, at
  22.05-96 kHz and 118-174 BPM. Band-limited sawtooth chords (pads or offbeat
  stabs) through a low-pass, a sawtooth bass line, a pitched kick, a clap,
  hats; two bars of drums, two of the tonic chord, the loop four times, the
  tonic again. `meson test` runs a quick subset of 44 (two keys of every
  progression and variant, and every case the old detector got wrong);
  `CYANBOX_KEY_FULL=1` runs all.
- **Real tracks**: 20 of `build-check/acceptance/list.txt` (every third:
  Japanese house, drum & bass, J-pop, a jazz-pop band, a film soundtrack).
  No ground truth; the check is that the key does not change with the level
  or the sample rate, and how stable it is between the halves.

A note on the synthetic loops: an aeolian loop i-VI-III-VII has exactly the
notes of its relative major's vi-IV-I-V, so no detector reading pitch
content can tell them apart from the chords alone; only the arrangement
can. The first version of the synthetic tracks had the tonic only in the
intro and outro, and both the old detector (all 36 aeolian tracks as the
relative major) and the new one (6 of 36) failed there. The loops now carry
a pedal pluck on the tonic, the riff dance tracks put over their chords, in
every progression of both modes.

### GiantSteps key (held out)

| | Before | libKeyFinder | Sha'ath's profile, new front end | Now |
| --- | --- | --- | --- | --- |
| Correct | 30.3 % | 60.4 % | 63.6 % | **64.7 %** |
| Fifth above | 10.6 | 7.8 | | 5.1 |
| Fifth below | 4.5 | 3.3 | | 2.8 |
| Relative | 10.3 | 6.5 | | 7.5 |
| Parallel | 8.6 | 5.6 | | 4.5 |
| Other | 35.8 | 16.4 | | 15.4 |
| MIREX score | 0.404 | 0.674 | 0.694 | **0.704** |
| Major tracks (93) | 34.4 % | 36.6 % | 41 % | 40.9 % |
| Minor tracks (511) | 29.5 % | 64.8 % | 68 % | 69.1 % |
| Named major | 277 | 109 | 109 | 116 |

### GiantSteps MTG (development)

| | Before | libKeyFinder | Now, cross-validated | Now, fitted on all |
| --- | --- | --- | --- | --- |
| Correct | 28.8 % | 54.7 % | **63.2 %** | 65.4 % |
| Fifth above / below | 9.5 / 7.6 | 7.4 / 3.4 | 4.6 / 3.6 | 4.2 / 3.0 |
| Relative | 6.6 | 6.0 | 4.4 | 4.0 |
| Parallel | 16.9 | 15.0 | 14.4 | 13.2 |
| Other | 30.5 | 13.5 | 9.8 | 10.2 |
| MIREX score | 0.389 | 0.632 | **0.696** | 0.713 |
| Major tracks (377) | 52.5 % | 46.7 % | 54.4 % | 57.3 % |
| Minor tracks (782) | 17.4 % | 58.6 % | 67.4 % | 69.3 % |

The small gap between cross-validated and fitted (63.2 against 65.4 %)
says the 72 template weights do not overfit 1159 tracks. Parallel errors
(right tonic, wrong mode) are the largest group left: much of EDM has no
third in its chords.

### Synthetic

| | Before | Now |
| --- | --- | --- |
| dj's sine loops (9) | 7 (G major as C, E major as Dbm) | 9 |
| Dance tracks (216) | 209: seven aeolian loops named by their sixth degree (Fm as Db) | 216 |
| Tuning, largest error | 7.6 cents (48 tracks over 3) | 2.0 cents |
| Confidence | 0.00-0.35, median 0.20 | 0.58-0.98, median 0.89 |

### Real tracks

| Track | Before | Now | Now, halves | libKeyFinder |
| --- | --- | --- | --- | --- |
| 夏の雨 | E (0.20) | E (0.45) | Dbm / E | Dbm |
| Change Everything | Em (0.11) | Em (0.78) | Em / Em | Em |
| Hot Vacation | Bm (0.10) | Bm (0.75) | Bm / Bm | Bm |
| Never seen | F# (0.13) | F# (0.80) | F# / F# | Ebm |
| Hu-Rin | E (0.13) | Dbm (0.30) | E / F | F#m |
| Feel My Love | B (0.03) | B (0.48) | B / B | Abm |
| Desolate | Ebm (0.03) | Bbm (0.76) | Bbm / Bbm | Bbm |
| Roundabout | Eb (0.21) | Fm (0.81) | Fm / Fm | Fm |
| Beautiful Colors (After the Rain) | G (0.03) | C (0.56) | C / C | C |
| sunflower | Eb (0.08) | Cm (0.60) | Cm / Cm | Cm |
| 琥珀 | Ebm (0.05) | Eb (0.37) | Ebm / Fm | Eb |
| Ai no Izumi | B (0.05) | B (0.53) | B / B | Abm |
| 12 Kagetsu | E (0.11) | A (0.55) | A / A | E |
| エントロピー | G (0.09) | Em (0.35) | G / A | Em |
| Chaotic inner world | E (0.10) | F#m (0.77) | F#m / F#m | F#m |
| Tonight: Hoshi No Furu Yoru Ni | Abm (0.09) | Ab (0.70) | Ab / Ab | Ab |
| Fortune Land E | E (0.14) | E (0.49) | A / E | E |
| all night | Eb (0.18) | Eb (0.72) | Eb / Eb | Eb |
| 24 | E (0.29) | E (0.67) | E / E | E |
| A lot of life (EG Remix) | G (0.15) | C (0.78) | C / C | G |

- Level: the same key at -20 dB and peak-normalised on all 20 (before: 17
  of 20 at -20 dB, its log compression had a fixed scale).
- Rate: the same key resampled to 44.1, 48 and 22.05 kHz on all 20 (before
  too).
- Halves: 15 and 17 of 20 agree with the whole (before 19 and 17). The five
  that move (夏の雨, Hu-Rin, 琥珀, エントロピー, Fortune Land E) are the
  ones with the lowest confidence (0.30-0.49, against 0.61 on average):
  J-pop that modulates, where a single key is a compromise.
- Agreement with libKeyFinder: 13 of 20 exactly and 4 more as the relative
  key (before: 7 exactly). Where they differ by a relative, cyanbox says
  major: it names major as often as the annotators of its development set
  did, KeyFinder less (it named 109 tracks major on GiantSteps key, cyanbox
  116, the truth is 93).

### Confidence

Accuracy by confidence, the final detector:

| Confidence | MTG (cross-validated) | GiantSteps key | GiantSteps key, named minor | named major |
| --- | --- | --- | --- | --- |
| 0.2-0.4 | 35 % of 115 | 41 % of 63 | 48 % of 48 | 20 % of 15 |
| 0.4-0.6 | 50 % of 387 | 46 % of 138 | 54 % of 102 | 25 % of 36 |
| 0.6-0.8 | 70 % of 447 | 70 % of 203 | 77 % of 164 | 41 % of 39 |
| 0.8-0.9 | 90 % of 154 | 74 % of 128 | 82 % of 112 | 19 % of 16 |
| 0.9-1.0 | 87 % of 55 | 89 % of 72 | 92 % of 62 | 70 % of 10 |

On the development set it is calibrated by construction. On the held-out
set it holds for minor answers and overstates major ones: GiantSteps key
is 85 % minor, so a major answer there is a priori less likely to be right
than on a set with a third of major keys, which the calibration was fitted
on. On the user's library, with more major keys, major answers should fare
better. The old detector's "confidence" (the gap between the two best
correlations) put 553 of 604 GiantSteps tracks under 0.2 and was right on
28 % of them.

## Why each step

Most choices were made with a numpy prototype of the front end on the
development set (decoded by ffmpeg at 11025 Hz), then confirmed with the C
code; the numbers below are from the prototype unless they say otherwise.

- **Tuning.** dj's estimate (peaks of the median-filtered mean spectrum from
  80 Hz, weighted by magnitude) is right on sines but up to 7.6 cents off on
  the synthetic dance tracks, pulled by the kick's sweep and the bass's few
  wide peaks. Per-frame peaks, power-weighted, from 150 Hz: within 2 cents
  on all 225. On a third of an earlier version of the synthetic tracks the
  largest errors were: that, 2.6 cents; from 80 Hz weighted by magnitude,
  4.3; from 300 Hz by power, 5.2; by log magnitude, 8.0; peaks of the mean
  spectrum instead of each frame's, 5.4-8.6.
- **Spectral kernel.** KeyFinder's (Sha'ath 2011): a band per semitone summing
  the magnitudes under a raised cosine 0.8 semitone wide, so a sinusoid reads
  the same at any pitch. The old triangular weights averaged the bins of a
  band, which reads a sinusoid at 1/f: with Sha'ath's profiles, 42.6 %
  against the kernel's 53.0 %. Peak picking (HPCP-style) without a
  threshold counts noise peaks, more of them in the wide high bands.
- **Window.** 3 s windows at 5.5 kHz (near KeyFinder's 3.7 s) against 0.74 s
  at 11 kHz: 57.8 against 59.1 % on the test set with Sha'ath's profiles, 52.1
  against 53.0 % on the development set. The shorter window is cheaper and
  keeps the bass's harmonics up to 4 kHz for the tuning. Hop 2048 against
  4096: 63.5 against 63.2 % (C, cross-validated): the same.
- **Whitening** over an octave: +1.9 points (62.5 to 64.4 %); half-widths of
  4, 6 and 8 semitones gave 64.7, 64.4, 64.9.
- **Compression** (log or square root before summing): no gain, and the old
  log1p with a fixed scale made the key depend on the level.
- **Percussion**: dj's nine-frame median per band, -0.8 point. The drums'
  energy is spread over all pitch classes; the templates' shape ignores a
  flat floor.
- **Frame weighting** to ignore drum-only frames, by the flatness of each
  frame's chroma: weighting by 1 - flatness -1.6, its square -3.6, dropping
  frames flatter than 0.9 -5.6, than 0.8 -18.5 points. Polyphonic frames are
  flat by that measure too, so the weighting threw away music; with
  magnitude sums a drum-only frame contributes little anyway.
- **Harmonic summation** (each band credited to the fundamentals it can be a
  harmonic of, HPCP's harmonics parameter, weight 0.6 per harmonic): -0.6 to
  +0.5. **Approximate transcription** (non-negative least squares against
  notes with harmonic series, as NNLS chroma): +6 to +13 points for
  Faraldo's profiles, 0 to +1 for Sha'ath's, -1 for Temperley's and for
  fitted templates.
- **Bands.** One chroma over C1..B6: 59.4 %. Bass, middle and treble as three
  vectors: 60.3 to 62.5 % (depending on whether C1 is in the bass: without,
  slightly better, and the kick lives there). Six octaves, or adding
  frame-normalised and log chroma: no better.
- **Profiles.** With the final front end (C, cosine on the summed chroma;
  Pearson where better), development / test: Sha'ath's 56.7 / 63.6 %,
  Krumhansl's 48.7 / 50.7, Temperley's 44.8 / 47.2, Faraldo's edma (Pearson)
  47.3 / 52.6, edmm 52.6 / 61.8 (its major profile is flat: it never names
  a major key, which pays on an 85 % minor test set), bgate 52.5 / 55.0.
  Sums of several sets did worse than Sha'ath's alone (Sha'ath + edma +
  bgate 50.2 / 53.1; five sets by Pearson 47.7 / 53.0). Fitted templates:
  63.2 (cross-validated) / 64.7, and 225 of 225 synthetic tracks where
  Sha'ath's miss 19 % of the minor ones; adding Sha'ath's cosine to the
  fitted scores at any weight changed the results by less than half a point.
  Most of the gain over KeyFinder on the test set is the front end
  (Sha'ath's profile goes from KeyFinder's 60.4 to 63.6 %); the templates
  add a point there, seven on the development set, and the synthetic minor
  keys.
- **Fitting.** One multinomial logistic regression over the 24 keys, the
  weights shared by all tonics (each track's chroma rotated to every tonic,
  so a key and its transpositions are the same problem), L2-regularised,
  L-BFGS (`gen_key_fixtures.py train`). Weighting the training tracks so
  that both modes count alike, instead of the bias below: 2-3 points worse
  than no correction, where the bias costs 1.3.
- **Major bias.** Without it the detector names 24.6 % of the development set
  major against 32.5 % annotated, and 46 % of the major tracks right; with
  it 32.6 % and 54 %. It costs 1.3 points overall there, 0.2 on the test set.
  The user's library (J-pop, house) has more major keys than Beatport's.
- **Confidence.** The softmax's top probability alone, at the fitted
  temperature, is calibrated on the development set too, but on the
  held-out set its answers at 0.9 or more were right 81 % of the time; with
  the 20-second segments' agreement added, 86 % (prototype; 89 % in C). The
  segments cost one more pass over the stored frames.

## Limits

- Keys are one per track. A track that modulates gets the compromise key,
  with a low confidence; the halves differ.
- The templates were fitted on Beatport EDM. J-pop with borrowed chords and
  jazz-pop are outside what they saw; the confidence there comes from the
  same calibration and is less sure to mean the same.
- Modes other than major and natural/harmonic minor (dorian, phrygian
  techno riffs) are named as the nearest of the two; the annotators did the
  same.
- No neural network: convolutional networks trained on larger collections
  (Korzeniowski and Widmer, EUSIPCO 2017, ISMIR 2018) do better on this
  benchmark. This detector is a template matcher with 72 fitted weights.

## References

- I. Sha'ath, "Estimation of key in digital music recordings", MSc thesis,
  Birkbeck, University of London, 2011; libKeyFinder (`constants.cpp`,
  `chromatransform.cpp`, `toneprofiles.cpp`). Profiles and spectral kernel
  taken as data; libKeyFinder itself is GPL and only ran locally as a
  reference.
- Á. Faraldo, E. Gómez, S. Jordà, P. Herrera, "Key estimation in electronic
  dance music", ECIR 2016 (edma, edmm).
- Á. Faraldo, S. Jordà, P. Herrera, "A multi-profile method for key
  estimation in EDM", AES Semantic Audio 2017 (bgate, braw). Profile values
  as in Essentia's `key.cpp`.
- D. Temperley, "What's key for key? The Krumhansl-Schmuckler key-finding
  algorithm reconsidered", Music Perception 17, 1999; the Kostka-Payne
  profiles of dj's detector (MIREX 2005).
- C. Krumhansl, "Cognitive foundations of musical pitch", 1990.
- E. Gómez, "Tonal description of music audio signals", PhD thesis, UPF,
  2006 (HPCP).
- P. Knees, Á. Faraldo, P. Herrera, R. Vogl, S. Böck, F. Hörschläger, M. Le
  Goff, "Two data sets for tempo estimation and key detection in electronic
  dance music annotated from user corrections", ISMIR 2015.
- MIREX audio key detection; mir_eval's `key.weighted_score` for the score.
