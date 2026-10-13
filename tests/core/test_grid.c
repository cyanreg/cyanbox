/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* Beat grid arithmetic (src/core/grid.c): one section, tempo changes,
 * phase restarts, pickup beats, the edits, and that every route to a beat
 * time (beat list, index, quantize) lands on the same bits. Times are
 * chosen so the arithmetic is exact in binary where a check uses ==. Then
 * a dynamic grid (measured beats between straight sections) through every
 * query and every tool. */

#include "rxbox/model.h"

#include <time.h>

#include "util.h"

static RXGrid grid(int nb, const RXTempoSection *s)
{
    RXGrid g = { .beats_per_bar = 4 };
    for (int i = 0; i < nb; i++)
        rx_grid_add_section(&g, s[i].start, s[i].bpm, s[i].beat);
    return g;
}

/* One section, downbeat 0.5 s, 120 BPM: rx_grid_init starts it at its
 * first beat at or after 0, a pickup at 0.0 (beat 4). */
static void test_single(void)
{
    RXGrid g;
    CHECK(rx_grid_init(&g, 0.5, 120.0) == 0);
    CHECK(rx_grid_valid(&g) && g.sections[0].start == 0.0 && g.sections[0].beat == 4);
    CHECK(rx_grid_bpm(&g) == 120.0);
    CHECK(rx_grid_bpm_at(&g, 100.0) == 120.0);
    CHECK(rx_grid_section_at(&g, -1.0) == 0);

    RXBeat *b;
    int nb;
    CHECK(rx_grid_beats(&g, 3.0, &b, &nb) == 0);
    CHECK(nb == 6);
    static const int numbers[] = { 4, 1, 2, 3, 4, 1 };
    for (int i = 0; i < nb && i < 6; i++) {
        CHECK(b[i].time == 0.5 * i);
        CHECK(b[i].number == numbers[i]);
        CHECK(b[i].bpm == 120.0);
    }
    free(b);

    CHECK(rx_grid_time_to_beat(&g, 0.5) == 0.0);
    CHECK(rx_grid_time_to_beat(&g, 0.0) == -1.0);
    CHECK(rx_grid_time_to_beat(&g, 0.75) == 0.5);
    CHECK(rx_grid_beat_to_time(&g, 4) == 2.5);
    CHECK(rx_grid_beat_to_time(&g, -1) == 0.0);

    /* dj's test_positions: "bar 1" is 0.5, "bar 2.3" is 3.5. */
    CHECK(rx_grid_bar_time(&g, 1, 1) == 0.5);
    CHECK(rx_grid_bar_time(&g, 2, 3) == 3.5);
    CHECK(rx_grid_bar_time(&g, 2, 5) == 4.5);       /* counts on, as dj */

    int bar, beat;
    rx_grid_bar_of(&g, 0.6, &bar, &beat);
    CHECK(bar == 1 && beat == 1);
    rx_grid_bar_of(&g, 3.6, &bar, &beat);
    CHECK(bar == 2 && beat == 3);
    rx_grid_bar_of(&g, 0.1, &bar, &beat);
    CHECK(bar == 0 && beat == 4);

    int number;
    CHECK(rx_grid_nearest_beat(&g, 0.74, &number) == 0.5 && number == 1);
    CHECK(rx_grid_nearest_beat(&g, 0.76, &number) == 1.0 && number == 2);
    CHECK(rx_grid_quantize(&g, 0.249) == 0.0);
    CHECK(rx_grid_quantize(&g, 0.25) == 0.0);       /* a tie goes back */
    CHECK(rx_grid_quantize(&g, 100.26) == 100.5);
    CHECK(rx_grid_quantize(&g, -0.3) == 0.0);       /* nothing before the first beat */

    /* Before/after with the 1 ms "at" tolerance. */
    CHECK(rx_grid_beat_before(&g, 1.0005) == 1.0);
    CHECK(rx_grid_beat_before(&g, 0.9995) == 1.0);
    CHECK(rx_grid_beat_before(&g, 0.998) == 0.5);
    CHECK(rx_grid_beat_after(&g, 1.0) == 1.5);
    CHECK(rx_grid_beat_after(&g, 0.9985) == 1.0);
    CHECK(rx_grid_beat_after(&g, 0.9995) == 1.5);

    CHECK(rx_grid_offset_beats(&g, 0.6, 2) == 1.6);
    CHECK_NEAR(rx_grid_offset_beats(&g, 0.6, -0.5), 0.35, 1e-12);

    rx_grid_shift(&g, 0.1);
    CHECK_NEAR(rx_grid_quantize(&g, 0.62), 0.6, 1e-12);
    rx_grid_free(&g);
    CHECK(!rx_grid_valid(&g));
    CHECK(rx_grid_beats(&g, 10, &b, &nb) == 0 && nb == 0 && !b);
}

