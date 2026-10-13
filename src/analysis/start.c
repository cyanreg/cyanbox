/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* Where the music starts, measured on the audio's level (docs/BEATS.md,
 * "Where the music starts").
 *
 * The onset signals cannot say it: each is normalised to the whole track
 * (local mean removed, unit variance), so a fade-in's first seconds are
 * nothing to them, and dj's rule (the first frame of flux above a quarter
 * of the track's 99th percentile) finds the first *loud* onset, which in a
 * fade is anywhere along the rise. The level can. Frames of 294 samples
 * (6.7 ms, the onsets' frame rate), levels smoothed over three (20 ms, so
 * that a low note's cycle does not make a frame look silent):
 *
 *  - the loud level: the 90th percentile of the frame levels;
 *  - the floor: the commonest level (1 dB bins) of the frames before the
 *    music is surely playing (within 45 dB of the loud level), digital
 *    silence counted as a level of its own. Dither (-90 to -100 dB) and
 *    hiss (-80 dB) are floors. A floor is at least 60 dB under the loud
 *    level and 60 ms long, or there is none and the music starts at once:
 *    an MP3's encoder delay (25 ms with LAME) is no silence, and a track
 *    cut out of a mix would "start" there with whatever is playing;
 *  - the threshold: 20 dB above the floor and no more than 65 dB under the
 *    loud level, so that dither or hiss after digital silence is not the
 *    music, while a fade from -60 dB still is from its first hit. The music
 *    stays over it for a third of the next 200 ms; a click in a pregap
 *    does not;
 *  - a hit: two frames after the level crosses the threshold it is 25 dB
 *    over it. Its time is taken to the sample: the first sample at 3 % of
 *    the hit's peak (a sine kick from zero phase is there within two
 *    samples), above the floor's noise;
 *  - anything slower starts where its rise leaves the floor: the first
 *    frame of the run of frames more than 6 dB above the floor's commonest
 *    level that ends in the threshold crossing (out of digital silence,
 *    its first sound). A fade rises for a second or more until it is
 *    within 15 dB of the loud level; a swell (a riser, a reversed cymbal,
 *    a pad's attack) rises faster. A producer's fade begins on a bar line,
 *    where it leaves the floor, not where it happens to cross a threshold:
 *    on pushover's "PASSIVE" the rise leaves the -96 dB dither at 1.03 s,
 *    23 ms before a beat, is 20 dB over it at 1.48 s and reaches -60 dB at
 *    1.75 s. */

#include "beats_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define FRAME        RX_BEAT_HOP
#define SILENT_DB    (-200.0)
#define HIST_LO      (-160)         /* level histogram, 1 dB bins */
#define HIST_HI      30
#define HIST_N       (HIST_HI - HIST_LO)

#define LOUD_PCT     0.90           /* the loud level: this percentile of the frame levels */
#define SURE_BELOW   45.0           /* the music is surely playing within this of the loud level */
#define FLOOR_BELOW  60.0           /* a floor lies at least this far under the loud level */
#define START_ABOVE  20.0           /* the music starts this far above its floor */
#define START_BELOW  65.0           /* and at most this far under the loud level */
#define AUDIBLE_BELOW 50.0          /* over a noise floor: within this of the loud level */
#define HIT_JUMP     25.0           /* a hit: this far over the start's threshold two frames on */
#define RISE_WITHIN  15.0           /* a rise ends within this of the loud level */
#define RISE_MAX_S   30.0           /* (measured up to here) */
#define FADE_RISE    1.0            /* a fade rises for at least this long; a swell less */
#define MIN_FLOOR_S  0.060          /* a shorter floor is no silence */
#define ORIGIN_ABOVE 6.0            /* a rise leaves the floor this far above it */
#define HIT_PEAK     0.03           /* a hit's first sample: 3 % of its peak */
#define SUSTAIN_S    0.200          /* the music stays over the threshold for a third of */
#define SUSTAIN_PART 3              /* the next 200 ms: a click in a pregap does not */

static double level_db(double e)
{
    return e > 1e-20 ? 10.0 * log10(e) : SILENT_DB;
}

static int bin_of(double db)
{
    return RX_CLAMP((int)floor(db) - HIST_LO, 0, HIST_N - 1);
}

/* The level of frames [f - 1, f + 1]: 20 ms, so that a low note's cycle
 * does not make a frame look silent. */
static double smooth_db(const float *e, int nf, int f)
{
    int a = RX_MAX(f - 1, 0), b = RX_MIN(f + 1, nf - 1);
    double s = 0.0;
    for (int i = a; i <= b; i++)
        s += e[i];
    return level_db(s / (b - a + 1));
}

/* Whether the level from frame f on stays over thr for a part of the next
 * SUSTAIN_S: music does, a click or a tick in a CD's pregap does not. */
static bool sustained(const float *e, int nf, int f, double thr)
{
    int n = (int)(SUSTAIN_S * RX_BEAT_RATE / FRAME), over = 0;
    for (int i = f; i < RX_MIN(f + n, nf); i++)
        over += smooth_db(e, nf, i) >= thr;
    return over * SUSTAIN_PART >= n;
}

/* The commonest level (1 dB bins) of frames [a, b), digital silence
 * counted as a level of its own when `with_silence`. The lowest bin wins
 * a tie. SILENT_DB when that is digital silence or nothing is counted. */
static double common_level(const float *e, int a, int b, bool with_silence)
{
    int hist[HIST_N + 1] = { 0 };       /* [0]: digital silence */
    for (int f = a; f < b; f++) {
        double l = level_db(e[f]);
        if (l > SILENT_DB)
            hist[1 + bin_of(l)]++;
        else if (with_silence)
            hist[0]++;
    }
    int best = -1;
    for (int i = 0; i <= HIST_N; i++) {
        if (hist[i] && (best < 0 || hist[i] > hist[best]))
            best = i;
    }
    if (best <= 0)
        return SILENT_DB;
    return HIST_LO + (best - 1) + 0.5;
}

int rx_music_start(const float *x, int64_t n, RXMusicStart *out)
{
    *out = (RXMusicStart) {
        .kind     = RX_START_AT_ONCE,
        .floor_db = SILENT_DB,
        .loud_db  = SILENT_DB,
    };
    const double fsec = (double)FRAME / RX_BEAT_RATE;
    int64_t nf64 = n / FRAME;
    if (nf64 < 16 || nf64 > INT32_MAX)
        return 0;
    const int nf = (int)nf64;
    float *e = malloc((size_t)nf * sizeof(*e));
    if (!e)
        return RX_ERROR(ENOMEM);

    /* The frames' mean squares, and the loud level from their histogram. */
    int hist[HIST_N] = { 0 }, audible = 0;
    for (int f = 0; f < nf; f++) {
        const float *p = x + (int64_t)f * FRAME;
        double s = 0.0;
        for (int i = 0; i < FRAME; i++)
            s += (double)p[i] * p[i];
        e[f] = (float)(s / FRAME);
        double l = level_db(e[f]);
        if (l > SILENT_DB) {
            hist[bin_of(l)]++;
            audible++;
        }
    }
    if (!audible) {
        free(e);
        return 0;
    }
    int want = (int)(LOUD_PCT * audible), acc = 0, lb = 0;
    while (lb < HIST_N - 1 && acc + hist[lb] <= want)
        acc += hist[lb++];
    const double loud = HIST_LO + lb + 0.5;
    out->loud_db = loud;

    /* Where the music is surely playing; the floor is what comes before. */
    int fq = 0;
    while (fq < nf && smooth_db(e, nf, fq) < loud - SURE_BELOW)
        fq++;
    const int min_floor = (int)ceil(MIN_FLOOR_S / fsec);
    double base = fq >= min_floor ? common_level(e, 0, fq, true) : 0.0;
    int fs = 0;
    /* After digital silence the first sound is the music; over a noise
     * floor (dither, hiss) a fade is music once it is audible over it, 50 dB
     * under the loud level (-60 dBFS on a loud master): below that a
     * listener hears the floor. */
    const bool digital = base <= SILENT_DB;
    double thr = digital ? loud - START_BELOW
                         : fmax(base + START_ABOVE, loud - AUDIBLE_BELOW);
    if (fq >= min_floor && base <= loud - FLOOR_BELOW) {
        while (fs < nf && (smooth_db(e, nf, fs) < thr || !sustained(e, nf, fs, thr)))
            fs++;
    }
    if (fs < min_floor || fs >= nf) {
        /* The music starts at once: perhaps rising, but from the first frame. */
        free(e);
        return 0;
    }
    out->floor_db = base;

    /* How the music arrives: how far over the threshold the level is two
     * frames on, and how long it takes to come within 15 dB of the loud
     * level. */
    out->jump_db = smooth_db(e, nf, RX_MIN(fs + 2, nf - 1)) - thr;
    int fend = RX_MIN(nf, fs + (int)(RISE_MAX_S / fsec)), fl = fs;
    while (fl < fend && smooth_db(e, nf, fl) < loud - RISE_WITHIN)
        fl++;
    out->rise = (fl - fs) * fsec;
    out->audible = fs * fsec;

    if (out->jump_db >= HIT_JUMP) {
        /* The hit's first sample: 3 % of its peak (a sine kick from zero
         * phase is there within two samples), above the floor's noise. */
        int64_t a = (int64_t)RX_MAX(fs - 2, 0) * FRAME;
        int64_t b = RX_MIN((int64_t)(fs + 4) * FRAME + RX_BEAT_RATE * 3 / 100, n);
        float peak = 0.0f;
        for (int64_t i = a; i < b; i++)
            peak = fmaxf(peak, fabsf(x[i]));
        double amp = fmax(HIT_PEAK * peak, pow(10.0, (base + START_ABOVE) / 20.0));
        int64_t at = a;
        while (at < b && fabsf(x[at]) < amp)
            at++;
        out->kind = RX_START_HIT;
        out->start = (double)at / RX_BEAT_RATE;
    } else {
        /* Over a noise floor, where the rise becomes audible. Out of digital
         * silence, where it leaves the silence: its first sound, or where
         * it rises out of the dither that some masters put before it. */
        int fo = fs;
        if (digital) {
            double fl0 = common_level(e, 0, fs, false);
            if (fl0 > SILENT_DB) {
                while (fo > 0 && smooth_db(e, nf, fo - 1) > fl0 + ORIGIN_ABOVE)
                    fo--;
            } else {
                while (fo > 0 && e[fo - 1] > 0.0f)
                    fo--;
            }
        }
        out->kind = out->rise >= FADE_RISE ? RX_START_FADE : RX_START_SWELL;
        out->start = fo * fsec;
    }
    free(e);
    return 0;
}
