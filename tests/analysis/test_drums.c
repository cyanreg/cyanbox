/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* Where the drums come in (cb_beats_drums, src/analysis/drums.c) on made
 * tracks at 125 BPM, the grid's bar 1 at 0.5 s, where the answer is known:
 *
 *  - a pad for eight bars, then a kick on every beat: bar 9;
 *  - the kick from the first beat: bar 1;
 *  - one opening hit on bar 1, the pad for eight bars, then the kick: bar 9
 *    (a hit is no drums: two of four downbeats must have them);
 *  - the kick fading in over four bars, 7.5 dB a bar: bar 5, where it is
 *    full (bar 4, 7.5 dB under, is still rising);
 *  - a bass alone for eight bars, 6 dB under the kick and bass over the
 *    whole beat but 13 dB under them in the kick's first 80 ms: bar 9;
 *  - the kick only from 42 % of the track (a chorus): the start of the
 *    music;
 *
 * and the automatic cue: on the first downbeat at or after where the drums
 * come in; the cue an earlier analysis placed is taken off (on the grid's
 * first downbeat, or where this one puts it) and placed again, a cue the
 * DJ made (a name, a colour, a hot cue, a loop, somewhere else) is not. */

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

#define RATE 44100
#define BPM  125.0
#define T0   0.5
#define BEAT (60.0 / BPM)
#define BAR  (4 * BEAT)

typedef struct Track {
    float *x;
    int64_t n;
} Track;

static int track_new(Track *t, double seconds)
{
    t->n = (int64_t)(seconds * RATE);
    t->x = calloc((size_t)t->n, sizeof(float));
    return t->x ? 0 : -1;
}

/* A kick: 55 Hz, falling 25 a second, 0.8 high, `db` under that. */
static void kick(Track *t, double at, double db)
{
    const double g = 0.8 * pow(10.0, db / 20.0);
    for (int64_t i = (int64_t)(at * RATE); i < t->n && i < (int64_t)((at + 0.4) * RATE); i++) {
        double s = (double)i / RATE - at;
        t->x[i] += (float)(g * sin(2 * M_PI * 55.0 * s) * exp(-25.0 * s));
    }
}

/* A steady tone over [from, to). */
static void tone(Track *t, double from, double to, double hz, double amp)
{
    for (int64_t i = (int64_t)(from * RATE); i < t->n && i < (int64_t)(to * RATE); i++)
        t->x[i] += (float)(amp * sin(2 * M_PI * hz * ((double)i / RATE)));
}

/* Kicks on every beat of bars [bar, end) (1-based), `db` under full. */
static void kicks(Track *t, int bar, double end_s, double db)
{
    for (double at = T0 + (bar - 1) * BAR; at < end_s; at += BEAT)
        kick(t, at, db);
}

static double drums(const Track *t, double music_start)
{
    CBBeatResult r;
    memset(&r, 0, sizeof(r));
    cb_grid_init(&r.grid, T0, BPM);
    r.music_start = music_start;
    r.start_kind = CB_START_HIT;
    int ret = cb_beats_drums(t->x, t->n, RATE, &r);
    double at = ret < 0 ? -1.0 : r.drums_start;
    cb_beat_result_free(&r);
    return at;
}

static void check_bar(const char *what, double got, int bar)
{
    double want = T0 + (bar - 1) * BAR;
    CHECK(fabs(got - want) < 1e-6, "%s: drums at %.4f s, want bar %d (%.4f s)", what, got, bar, want);
}

static void test_drums(void)
{
    const double len = 120.0;
    Track t;

    if (track_new(&t, len) < 0)
        return;
    tone(&t, T0, len, 440.0, 0.05);
    kicks(&t, 9, len, 0.0);
    check_bar("pad, then the kick", drums(&t, T0), 9);
    free(t.x);

    if (track_new(&t, len) < 0)
        return;
    tone(&t, T0, len, 440.0, 0.05);
    kicks(&t, 1, len, 0.0);
    check_bar("the kick from the start", drums(&t, T0), 1);
    free(t.x);

    if (track_new(&t, len) < 0)
        return;
    tone(&t, T0, len, 440.0, 0.05);
    kick(&t, T0, 0.0);
    kicks(&t, 9, len, 0.0);
    check_bar("an opening hit", drums(&t, T0), 9);
    free(t.x);

    if (track_new(&t, len) < 0)
        return;
    tone(&t, T0, len, 440.0, 0.05);
    for (int bar = 1; bar <= 4; bar++)
        kicks(&t, bar, T0 + bar * BAR, -30.0 + 7.5 * (bar - 1));
    kicks(&t, 5, len, 0.0);
    check_bar("a fade-in", drums(&t, T0), 5);
    free(t.x);

    if (track_new(&t, len) < 0)
        return;
    tone(&t, T0, len, 50.0, 0.09);
    kicks(&t, 9, len, 0.0);
    check_bar("a bass alone", drums(&t, T0), 9);
    free(t.x);

    if (track_new(&t, len) < 0)
        return;
    tone(&t, T0, len, 440.0, 0.05);
    kicks(&t, 27, len, 0.0);
    double at = drums(&t, T0);
    CHECK(fabs(at - T0) < 1e-9, "a chorus at %.1f s: drums at %.4f s, want the start", T0 + 26 * BAR,
          at);
    free(t.x);
}

