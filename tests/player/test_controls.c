/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* The deck's buttons (RxDeckCtl) on the null backend in manual mode: CUE
 * (set, back cue, preview, preview held into PLAY), hot cues (set, call,
 * hot loop, rename, delete), the MEMORY cluster with its 20 ms tolerance,
 * loops (IN, OUT, EXIT, RELOOP, auto loops, /2, x2), beat jump with and
 * without QUANTIZE (across a tempo change too), active memory loops
 * engaging from rx_ctl_tick, and grid edits moving musical cues.
 *
 * The track is 60 s of 44.1 kHz clicks at 128 BPM with a grid built by
 * rx_grid_init; the output runs at the track's rate so every position is
 * exact. Time advances in 10 ms steps with a tick after each, as the
 * interface would. */

#include "rxbox/player.h"

#include <math.h>
#include <stdio.h>

#define RATE    44100
#define BPM     128.0
#define SECONDS 60.0

static int failures;

#define CHECK(cond, ...)                                              \
    do {                                                              \
        if (!(cond)) {                                                \
            fprintf(stderr, "%s:%d: %s: ", __FILE__, __LINE__, #cond); \
            fprintf(stderr, __VA_ARGS__);                             \
            fputc('\n', stderr);                                      \
            failures++;                                               \
        }                                                             \
    } while (0)

#define NEAR(a, b) (fabs((a) - (b)) < 1e-6)

/* Beat k of the grid. */
static double B(double k)
{
    return k * 60.0 / BPM;
}

static RxPlayer *p;
static RxDeckCtl c;
static RxTrack *track;
static int changes;

static void on_changed(void *opaque, int deck)
{
    changes++;
}

static RxAudio *make_track(void)
{
    RxAudio *a = calloc(1, sizeof(*a));
    if (!a)
        abort();
    a->rate = RATE;
    a->channels = 2;
    a->nb_frames = (int64_t)(SECONDS * RATE);
    a->samples = calloc((size_t)a->nb_frames * 2, sizeof(float));
    if (!a->samples)
        abort();
    for (int k = 0;; k++) {
        int64_t f = (int64_t)floor(B(k) * RATE + 0.5);
        if (f >= a->nb_frames)
            break;
        a->samples[2 * f] = a->samples[2 * f + 1] = 0.5f;
    }
    return a;
}

static void on_release(void *opaque, RxAudio *audio)
{
    free(audio->samples);
    free(audio);
}

/* A second section at 30 s (beat 64) at 120 BPM when asked. */
static void setup(bool tempo_change)
{
    RxPlayerOptions opt;
    rx_player_options_default(&opt);
    opt.backend = RX_AUDIO_NULL;
    opt.rate = RATE;
    opt.period = -256;
    opt.release = on_release;
    if (rx_player_create(&p, &opt) < 0)
        abort();

    track = rx_track_new("/synthetic/clicks-128.flac");
    if (!track || rx_grid_init(&track->grid, 0.0, BPM) < 0)
        abort();
    if (tempo_change && rx_grid_add_section(&track->grid, 30.0, 120.0, 0) < 0)
        abort();
    track->duration = SECONDS;
    track->sample_rate = RATE;
    track->channels = 2;

    if (rx_deck_load(p, 0, make_track(), &track->grid) < 0)
        abort();
    rx_ctl_init(&c, p, 0, on_changed, NULL);
    rx_ctl_load(&c, track);
    changes = 0;
}

static void teardown(void)
{
    rx_ctl_unload(&c);
    rx_player_destroy(p);
    rx_track_free(track);
}

static RxDeckState st(void)
{
    RxDeckState s;
    rx_deck_state(p, 0, &s);
    return s;
}

static double pos(void)
{
    return st().position;
}

static bool playing(void)
{
    return st().playing;
}

/* Plays on for `seconds`, ticking every 10 ms. */
static void run(double seconds)
{
    int frames = (int)lround(seconds * RATE);
    while (frames > 0) {
        int n = RX_MIN(frames, RATE / 100);
        rx_player_null_advance(p, n);
        rx_ctl_tick(&c);
        frames -= n;
    }
}

/* Plays on, checking the playhead never leaves [in, out). */
static bool stays_inside(double seconds, double in, double out)
{
    bool ok = true;
    for (int i = 0; i < (int)(seconds * 100); i++) {
        run(0.01);
        double t = pos();
        if (t < in - 1e-9 || t >= out)
            ok = false;
    }
    return ok;
}

static int nb_memory(void)
{
    int n = 0;
    for (int i = 0; i < track->nb_cues; i++)
        n += !rx_cue_is_hot(&track->cues[i]);
    return n;
}

/* ----------------------------------------------------------------- tests */

static void test_cue(void)
{
    setup(false);
    CHECK(c.cue_point == 0.0 && pos() == 0.0, "auto cue at the start");

    /* Paused elsewhere: CUE sets the cue point, on the nearest beat. */
    rx_ctl_seek(&c, 1.0);
    rx_ctl_cue_press(&c);
    rx_ctl_cue_release(&c);
    CHECK(c.cue_point == B(2), "cue point %f, want %f", c.cue_point, B(2));
    CHECK(pos() == B(2) && !playing(), "the paused playhead moves onto it");

    /* Paused on it: preview while held, back and paused on release. */
    rx_ctl_cue_press(&c);
    CHECK(c.cue_held && playing(), "preview plays");
    run(0.5);
    CHECK(NEAR(pos(), B(2) + 0.5), "previewing: %f", pos());
    rx_ctl_cue_release(&c);
    CHECK(!c.cue_held && !playing() && pos() == B(2), "released: back on the cue point");

    /* Playing: back cue, paused. */
    rx_ctl_play_pause(&c);
    CHECK(playing(), "PLAY");
    run(1.0);
    rx_ctl_cue_press(&c);
    rx_ctl_cue_release(&c);
    CHECK(!playing() && pos() == B(2), "back cue: %f", pos());

    /* PLAY while previewing: carries on after CUE is released. */
    rx_ctl_cue_press(&c);
    run(0.2);
    rx_ctl_play_pause(&c);
    rx_ctl_cue_release(&c);
    run(0.2);
    CHECK(playing() && NEAR(pos(), B(2) + 0.4), "kept playing: %f", pos());

    /* Without QUANTIZE the cue point is exactly where the deck stands. */
    rx_ctl_play_pause(&c);
    CHECK(!playing(), "PAUSE");
    rx_ctl_set_quantize(&c, false);
    rx_ctl_seek(&c, 1.0);
    rx_ctl_cue_press(&c);
    rx_ctl_cue_release(&c);
    CHECK(c.cue_point == 1.0 && pos() == 1.0, "unquantised cue point %f", c.cue_point);

    /* A 10 ms fine move off the cue point sets a new one, not a preview. */
    rx_ctl_beat_jump(&c, 0.0);
    rx_ctl_cue_press(&c);
    CHECK(!playing() && NEAR(c.cue_point, 1.01), "nudged cue point %f", c.cue_point);
    rx_ctl_cue_release(&c);

    /* While playing, set-cue-here takes the playhead without stopping. */
    rx_ctl_set_quantize(&c, true);
    rx_ctl_play_pause(&c);
    run(1.0);
    rx_ctl_set_cue_here(&c);
    CHECK(playing() && c.cue_point == B(4), "cue here %f", c.cue_point);
    CHECK(changes == 0, "no cue list edit: %d", changes);
    teardown();
}

static void test_hot_cues(void)
{
    setup(false);

    /* Empty pad: stores the playhead on the nearest beat, musical. */
    rx_ctl_seek(&c, 2.0);
    rx_ctl_hot_cue_press(&c, 0);
    rx_ctl_hot_cue_release(&c, 0);
    int a = rx_track_hot_cue(track, 0);
    CHECK(a >= 0, "pad A set");
    if (a >= 0) {
        const RxCue *cue = &track->cues[a];
        CHECK(cue->time == B(4) && !rx_cue_is_loop(cue), "at %f", cue->time);
        CHECK(rx_cue_is_musical(cue) && cue->grid_beat == 4.0, "musical, beat %f", cue->grid_beat);
    }
    CHECK(changes == 1, "changed once: %d", changes);
    CHECK(!playing() && pos() == 2.0, "setting leaves the transport alone");

    /* Set pad: jump there and play, from pause too. */
    rx_ctl_seek(&c, 10.0);
    rx_ctl_hot_cue_press(&c, 0);
    CHECK(playing() && pos() == B(4), "called: %f", pos());
    CHECK(c.cue_point == 0.0, "a hot cue is not the cue point");
    run(0.5);
    CHECK(NEAR(pos(), B(4) + 0.5), "playing on: %f", pos());
    rx_ctl_hot_cue_release(&c, 0);
    CHECK(playing(), "the pad is not a gate");

    /* Without QUANTIZE: the exact playhead, absolute. */
    rx_ctl_play_pause(&c);
    rx_ctl_set_quantize(&c, false);
    double here = pos();
    rx_ctl_hot_cue_press(&c, 1);
    int b = rx_track_hot_cue(track, 1);
    CHECK(b >= 0 && track->cues[b].time == here && !rx_cue_is_musical(&track->cues[b]),
          "pad B absolute at %f", here);
    rx_ctl_set_quantize(&c, true);

    /* Name and colour without moving it. */
    rx_ctl_hot_cue_set(&c, 0, "drop", "#ff0000");
    a = rx_track_hot_cue(track, 0);
    CHECK(a >= 0 && !strcmp(track->cues[a].name, "drop") &&
          !strcmp(track->cues[a].color, "#ff0000") && track->cues[a].time == B(4), "renamed");

    rx_ctl_hot_cue_delete(&c, 0);
    CHECK(rx_track_hot_cue(track, 0) < 0, "deleted");
    CHECK(changes == 4, "changes %d", changes);

    /* An empty pad during a loop stores a hot loop. */
    rx_ctl_seek(&c, B(20));
    rx_ctl_play_pause(&c);
    rx_ctl_auto_loop(&c, 4);
    CHECK(c.looping, "looping");
    rx_ctl_hot_cue_press(&c, 2);
    int h = rx_track_hot_cue(track, 2);
    CHECK(h >= 0 && track->cues[h].time == B(20) && track->cues[h].loop_end == B(24) &&
          track->cues[h].loop_beats == 4.0, "hot loop C");
    CHECK(c.looping, "still looping");

    /* Called later, it plays as a loop. */
    rx_ctl_loop_exit(&c);
    rx_ctl_seek(&c, 40.0);
    run(0.1);
    rx_ctl_hot_cue_press(&c, 2);
    RxDeckState s = st();
    CHECK(c.looping && s.playing && s.position == B(20), "hot loop called");
    CHECK(s.loop_enabled && s.loop_in == B(20) && s.loop_out == B(24), "deck loop %f %f",
          s.loop_in, s.loop_out);
    CHECK(stays_inside(4.0, B(20), B(24)), "loops");

    /* A hot cue call leaves a playing loop. */
    rx_ctl_hot_cue_press(&c, 1);
    CHECK(!c.looping && !st().loop_enabled, "left the loop");
    teardown();
}

static void test_memory(void)
{
    setup(false);

    /* Three memory cues on beats 8, 16 and 24: CUE then MEMORY. */
    for (int k = 8; k <= 24; k += 8) {
        rx_ctl_seek(&c, B(k) + 0.1);
        rx_ctl_cue_press(&c);
        rx_ctl_cue_release(&c);
        rx_ctl_memory_set(&c);
    }
    CHECK(nb_memory() == 3 && changes == 3, "three memory cues: %d", nb_memory());
    rx_ctl_memory_set(&c);
    CHECK(nb_memory() == 3 && changes == 3, "no duplicate");

    /* MEMORY stores the cue point, not the playhead. */
    rx_ctl_seek(&c, 20.0);
    rx_ctl_memory_set(&c);
    CHECK(nb_memory() == 3, "the cue point (beat 24) is already stored");

    /* Previous/next around beat 16 with the 20 ms tolerance. */
    rx_ctl_seek(&c, B(16) + 0.015);
    rx_ctl_memory_next(&c);
    CHECK(pos() == B(24) && c.cue_point == B(24), "next from +15 ms: %f", pos());
    rx_ctl_seek(&c, B(16) + 0.015);
    rx_ctl_memory_prev(&c);
    CHECK(pos() == B(8) && c.cue_point == B(8), "prev from +15 ms skips the one it is on: %f",
          pos());
    rx_ctl_seek(&c, B(16) + 0.025);
    rx_ctl_memory_prev(&c);
    CHECK(pos() == B(16), "prev from +25 ms: %f", pos());
    rx_ctl_seek(&c, B(16) - 0.015);
    rx_ctl_memory_next(&c);
    CHECK(pos() == B(24), "next from -15 ms: %f", pos());
    rx_ctl_seek(&c, B(16) - 0.025);
    rx_ctl_memory_next(&c);
    CHECK(pos() == B(16), "next from -25 ms: %f", pos());
    rx_ctl_seek(&c, B(8) - 0.5);
    rx_ctl_memory_prev(&c);
    CHECK(pos() == B(8) - 0.5, "nothing before the first");

    /* Calling keeps the playing state. */
    rx_ctl_seek(&c, B(16) + 0.1);
    rx_ctl_play_pause(&c);
    rx_ctl_memory_prev(&c);
    CHECK(playing() && pos() == B(16), "called while playing");
    rx_ctl_play_pause(&c);

    /* DELETE: only the one the playhead is on, within 20 ms. */
    rx_ctl_seek(&c, 5.0);
    rx_ctl_memory_delete(&c);
    CHECK(nb_memory() == 3, "nothing to delete at 5 s");
    rx_ctl_seek(&c, B(16) + 0.015);
    rx_ctl_memory_delete(&c);
    CHECK(nb_memory() == 2 && rx_track_memory_cue_at(track, B(16), 0.02) < 0, "deleted beat 16");
    CHECK(changes == 4, "changes %d", changes);

    /* MEMORY during a loop stores the loop. */
    rx_ctl_seek(&c, B(32));
    rx_ctl_auto_loop(&c, 4);
    rx_ctl_memory_set(&c);
    int m = rx_track_memory_cue_at(track, B(32), 0.02);
    CHECK(m >= 0 && rx_cue_is_loop(&track->cues[m]) && track->cues[m].loop_end == B(36),
          "memory loop stored");
    rx_ctl_loop_exit(&c);

    /* Calling a memory loop calls the loop. */
    rx_ctl_seek(&c, B(30));
    rx_ctl_memory_next(&c);
    CHECK(c.looping && pos() == B(32) && st().loop_enabled && st().loop_out == B(36),
          "memory loop called");
    rx_ctl_loop_exit(&c);

    /* Auto cue: a new load waits at the first memory cue. */
    rx_ctl_load(&c, track);
    CHECK(c.cue_point == B(8) && pos() == B(8), "auto cue %f", c.cue_point);
    teardown();
}

static void test_loops(void)
{
    setup(false);

    rx_ctl_seek(&c, B(8) + 0.05);
    rx_ctl_play_pause(&c);
    rx_ctl_loop_in(&c);
    CHECK(c.loop_in == B(8) && c.loop_out < 0 && !c.looping, "IN at %f", c.loop_in);
    CHECK(c.cue_point == B(8), "IN is also the cue point");
    CHECK(playing(), "IN keeps playing");

    /* OUT a little after beat 12: quantised behind the playhead, so the
     * playhead folds back into the loop at the same phase. */
    run(1.9);
    rx_ctl_loop_out(&c);
    RxDeckState s = st();
    CHECK(c.looping && c.loop_out == B(12), "OUT at %f", c.loop_out);
    CHECK(s.loop_enabled && s.loop_in == B(8) && s.loop_out == B(12), "deck loop");
    run(0.0);
    rx_player_null_advance(p, 1);
    CHECK(fabs(pos() - (B(8) + 0.075 + 1.0 / RATE)) < 1e-6, "folded: %f", pos());
    CHECK(stays_inside(8.0, B(8), B(12)), "loops");

    /* EXIT, then RELOOP from the in point. */
    rx_ctl_exit_reloop(&c);
    CHECK(!c.looping && !st().loop_enabled && c.loop_in == B(8) && c.loop_out == B(12),
          "exited, loop kept");
    run(3.0);
    CHECK(pos() > B(12), "plays on: %f", pos());
    rx_ctl_exit_reloop(&c);
    CHECK(c.looping && pos() == B(8) && playing(), "reloop");
    CHECK(stays_inside(3.0, B(8), B(12)), "loops again");
    rx_ctl_loop_exit(&c);
    CHECK(!c.looping, "exit");

    /* An OUT quantised onto the in point closes on the next beat. */
    rx_ctl_play_pause(&c);
    rx_ctl_seek(&c, B(40) + 0.01);
    rx_ctl_loop_in(&c);
    CHECK(pos() == B(40), "a paused IN moves onto the beat");
    rx_ctl_loop_out(&c);
    CHECK(c.looping && c.loop_in == B(40) && c.loop_out == B(41), "one beat: %f %f",
          c.loop_in, c.loop_out);
    rx_ctl_loop_exit(&c);

    /* OUT with nothing pending uses the cue point as the in point. */
    rx_ctl_seek(&c, B(42) + 0.1);
    rx_ctl_cue_press(&c);
    rx_ctl_cue_release(&c);
    rx_ctl_play_pause(&c);
    run(1.0);
    rx_ctl_loop_out(&c);
    CHECK(c.looping && c.loop_in == B(42) && c.loop_out == B(44), "from the cue point: %f %f",
          c.loop_in, c.loop_out);
    teardown();
}

static void test_auto_loop(void)
{
    setup(false);

    rx_ctl_seek(&c, 10.0);
    rx_ctl_play_pause(&c);
    rx_ctl_auto_loop(&c, 4);
    RxDeckState s = st();
    CHECK(c.looping && c.loop_in == B(21) && c.loop_out == B(25), "4 beats: %f %f",
          c.loop_in, c.loop_out);
    CHECK(s.loop_enabled && s.loop_out == B(25) && c.last_hot_loop_beats == 4.0, "on the deck");
    CHECK(stays_inside(3.0, B(21), B(25)), "loops");

    rx_ctl_loop_half(&c);
    CHECK(c.loop_in == B(21) && c.loop_out == B(23) && st().loop_out == B(23), "/2: %f",
          c.loop_out);
    CHECK(c.last_hot_loop_beats == 2.0, "2 beats");
    CHECK(stays_inside(2.0, B(21), B(23)), "loops /2");
    rx_ctl_loop_double(&c);
    CHECK(c.loop_out == B(25) && st().loop_out == B(25), "x2: %f", c.loop_out);
    rx_ctl_loop_double(&c);
    CHECK(c.loop_out == B(29), "x2 again: %f", c.loop_out);

    /* Another size resizes from the in point; the same size again exits. */
    rx_ctl_auto_loop(&c, 16);
    CHECK(c.looping && c.loop_in == B(21) && c.loop_out == B(37), "16 beats");
    rx_ctl_auto_loop(&c, 16);
    CHECK(!c.looping && !st().loop_enabled, "same size: exit");

    /* Fractions: down to 1/32 beat. */
    rx_ctl_auto_loop(&c, 0.25);
    CHECK(c.looping && NEAR(c.loop_out - c.loop_in, B(0.25)), "1/4 beat");
    for (int i = 0; i < 8; i++)
        rx_ctl_loop_half(&c);
    CHECK(NEAR(c.loop_out - c.loop_in, B(1.0 / 32)), "stops at 1/32: %f", c.loop_out - c.loop_in);
    rx_ctl_loop_exit(&c);

    /* Without QUANTIZE the loop starts at the playhead itself. */
    rx_ctl_set_quantize(&c, false);
    rx_ctl_play_pause(&c);
    rx_ctl_seek(&c, 20.0);
    rx_ctl_auto_loop(&c, 4);
    CHECK(c.loop_in == 20.0 && NEAR(c.loop_out, 20.0 + B(4)), "unquantised: %f %f",
          c.loop_in, c.loop_out);
    rx_ctl_loop_exit(&c);

    /* No grid, no beat loop. */
    RxGrid saved = track->grid;
    track->grid = (RxGrid) { 0 };
    rx_ctl_auto_loop(&c, 4);
    CHECK(!c.looping, "no grid");
    track->grid = saved;
    teardown();
}

static void test_beat_jump(void)
{
    setup(true);

    rx_ctl_seek(&c, 10.0);
    rx_ctl_beat_jump(&c, 4);
    CHECK(NEAR(pos(), 10.0 + B(4)) && !playing(), "+4: %f", pos());
    rx_ctl_beat_jump(&c, -4);
    CHECK(NEAR(pos(), 10.0), "-4: %f", pos());

    rx_ctl_play_pause(&c);
    rx_ctl_beat_jump(&c, 4);
    CHECK(playing() && NEAR(pos(), 10.0 + B(4)), "+4 playing: %f", pos());
    rx_ctl_play_pause(&c);

    /* Across the change to 120 BPM at 30 s (beat 64). With QUANTIZE the
     * jump counts grid beats: 1.0667 beats of 128 to the change, then
     * 2.9333 of 120. Without, it is 4 beats of the tempo at the playhead. */
    rx_ctl_seek(&c, 29.5);
    rx_ctl_beat_jump(&c, 4);
    double want = 30.0 + (4.0 - 0.5 / B(1)) * 0.5;
    CHECK(NEAR(pos(), want), "on the grid: %f, want %f", pos(), want);
    rx_ctl_set_quantize(&c, false);
    rx_ctl_seek(&c, 29.5);
    rx_ctl_beat_jump(&c, 4);
    CHECK(NEAR(pos(), 29.5 + B(4)), "in seconds: %f", pos());
    rx_ctl_seek(&c, 32.0);
    rx_ctl_beat_jump(&c, -4);
    CHECK(NEAR(pos(), 30.0), "120 BPM back: %f", pos());
    rx_ctl_set_quantize(&c, true);

    /* Fine: 10 ms either way. */
    rx_ctl_seek(&c, 10.0);
    rx_ctl_beat_jump(&c, 0.0);
    CHECK(NEAR(pos(), 10.01), "fine forward: %f", pos());
    rx_ctl_beat_jump(&c, -0.0);
    CHECK(NEAR(pos(), 10.0), "fine back: %f", pos());

    /* Clamped to the track. */
    rx_ctl_seek(&c, 0.5);
    rx_ctl_beat_jump(&c, -4);
    CHECK(pos() == 0.0, "start: %f", pos());

    /* During a loop the loop moves with the playhead. */
    rx_ctl_seek(&c, B(8));
    rx_ctl_play_pause(&c);
    rx_ctl_auto_loop(&c, 4);
    run(0.3);
    rx_ctl_beat_jump(&c, 4);
    RxDeckState s = st();
    CHECK(c.looping && c.loop_in == B(12) && c.loop_out == B(16), "loop moved: %f %f",
          c.loop_in, c.loop_out);
    CHECK(s.loop_in == B(12) && NEAR(s.position, B(12) + 0.3), "deck: %f", s.position);
    CHECK(stays_inside(3.0, B(12), B(16)), "loops there");
    teardown();
}

static void test_active_loop(void)
{
    setup(false);

    /* A memory loop on beats 32-36, made active. */
    rx_ctl_seek(&c, B(32));
    rx_ctl_auto_loop(&c, 4);
    rx_ctl_memory_set(&c);
    rx_ctl_loop_exit(&c);
    int m = rx_track_memory_cue_at(track, B(32), 0.02);
    CHECK(m >= 0, "stored");
    rx_ctl_memory_set_active(&c, m, true);
    m = rx_track_memory_cue_at(track, B(32), 0.02);
    CHECK(m >= 0 && track->cues[m].active, "active");
    CHECK(changes == 2, "changes %d", changes);

    /* Played into: engages at its in point. */
    rx_ctl_seek(&c, 14.0);
    rx_ctl_play_pause(&c);
    run(0.9);
    CHECK(!c.looping, "not yet");
    run(0.2);
    RxDeckState s = st();
    CHECK(c.looping && c.active_memory_loop == m, "engaged");
    CHECK(s.loop_enabled && s.loop_in == B(32) && s.loop_out == B(36), "on the deck");
    CHECK(stays_inside(5.0, B(32), B(36)), "loops");

    /* Exit: plays on and does not engage again. */
    rx_ctl_exit_reloop(&c);
    CHECK(!c.looping && c.active_memory_loop == -1, "exited");
    run(2.5);
    CHECK(!c.looping && pos() > B(36), "passed: %f", pos());

    /* A jump past the in point does not engage it. */
    rx_ctl_seek(&c, B(32) + 0.2);
    run(1.0);
    CHECK(!c.looping, "jumped past");

    /* Paused just before: nothing until played into. */
    rx_ctl_play_pause(&c);
    rx_ctl_seek(&c, B(32) - 0.05);
    run(0.5);
    CHECK(!c.looping && !playing(), "paused");
    rx_ctl_play_pause(&c);
    run(0.1);
    CHECK(c.looping, "engaged once played");

    /* Landing exactly on the in point (a hot cue there) and playing on
     * counts as reaching it. */
    rx_ctl_loop_exit(&c);
    rx_ctl_seek(&c, B(30));
    rx_ctl_hot_cue_press(&c, 0);
    rx_ctl_seek(&c, B(32));
    rx_ctl_hot_cue_press(&c, 1);
    rx_ctl_hot_cue_press(&c, 1);
    CHECK(!c.looping && pos() == B(32), "called onto the in point");
    run(0.05);
    CHECK(c.looping && c.active_memory_loop >= 0, "engaged from its in point");

    /* A loop the DJ is playing around the in point is not taken over. */
    rx_ctl_loop_exit(&c);
    rx_ctl_hot_cue_press(&c, 0);
    rx_ctl_auto_loop(&c, 8);
    CHECK(c.looping && c.loop_in == B(30) && c.loop_out == B(38), "8 beats around it");
    run(8.0);
    CHECK(c.looping && c.loop_in == B(30) && c.active_memory_loop == -1, "still the DJ's loop");

    /* Deactivated: played into, it stays a plain memory loop. */
    rx_ctl_loop_exit(&c);
    m = rx_track_memory_cue_at(track, B(32), 0.02);
    rx_ctl_memory_set_active(&c, m, false);
    CHECK(!track->cues[m].active, "inactive");
    rx_ctl_seek(&c, B(32) - 0.2);
    run(0.5);
    CHECK(!c.looping, "inactive loop does not engage");
    teardown();
}

static void test_grid_changed(void)
{
    setup(false);
    rx_ctl_seek(&c, B(8));
    rx_ctl_hot_cue_press(&c, 0);            /* musical */
    rx_ctl_set_quantize(&c, false);
    rx_ctl_seek(&c, 5.0);
    rx_ctl_hot_cue_press(&c, 1);            /* absolute */
    changes = 0;

    rx_grid_shift(&track->grid, 0.1);
    rx_ctl_grid_changed(&c);
    int a = rx_track_hot_cue(track, 0), b = rx_track_hot_cue(track, 1);
    CHECK(a >= 0 && NEAR(track->cues[a].time, B(8) + 0.1), "musical cue follows: %f",
          a >= 0 ? track->cues[a].time : -1.0);
    CHECK(b >= 0 && track->cues[b].time == 5.0, "absolute cue stays");
    CHECK(changes == 1, "changed");
    teardown();
}

int main(void)
{
    test_cue();
    test_hot_cues();
    test_memory();
    test_loops();
    test_auto_loop();
    test_beat_jump();
    test_active_loop();
    test_grid_changed();

    if (failures) {
        fprintf(stderr, "%d failures\n", failures);
        return 1;
    }
    printf("player controls: all tests passed\n");
    return 0;
}
