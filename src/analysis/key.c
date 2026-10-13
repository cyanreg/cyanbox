/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* Musical key: a tuning-corrected, whitened semitone spectrum, folded into
 * three chroma vectors (bass, middle, treble) and matched against key
 * templates fitted on electronic dance music. docs/KEY.md has the
 * evaluation behind every choice: the GiantSteps MTG key set (1159 tracks)
 * is the development set everything was tuned on, the GiantSteps key set
 * (604) the held-out test, plus 216 synthetic tracks and 20 of the user's.
 * On the test set this names 64.7 % of the keys exactly (MIREX score
 * 0.704); dj's detector, which this file replaced, 30.3 % (0.404), and
 * libKeyFinder 60.4 % (0.674).
 *
 * The chain:
 *
 * 1. Decimation to ~11 kHz: by the integer d nearest rate / 11025 (44.1 kHz
 *    is 4, 48 kHz gives 12 kHz), through a Kaiser-windowed sinc of 16d + 1
 *    taps cut at the new Nyquist. Nothing above B6 is used but the tuning
 *    peaks (to 4 kHz), and those lie in the passband.
 * 2. An 8192-point Hann STFT every 4096 samples (0.74 s windows, 1.35 Hz
 *    bins at 11025 Hz), av_tx. A hop of 2048 measured the same.
 * 3. The tuning: the spectral peaks between 150 Hz and 4 kHz within 40 dB
 *    of the frame's loudest, in windows that do not overlap, each read to a
 *    fraction of a bin by a parabola through the log magnitudes; the
 *    circular mean of their offsets from the equal-tempered semitones,
 *    weighted by power. dj's estimate (the median-filtered mean spectrum's
 *    peaks from 80 Hz, weighted by magnitude) is right on sines but up to
 *    7.6 cents off on the synthetic tracks, pulled by the kick's pitch sweep
 *    and the bass's few, wide peaks; this one is within 2 cents on all of
 *    them.
 * 4. A semitone spectrum, C1..B6, on the tuned semitones: each band sums
 *    the magnitudes under a raised-cosine window 0.8 semitone wide (at
 *    least two bins), scaled so that a sinusoid reads the same at any
 *    pitch. That is KeyFinder's spectral kernel (Sha'ath, "Estimation of
 *    key in digital music recordings", MSc thesis, Birkbeck 2011;
 *    libKeyFinder's chromatransform.cpp). Windows of 3 s, nearer its
 *    3.7 s, did no better than these 0.74 s ones.
 * 5. Whitening: each frame divided by its own moving average over 13
 *    semitones (an octave), so that a loud bass or a bright mix does not
 *    decide the key by level, and quiet passages count as well as loud
 *    ones. +2 points on the development set. The other HPCP-style steps
 *    measured no better (Gómez, "Tonal description of music audio
 *    signals", PhD thesis, UPF 2006): harmonic summation (each band
 *    credited to the fundamentals it can be a harmonic of) -0.6..+0.5, a
 *    nine-frame median against drums -0.8, weighting frames by how peaked
 *    their chroma is, to ignore drum-only frames, -1.6..-5.6: a drum-only
 *    frame adds a nearly flat chroma that the templates' shape ignores,
 *    while the weighting also drops tonal frames it cannot tell from drums.
 * 6. Three chroma vectors summed over the track: C2..B2 (the bass line),
 *    C3..B4 and C5..B6, each scaled to unit length. C1 is left out: the
 *    kick lives there, and the bass's harmonics carry its pitch class.
 * 7. Each of the 24 keys scores the dot product of the three vectors,
 *    rotated to its tonic, with the template of its mode (36 weights per
 *    mode, TEMPLATES below), fitted as one multinomial logistic regression
 *    over the 24 keys with the weights shared by all tonics
 *    (gen_key_fixtures.py train) on the development set. The published
 *    profiles on the same front end, development / test accuracy:
 *    Sha'ath's (KeyFinder's) 56.7 / 63.6 %, Krumhansl's 48.7 / 50.7,
 *    Temperley's 44.8 / 47.2, Faraldo's EDM profiles (Faraldo, Gómez, Jordà,
 *    Herrera, "Key estimation in electronic dance music", ECIR 2016)
 *    "edma" 47.3 / 52.6 and "edmm" 52.6 / 61.8 (it never names a major
 *    key), their gated "bgate" (Faraldo, Jordà, Herrera, "A multi-profile
 *    method for key estimation in EDM", AES Semantic Audio 2017) 52.5 /
 *    55.0; sums of several of them did worse than Sha'ath's alone. The
 *    fitted templates: 63.2 % (cross-validated) / 64.7 %, and they name
 *    every synthetic track, where Sha'ath's miss 19 % of the minor ones.
 *    Adding Sha'ath's scores to them changed nothing.
 * 8. A major key gets MAJOR_BIAS added to its score: with it the detector
 *    names as many tracks major as the development set's annotators did
 *    (32.6 against 32.5 %); without, 24.6 %, the dataset's minor majority
 *    leaking into the templates. It costs 1.3 points there and 0.2 on the
 *    test set, and gains 8 on major tracks in both: the user's library has
 *    more of them than Beatport's.
 * 9. Ties (equal scores to 1e-9, as a perfectly symmetric chroma gives)
 *    go to the higher cosine against Sha'ath's profiles on the summed
 *    chroma, then to minor (most dance music is), then to the lower tonic.
 *
 * The confidence is a probability that the key is right: a softmax over
 * the 24 scores at the temperature that fits the development set best; its
 * top probability, with the agreement of the track's 20-second segments
 * with the whole (the share whose own best key is the same), mapped
 * through a logistic fitted on the development set's cross-validated
 * answers (dj's margin between the two best correlations, 0..0.33 on the
 * user's tracks, said little). On the test set the answers at 0.9 or more
 * are right 89 % of the time, at 0.6..0.8 70 %, at 0.4..0.6 46 %. Answers of major keys
 * are less reliable there than their confidence says, as the test set is
 * 85 % minor where the development set is 67 %.
 *
 * Memory: the whitened 72-band frames of the whole track (2.8 MB an hour),
 * one FFT and a ring of the decimated signal (32 kB), each pass decimating
 * again as its frames need it (the STFT below). Time: ~110 ms for five
 * minutes at 44.1 kHz on one core (dj's: ~550 ms): 30 in each of the two
 * decimations, 30 in the tuning pass, 20 in the band pass.
 *
 * Silence (no tonal energy at all) gives "" and confidence 0, as before. */

#include "rxbox/analysis.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavutil/mem.h>
#include <libavutil/tx.h>

#include "rxbox/common.h"

#define KEY_RATE 11025.0
#define N_FFT 8192
#define HOP 4096
/* The bands: C1 .. B6 */
#define MIDI_LO 24
#define NB_BANDS 72
/* Whitening half-width in semitones */
#define WHITE 6
/* The chroma vectors: C2..B2, C3..B4, C5..B6 (band index ranges) */
#define NB_FEAT 36
/* Tuning peaks */
#define PEAK_LO 150.0
#define PEAK_HI 4000.0
#define PEAK_FLOOR 1e-2f
#define TUNE_STEP (N_FFT / HOP)
#define SEG_SECONDS 20.0

static const char *const MAJOR_NAMES[12] = {
    "C", "Db", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B",
};
/* Spelled as rekordbox displays them */
static const char *const MINOR_NAMES[12] = {
    "Cm", "Dbm", "Dm", "Ebm", "Em", "Fm", "F#m", "Gm", "Abm", "Am", "Bbm", "Bm",
};

/* Sha'ath's profiles (libKeyFinder constants.cpp), for ties only */
static const double SHAATH[2][12] = {
    { 7.23900502618145, 3.50351166725159, 3.58445177536649, 2.84511816478676, 5.81898892118550,
      4.55865057415321, 2.44778850545507, 6.99473192146830, 3.39106613673505, 4.55614256655143,
      4.07392666663524, 4.45932757378887 },
    { 7.00255045060284, 3.14360279015997, 4.35904319714963, 5.40418120718934, 3.67234420879306,
      4.08971184917798, 3.90791435991554, 6.19960288562316, 3.63424625625277, 2.87241191079876,
      5.35467999794543, 3.83242038595048 },
};

/* gen_key_fixtures.py train: per mode, the weights of the bass, middle and
 * treble chroma, each by interval above the tonic. */
static const double TEMPLATES[2][NB_FEAT] = {
    {
        +3.831086, +0.422911, -0.541941, -0.268662, -0.156559, -0.494596,
        -1.285167, +2.300662, -1.089226, -0.421346, -1.739469, -0.762827,
        +9.146794, -4.450626, +2.018196, -7.888354, +7.598707, -1.006002,
        -4.606749, +6.932495, -5.131950, +0.977236, -2.524926, +0.054478,
        +8.470257, -6.448374, +2.721740, -4.680061, +7.415636, -0.726577,
        -6.426598, +8.398842, -8.124914, -0.729397, -4.172607, +0.629826,
    },
    {
        +4.320080, +0.815372, -1.393019, +0.151058, -1.310203, -0.692815,
        -0.389036, +1.268844, -0.019809, -3.557954, +1.532833, -0.520218,
        +8.244831, -5.135689, +0.128518, +1.839275, +1.997730, -2.337059,
        -4.984217, +8.378914, -3.717863, -4.856572, +2.971073, -3.648241,
        +9.820503, -5.160344, +0.129472, +6.367447, -2.777826, +0.897393,
        -3.997295, +10.009723, -5.713693, -3.671759, +2.989522, -5.220917,
    },
};
static const double MAJOR_BIAS = 0.442;
static const double TEMPERATURE = 0.9349;
/* confidence = 1 / (1 + exp(-(CAL_A * logit(p) + CAL_B * agreement + CAL_C))) */
static const double CAL_A = 0.62796;
static const double CAL_B = 0.79616;
static const double CAL_C = -0.33589;

/* ------------------------------------------------------------- decimation */

static double bessel_i0(double x)
{
    double sum = 1.0, term = 1.0, q = x * x / 4;
    for (int k = 1; k < 500; k++) {
        term *= q / ((double)k * k);
        sum += term;
        if (term < sum * 1e-17)
            break;
    }
    return sum;
}

/* Every d-th sample of x (d >= 2) after a low-pass at the new Nyquist: a
 * sinc under a Kaiser window (beta 6, ~60 dB) of 16d + 1 taps, padded with
 * zeros to a multiple of 8 so that the dot products run as eight lanes.
 * Output k is centred on input k * d, zero padded at the ends. One sample
 * at a time, as the STFT reads them (below). */
typedef struct Decimator {
    const float *x;
    int64_t n;                      /* input samples */
    int64_t ny;                     /* output samples */
    int d, half, taps, padded;
    float *h;                       /* the taps, padded */
} Decimator;

static int decimator_init(Decimator *dc, const float *x, int64_t n, int d)
{
    int half = 8 * d, taps = 2 * half + 1, padded = (taps + 7) & ~7;
    *dc = (Decimator) {
        .x      = x,
        .n      = n,
        .ny     = (n + d - 1) / d,
        .d      = d,
        .half   = half,
        .taps   = taps,
        .padded = padded,
    };
    float *h = calloc(padded, sizeof(*h));
    if (!h)
        return RX_ERROR(ENOMEM);
    double sum = 0.0, i0b = bessel_i0(6.0);
    for (int i = 0; i < taps; i++) {
        double m = i - half, r = m / half;
        double s = m == 0 ? 1.0 : sin(M_PI * m / d) / (M_PI * m / d);
        h[i] = (float)(s * bessel_i0(6.0 * sqrt(1 - r * r)) / i0b);
        sum += h[i];
    }
    for (int i = 0; i < taps; i++)
        h[i] = (float)(h[i] / sum);
    dc->h = h;
    return 0;
}

/* Output sample k */
static float decimate_at(const Decimator *dc, int64_t k)
{
    const float *h = dc->h, *x = dc->x;
    int64_t c = k * dc->d;
    if (c >= dc->half && c - dc->half + dc->padded <= dc->n) {
        const float *p = x + c - dc->half;
        float acc[8] = { 0 };
        for (int i = 0; i < dc->padded; i += 8)
            for (int j = 0; j < 8; j++)
                acc[j] += h[i + j] * p[i + j];
        float lo4 = (acc[0] + acc[1]) + (acc[2] + acc[3]);
        float hi4 = (acc[4] + acc[5]) + (acc[6] + acc[7]);
        return lo4 + hi4;
    }
    float acc = 0;
    for (int i = 0; i < dc->taps; i++) {
        int64_t j = c - dc->half + i;
        if (j >= 0 && j < dc->n)
            acc += h[i] * x[j];
    }
    return acc;
}

/* ------------------------------------------------------------------- STFT */

/* The STFT reads the signal itself or, decimated, a ring of its last N_FFT
 * samples, filled as the frames need them. Each pass decimates again from
 * the start (stft_rewind) instead of reading a copy of the whole decimated
 * track: a quarter of the input at 44.1 kHz, 159 MB an hour, which made
 * the key the largest stage of rx_analyse (docs/BEATS.md, "Memory"). The
 * frames read the same samples either way, to the bit. */
typedef struct Stft {
    const float *x;                 /* the signal, when it is not decimated */
    const Decimator *dc;            /* else this, through the ring */
    float *ring;                    /* decimated sample j at j % N_FFT */
    int64_t filled;                 /* samples [0, filled) made; the ring holds the last N_FFT */
    int64_t n;                      /* samples the frames read */
    AVTXContext *tx;
    av_tx_fn fn;
    float *win, *frame, *mag;
    AVComplexFloat *spec;
} Stft;

static void stft_free(Stft *s)
{
    av_tx_uninit(&s->tx);
    av_freep(&s->win);
    av_freep(&s->frame);
    av_freep(&s->spec);
    av_freep(&s->mag);
    av_freep(&s->ring);
}

/* Over x (n samples) as it is, or over its decimation by dc */
static int stft_init(Stft *s, const float *x, int64_t n, const Decimator *dc)
{
    *s = (Stft) {
        .x  = dc ? NULL : x,
        .dc = dc,
        .n  = dc ? dc->ny : n,
    };
    float scale = 1.0f;
    /* AVERROR(e) is -e on POSIX systems, the same convention as rxbox's */
    int ret = av_tx_init(&s->tx, &s->fn, AV_TX_FLOAT_RDFT, 0, N_FFT, &scale, 0);
    if (ret < 0)
        return ret;
    s->win = av_malloc(sizeof(*s->win) * N_FFT);
    s->frame = av_malloc(sizeof(*s->frame) * (N_FFT + 2));
    s->spec = av_malloc(sizeof(*s->spec) * (N_FFT / 2 + 1));
    s->mag = av_malloc(sizeof(*s->mag) * (N_FFT / 2 + 1));
    if (dc)
        s->ring = av_malloc(sizeof(*s->ring) * N_FFT);
    if (!s->win || !s->frame || !s->spec || !s->mag || (dc && !s->ring))
        return RX_ERROR(ENOMEM);
    for (int i = 0; i < N_FFT; i++)
        s->win[i] = (float)(0.5 - 0.5 * cos(2 * M_PI * i / N_FFT));
    return 0;
}

/* A new pass, from the first frame */
static void stft_rewind(Stft *s)
{
    s->filled = 0;
}

/* The magnitudes of bins [0, nb) of frame f, centred on sample f * HOP.
 * Within a pass the frames come in order: the ring holds the N_FFT
 * samples up to the end of the latest frame, and no frame starts before
 * the one before it. */
static void stft_frame(Stft *s, int64_t f, int nb)
{
    int64_t start = f * HOP - N_FFT / 2;
    if (s->dc) {
        int64_t end = RX_MIN(start + N_FFT, s->n);
        for (; s->filled < end; s->filled++)
            s->ring[s->filled % N_FFT] = decimate_at(s->dc, s->filled);
        for (int i = 0; i < N_FFT; i++) {
            int64_t j = start + i;
            s->frame[i] = j >= 0 && j < s->n ? s->ring[j % N_FFT] * s->win[i] : 0.0f;
        }
    } else {
        for (int i = 0; i < N_FFT; i++) {
            int64_t j = start + i;
            s->frame[i] = j >= 0 && j < s->n ? s->x[j] * s->win[i] : 0.0f;
        }
    }
    s->fn(s->tx, s->spec, s->frame, sizeof(float));
    for (int b = 0; b < nb; b++)
        s->mag[b] = hypotf(s->spec[b].re, s->spec[b].im);
}

/* ----------------------------------------------------------------- tuning */

/* The power-weighted circular mean of the peaks' offsets from the
 * equal-tempered semitones, in semitones, over every TUNE_STEP-th frame
 * (windows that do not overlap: the tuning is one number for the whole
 * track, and the peaks are most of the cost). */
static double estimate_tuning(Stft *s, int64_t nb_frames, double fs)
{
    double df = fs / N_FFT, re = 0.0, im = 0.0;
    int lo = RX_MAX((int)ceil(PEAK_LO / df), 1);
    int hi = RX_MIN((int)(fmin(PEAK_HI, 0.45 * fs) / df), N_FFT / 2 - 1);
    if (hi <= lo)
        return 0.0;
    for (int64_t f = 0; f < nb_frames; f += TUNE_STEP) {
        stft_frame(s, f, hi + 2);
        float top = 0;
        for (int b = lo; b <= hi; b++)
            top = fmaxf(top, s->mag[b]);
        if (top <= 0)
            continue;
        float floor_mag = top * PEAK_FLOOR;
        for (int b = lo; b <= hi; b++) {
            float m = s->mag[b];
            if (!(m > floor_mag && m > s->mag[b - 1] && m >= s->mag[b + 1]))
                continue;
            float a = logf(s->mag[b - 1] + 1e-9f), c = logf(s->mag[b + 1] + 1e-9f), l = logf(m);
            float off = RX_CLAMP(0.5f * (a - c) / (a - 2 * l + c - 1e-12f), -0.5f, 0.5f);
            float peak = l - 0.25f * (a - c) * off;
            float dev = 12 * log2f((b + off) * (float)df / 440.0f);
            float w = expf(2 * peak);
            float sn, cs;
            sincosf(2 * (float)M_PI * (dev - rintf(dev)), &sn, &cs);
            re += w * cs;
            im += w * sn;
        }
    }
    if (re == 0.0 && im == 0.0)
        return 0.0;
    return atan2(im, re) / (2 * M_PI);
}

/* ------------------------------------------------------------- the bands */

typedef struct Kernel {
    int start[NB_BANDS], len[NB_BANDS];
    float *w;                       /* the bands' weights, one after another */
    int nb_bins;                    /* bins the bands read: [0, nb_bins) */
} Kernel;

/* KeyFinder's kernel on the tuned semitones (see the top of the file) */
static int kernel_init(Kernel *k, double fs, double tuning)
{
    *k = (Kernel) { 0 };
    int total = 0;
    double width[NB_BANDS], centre[NB_BANDS];
    for (int m = 0; m < NB_BANDS; m++) {
        double fc = 440.0 * pow(2.0, (MIDI_LO + m - 69 + tuning) / 12);
        centre[m] = fc * N_FFT / fs;
        width[m] = fmax(centre[m] * 0.8 * (pow(2.0, 1 / 12.0) - 1), 2.0);
        double b0 = centre[m] - width[m] / 2;
        int first = (int)ceil(b0), last = (int)floor(b0 + width[m]);
        /* Bands above Nyquist (very low rates) stay empty */
        if (last >= N_FFT / 2)
            last = first - 1;
        k->start[m] = first;
        k->len[m] = RX_MAX(last - first + 1, 0);
        total += k->len[m];
        if (k->len[m])
            k->nb_bins = RX_MAX(k->nb_bins, last + 1);
    }
    k->w = malloc(sizeof(*k->w) * RX_MAX(total, 1));
    if (!k->w)
        return RX_ERROR(ENOMEM);
    float *w = k->w;
    for (int m = 0; m < NB_BANDS; m++) {
        double b0 = centre[m] - width[m] / 2, sum = 0.0;
        for (int i = 0; i < k->len[m]; i++) {
            w[i] = (float)(1 - cos(2 * M_PI * (k->start[m] + i - b0) / width[m]));
            sum += w[i];
        }
        for (int i = 0; i < k->len[m]; i++)
            w[i] = (float)(w[i] / sum * width[m]);
        w += k->len[m];
    }
    return 0;
}

/* --------------------------------------------------------------- features */

typedef struct KeyFrames {
    int64_t nb_frames;
    float *bands;                   /* nb_frames x NB_BANDS, whitened */
    double tuning;                  /* semitones */
    double fs;                      /* the analysis rate */
} KeyFrames;

static void key_frames_free(KeyFrames *kf)
{
    free(kf->bands);
    kf->bands = NULL;
}

/* Steps 1-5: the whitened semitone bands of every frame */
static int key_frames(const float *mono, int64_t n, int rate, KeyFrames *kf)
{
    *kf = (KeyFrames) { 0 };
    int ret = 0;
    Decimator dc = { 0 };
    Stft s = { 0 };
    Kernel k = { 0 };

    /* Rates up to ~16.5 kHz are analysed as they are */
    int d = RX_MAX((int)lrint(rate / KEY_RATE), 1);
    if (d > 1 && (ret = decimator_init(&dc, mono, n, d)) < 0)
        goto fail;
    ret = stft_init(&s, mono, n, d > 1 ? &dc : NULL);
    if (ret < 0)
        goto fail;
    kf->fs = (double)rate / d;
    kf->nb_frames = s.n / HOP + 1;

    kf->tuning = estimate_tuning(&s, kf->nb_frames, kf->fs);
    ret = kernel_init(&k, kf->fs, kf->tuning);
    if (ret < 0)
        goto fail;

    kf->bands = malloc(sizeof(*kf->bands) * kf->nb_frames * NB_BANDS);
    if (!kf->bands) {
        ret = RX_ERROR(ENOMEM);
        goto fail;
    }
    float top = 0;
    stft_rewind(&s);
    for (int64_t f = 0; f < kf->nb_frames; f++) {
        stft_frame(&s, f, k.nb_bins);
        float *row = &kf->bands[f * NB_BANDS];
        const float *w = k.w;
        for (int m = 0; m < NB_BANDS; m++) {
            float acc = 0;
            for (int i = 0; i < k.len[m]; i++)
                acc += w[i] * s.mag[k.start[m] + i];
            w += k.len[m];
            row[m] = acc;
            top = fmaxf(top, acc);
        }
    }

    /* Whitening: each band over the mean of the 2 * WHITE + 1 around it,
     * the edge bands repeated beyond the ends; the floor keeps silence at
     * zero rather than amplifying rounding noise */
    float floor_mag = 1e-6f * top + 1e-12f;
    for (int64_t f = 0; f < kf->nb_frames; f++) {
        float *row = &kf->bands[f * NB_BANDS], env[NB_BANDS];
        for (int m = 0; m < NB_BANDS; m++) {
            float acc = 0;
            for (int j = m - WHITE; j <= m + WHITE; j++)
                acc += row[RX_CLAMP(j, 0, NB_BANDS - 1)];
            env[m] = acc / (2 * WHITE + 1);
        }
        for (int m = 0; m < NB_BANDS; m++)
            row[m] = row[m] / (env[m] + floor_mag);
    }

fail:
    if (ret < 0)
        key_frames_free(kf);
    stft_free(&s);
    free(k.w);
    free(dc.h);
    return ret;
}

/* Step 6: the three chroma vectors of frames [f0, f1), each of unit length
 * (all zero if the frames hold nothing). Pitch class 0 is C. */
static void key_chroma(const KeyFrames *kf, int64_t f0, int64_t f1, double out[NB_FEAT])
{
    static const int range[3][2] = { { 12, 24 }, { 24, 48 }, { 48, 72 } };
    memset(out, 0, sizeof(*out) * NB_FEAT);
    for (int64_t f = f0; f < f1; f++) {
        const float *row = &kf->bands[f * NB_BANDS];
        for (int v = 0; v < 3; v++)
            for (int m = range[v][0]; m < range[v][1]; m++)
                out[12 * v + (MIDI_LO + m) % 12] += row[m];
    }
    for (int v = 0; v < 3; v++) {
        double norm = 0.0;
        for (int i = 0; i < 12; i++)
            norm += out[12 * v + i] * out[12 * v + i];
        norm = sqrt(norm);
        for (int i = 0; i < 12; i++)
            out[12 * v + i] = norm > 0 ? out[12 * v + i] / norm : 0.0;
    }
}

/* Step 7-8: the score of each key, major tonics 0..11 then minor. */
static void key_scores(const double x[NB_FEAT], double score[24])
{
    for (int mode = 0; mode < 2; mode++) {
        for (int k = 0; k < 12; k++) {
            double acc = mode ? 0.0 : MAJOR_BIAS;
            for (int v = 0; v < 3; v++)
                for (int i = 0; i < 12; i++)
                    acc += TEMPLATES[mode][12 * v + i] * x[12 * v + (i + k) % 12];
            score[12 * mode + k] = acc;
        }
    }
}

/* Sha'ath's cosine for key `key` on the summed chroma: the tie-breaker */
static double shaath_cosine(const double x[NB_FEAT], int key)
{
    const double *p = SHAATH[key / 12];
    double c[12], dot = 0.0, nc = 0.0, np = 0.0;
    for (int i = 0; i < 12; i++)
        c[i] = x[i] + x[12 + i] + x[24 + i];
    for (int i = 0; i < 12; i++) {
        double v = c[(i + key) % 12];
        dot += v * p[i];
        nc += v * v;
        np += p[i] * p[i];
    }
    return nc > 0 ? dot / sqrt(nc * np) : 0.0;
}

/* Step 9: the best key, ties broken as described at the top */
static int best_key(const double x[NB_FEAT], const double score[24])
{
    int best = 0;
    for (int k = 1; k < 24; k++) {
        double d = score[k] - score[best];
        if (d > 1e-9) {
            best = k;
        } else if (d >= -1e-9) {
            double sk = shaath_cosine(x, k), sb = shaath_cosine(x, best);
            if (sk > sb + 1e-12 || (fabs(sk - sb) <= 1e-12 && k >= 12 && best < 12))
                best = k;
        }
    }
    return best;
}

/* ------------------------------------------------------------------- key */

int rx_analyse_key(const float *mono, int64_t nb_samples, int rate, RXKeyResult *out)
{
    *out = (RXKeyResult) { 0 };
    if (rate <= 0 || nb_samples < 0 || (nb_samples && !mono))
        return RX_ERROR(EINVAL);
    if (!nb_samples)
        return 0;

    KeyFrames kf;
    int ret = key_frames(mono, nb_samples, rate, &kf);
    if (ret < 0)
        return ret;

    double x[NB_FEAT], score[24];
    key_chroma(&kf, 0, kf.nb_frames, x);
    double energy = 0.0;
    for (int i = 0; i < NB_FEAT; i++)
        energy += x[i];
    if (energy <= 0) {
        key_frames_free(&kf);
        return 0;
    }
    key_scores(x, score);
    int best = best_key(x, score);

    /* The probability of the best key */
    double sum = 0.0;
    for (int k = 0; k < 24; k++)
        sum += exp(TEMPERATURE * (score[k] - score[best]));
    double p = RX_CLAMP(1.0 / sum, 1e-6, 1 - 1e-6);

    /* The agreement of the 20-second segments; a last one shorter than
     * half a segment, and silent ones, are left out */
    int64_t seg = RX_MAX(llrint(SEG_SECONDS * kf.fs / HOP), 1);
    int nb_seg = 0, nb_agree = 0;
    for (int64_t f0 = 0; f0 < kf.nb_frames; f0 += seg) {
        int64_t f1 = RX_MIN(f0 + seg, kf.nb_frames);
        if (f0 && f1 - f0 < seg / 2)
            break;
        double xs[NB_FEAT], ss[24], e = 0.0;
        key_chroma(&kf, f0, f1, xs);
        for (int i = 0; i < NB_FEAT; i++)
            e += xs[i];
        if (e <= 0)
            continue;
        key_scores(xs, ss);
        nb_agree += best_key(xs, ss) == best;
        nb_seg++;
    }
    double agreement = nb_seg ? (double)nb_agree / nb_seg : 0.0;

    const char *name = best >= 12 ? MINOR_NAMES[best - 12] : MAJOR_NAMES[best];
    snprintf(out->key, sizeof(out->key), "%s", name);
    out->confidence = 1 / (1 + exp(-(CAL_A * log(p / (1 - p)) + CAL_B * agreement + CAL_C)));
    out->tuning_cents = 100 * kf.tuning;
    key_frames_free(&kf);
    return 0;
}
