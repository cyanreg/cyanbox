/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* The grid with Beat This! (cb_beats_hybrid, src/analysis/hybrid.c) on made
 * activations, beside cyanbox's result for the same "track", where the
 * answer is known:
 *
 *  - cyanbox right: its grid is kept (every beat within a microsecond),
 *    and the bar is where the downbeat activation is;
 *  - cyanbox on the off-beat and unsure of it (a phase flag): its tempo, to
 *    the bit, at Beat This!'s phase;
 *  - a grid 0.3 of a beat late when cyanbox is sure (no flag, a margin of
 *    20): still moved, that being more than the quarter of a beat moving a
 *    sure grid takes (and moved the right way);
 *  - Beat This! 60 ms late on a grid cyanbox is sure of (as on MK's "Good
 *    Time (Extended Mix)", heard right, 49 ms): the grid kept, and not
 *    flagged low-confidence;
 *  - cyanbox at half the tempo: its grid doubled, Beat This!'s level;
 *  - no grid from cyanbox: Beat This!'s own, steady, at its tempo;
 *  - a hit after silence, and no grid from cyanbox: no beat before the
 *    music starts (Beat This!'s line reaches back to 0);
 *  - under 8 s: no grid, as cyanbox's own.
 *
 * Beat This!'s beats come LAG (9 ms) after cyanbox's on the same audio, as
 * the network's did on the osu! maps; the made activations are placed so. */

#include "cyanbox/analysis.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;

#define CHECK(cond, ...)                                                    \
    do {                                                                    \
        if (!(cond)) {                                                      \
            printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond);          \
            printf(__VA_ARGS__);                                            \
            printf("\n");                                                   \
            fails++;                                                        \
        }                                                                   \
    } while (0)

#define FPS 50.0
#define LAG 0.009

/* Logits for beats every 60 / bpm s from t0 (cyanbox's timeline), a
 * downbeat on every fourth from the one at index `down`: a bump of +6 over
 * -8, a frame and a half wide, as the network's are on a clear beat. */
typedef struct Act {
    float *beat, *down;
    int n;
} Act;

static void make_act_late(Act *a, double seconds, double bpm, double t0, int down, double late)
{
    a->n = (int)(seconds * FPS) + 1;
    a->beat = malloc(a->n * sizeof(float));
    a->down = malloc(a->n * sizeof(float));
    for (int f = 0; f < a->n; f++)
        a->beat[f] = a->down[f] = -8.0f;
    double p = 60.0 / bpm;
    for (int k = 0; t0 + k * p < seconds; k++) {
        double c = (t0 + k * p + LAG + late) * FPS;
        bool d = ((k - down) % 4 + 4) % 4 == 0;
        for (int f = (int)c - 4; f <= (int)c + 4; f++) {
            if (f < 0 || f >= a->n)
                continue;
            float v = (float)(14.0 * exp(-(f - c) * (f - c) / (2 * 0.7 * 0.7)));
            a->beat[f] = fmaxf(a->beat[f], -8.0f + v);
            if (d)
                a->down[f] = fmaxf(a->down[f], -8.0f + v);
        }
    }
}

static void make_act(Act *a, double seconds, double bpm, double t0, int down)
{
    make_act_late(a, seconds, bpm, t0, down, 0.0);
}

static void act_free(Act *a)
{
    free(a->beat);
    free(a->down);
}

/* cyanbox's result: one straight section at bpm from t0. */
static void make_cb(CBBeatResult *r, double bpm, double t0, unsigned flags, double margin)
{
    memset(r, 0, sizeof(*r));
    if (bpm > 0)
        cb_grid_init(&r->grid, t0, bpm);
    r->flags = flags;
    r->phase_margin = margin;
    r->confidence = 0.9;
    r->start_kind = CB_START_AT_ONCE;
}

/* How many of the grid's beats in [lo, hi) are within tol of the line
 * t0 + k * 60 / bpm, of how many there are. */
static double on_line(const CBGrid *g, double dur, double bpm, double t0, double tol, double lo,
                      double hi, int *count)
{
    CBBeat *b = NULL;
    int nb = 0;
    *count = 0;
    if (cb_grid_beats(g, dur, &b, &nb) < 0)
        return 0.0;
    double p = 60.0 / bpm;
    int on = 0, n = 0;
    for (int i = 0; i < nb; i++) {
        if (b[i].time < lo || b[i].time >= hi)
            continue;
        double k = round((b[i].time - t0) / p);
        on += fabs(b[i].time - (t0 + k * p)) < tol;
        n++;
    }
    free(b);
    *count = n;
    return n ? (double)on / n : 0.0;
}

/* The number in the bar of the grid's beat nearest t. */
static int number_at(const CBGrid *g, double t)
{
    int number = 0;
    cb_grid_nearest_beat(g, t, &number);
    return number;
}

static void test_cyanbox_right(void)
{
    const double dur = 120.0, bpm = 128.0, t0 = 0.4;
    Act a;
    make_act(&a, dur, bpm, t0, 2);
    CBBeatResult r;
    make_cb(&r, bpm, t0, 0, 20.0);
    int ret = cb_beats_hybrid(a.beat, a.down, a.n, dur, &r);
    CHECK(ret == 0, "%d", ret);
    CHECK(r.grid.nb_sections == 1 && !r.grid.sections[0].measured, "%d sections",
          r.grid.nb_sections);
    CHECK(fabs(cb_grid_bpm(&r.grid) - bpm) < 1e-6, "%.6f BPM", cb_grid_bpm(&r.grid));
    int n;
    double share = on_line(&r.grid, dur, bpm, t0, 1e-6, 0.0, dur, &n);
    CHECK(share == 1.0 && n > 250, "%.3f of %d beats on cyanbox's", share, n);
    /* beat index 2 is the downbeat */
    double p = 60.0 / bpm;
    CHECK(number_at(&r.grid, t0 + 2 * p) == 1 && number_at(&r.grid, t0 + 3 * p) == 2 &&
          number_at(&r.grid, t0 + 6 * p) == 1, "numbers %d %d %d", number_at(&r.grid, t0 + 2 * p),
          number_at(&r.grid, t0 + 3 * p), number_at(&r.grid, t0 + 6 * p));
    CHECK(r.confidence > 0.95, "confidence %.3f", r.confidence);
    CHECK(!(r.flags & (CB_GRID_FLAG_LOW_CONFIDENCE | CB_GRID_FLAG_DYNAMIC)), "flags %#x", r.flags);
    cb_beat_result_free(&r);
    act_free(&a);
}

static void test_off_beat(bool sure)
{
    const double dur = 150.0, bpm = 124.0, t0 = 0.3, p = 60.0 / bpm;
    Act a;
    make_act(&a, dur, bpm, t0, 0);
    CBBeatResult r;
    make_cb(&r, bpm, t0 + (sure ? 0.3 : 0.5) * p, sure ? 0 : CB_GRID_FLAG_PHASE_UNCERTAIN,
            sure ? 20.0 : 1.2);
    int ret = cb_beats_hybrid(a.beat, a.down, a.n, dur, &r);
    CHECK(ret == 0, "%d", ret);
    int n;
    double share = on_line(&r.grid, dur, bpm, t0, 0.003, 1.0, dur - 1.0, &n);
    CHECK(share > 0.99 && n > 250, "%s: %.3f of %d beats on the beat", sure ? "sure" : "unsure",
          share, n);
    /* cyanbox's tempo itself, not one fitted to Beat This!'s beats */
    CHECK(r.grid.nb_sections == 1 && fabs(cb_grid_bpm(&r.grid) - bpm) < 1e-9, "%d sections, "
          "%.9f BPM", r.grid.nb_sections, cb_grid_bpm(&r.grid));
    CHECK(number_at(&r.grid, t0 + 8 * p) == 1, "number %d", number_at(&r.grid, t0 + 8 * p));
    cb_beat_result_free(&r);
    act_free(&a);
}

static void test_late(void)
{
    const double dur = 120.0, bpm = 128.0, t0 = 0.4;
    Act a;
    make_act_late(&a, dur, bpm, t0, 0, 0.060);
    CBBeatResult r;
    make_cb(&r, bpm, t0, 0, 20.0);
    int ret = cb_beats_hybrid(a.beat, a.down, a.n, dur, &r);
    CHECK(ret == 0, "%d", ret);
    int n;
    double share = on_line(&r.grid, dur, bpm, t0, 1e-6, 0.0, dur, &n);
    CHECK(share == 1.0 && n > 250, "%.3f of %d beats on cyanbox's", share, n);
    CHECK(r.confidence > 0.9 && !(r.flags & CB_GRID_FLAG_LOW_CONFIDENCE),
          "confidence %.3f, flags %#x", r.confidence, r.flags);
    cb_beat_result_free(&r);
    act_free(&a);
}

static void test_half_tempo(void)
{
    const double dur = 120.0, bpm = 140.0, t0 = 0.25;
    Act a;
    make_act(&a, dur, bpm, t0, 1);
    CBBeatResult r;
    make_cb(&r, bpm / 2, t0, CB_GRID_FLAG_OCTAVE_UNCERTAIN, 5.0);
    int ret = cb_beats_hybrid(a.beat, a.down, a.n, dur, &r);
    CHECK(ret == 0, "%d", ret);
    /* cyanbox's grid doubled, not one fitted to Beat This!'s beats */
    CHECK(r.grid.nb_sections == 1 && fabs(cb_grid_bpm(&r.grid) - bpm) < 1e-6,
          "%d sections, %.9f BPM", r.grid.nb_sections, cb_grid_bpm(&r.grid));
    int n;
    double share = on_line(&r.grid, dur, bpm, t0, 0.003, 1.0, dur - 1.0, &n);
    CHECK(share > 0.99 && n > 250, "%.3f of %d beats on the beat", share, n);
    cb_beat_result_free(&r);
    act_free(&a);
}

static void test_no_grid(void)
{
    const double dur = 100.0, bpm = 125.0, t0 = 0.11;
    Act a;
    make_act(&a, dur, bpm, t0, 3);
    CBBeatResult r;
    make_cb(&r, 0, 0, CB_GRID_FLAG_LOW_CONFIDENCE, 0.0);
    int ret = cb_beats_hybrid(a.beat, a.down, a.n, dur, &r);
    CHECK(ret == 0, "%d", ret);
    CHECK(r.grid.nb_sections == 1 && !r.grid.sections[0].measured, "%d sections",
          r.grid.nb_sections);
    CHECK(fabs(cb_grid_bpm(&r.grid) - bpm) < 0.01, "%.4f BPM", cb_grid_bpm(&r.grid));
    int n;
    double share = on_line(&r.grid, dur, bpm, t0, 0.003, 0.0, dur, &n);
    CHECK(share == 1.0 && n > 200, "%.3f of %d beats on the beat", share, n);
    double p = 60.0 / bpm;
    CHECK(number_at(&r.grid, t0 + 3 * p) == 1 && number_at(&r.grid, t0 + 4 * p) == 2,
          "numbers %d %d", number_at(&r.grid, t0 + 3 * p), number_at(&r.grid, t0 + 4 * p));
    cb_beat_result_free(&r);
    act_free(&a);
}

static void test_music_start(void)
{
    /* The beats start at 10 s, after silence: a hit there. */
    const double dur = 90.0, bpm = 130.0, t0 = 10.0;
    Act a;
    make_act(&a, dur, bpm, t0, 0);
    CBBeatResult r;
    make_cb(&r, 0, 0, CB_GRID_FLAG_LOW_CONFIDENCE, 0.0);
    r.music_start = t0;
    r.start_kind = CB_START_HIT;
    int ret = cb_beats_hybrid(a.beat, a.down, a.n, dur, &r);
    CHECK(ret == 0, "%d", ret);
    CHECK(cb_grid_valid(&r.grid) && fabs(r.grid.sections[0].start - t0) < 0.003,
          "first beat at %.4f", r.grid.nb_sections ? r.grid.sections[0].start : NAN);
    CHECK(number_at(&r.grid, t0) == 1, "number %d", number_at(&r.grid, t0));
    cb_beat_result_free(&r);
    act_free(&a);
}

static void test_short(void)
{
    Act a;
    make_act(&a, 7.0, 128.0, 0.1, 0);
    CBBeatResult r;
    make_cb(&r, 0, 0, CB_GRID_FLAG_LOW_CONFIDENCE, 0.0);
    int ret = cb_beats_hybrid(a.beat, a.down, a.n, 7.0, &r);
    CHECK(ret == 0 && r.grid.nb_sections == 0 && r.flags == CB_GRID_FLAG_LOW_CONFIDENCE,
          "%d, %d sections, flags %#x", ret, r.grid.nb_sections, r.flags);
    cb_beat_result_free(&r);
    act_free(&a);
}

int main(void)
{
    test_cyanbox_right();
    test_off_beat(false);
    test_off_beat(true);
    test_late();
    test_half_tempo();
    test_no_grid();
    test_music_start();
    test_short();
    if (fails) {
        printf("test_hybrid: %d failures\n", fails);
        return 1;
    }
    printf("test_hybrid: ok\n");
    return 0;
}