/* 120 BPM from 0, then 60 BPM from 4.0 continuing the count. */
static void test_sections(void)
{
    RXGrid g = grid(2, (RXTempoSection[]) { { 0.0, 120.0, 1 }, { 4.0, 60.0, 0 } });
    CHECK(rx_grid_valid(&g));
    CHECK(rx_grid_section_at(&g, 3.9) == 0);
    CHECK(rx_grid_section_at(&g, 4.0) == 1);
    CHECK(rx_grid_bpm_at(&g, 4.5) == 60.0);

    RXBeat *b;
    int nb;
    CHECK(rx_grid_beats(&g, 8.0, &b, &nb) == 0);
    CHECK(nb == 12);
    if (nb == 12) {
        CHECK(b[7].time == 3.5 && b[7].number == 4 && b[7].bpm == 120.0);
        CHECK(b[8].time == 4.0 && b[8].number == 1 && b[8].bpm == 60.0);
        CHECK(b[11].time == 7.0 && b[11].number == 4);
    }
    free(b);

    CHECK(rx_grid_time_to_beat(&g, 4.0) == 8.0);
    CHECK(rx_grid_time_to_beat(&g, 5.0) == 9.0);
    CHECK(rx_grid_time_to_beat(&g, 3.75) == 7.5);
    CHECK(rx_grid_beat_to_time(&g, 7.5) == 3.75);
    CHECK(rx_grid_beat_to_time(&g, 10.0) == 6.0);

    /* A beat jump steps at each section's own tempo. */
    CHECK(rx_grid_offset_beats(&g, 3.0, 4) == 6.0);
    CHECK(rx_grid_offset_beats(&g, 6.0, -4) == 3.0);
    CHECK(rx_grid_offset_beats(&g, 3.25, 1.5) == 4.0);

    int bar, beat;
    CHECK(rx_grid_bar_time(&g, 3, 1) == 4.0);
    CHECK(rx_grid_bar_time(&g, 3, 2) == 5.0);
    rx_grid_bar_of(&g, 5.5, &bar, &beat);
    CHECK(bar == 3 && beat == 2);
    CHECK(rx_grid_quantize(&g, 3.8) == 4.0);
    CHECK(rx_grid_beat_after(&g, 3.5) == 4.0);
    CHECK(rx_grid_beat_before(&g, 4.6) == 4.0);

    /* A section start that is not on the previous section's beats: the
     * gap shorter than a beat is interpolated, and both ways agree. */
    rx_grid_free(&g);
    g = grid(2, (RXTempoSection[]) { { 0.0, 120.0, 1 }, { 3.8, 120.0, 0 } });
    CHECK(rx_grid_time_to_beat(&g, 3.5) == 7.0);
    CHECK(rx_grid_time_to_beat(&g, 3.8) == 8.0);
    CHECK_NEAR(rx_grid_time_to_beat(&g, 3.65), 7.5, 1e-12);
    CHECK_NEAR(rx_grid_beat_to_time(&g, 7.5), 3.65, 1e-12);
    CHECK(rx_grid_quantize(&g, 3.66) == 3.8);
    rx_grid_free(&g);
}

/* A section restarting the count: 1 2 3 4 | 1 2 | 1 2 3 4 */
static void test_restart(void)
{
    RXGrid g = grid(2, (RXTempoSection[]) { { 0.0, 120.0, 1 }, { 3.0, 120.0, 1 } });
    RXBeat *b;
    int nb;
    CHECK(rx_grid_beats(&g, 5.0, &b, &nb) == 0);
    static const int numbers[] = { 1, 2, 3, 4, 1, 2, 1, 2, 3, 4 };
    CHECK(nb == 10);
    for (int i = 0; i < nb && i < 10; i++)
        CHECK(b[i].number == numbers[i]);
    free(b);

    int bar, beat;
    rx_grid_bar_of(&g, 2.6, &bar, &beat);
    CHECK(bar == 2 && beat == 2);
    rx_grid_bar_of(&g, 3.2, &bar, &beat);
    CHECK(bar == 3 && beat == 1);
    CHECK(rx_grid_bar_time(&g, 3, 1) == 3.0);
    CHECK(rx_grid_bar_time(&g, 2, 2) == 2.5);
    /* Bar 2 has no beat 3: counted on from its first beat. */
    CHECK(rx_grid_bar_time(&g, 2, 3) == 3.0);
    CHECK(rx_grid_time_to_beat(&g, 3.0) == 6.0);

    /* set_downbeat on the second section makes 4.0 its beat 1. */
    rx_grid_set_downbeat(&g, 4.1);
    CHECK(g.sections[1].start == 3.0 && g.sections[1].beat == 3);
    rx_grid_bar_of(&g, 4.0, &bar, &beat);
    CHECK(beat == 1);
    rx_grid_bar_of(&g, 3.0, &bar, &beat);
    CHECK(beat == 3);
    rx_grid_free(&g);
}

/* The first section may name any beat: beat 3 at 0.0 is a downbeat at 1.0. */
static void test_first_section_beat(void)
{
    RXGrid a = grid(1, (RXTempoSection[]) { { 0.0, 120.0, 3 } });
    RXGrid b;
    rx_grid_init(&b, 1.0, 120.0);
    RXBeat *ba, *bb;
    int na, nbb;
    rx_grid_beats(&a, 10.0, &ba, &na);
    rx_grid_beats(&b, 10.0, &bb, &nbb);
    CHECK(na == nbb && na == 20);
    for (int i = 0; i < na && i < nbb; i++)
        CHECK(ba[i].time == bb[i].time && ba[i].number == bb[i].number);
    free(ba);
    free(bb);
    CHECK(rx_grid_time_to_beat(&a, 1.0) == 0.0);
    CHECK(rx_grid_time_to_beat(&a, 0.0) == -2.0);
    CHECK(rx_grid_bar_time(&a, 1, 1) == 1.0);
    rx_grid_free(&a);
    rx_grid_free(&b);
}