/* A 100 s track on the grid (bar 1 at T0, BPM), with one cue or none. */
static CBTrack *track_with(const CBCue *c)
{
    CBTrack *t = cb_track_new("/test/drums.flac");
    if (!t)
        return NULL;
    t->duration = 100.0;
    cb_grid_init(&t->grid, T0, BPM);
    if (c)
        cb_track_add_cue(t, c);
    return t;
}

static CBCue plain_at(const CBGrid *g, double at)
{
    CBCue c;
    cb_cue_init(&c, at);
    cb_cue_attach_to_grid(&c, g);
    return c;
}

static void test_auto_cue(void)
{
    CBBeatResult b;
    memset(&b, 0, sizeof(b));
    cb_grid_init(&b.grid, T0, BPM);
    b.music_start = T0;
    b.drums_start = T0 + 8 * BAR;
    const double drums_at = T0 + 8 * BAR;

    /* Placed on a track without cues: on the drums' downbeat, musical. */
    CBTrack *t = track_with(NULL);
    CHECK(t && cb_analysis_auto_cues(t, &b) == 0 && t->nb_cues == 1 &&
          fabs(t->cues[0].time - drums_at) < 1e-6 && cb_cue_is_musical(&t->cues[0]),
          "new: %d cues, at %.4f", t ? t->nb_cues : -1, t && t->nb_cues ? t->cues[0].time : NAN);
    /* Not where 0 says it is not known: on the music's first downbeat. */
    CBTrack *t0 = track_with(NULL);
    CBBeatResult b0 = b;
    b0.drums_start = 0.0;
    CHECK(t0 && cb_analysis_auto_cues(t0, &b0) == 0 && t0->nb_cues == 1 &&
          fabs(t0->cues[0].time - T0) < 1e-6, "unknown: at %.4f",
          t0 && t0->nb_cues ? t0->cues[0].time : NAN);
    cb_track_free(t0);

    /* An earlier analysis's: on the first downbeat, or where this one puts
     * it; taken off, and placed again whatever else the track has. */
    static const double ours[] = { T0, T0 + 8 * BAR };
    for (int i = 0; i < 2; i++) {
        CBCue c = plain_at(&t->grid, ours[i]);
        CBTrack *u = track_with(&c);
        CBCue hot;
        cb_cue_init(&hot, 30.0);
        hot.hot = 0;
        if (u)
            cb_track_add_cue(u, &hot);
        int taken = u ? cb_analysis_take_auto_cue(u, &b) : -1;
        CHECK(taken == 1 && u->nb_cues == 1 && cb_cue_is_hot(&u->cues[0]), "ours at %.2f: %d, %d cues",
              ours[i], taken, u ? u->nb_cues : -1);
        CHECK(u && cb_analysis_add_auto_cue(u, &b) == 0 && u->nb_cues == 2 &&
              cb_track_memory_cue_nth(u, 1) >= 0 &&
              fabs(u->cues[cb_track_memory_cue_nth(u, 1)].time - drums_at) < 1e-6,
              "placed again: %d cues", u ? u->nb_cues : -1);
        cb_track_free(u);
    }

    /* The DJ's: kept. */
    for (int i = 0; i < 6; i++) {
        CBCue c = plain_at(&t->grid, T0);
        const char *what = "";
        switch (i) {
        case 0: snprintf(c.name, sizeof(c.name), "intro"); what = "named"; break;
        case 1: snprintf(c.color, sizeof(c.color), "#ff0000"); what = "coloured"; break;
        case 2: c.hot = 1; what = "hot"; break;
        case 3: c.loop_end = T0 + BAR; c.loop_beats = 4.0; what = "a loop"; break;
        case 4: c = plain_at(&t->grid, T0 + 4 * BAR); what = "bar 5"; break;
        case 5: cb_cue_init(&c, T0 + 8 * BAR); what = "absolute"; break;
        }
        CBTrack *u = track_with(&c);
        int taken = u ? cb_analysis_take_auto_cue(u, &b) : -1;
        CHECK(taken == 0 && u->nb_cues == 1, "%s: %d, %d cues", what, taken, u ? u->nb_cues : -1);
        cb_track_free(u);
    }
    /* And a track with cues of the DJ's gets none. */
    CBCue own;
    cb_cue_init(&own, 30.0);
    own.hot = 2;
    CBTrack *v = track_with(&own);
    CHECK(v && cb_analysis_auto_cues(v, &b) == 0 && v->nb_cues == 1, "%d cues",
          v ? v->nb_cues : -1);
    cb_track_free(v);
    cb_track_free(t);
    cb_beat_result_free(&b);
}

int main(void)
{
    test_drums();
    test_auto_cue();
    if (fails) {
        printf("test_drums: %d failures\n", fails);
        return 1;
    }
    printf("test_drums: ok\n");
    return 0;
}
