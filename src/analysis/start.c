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
 *  - the threshold: after digital silence, 65 dB under the loud level, so
 *    that a few ms of dither before the music is not the music, while a
 *    fade from -60 dB still is from its first hit; over a noise floor
 *    (dither, hiss), 20 dB above the floor but never lower than 50 dB under
 *    the loud level (-60 dBFS on a loud master): a fade is music where it
 *    is heard over the floor. The music stays over the threshold for a
 *    third of the next 200 ms; a click in a pregap does not;
 *  - a hit: two frames after the level crosses the threshold it is 25 dB
 *    over it. Its time is taken to the sample: the first sample at 3 % of
 *    the hit's peak (a sine kick from zero phase is there within two
 *    samples), above the floor's noise;
 *  - anything slower starts, out of digital silence, where it leaves the
 *    silence: its first sound, or, with some dither before it, the first
 *    frame of the run of frames more than 6 dB above the dither's
 *    commonest level that ends in the threshold crossing. Over a noise
 *    floor it starts at the threshold, where it is heard, not where it
 *    leaves the floor: on pushover's "PASSIVE" the rise leaves the -96 dB
 *    dither at 1.03 s, but nothing can be heard there (docs/BEATS.md,
 *    section 11). A fade rises for a second or more until it is within
 *    15 dB of the loud level; a swell (a riser, a reversed cymbal, a pad's
 *    attack) rises faster;
 *  - but a fade may be only the way in: the first transient after its
 *    origin that stands 10 dB over everything in the half second before
 *    it is a hit, and the music starts there ("PASSIVE" again: a pad
 *    fades in, the first beat is the hit at 3.6 s). The fade's origin is
 *    kept as `intro`. */

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
#define TRANSIENT_JUMP    12.0      /* a transient: the level rises this much over four frames */
#define TRANSIENT_AFTER_S 1.0       /* looked for until this long after the rise is done */
#define QUIET_BEFORE_S    0.5       /* the hit stands over the loudest of this much before it */
#define QUIET_BELOW       10.0      /* by at least this */
#define HIT_OVER_BEFORE   1.5       /* a hit after a fade begins where it is this much over the fade's peaks */
#define ATTACK_FRAMES     2         /* a sound loudest this soon in its first 100 ms attacks
                                     * like a hit (a kick fading in) */
#define ATTACK_LOOK       15

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

/* Whether the sound starting at frame f attacks like a hit: the loudest
 * of its first 100 ms comes within its first two frames and then decays.
 * A kick fading in from -60 dB does; a pad or a riser, rising for seconds,
 * is loudest at the end of any stretch. */
static bool fast_attack(const float *e, int nf, int f)
{
    double peak = SILENT_DB;
    int at = f;
    for (int i = f; i <= RX_MIN(f + ATTACK_LOOK, nf - 1); i++) {
        double l = smooth_db(e, nf, i);
        if (l > peak) {
            peak = l;
            at = i;
        }
    }
    return at - f <= ATTACK_FRAMES;
}

/* The first sample of the hit at frame fs: 3 % of its peak (a sine kick
 * from zero phase is there within two samples), above the floor's noise
 * and, after a fade, above the fade's own peaks of the 50 ms before. */
static double hit_sample(const float *x, int64_t n, int fs, double floor_amp, bool after_fade)
{
    int64_t a = (int64_t)RX_MAX(fs - 2, 0) * FRAME;
    int64_t b = RX_MIN((int64_t)(fs + 4) * FRAME + RX_BEAT_RATE * 3 / 100, n);
    float peak = 0.0f;
    for (int64_t i = a; i < b; i++)
        peak = fmaxf(peak, fabsf(x[i]));
    double amp = fmax(HIT_PEAK * peak, floor_amp);
    if (after_fade) {
        float before = 0.0f;
        for (int64_t i = RX_MAX(a - 7 * FRAME, 0); i < a; i++)
            before = fmaxf(before, fabsf(x[i]));
        amp = fmax(amp, HIT_OVER_BEFORE * before);
    }
    int64_t at = a;
    while (at < b && fabsf(x[at]) < amp)
        at++;
    return (double)at / RX_BEAT_RATE;
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

    const double floor_amp = pow(10.0, (base + START_ABOVE) / 20.0);
    if (out->jump_db >= HIT_JUMP) {
        out->kind = RX_START_HIT;
        out->start = hit_sample(x, n, fs, floor_amp, false);
        out->intro = out->start;
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
        out->intro = fo * fsec;

        /* A fade or a swell may be only the way in. pushover's "PASSIVE"
         * fades a pad in from 1.9 s, and its first beat is the hit at
         * 3.6 s, 22 dB over the pad within 5 ms: it is all there in the
         * level. The first transient after the rise's origin (12 dB over
         * four frames, 27 ms, at the music's level) that stands at least
         * 10 dB over the loudest of the half second before it is that hit,
         * and the music starts there. A sound that attacks like a hit at
         * the origin itself is a fade with the beat under it (drums fading
         * in: the synthetic fades) and is not searched; a later transient
         * that is not clearly over what came before is a louder kick in
         * such a fade. Both stay a fade. The rise out of digital silence
         * is no transient. */
        int f1 = -1;
        int fstop = RX_MIN(nf - 3, fl + (int)(TRANSIENT_AFTER_S / fsec));
        for (int f = RX_MAX(fo, 2); !fast_attack(e, nf, fo) && f < fstop; f++) {
            double pre = smooth_db(e, nf, f - 2), post = smooth_db(e, nf, f + 2);
            if (pre > SILENT_DB && post >= thr && post - pre >= TRANSIENT_JUMP) {
                f1 = f;
                break;
            }
        }
        if (f1 > fo + 2) {
            double post = smooth_db(e, nf, f1 + 2), before = SILENT_DB;
            for (int f = RX_MAX(f1 - 2 - (int)(QUIET_BEFORE_S / fsec), 0); f <= f1 - 2; f++)
                before = fmax(before, smooth_db(e, nf, f));
            if (post - before >= QUIET_BELOW) {
                out->kind = RX_START_HIT;
                out->jump_db = post - before;
                out->start = hit_sample(x, n, f1, floor_amp, true);
                free(e);
                return 0;
            }
        }
        out->kind = out->rise >= FADE_RISE ? RX_START_FADE : RX_START_SWELL;
        out->start = fo * fsec;
    }
    free(e);
    return 0;
}