/* The first section's start is the grid's first beat: a track whose music
 * starts at 0.72 s has no beat, no bar and nothing to snap to before it,
 * while the index arithmetic runs on (a cue in the silence has an index). */
static void test_grid_start(void)
{
    const double P = 60.0 / 134.0;
    RXGrid g = grid(1, (const RXTempoSection[]) { { 0.72, 134.0, 1 } });
    RXBeat *b;
    int nb;
    CHECK(rx_grid_beats(&g, 10.0, &b, &nb) == 0 && nb > 0);
    CHECK(nb > 0 && b[0].time == 0.72 && b[0].number == 1);
    free(b);

    int bar = 7, beat = 7, number = 0;
    rx_grid_bar_of(&g, 0.3, &bar, &beat);
    CHECK(bar == 0 && beat == 0);
    rx_grid_bar_of(&g, 0.72, &bar, &beat);
    CHECK(bar == 1 && beat == 1);
    CHECK(rx_grid_nearest_beat(&g, 0.3, &number) == 0.72 && number == 1);
    CHECK(rx_grid_quantize(&g, 0.0) == 0.72);
    CHECK(rx_grid_beat_before(&g, 0.3) == 0.3);
    CHECK(rx_grid_beat_after(&g, 0.3) == 0.72);
    CHECK(rx_grid_time_to_beat(&g, 0.72) == 0.0);
    CHECK_NEAR(rx_grid_time_to_beat(&g, 0.72 - P), -1.0, 1e-12);
    CHECK_NEAR(rx_grid_beat_to_time(&g, -1.0), 0.72 - P, 1e-12);
    CHECK_NEAR(rx_grid_offset_beats(&g, 0.1, 2.0), 0.1 + 2.0 * P, 1e-12);
    CHECK(rx_grid_bar_time(&g, 1, 1) == 0.72);

    /* A first beat that is beat 3: a pickup bar 0, bar 1 two beats on. */
    rx_grid_free(&g);
    g = grid(1, (const RXTempoSection[]) { { 1.0, 120.0, 3 } });
    CHECK(rx_grid_beats(&g, 4.0, &b, &nb) == 0 && nb == 6);
    static const int numbers[] = { 3, 4, 1, 2, 3, 4 };
    for (int i = 0; i < nb && i < 6; i++)
        CHECK(b[i].time == 1.0 + 0.5 * i && b[i].number == numbers[i]);
    free(b);
    rx_grid_bar_of(&g, 1.2, &bar, &beat);
    CHECK(bar == 0 && beat == 3);
    rx_grid_bar_of(&g, 2.0, &bar, &beat);
    CHECK(bar == 1 && beat == 1);
    CHECK(rx_grid_time_to_beat(&g, 2.0) == 0.0 && rx_grid_time_to_beat(&g, 1.0) == -2.0);
    rx_grid_free(&g);
}

/* rx_grid_start_at moves the first beat forward by whole beats: every beat
 * from there on keeps its time and number, a later section its count. */
static void test_start_at(void)
{
    const RXTempoSection secs[] = { { 0.0, 120.0, 1 }, { 8.0, 125.0, 0 } };
    RXGrid ref = grid(2, secs);
    RXBeat *rb;
    int nr;
    rx_grid_beats(&ref, 20.0, &rb, &nr);

    static const double at[] = { -1.0, 0.0, 1.7, 2.6, 7.9, 9.0 };
    static const double first[] = { 0.0, 0.0, 2.0, 3.0, 8.0, 9.44 };
    static const int number[] = { 1, 1, 1, 3, 1, 4 };
    for (size_t i = 0; i < RX_ARRAY_ELEMS(at); i++) {
        RXGrid g = grid(2, secs);
        CHECK(rx_grid_start_at(&g, at[i]) == 0);
        CHECK(rx_grid_valid(&g));
        CHECK_NEAR(g.sections[0].start, first[i], 1e-12);
        CHECK(g.sections[0].beat == number[i]);
        CHECK(g.nb_sections == (first[i] < 8.0 ? 2 : 1));
        RXBeat *b;
        int nb;
        rx_grid_beats(&g, 20.0, &b, &nb);
        int j = 0;
        while (j < nr && rb[j].time < first[i] - 1e-9)
            j++;
        CHECK(nb == nr - j);
        for (int k = 0; k < nb && j + k < nr; k++)
            CHECK(fabs(b[k].time - rb[j + k].time) < 1e-12 && b[k].number == rb[j + k].number);
        free(b);
        /* Bar 1 is the first downbeat at or after the new start. */
        int bar, beat;
        rx_grid_bar_of(&g, first[i], &bar, &beat);
        CHECK(beat == number[i] && bar == (number[i] == 1 ? 1 : 0));
        rx_grid_free(&g);
    }
    free(rb);
    rx_grid_free(&ref);
}

