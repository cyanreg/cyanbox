/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* The plain-data model (src/core/model.c): cue list rules, musical cues
 * following a grid edit, tag metadata, and track copies. */

#include "rxbox/model.h"

#include "util.h"

static RXCue cue(double time, int hot)
{
    RXCue c;
    rx_cue_init(&c, time);
    c.hot = hot;
    return c;
}

static void test_cue_list(void)
{
    RXTrack *t = rx_track_new("/music/x.flac");
    CHECK(t && t->grid.beats_per_bar == 4 && !t->nb_cues);

    RXCue c = cue(10.0, -1);
    CHECK(!rx_cue_is_loop(&c) && !rx_cue_is_hot(&c) && !rx_cue_is_musical(&c));
    CHECK(rx_cue_letter(&c) == ' ');

    /* Sorted by time, equal times keep their order. */
    CHECK(rx_track_add_cue(t, &c) == 0);
    c = cue(5.0, -1);
    CHECK(rx_track_add_cue(t, &c) == 0);
    c = cue(20.0, -1);
    CHECK(rx_track_add_cue(t, &c) == 2);
    c = cue(10.0, 0);
    CHECK(rx_track_add_cue(t, &c) == 2);
    CHECK(t->nb_cues == 4);
    CHECK(t->cues[0].time == 5.0 && t->cues[1].time == 10.0 && t->cues[1].hot == -1);
    CHECK(t->cues[2].time == 10.0 && t->cues[2].hot == 0 && rx_cue_letter(&t->cues[2]) == 'A');

    /* Hot cue slots are unique: a new A replaces the old A. */
    c = cue(30.0, 0);
    snprintf(c.name, sizeof(c.name), "drop");
    CHECK(rx_track_add_cue(t, &c) == 3);
    CHECK(t->nb_cues == 4);
    CHECK(rx_track_hot_cue(t, 0) == 3 && !strcmp(t->cues[3].name, "drop"));
    CHECK(rx_track_hot_cue(t, 1) == -1);
    c = cue(1.0, 8);
    CHECK(rx_track_add_cue(t, &c) == RX_ERROR(EINVAL));
    c = cue(NAN, 1);
    CHECK(rx_track_add_cue(t, &c) == RX_ERROR(EINVAL));

    /* Memory cues around a time, 20 ms tolerance as the CDJ. Memory cues
     * are at 5, 10, 20; the hot cue at 30 never counts. */
    CHECK(rx_track_memory_cue_at(t, 10.015, 0.02) == 1);
    CHECK(rx_track_memory_cue_at(t, 10.025, 0.02) == -1);
    CHECK(rx_track_memory_cue_at(t, 30.0, 0.02) == -1);
    CHECK(rx_track_memory_cue_before(t, 10.0, 0.02) == 0);
    CHECK(rx_track_memory_cue_before(t, 10.03, 0.02) == 1);
    CHECK(rx_track_memory_cue_before(t, 5.0, 0.02) == -1);
    CHECK(rx_track_memory_cue_after(t, 10.0, 0.02) == 2);
    CHECK(rx_track_memory_cue_after(t, 9.97, 0.02) == 1);
    CHECK(rx_track_memory_cue_after(t, 20.0, 0.02) == -1);
    CHECK(rx_track_memory_cue_nth(t, 1) == 0);
    CHECK(rx_track_memory_cue_nth(t, 3) == 2);
    CHECK(rx_track_memory_cue_nth(t, 4) == -1);
    CHECK(rx_track_memory_cue_nth(t, 0) == -1);

    CHECK(rx_track_remove_cue(t, 0) == 0 && t->nb_cues == 3 && t->cues[0].time == 10.0);
    CHECK(rx_track_remove_cue(t, 3) == RX_ERROR(EINVAL));

    /* Deep copies. */
    RXTrack *d = rx_track_dup(t);
    CHECK(d && d != t && d->cues != t->cues && d->path != t->path);
    CHECK(d->nb_cues == 3 && !strcmp(d->path, t->path) && !strcmp(d->cues[2].name, "drop"));
    rx_track_free(d);
    rx_track_free(t);
}