static void test_edits(void)
{
    RXGrid g;
    rx_grid_init(&g, 0.5, 120.0);
    RXBeat *before, *after;
    int nb0, nb1;
    rx_grid_beats(&g, 20.0, &before, &nb0);

    /* Set downbeat on the first section moves its count, not its start
     * (the grid's first beat): 0.0 becomes the beat three before 1.5. */
    rx_grid_set_downbeat(&g, 1.4);
    CHECK(g.nb_sections == 1 && g.sections[0].start == 0.0 && g.sections[0].beat == 2);
    int bar, beat;
    rx_grid_bar_of(&g, 1.5, &bar, &beat);
    CHECK(bar == 1 && beat == 1);
    CHECK(rx_grid_time_to_beat(&g, 1.5) == 0.0);
    rx_grid_beats(&g, 20.0, &after, &nb1);
    CHECK(nb0 == nb1);
    for (int i = 0; i < nb0 && i < nb1; i++)
        CHECK(before[i].time == after[i].time);
    free(after);

    /* A split changes nothing until one side is edited. */
    int idx = rx_grid_split_at(&g, 5.4);
    CHECK(idx == 1 && g.nb_sections == 2 && g.sections[1].start == 5.5 && g.sections[1].beat == 0);
    rx_grid_beats(&g, 20.0, &after, &nb1);
    CHECK(nb0 == nb1);
    for (int i = 0; i < nb0 && i < nb1; i++)
        CHECK(before[i].time == after[i].time);
    free(after);
    CHECK(rx_grid_split_at(&g, 5.6) == 1);          /* already a boundary */
    CHECK(g.nb_sections == 2);

    double b55 = rx_grid_time_to_beat(&g, 5.5);
    rx_grid_set_bpm_at(&g, 6.0, 60.0);
    CHECK(g.sections[0].bpm == 120.0 && g.sections[1].bpm == 60.0);
    CHECK(rx_grid_time_to_beat(&g, 6.5) == b55 + 1);
    CHECK(rx_grid_offset_beats(&g, 5.0, 2) == 6.5);

    CHECK(rx_grid_merge_at(&g, 5.0) == 0);
    CHECK(g.nb_sections == 1);
    CHECK(rx_grid_merge_at(&g, 5.0) == RX_ERROR(ENOENT));

    /* A split before the grid's first beat is at that beat: the first
     * section, nothing to split. One inside it changes nothing either. */
    free(before);
    rx_grid_beats(&g, 20.0, &before, &nb0);
    idx = rx_grid_split_at(&g, -1.0);
    CHECK(idx == 0 && g.nb_sections == 1);
    idx = rx_grid_split_at(&g, 3.1);
    CHECK(idx == 1 && g.nb_sections == 2 && g.sections[1].start == 3.0);
    rx_grid_beats(&g, 20.0, &after, &nb1);
    CHECK(nb0 == nb1);
    for (int i = 0; i < nb0 && i < nb1; i++)
        CHECK(before[i].time == after[i].time && before[i].number == after[i].number);
    free(after);
    free(before);

    /* add_section replaces one at the same start; remove; copy. */
    CHECK(rx_grid_add_section(&g, g.sections[1].start + 1e-6, 125.0, 1) == 1);
    CHECK(g.nb_sections == 2 && g.sections[1].bpm == 125.0);
    CHECK(rx_grid_add_section(&g, 10.0, 0.0, 0) == RX_ERROR(EINVAL));
    RXGrid c = { 0 };
    CHECK(rx_grid_copy(&c, &g) == 0 && c.nb_sections == 2 && c.sections != g.sections);
    CHECK(rx_grid_remove_section(&c, 0) == 0 && c.nb_sections == 1);
    CHECK(rx_grid_remove_section(&c, 3) == RX_ERROR(EINVAL));
    rx_grid_free(&c);
    rx_grid_free(&g);
}

/* Every route to a beat gives the same bits, at a tempo that is not
 * exact in binary. */
static void test_exact(void)
{
    RXGrid g = grid(3, (RXTempoSection[]) {
        { 0.3125, 128.0, 1 }, { 100.123, 127.5, 0 }, { 200.7, 130.0, 3 },
    });
    RXBeat *b;
    int nb;
    rx_grid_beats(&g, 400.0, &b, &nb);
    CHECK(nb > 800);
    int first_down = 0;
    while (b[first_down].number != 1)
        first_down++;
    for (int i = 0; i < nb; i++) {
        double idx = i - first_down;
        CHECK(rx_grid_beat_to_time(&g, idx) == b[i].time);
        CHECK(rx_grid_time_to_beat(&g, b[i].time) == idx);
        CHECK(rx_grid_quantize(&g, b[i].time + 1e-4) == b[i].time);
        CHECK(rx_grid_quantize(&g, b[i].time - 1e-4) == b[i].time);
        int number;
        rx_grid_nearest_beat(&g, b[i].time, &number);
        CHECK(number == b[i].number);
        if (i + 1 < nb)
            CHECK(rx_grid_beat_after(&g, b[i].time) == b[i + 1].time);
    }

    /* The player calls these on every press: they must be cheap. */
    clock_t t0 = clock();
    double sum = 0;
    for (int i = 0; i < 1000000; i++)
        sum += rx_grid_quantize(&g, i * 0.0004);
    double secs = (double)(clock() - t0) / CLOCKS_PER_SEC;
    printf("1e6 quantize on a 3-section grid: %.3f s (%g)\n", secs, sum);
    CHECK(secs < 2.0);
    free(b);
    rx_grid_free(&g);
}

/* A tempo no music has (a damaged row, a corrupt stick) is not a grid:
 * above it the beat list of one track runs to billions of entries and the
 * walks lose all precision. It is refused where a tempo comes in and makes
 * a grid invalid where one is found. */
static void test_absurd_tempo(void)
{
    RXGrid g;
    CHECK(rx_grid_init(&g, 0.0, 1e9) == RX_ERROR(EINVAL));
    CHECK(rx_grid_init(&g, 0.0, 1e-9) == RX_ERROR(EINVAL));
    CHECK(rx_grid_init(&g, 0.0, 0.0) == RX_ERROR(EINVAL));
    CHECK(rx_grid_init(&g, 0.5, 999.0) == 0);
    CHECK(rx_grid_add_section(&g, 10.0, 1e12, 0) == RX_ERROR(EINVAL) && g.nb_sections == 1);
    rx_grid_set_bpm_at(&g, 1.0, 1e9);
    CHECK(g.sections[0].bpm == 999.0);

    g.sections[0].bpm = 1e9;                /* as a damaged row loads */
    CHECK(!rx_grid_valid(&g));
    RXBeat *b;
    int nb;
    int64_t t0 = rx_time_us();
    CHECK(rx_grid_beats(&g, 1.0, &b, &nb) == 0 && nb == 0 && !b);
    free(b);
    CHECK(rx_grid_quantize(&g, 100.0) == 100.0);
    CHECK(rx_time_us() - t0 < 1000000);
    rx_grid_free(&g);

    /* A duration no file has (a corrupt header's) is refused at once, not
     * counted beat by beat first. */
    rx_grid_init(&g, 0.5, 120.0);
    t0 = rx_time_us();
    CHECK(rx_grid_beats(&g, 2e9, &b, &nb) == RX_ERROR(ERANGE) && !nb && !b);
    CHECK(rx_grid_beats(&g, INFINITY, &b, &nb) == RX_ERROR(ERANGE) && !nb && !b);
    CHECK(rx_time_us() - t0 < 1000000);
    rx_grid_free(&g);
}

/* A time that is not a number (an unset playhead, a failed computation)
 * is not on the grid: queries give it back, edits leave the grid alone. */
static void test_not_a_time(void)
{
    RXGrid g = grid(2, (const RXTempoSection[]) { { 0.5, 120.0, 1 }, { 60.5, 126.0, 0 } });
    int num = 7, bar = 7, beat = 7;
    CHECK(isnan(rx_grid_nearest_beat(&g, NAN, &num)) && num == 0);
    CHECK(isnan(rx_grid_quantize(&g, NAN)));
    CHECK(isnan(rx_grid_beat_before(&g, NAN)) && isnan(rx_grid_beat_after(&g, NAN)));
    CHECK(rx_grid_quantize(&g, INFINITY) == INFINITY && rx_grid_quantize(&g, -INFINITY) == -INFINITY);
    rx_grid_bar_of(&g, NAN, &bar, &beat);
    CHECK(bar == 0 && beat == 0);

    rx_grid_set_downbeat(&g, NAN);
    rx_grid_set_bpm_at(&g, NAN, 90.0);
    CHECK(rx_grid_split_at(&g, NAN) == RX_ERROR(EINVAL));
    CHECK(rx_grid_merge_at(&g, NAN) == RX_ERROR(EINVAL));
    CHECK(rx_grid_split_at(&g, INFINITY) == RX_ERROR(EINVAL));
    CHECK(g.nb_sections == 2 && g.sections[0].start == 0.5 && g.sections[0].beat == 1);
    CHECK(g.sections[0].bpm == 120.0 && g.sections[1].start == 60.5 && g.sections[1].bpm == 126.0);
    rx_grid_free(&g);
}

/* rbxport's SAME_HIT: a section that starts 2 ms after a beat of the one
 * before has its first beat on that same hit, and the hit is the new
 * tempo's. The old beat goes from the list and from every other route to
 * a beat, and the count goes on through the new one. (dj kept both.) */
static void test_same_hit(void)
{
    RXGrid g = grid(2, (const RXTempoSection[]) { { 0.5, 120.0, 1 }, { 10.502, 125.0, 0 } });
    RXBeat *b;
    int nb;
    CHECK(rx_grid_beats(&g, 12.0, &b, &nb) == 0);
    CHECK(nb == 24);
    int close = 0, at = -1;
    for (int i = 0; i < nb; i++) {
        if (i && b[i].time - b[i - 1].time < 0.25)
            close++;
        if (b[i].time == 10.502)
            at = i;
    }
    CHECK(!close && at == 20);
    if (at == 20) {
        CHECK(b[at - 1].time == 10.0 && b[at - 1].number == 4 && b[at - 1].bpm == 120.0);
        CHECK(b[at].number == 1 && b[at].bpm == 125.0);
    }
    free(b);

    CHECK(rx_grid_nearest_beat(&g, 10.5, NULL) == 10.502);
    CHECK(rx_grid_quantize(&g, 10.49) == 10.502);
    CHECK(rx_grid_beat_after(&g, 10.0) == 10.502);
    CHECK(rx_grid_beat_before(&g, 10.501) == 10.0);
    CHECK(rx_grid_time_to_beat(&g, 10.0) == 19.0 && rx_grid_time_to_beat(&g, 10.502) == 20.0);
    CHECK(rx_grid_beat_to_time(&g, 20.0) == 10.502);
    int bar, beat;
    rx_grid_bar_of(&g, 10.502, &bar, &beat);
    CHECK(bar == 6 && beat == 1);
    CHECK(rx_grid_bar_time(&g, 6, 1) == 10.502);

    /* Half a period after the beat: two hits, both kept. */
    rx_grid_free(&g);
    g = grid(2, (const RXTempoSection[]) { { 0.5, 120.0, 1 }, { 10.75, 125.0, 0 } });
    CHECK(rx_grid_beats(&g, 12.0, &b, &nb) == 0 && nb == 24);
    CHECK(nb == 24 && b[20].time == 10.5 && b[21].time == 10.75);
    free(b);
    rx_grid_free(&g);
}