static void test_musical(void)
{
    RXTrack *t = rx_track_new("/music/y.flac");
    rx_grid_init(&t->grid, 0.5, 120.0);

    /* A cue a hair off bar 2 snaps onto it; a 4-beat loop measured on the grid. */
    RXCue c = cue(2.5 + 1e-9, -1);
    c.loop_end = 4.5;
    rx_cue_attach_to_grid(&c, &t->grid);
    CHECK(rx_cue_is_musical(&c) && c.grid_beat == 4.0 && c.time == 2.5);
    CHECK(c.loop_beats == 4.0 && c.loop_end == 4.5);
    rx_track_add_cue(t, &c);

    RXCue a = cue(3.0, 1);
    rx_track_add_cue(t, &a);        /* absolute */
    RXCue h = cue(1.0, 0);
    rx_cue_attach_to_grid(&h, &t->grid);
    rx_track_add_cue(t, &h);
    CHECK(h.grid_beat == 1.0 && !rx_cue_is_loop(&h) && isnan(h.loop_beats));

    /* Half the tempo: musical cues follow, the absolute one stays, and the
     * list is sorted again (bar 2 moved past 3.0). */
    rx_grid_set_bpm_at(&t->grid, 1.0, 60.0);
    rx_track_resolve_cues(t);
    CHECK(t->nb_cues == 3);
    CHECK(t->cues[0].time == 1.5 && t->cues[0].hot == 0);
    CHECK(t->cues[1].time == 3.0 && t->cues[1].hot == 1);
    CHECK(t->cues[2].time == 4.5 && t->cues[2].loop_end == 8.5);

    /* A grid that moves the downbeat moves "bar 2" with it. */
    rx_grid_shift(&t->grid, 0.25);
    rx_track_resolve_cues(t);
    CHECK(t->cues[0].time == 1.75 && t->cues[2].time == 4.75 && t->cues[2].loop_end == 8.75);

    /* An absolute cue with a loop length in beats follows the tempo too. */
    RXCue l = cue(10.0, -1);
    l.loop_end = 12.0;
    l.loop_beats = 2.0;
    rx_track_add_cue(t, &l);
    rx_grid_set_bpm_at(&t->grid, 1.0, 120.0);
    rx_track_resolve_cues(t);
    int i = t->nb_cues - 1;
    CHECK(t->cues[i].time == 10.0 && t->cues[i].loop_end == 11.0);

    /* No grid: attach leaves the cue absolute. */
    RXGrid none = { 0 };
    RXCue n = cue(7.0, -1);
    rx_cue_attach_to_grid(&n, &none);
    CHECK(!rx_cue_is_musical(&n));
    rx_track_free(t);
}

static void test_meta(void)
{
    RXMeta m = { 0 };
    CHECK(rx_meta_set(&m.title, "Track") == 0 && !strcmp(m.title, "Track"));
    CHECK(rx_meta_set(&m.artist, "") == 0 && !m.artist);
    CHECK(rx_meta_set(&m.title, NULL) == 0 && !m.title);
    rx_meta_set(&m.title, "日本語のタイトル");
    m.year = 2024;
    m.rating = 4;
    snprintf(m.color, sizeof(m.color), "pink");
    CHECK(rx_meta_add_tag(&m, "Situation/Peak Time") == 0);
    CHECK(rx_meta_add_tag(&m, "vocal") == 0);
    CHECK(rx_meta_add_tag(&m, "vocal") == 0 && m.nb_tags == 2);
    CHECK(rx_meta_add_tag(&m, "") == RX_ERROR(EINVAL));
    CHECK(rx_meta_has_tag(&m, "vocal") && !rx_meta_has_tag(&m, "Vocal"));

    RXMeta c = { 0 };
    CHECK(rx_meta_copy(&c, &m) == 0);
    CHECK(c.title != m.title && !strcmp(c.title, m.title) && c.year == 2024 && c.rating == 4);
    CHECK(c.nb_tags == 2 && c.tags != m.tags && !strcmp(c.tags[1], "vocal"));
    CHECK(!strcmp(c.color, "pink"));
    /* Copying over a filled struct frees what it held. */
    CHECK(rx_meta_copy(&c, &m) == 0);
    rx_meta_clear_tags(&c);
    CHECK(!c.nb_tags && !c.tags);
    rx_meta_free(&c);
    rx_meta_free(&m);
    CHECK(!m.title && !m.nb_tags);

    RXTrack *t = rx_track_new("/music/Album/01 Track.flac");
    char buf[64];
    CHECK_STR(rx_track_display(t, buf, sizeof(buf)), "01 Track.flac");
    rx_meta_set(&t->meta.title, "Title");
    CHECK_STR(rx_track_display(t, buf, sizeof(buf)), "Title");
    rx_meta_set(&t->meta.artist, "Artist");
    CHECK_STR(rx_track_display(t, buf, sizeof(buf)), "Artist - Title");
    rx_track_free(t);
}

/* A loop on the pickup stays a loop when a grid edit carries it before 0
 * (loop_end < 0 means "no loop": a save then would drop it for good), and
 * comes back where it was when the edit is undone. Beat indices count
 * from the first downbeat after 0, so only cues before it can go there. */
static void test_loop_before_zero(void)
{
    RXTrack *t = rx_track_new("/music/z.flac");
    rx_grid_init(&t->grid, 0.5, 120.0);
    RXCue c = cue(0.0, -1);                 /* the pickup beat, half a beat long */
    c.loop_end = 0.25;
    rx_cue_attach_to_grid(&c, &t->grid);
    CHECK(c.grid_beat == -1.0 && c.loop_beats == 0.5);
    rx_track_add_cue(t, &c);

    rx_grid_shift(&t->grid, -0.4);
    rx_track_resolve_cues(t);
    CHECK(fabs(t->cues[0].time + 0.4) < 1e-12);
    CHECK(rx_cue_is_loop(&t->cues[0]) && t->cues[0].loop_beats == 0.5);

    rx_grid_shift(&t->grid, 0.4);
    rx_track_resolve_cues(t);
    CHECK(fabs(t->cues[0].time) < 1e-12 && fabs(t->cues[0].loop_end - 0.25) < 1e-12);
    rx_track_free(t);
}

int main(void)
{
    test_cue_list();
    test_musical();
    test_meta();
    test_loop_before_zero();
    return test_done("core_cues");
}