/* The beats of g in [0, duration) as an array of times; nb in *nb. */
static RXBeat *beats_of(const RXGrid *g, double duration, int *nb)
{
    RXBeat *b = NULL;
    *nb = 0;
    CHECK(rx_grid_beats(g, duration, &b, nb) == 0);
    return b;
}

/* A straight intro (120 BPM from 0), then five beats as a drummer played
 * them (intervals 0.50, 0.52, 0.49, 0.51 s), then a straight outro at 118
 * from the beat after the last measured one. The measured beats are the
 * grid's beats, the index is linear between them, the count runs through,
 * and every route to a beat lands on the same bits. */
static const double TM[] = { 4.0, 4.5, 5.02, 5.51, 6.02 };
static const double OUTRO = 6.52;

static RXGrid dynamic_grid(void)
{
    RXGrid g = { .beats_per_bar = 4 };
    CHECK(rx_grid_add_section(&g, 0.0, 120.0, 1) == 0);
    for (size_t i = 0; i < RX_ARRAY_ELEMS(TM); i++) {
        double next = i + 1 < RX_ARRAY_ELEMS(TM) ? TM[i + 1] : OUTRO;
        CHECK(rx_grid_add_beat(&g, TM[i], 60.0 / (next - TM[i]), 0) == (int)i + 1);
    }
    CHECK(rx_grid_add_section(&g, OUTRO, 118.0, 0) == 6);
    return g;
}

static void test_dynamic_queries(void)
{
    RXGrid g = dynamic_grid();
    CHECK(rx_grid_valid(&g) && rx_grid_dynamic(&g) && g.nb_sections == 7);
    CHECK(!g.sections[0].measured && g.sections[1].measured && g.sections[5].measured);
    CHECK(!g.sections[6].measured);
    RXGrid plain;
    rx_grid_init(&plain, 0.5, 120.0);
    CHECK(!rx_grid_dynamic(&plain));
    rx_grid_free(&plain);

    /* The beat list: the intro's eight beats, the five measured ones at
     * their own times and tempos, the outro's. */
    int nb;
    RXBeat *b = beats_of(&g, 10.0, &nb);
    CHECK(nb == 20);
    static const int numbers[] = { 1, 2, 3, 4, 1, 2, 3, 4, 1, 2, 3, 4, 1, 2, 3, 4, 1, 2, 3, 4 };
    for (int i = 0; i < nb && i < 20; i++) {
        CHECK(b[i].number == numbers[i]);
        if (i < 8)
            CHECK(b[i].time == 0.5 * i && b[i].bpm == 120.0);
        else if (i < 13)
            CHECK(b[i].time == TM[i - 8] && b[i].bpm == g.sections[i - 7].bpm);
        else
            CHECK(b[i].time == OUTRO + (double)(i - 13) * (60.0 / 118.0) && b[i].bpm == 118.0);
    }
    if (nb == 20) {
        CHECK_NEAR(b[9].bpm, 60.0 / 0.52, 1e-9);
        CHECK(b[12].bpm == 120.0);
    }

    /* Indices: integer on the beats, linear between two measured beats. */
    CHECK(rx_grid_time_to_beat(&g, 4.5) == 9.0);
    CHECK_NEAR(rx_grid_time_to_beat(&g, 4.76), 9.5, 1e-12);
    CHECK_NEAR(rx_grid_beat_to_time(&g, 9.5), 4.76, 1e-12);
    CHECK_NEAR(rx_grid_time_to_beat(&g, 6.27), 12.5, 1e-12);
    CHECK(rx_grid_offset_beats(&g, 4.0, 2) == 5.02);
    CHECK(rx_grid_offset_beats(&g, 5.02, -2) == 4.0);
    CHECK(rx_grid_offset_beats(&g, 5.51, 2) == OUTRO);
    CHECK_NEAR(rx_grid_offset_beats(&g, 4.25, 1), 4.76, 1e-12);

    /* The nearest beat, before, after, quantize. */
    int number;
    CHECK(rx_grid_nearest_beat(&g, 5.2, &number) == 5.02 && number == 3);
    CHECK(rx_grid_nearest_beat(&g, 5.3, &number) == 5.51 && number == 4);
    CHECK(rx_grid_quantize(&g, 5.4) == 5.51);
    CHECK(rx_grid_beat_after(&g, 5.02) == 5.51);
    CHECK(rx_grid_beat_before(&g, 5.5) == 5.02);
    CHECK(rx_grid_beat_after(&g, 6.02) == OUTRO);
    CHECK(rx_grid_beat_before(&g, 6.6) == OUTRO);

    /* Bars and numbers, the local tempo, the typical tempo. */
    int bar, beat;
    rx_grid_bar_of(&g, 5.51, &bar, &beat);
    CHECK(bar == 3 && beat == 4);
    rx_grid_bar_of(&g, 6.02, &bar, &beat);
    CHECK(bar == 4 && beat == 1);
    rx_grid_bar_of(&g, 6.6, &bar, &beat);
    CHECK(bar == 4 && beat == 2);
    CHECK(rx_grid_bar_time(&g, 4, 1) == 6.02);
    CHECK(rx_grid_bar_time(&g, 3, 3) == 5.02);
    CHECK_NEAR(rx_grid_bpm_at(&g, 5.1), 60.0 / 0.49, 1e-9);
    CHECK(rx_grid_bpm_at(&g, 7.0) == 118.0);
    CHECK(rx_grid_bpm_median(&g, 10.0) == 120.0);

    /* Every route to a beat gives the same bits. */
    for (int i = 0; i < nb; i++) {
        double idx = rx_grid_time_to_beat(&g, b[i].time);
        CHECK(idx == (double)i);
        CHECK(rx_grid_beat_to_time(&g, idx) == b[i].time);
        CHECK(rx_grid_quantize(&g, b[i].time + 1e-4) == b[i].time);
        CHECK(rx_grid_quantize(&g, b[i].time - 1e-4) == b[i].time);
        if (i + 1 < nb)
            CHECK(rx_grid_beat_after(&g, b[i].time) == b[i + 1].time);
    }
    free(b);

    /* A copy keeps the measured beats; a grid ending in measured beats
     * runs on from the last at its tempo. */
    RXGrid c = { 0 };
    CHECK(rx_grid_copy(&c, &g) == 0 && rx_grid_dynamic(&c) && c.sections[3].measured);
    rx_grid_free(&c);
    rx_grid_free(&g);
    RXGrid e = { .beats_per_bar = 4 };
    rx_grid_add_beat(&e, 1.0, 120.0, 1);
    rx_grid_add_beat(&e, 1.5, 120.0, 0);
    rx_grid_add_beat(&e, 2.0, 100.0, 0);
    b = beats_of(&e, 5.0, &nb);
    CHECK(nb == 7);
    if (nb == 7) {
        CHECK(b[2].time == 2.0 && b[3].time == 2.6 && b[6].time == 2.0 + 4 * 0.6);
        CHECK(b[3].number == 4 && b[3].bpm == 100.0);
    }
    free(b);
    CHECK(rx_grid_bpm_at(&e, 10.0) == 100.0);
    rx_grid_free(&e);
}

/* The tools on a dynamic grid: shift, set downbeat, a tempo typed or
 * tapped (straight from here), x2 and /2 on the beats, straighten (what
 * MERGE does there), split (nothing), start_at. */
static void test_dynamic_tools(void)
{
    RXGrid g = dynamic_grid();
    int nb0, nb1;
    RXBeat *before = beats_of(&g, 10.0, &nb0);

    rx_grid_shift(&g, 0.01);
    RXBeat *after = beats_of(&g, 10.0, &nb1);
    CHECK(nb0 == nb1);
    for (int i = 0; i < nb0 && i < nb1; i++)
        CHECK_NEAR(after[i].time, before[i].time + 0.01, 1e-12);
    free(after);
    rx_grid_shift(&g, -0.01);

    /* Set downbeat on a measured beat: its count moves, every time stays. */
    rx_grid_set_downbeat(&g, 5.5);
    CHECK(g.sections[4].beat == 1 && g.sections[4].start == 5.51);
    int bar, beat;
    rx_grid_bar_of(&g, 5.51, &bar, &beat);
    CHECK(beat == 1);
    rx_grid_bar_of(&g, 6.02, &bar, &beat);
    CHECK(beat == 2);
    rx_grid_bar_of(&g, OUTRO, &bar, &beat);
    CHECK(beat == 3);
    after = beats_of(&g, 10.0, &nb1);
    CHECK(nb0 == nb1);
    for (int i = 0; i < nb0 && i < nb1; i++)
        CHECK(after[i].time == before[i].time);
    free(after);
    g.sections[4].beat = 0;

    /* A split on a measured beat changes nothing. */
    CHECK(rx_grid_split_at(&g, 5.0) == 3 && g.nb_sections == 7);

    /* A tempo set inside the passage: straight at that tempo from the
     * beat at or before the playhead, the earlier beats as measured. */
    RXGrid s;
    rx_grid_copy(&s, &g);
    rx_grid_set_bpm_at(&s, 5.1, 110.0);
    CHECK(s.nb_sections == 5 && s.sections[1].measured && s.sections[2].measured);
    CHECK(!s.sections[3].measured && s.sections[3].start == 5.02 && s.sections[3].bpm == 110.0);
    CHECK(s.sections[3].beat == 0 && s.sections[4].start == OUTRO);
    after = beats_of(&s, 10.0, &nb1);
    CHECK(nb1 == 20);
    if (nb1 == 20) {
        CHECK(after[10].time == 5.02);
        CHECK_NEAR(after[11].time, 5.02 + 60.0 / 110.0, 1e-12);
        CHECK_NEAR(after[12].time, 5.02 + 120.0 / 110.0, 1e-12);
        CHECK(after[13].time == OUTRO && after[13].number == 2);
    }
    free(after);
    rx_grid_free(&s);

    /* Straighten (MERGE): the passage becomes one line held on its first
     * beat, its slope the least squares through its beats and the beat
     * after them. */
    rx_grid_copy(&s, &g);
    CHECK(rx_grid_merge_at(&s, 5.0) == 1);
    CHECK(s.nb_sections == 3 && !s.sections[1].measured && s.sections[1].start == 4.0);
    CHECK_NEAR(s.sections[1].bpm, 60.0 * 55.0 / 27.75, 1e-9);
    CHECK(s.sections[1].beat == 0 && s.sections[2].start == OUTRO);
    CHECK(rx_grid_straighten_at(&s, 5.0) == RX_ERROR(ENOENT));
    rx_grid_free(&s);

    /* x2: a beat at every midpoint, each at twice its local tempo. */
    rx_grid_copy(&s, &g);
    CHECK(rx_grid_double_at(&s, 5.0) == 4);           /* the new beat at 4.76 holds 5.0 */
    CHECK(s.nb_sections == 12);
    after = beats_of(&s, 10.0, &nb1);
    CHECK(nb1 == 25);
    if (nb1 == 25) {
        CHECK(after[8].time == 4.0 && after[9].time == 4.25 && after[10].time == 4.5);
        CHECK_NEAR(after[11].time, 4.76, 1e-12);
        CHECK(after[16].time == 6.02 && after[17].time == 6.27 && after[18].time == OUTRO);
        CHECK(after[8].bpm == 240.0 && after[9].bpm == 240.0 && after[9].number == 2);
        CHECK_NEAR(after[10].bpm, 120.0 / 0.52, 1e-9);
        CHECK(after[16].number == 1 && after[18].number == 3);
    }
    free(after);
    /* And x2 on the straight outro doubles its tempo as before. */
    CHECK(rx_grid_double_at(&s, 7.0) == 11 && s.sections[11].bpm == 236.0);
    rx_grid_free(&s);

    /* /2: every other beat from the passage's first; with five beats the
     * last goes too, so the outro is one new beat after the kept one. */
    rx_grid_copy(&s, &g);
    CHECK(rx_grid_halve_at(&s, 5.0) == 1);            /* the kept beat at 4.0 holds 5.0 */
    CHECK(s.nb_sections == 4);
    after = beats_of(&s, 10.0, &nb1);
    CHECK(nb1 == 17);
    if (nb1 == 17) {
        CHECK(after[8].time == 4.0 && after[9].time == 5.02 && after[10].time == OUTRO);
        CHECK(after[8].number == 1 && after[9].number == 2 && after[10].number == 3);
        CHECK_NEAR(after[8].bpm, 60.0 / 1.02, 1e-9);
        CHECK_NEAR(after[9].bpm, 60.0 / 1.5, 1e-9);
    }
    free(after);
    CHECK(rx_grid_halve_at(&s, 1.0) == 0 && s.sections[0].bpm == 60.0);
    rx_grid_free(&s);

    /* start_at inside the passage: the beats before go, the first kept
     * measured beat is the grid's first with its number. */
    rx_grid_copy(&s, &g);
    CHECK(rx_grid_start_at(&s, 4.2) == 0);
    CHECK(s.nb_sections == 5 && s.sections[0].measured && s.sections[0].start == 4.5);
    CHECK(s.sections[0].beat == 2);
    after = beats_of(&s, 10.0, &nb1);
    CHECK(nb1 == nb0 - 9);
    for (int i = 0; i < nb1 && i + 9 < nb0; i++)
        CHECK(after[i].time == before[i + 9].time && after[i].number == before[i + 9].number);
    free(after);
    rx_grid_free(&s);

    /* A measured beat added at a section's time replaces it. */
    rx_grid_copy(&s, &g);
    CHECK(rx_grid_add_beat(&s, OUTRO, 119.0, 0) == 6 && s.sections[6].measured && s.nb_sections == 7);
    rx_grid_free(&s);

    free(before);
    rx_grid_free(&g);

    /* A thousand measured beats: the walk is linear, and a query costs a
     * few microseconds. */
    RXGrid big = { .beats_per_bar = 4 };
    for (int i = 0; i < 1000; i++)
        rx_grid_add_beat(&big, 0.5 + i * 0.48 + (i % 7) * 0.001, 125.0, i ? 0 : 1);
    clock_t t0 = clock();
    double sum = 0;
    for (int i = 0; i < 20000; i++)
        sum += rx_grid_quantize(&big, i * 0.024);
    double secs = (double)(clock() - t0) / CLOCKS_PER_SEC;
    printf("2e4 quantize on a 1000-beat dynamic grid: %.3f s (%g)\n", secs, sum);
    CHECK(secs < 2.0);                     /* about 8 us each; the suite runs beside other work */
    int n;
    RXBeat *bb = beats_of(&big, 480.03, &n);            /* just past the last measured beat */
    CHECK(n == 1000);
    for (int i = 0; i < n; i++)
        CHECK(rx_grid_beat_to_time(&big, rx_grid_time_to_beat(&big, bb[i].time)) == bb[i].time);
    free(bb);
    rx_grid_free(&big);
}

int main(void)
{
    test_single();
    test_sections();
    test_restart();
    test_first_section_beat();
    test_grid_start();
    test_start_at();
    test_edits();
    test_exact();
    test_absurd_tempo();
    test_not_a_time();
    test_same_hit();
    test_dynamic_queries();
    test_dynamic_tools();
    return test_done("core_grid");
}
