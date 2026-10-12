/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* The playback engine on the null backend in manual mode: positions at
 * tempo 1.0 and 1.08 across a resampling ratio, seeks, sample-exact loop
 * wraps checked against the expected output frame by frame, pause and
 * resume, tempo changes while playing, the end of the track, buffer
 * release after the real-time side's acknowledgement, scrubbing, meters,
 * the master volume and the limiter.
 *
 * The track is synthetic: 44.1 kHz stereo, 60 s, a one-sample click on
 * every beat at 128 BPM whose height encodes the beat number (negated on
 * the right channel), so any frame of output says which track frame it
 * came from. The output is read by calling the mix directly, which in
 * manual mode is exactly what rx_player_null_advance does. */

#include "rxbox/player.h"

#include <math.h>
#include <stdio.h>
#include <sys/mman.h>
#include <time.h>

#include "src/player/player_internal.h"

#define RATE        44100
#define BPM         128.0
#define BEAT_FRAMES (RATE * 60.0 / BPM)     /* 20671.875 */
#define SECONDS     60.0
#define PERIOD      256

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

static int64_t click_frame(int k)
{
    return (int64_t)floor(k * BEAT_FRAMES + 0.5);
}

static float click_amp(int k)
{
    return 0.25f + 0.005f * (k % 100);
}

static RXAudio *make_track_frames(int channels, int64_t nb_frames)
{
    RXAudio *a = calloc(1, sizeof(*a));
    if (!a)
        abort();
    a->rate = RATE;
    a->channels = channels;
    a->nb_frames = nb_frames;
    a->samples = calloc((size_t)a->nb_frames * channels, sizeof(float));
    if (!a->samples)
        abort();
    for (int k = 0; click_frame(k) < a->nb_frames; k++) {
        float *s = a->samples + click_frame(k) * channels;
        s[0] = click_amp(k);
        if (channels > 1)
            s[1] = -click_amp(k);
    }
    return a;
}

static RXAudio *make_track(int channels, double seconds)
{
    return make_track_frames(channels, (int64_t)(seconds * RATE));
}

/* Releases are recorded so the tests can see when they happen. */
static RXAudio *released[16];
static int nb_released;

static void on_release(void *opaque, RXAudio *audio)
{
    if (nb_released < (int)RX_ARRAY_ELEMS(released))
        released[nb_released] = audio;
    nb_released++;
    free(audio->samples);
    free(audio);
}

static RXPlayer *make_player(int rate)
{
    RXPlayerOptions opt;
    rx_player_options_default(&opt);
    opt.backend = RX_AUDIO_NULL;
    opt.rate = rate;
    opt.period = -PERIOD;
    opt.release = on_release;

    RXPlayer *p = NULL;
    int ret = rx_player_create(&p, &opt);
    CHECK(ret == 0 && p, "create: %d", ret);
    if (!p)
        exit(1);
    CHECK(rx_player_running(p), "null backend is running");
    CHECK(rx_player_error(p) == NULL, "no error while running");
    CHECK(rx_player_rate(p) == rate, "rate %d", rx_player_rate(p));
    return p;
}

static RXDeckState st(RXPlayer *p)
{
    RXDeckState s;
    rx_deck_state(p, 0, &s);
    return s;
}

/* Renders `frames` into out (or a scratch buffer) in callback-sized
 * pieces, as the manual null backend does. */
static void render(RXPlayer *p, float *out, int frames)
{
    static float scratch[2 * PERIOD];
    while (frames > 0) {
        int n = RX_MIN(frames, PERIOD);
        rx_player_mix(p, out ? out : scratch, n);
        if (out)
            out += 2 * n;
        frames -= n;
    }
}

/* ----------------------------------------------------------------- tests */

static void test_positions(void)
{
    double ratio = (double)RATE / 48000;

    for (int pass = 0; pass < 2; pass++) {
        double tempo = pass ? 1.08 : 1.0;
        RXPlayer *p = make_player(48000);
        CHECK(rx_deck_load(p, 0, make_track(2, SECONDS), NULL) == 0, "load");
        rx_deck_set_tempo(p, 0, tempo);

        RXDeckState s = st(p);
        CHECK(s.loaded && !s.playing && s.position == 0.0, "loaded at 0, paused");
        CHECK(fabs(s.duration - SECONDS) < 1e-9, "duration %f", s.duration);
        CHECK(s.tempo == tempo, "tempo %f", s.tempo);

        rx_deck_play(p, 0);
        CHECK(st(p).playing, "playing at once");
        int n = 48000 * 7 + 123;
        rx_player_null_advance(p, n);
        s = st(p);
        double want = n * ratio * tempo;
        CHECK(fabs(s.position * RATE - want) < 1.0, "tempo %.2f: %.3f frames, want %.3f",
              tempo, s.position * RATE, want);
        CHECK(llabs(s.frame - (int64_t)floor(want)) <= 1, "frame %lld", (long long)s.frame);
        CHECK(s.playing, "still playing");
        rx_player_destroy(p);
    }
}

static void test_seek(void)
{
    double ratio = (double)RATE / 48000;
    RXPlayer *p = make_player(48000);
    rx_deck_load(p, 0, make_track(2, SECONDS), NULL);
    rx_player_null_advance(p, PERIOD);

    uint64_t serial = st(p).serial;
    rx_deck_seek(p, 0, 10.0);
    RXDeckState s = st(p);
    CHECK(s.position == 10.0, "seek reported at once: %f", s.position);
    CHECK(s.serial == serial, "not applied yet");
    rx_player_null_advance(p, PERIOD);
    s = st(p);
    CHECK(s.position == 10.0 && !s.playing, "paused at 10: %f", s.position);
    CHECK(s.serial > serial, "applied");

    rx_deck_play(p, 0);
    rx_player_null_advance(p, 4800);
    rx_deck_seek(p, 0, 20.0);
    CHECK(st(p).position == 20.0, "seek while playing reported at once");
    rx_player_null_advance(p, 4800);
    s = st(p);
    CHECK(fabs(s.position * RATE - (20.0 * RATE + 4800 * ratio)) < 1.0, "playing on from 20: %f",
          s.position);
    CHECK(s.playing, "a seek keeps playing");

    rx_deck_seek(p, 0, -5.0);
    CHECK(st(p).position == 0.0, "clamped to the start");
    rx_deck_seek(p, 0, 1e9);
    s = st(p);
    CHECK(s.position == SECONDS && !s.playing, "clamped to the end, stopped");

    rx_deck_play_from(p, 0, 30.0);
    s = st(p);
    CHECK(s.position == 30.0 && s.playing, "play_from");
    rx_player_destroy(p);
}

/* Output frame by frame against the track: 44.1 kHz in and out, tempo 1,
 * a loop of eight beats whose ends are whole frames. Every output frame
 * must be the track frame the loop arithmetic says, wraps included. */
static void test_loop_exact(void)
{
    RXPlayer *p = make_player(RATE);
    RXAudio *a = make_track(2, SECONDS);
    rx_deck_load(p, 0, a, NULL);

    double in = 8 * 60.0 / BPM, out = 16 * 60.0 / BPM;   /* 3.75 s, 7.5 s */
    int64_t in_f = (int64_t)(in * RATE), out_f = (int64_t)(out * RATE);
    CHECK(in_f == 165375 && out_f == 330750, "whole frames");

    rx_deck_set_loop(p, 0, in, out, true);
    rx_deck_play_from(p, 0, 3.0);
    RXDeckState s = st(p);
    CHECK(s.loop_enabled && s.loop_in == in && s.loop_out == out, "loop reported");

    int64_t t = 3 * RATE;
    int wraps = 0, bad = 0, clicks = 0;
    float buf[2 * PERIOD];
    for (int chunk = 0; chunk < 60 * RATE / PERIOD; chunk++) {
        render(p, buf, PERIOD);
        for (int n = 0; n < PERIOD; n++) {
            float l = a->samples[2 * t], r = a->samples[2 * t + 1];
            if (fabsf(buf[2 * n] - l) > 1e-6f || fabsf(buf[2 * n + 1] - r) > 1e-6f) {
                if (bad++ < 5)
                    fprintf(stderr, "frame %lld: %f %f, want %f %f\n", (long long)t,
                            buf[2 * n], buf[2 * n + 1], l, r);
            }
            clicks += l != 0.0f;
            if (++t == out_f) {
                t = in_f;
                wraps++;
            }
        }
        s = st(p);
        if (t >= in_f) {
            CHECK(s.position >= in && s.position < out, "inside the loop: %f", s.position);
            CHECK(s.frame == t, "position %lld, want %lld", (long long)s.frame, (long long)t);
        }
    }
    CHECK(bad == 0, "%d frames differ", bad);
    CHECK(wraps >= 15, "%d wraps", wraps);
    CHECK(clicks > 100, "%d clicks seen", clicks);

    /* Off: plays through the out point. */
    rx_deck_loop_enable(p, 0, false);
    rx_player_null_advance(p, 10 * RATE);
    s = st(p);
    CHECK(!s.loop_enabled && s.position > out, "loop off plays on: %f", s.position);

    /* On again with the playhead more than a loop length past it: it is
     * not pulled back. */
    rx_deck_loop_enable(p, 0, true);
    double before = st(p).position;
    rx_player_null_advance(p, RATE);
    CHECK(st(p).position > before, "a loop behind the playhead does not engage");

    rx_deck_clear_loop(p, 0);
    s = st(p);
    CHECK(s.loop_in < 0 && s.loop_out < 0 && !s.loop_enabled, "cleared");
    rx_player_destroy(p);
}

/* A loop whose length is not a whole number of frames, resampled: the
 * position stays inside it and on the arithmetic's track. */
static void test_loop_fractional(void)
{
    RXPlayer *p = make_player(48000);
    rx_deck_load(p, 0, make_track(2, SECONDS), NULL);
    double in = 8 * 60.0 / BPM, out = 12 * 60.0 / BPM;   /* 165375 .. 248062.5 */
    double inc = (double)RATE / 48000;
    rx_deck_set_loop(p, 0, in, out, true);
    rx_deck_play_from(p, 0, 4.0);

    double in_f = in * RATE, len = (out - in) * RATE, start = 4.0 * RATE;
    for (int i = 1; i <= 400; i++) {
        rx_player_null_advance(p, 4 * PERIOD + 17);
        RXDeckState s = st(p);
        double want = start + (double)i * (4 * PERIOD + 17) * inc;
        want = in_f + fmod(want - in_f, len);
        CHECK(s.position >= in && s.position < out, "inside: %f", s.position);
        CHECK(fabs(s.position * RATE - want) < 1e-3, "at %f, want %f", s.position * RATE, want);
        if (failures)
            break;
    }
    rx_player_destroy(p);
}

/* Switching a loop on with the playhead just past its out point (LOOP OUT
 * quantised behind the playhead) folds it back at the same phase. */
static void test_loop_fold(void)
{
    RXPlayer *p = make_player(RATE);
    rx_deck_load(p, 0, make_track(2, SECONDS), NULL);
    rx_deck_seek(p, 0, 7.6);
    rx_player_null_advance(p, PERIOD);
    rx_deck_set_loop(p, 0, 3.75, 7.5, true);
    rx_player_null_advance(p, PERIOD);
    RXDeckState s = st(p);
    CHECK(fabs(s.position - 3.85) < 1e-9, "folded to 3.85: %f", s.position);

    rx_deck_seek(p, 0, 12.0);
    rx_player_null_advance(p, PERIOD);
    rx_deck_set_loop(p, 0, 3.75, 7.5, true);
    rx_player_null_advance(p, PERIOD);
    CHECK(st(p).position == 12.0, "more than a length past: left alone, %f", st(p).position);
    rx_player_destroy(p);
}

/* A loop that ends at the end of the track. For about one track length in
 * fourteen, nb_frames / rate * rate rounds above nb_frames, so the out
 * point the owner sends lies a fraction of a frame past the last frame. At
 * tempo 1 on a device at the track's rate the playhead lands exactly on
 * nb_frames: it must wrap there, not stop at the end of the track. */
static void test_loop_at_end(void)
{
    int64_t n = 10 * RATE;
    while ((double)n / RATE * RATE <= (double)n)
        n++;
    RXPlayer *p = make_player(RATE);
    rx_deck_load(p, 0, make_track_frames(2, n), NULL);
    double dur = (double)n / RATE;

    /* From a whole frame, so every step lands on one. */
    int64_t start = n - RATE / 2;
    while ((double)start / RATE * RATE != (double)start)
        start--;
    rx_deck_set_loop(p, 0, dur - 1.0, dur, true);
    rx_deck_play_from(p, 0, (double)start / RATE);
    for (int i = 0; i < 8; i++) {
        rx_player_null_advance(p, RATE / 4);
        RXDeckState s = st(p);
        CHECK(s.playing, "%lld frames: still looping after %d/4 s", (long long)n, i + 1);
        CHECK(s.position >= dur - 1.0 && s.position < dur, "inside the loop: %f of %f",
              s.position, dur);
        if (!s.playing)
            break;
    }
    rx_player_destroy(p);
}

/* Positions keep their fraction in [0, 1): a difference of two fractions
 * one ulp apart must not round up to a fraction of exactly 1.0, which
 * rx_pos_cmp would order before the equal whole frame. */
static void test_pos_arith(void)
{
    RXPos a = { .i = 10, .f = 0.3 }, b = { .i = 3, .f = nextafter(0.3, 1.0) };
    RXPos r = rx_pos_sub(a, b);
    CHECK(r.f >= 0.0 && r.f < 1.0, "fraction %.17g", r.f);
    CHECK(rx_pos_cmp(r, (RXPos) { .i = 7 }) == 0, "7 - 2^-54 rounds to frame 7: %lld + %.17g",
          (long long)r.i, r.f);

    RXPos c = { .i = 5, .f = 0.25 };
    rx_pos_add(&c, 2.75);
    CHECK(c.i == 8 && c.f == 0.0, "add carries: %lld + %f", (long long)c.i, c.f);
    RXPos d = rx_pos_sub((RXPos) { .i = 8, .f = 0.25 }, (RXPos) { .i = 2, .f = 0.5 });
    CHECK(d.i == 5 && d.f == 0.75, "sub borrows: %lld + %f", (long long)d.i, d.f);
}

static void test_pause_resume(void)
{
    double ratio = (double)RATE / 48000;
    RXPlayer *p = make_player(48000);
    rx_deck_load(p, 0, make_track(2, SECONDS), NULL);

    rx_deck_play(p, 0);
    rx_player_null_advance(p, 10000);
    rx_deck_pause(p, 0);
    CHECK(!st(p).playing, "paused at once");
    rx_player_null_advance(p, PERIOD);
    double paused = st(p).position;
    CHECK(fabs(paused * RATE - 10000 * ratio) < 1.0, "paused at %f", paused * RATE);
    rx_player_null_advance(p, 20000);
    CHECK(st(p).position == paused, "a paused deck does not move");

    rx_deck_play(p, 0);
    rx_player_null_advance(p, 30000);
    RXDeckState s = st(p);
    CHECK(fabs(s.position * RATE - 40000 * ratio) < 1.0, "resumed: %f, want %f",
          s.position * RATE, 40000 * ratio);
    rx_player_destroy(p);
}

static void test_tempo_change(void)
{
    double ratio = (double)RATE / 48000;
    RXPlayer *p = make_player(48000);
    rx_deck_load(p, 0, make_track(2, SECONDS), NULL);
    rx_deck_play(p, 0);
    rx_player_null_advance(p, 48000);
    rx_deck_set_tempo(p, 0, 1.08);
    rx_player_null_advance(p, 48000 * 3);
    double want = 48000 * ratio + 48000 * 3 * ratio * 1.08;
    CHECK(fabs(st(p).position * RATE - want) < 1.0, "%f, want %f", st(p).position * RATE, want);

    /* Bend adds to the ratio while held. */
    rx_deck_set_tempo(p, 0, 1.0);
    rx_deck_set_bend(p, 0, 0.02);
    rx_player_null_advance(p, 48000);
    rx_deck_set_bend(p, 0, 0.0);
    rx_player_null_advance(p, 48000);
    want += 48000 * ratio * 1.02 + 48000 * ratio;
    CHECK(fabs(st(p).position * RATE - want) < 1.0, "bend: %f, want %f", st(p).position * RATE,
          want);

    CHECK(!rx_deck_set_master_tempo(p, 0, true), "master tempo is not offered");
    rx_player_destroy(p);
}

static void test_end(void)
{
    RXPlayer *p = make_player(48000);
    rx_deck_load(p, 0, make_track(2, SECONDS), NULL);
    rx_deck_play_from(p, 0, SECONDS - 0.5);
    rx_player_null_advance(p, 48000);
    RXDeckState s = st(p);
    CHECK(!s.playing, "stops at the end");
    CHECK(s.position == SECONDS, "at the end: %f", s.position);
    rx_deck_play(p, 0);
    rx_player_null_advance(p, PERIOD);
    CHECK(!st(p).playing, "PLAY at the end does nothing");
    rx_player_destroy(p);
}

static void test_release(void)
{
    nb_released = 0;
    RXPlayer *p = make_player(48000);
    RXAudio *a = make_track(2, 10.0), *b = make_track(1, 10.0);

    CHECK(rx_deck_load(p, RX_DECKS, a, NULL) == RX_ERROR(EINVAL), "deck out of range");
    CHECK(rx_deck_load(p, 0, a, NULL) == 0, "load a");
    CHECK(rx_deck_audio(p, 0) == a, "a is the deck's buffer");
    rx_deck_play(p, 0);
    rx_player_null_advance(p, PERIOD);

    /* The swap is queued, not applied: a may still be read. */
    CHECK(rx_deck_load(p, 0, b, NULL) == 0, "load b");
    st(p);
    CHECK(nb_released == 0, "nothing released before the acknowledgement");
    CHECK(rx_deck_audio(p, 0) == b && !st(p).playing && st(p).position == 0.0,
          "the owner sees b at once");

    rx_player_null_advance(p, PERIOD);
    st(p);
    CHECK(nb_released == 1 && released[0] == a, "a released after the acknowledgement");

    rx_deck_unload(p, 0);
    CHECK(!rx_deck_loaded(p, 0) && !rx_deck_audio(p, 0) && !st(p).loaded, "unloaded at once");
    CHECK(nb_released == 1, "b still held");
    rx_player_null_advance(p, PERIOD);
    st(p);
    CHECK(nb_released == 2 && released[1] == b, "b released");

    /* Destroying with a buffer loaded and one pending releases both. */
    rx_deck_load(p, 0, make_track(2, 1.0), NULL);
    rx_deck_load(p, 0, make_track(2, 1.0), NULL);
    rx_player_destroy(p);
    CHECK(nb_released == 4, "destroy releases everything: %d", nb_released);
}

/* The swap while voices are still sounding: a de-click tail and scrub
 * grains are in flight when B replaces A. Voices keep positions, not
 * buffers, so once A is released (and freed by on_release) nothing reads
 * it, which ASan checks; and they stop at the swap rather than play A's
 * positions out of B, which is a constant here so any of them would be
 * heard. */
static void test_release_voices(void)
{
    nb_released = 0;
    RXPlayer *p = make_player(48000);
    RXAudio *a = make_track(2, 10.0);
    rx_deck_load(p, 0, a, NULL);
    rx_deck_play_from(p, 0, 1.0);
    rx_player_null_advance(p, PERIOD);

    rx_deck_scrub_begin(p, 0);
    rx_deck_scrub_to(p, 0, 2.0);
    render(p, NULL, 100);

    RXAudio *b = make_track(2, 5.0);
    for (int64_t i = 0; i < 2 * b->nb_frames; i++)
        b->samples[i] = 0.5f;
    CHECK(rx_deck_load(p, 0, b, NULL) == 0, "load b");
    render(p, NULL, 1);
    st(p);
    CHECK(nb_released == 1 && released[0] == a, "a released after the swap");

    static float buf[2 * 4800];
    render(p, buf, 4800);
    float peak = 0.0f;
    for (int n = 0; n < 2 * 4800; n++)
        peak = fmaxf(peak, fabsf(buf[n]));
    CHECK(peak == 0.0f, "nothing of a is heard: %f", peak);
    CHECK(!st(p).scrubbing && !st(p).playing, "b waits");
    rx_player_destroy(p);
}

static void unmap_release(void *opaque, RXAudio *audio)
{
    munmap(audio->samples, (size_t)audio->nb_frames * audio->channels * sizeof(float));
    free(audio);
}

/* A track longer than 2^31 frames (12.4 hours at 48 kHz): positions, a
 * seek, a loop near the end and the end itself, past any 32-bit frame
 * count. The samples are a sparse mapping that reads as zeros. */
static void test_long_track(void)
{
    int64_t n = ((int64_t)1 << 31) + 30 * 48000;
    size_t bytes = (size_t)n * sizeof(float);
    float *samples = mmap(NULL, bytes, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE,
                          -1, 0);
    if (samples == MAP_FAILED) {
        printf("skipping the long track: no address space\n");
        return;
    }
    RXAudio *a = calloc(1, sizeof(*a));
    if (!a)
        abort();
    *a = (RXAudio) {
        .samples   = samples,
        .channels  = 1,
        .rate      = 48000,
        .nb_frames = n,
    };

    RXPlayerOptions opt;
    rx_player_options_default(&opt);
    opt.backend = RX_AUDIO_NULL;
    opt.rate = 44100;
    opt.period = -PERIOD;
    opt.release = unmap_release;
    RXPlayer *p = NULL;
    if (rx_player_create(&p, &opt) < 0)
        abort();
    CHECK(rx_deck_load(p, 0, a, NULL) == 0, "load");

    double dur = (double)n / 48000;
    double inc = 48000.0 / 44100;
    rx_deck_play_from(p, 0, dur - 20.0);
    rx_player_null_advance(p, 44100);
    RXDeckState s = st(p);
    double want = (dur - 20.0) * 48000 + 44100 * inc;
    CHECK(fabs(s.position * 48000 - want) < 1e-3, "%f frames, want %f", s.position * 48000, want);
    CHECK(s.frame == (int64_t)floor(want) && s.frame > INT32_MAX, "frame %lld",
          (long long)s.frame);

    /* A loop of 1.5 beats at 128 BPM, 10 s before the end, wraps on its
     * arithmetic as it does at the start of a track. */
    double in = dur - 10.0, out = in + 1.5 * 60.0 / 128.0;
    rx_deck_set_loop(p, 0, in, out, true);
    rx_deck_play_from(p, 0, in);
    double start = in * 48000, len = (out - in) * 48000;
    for (int i = 1; i <= 20; i++) {
        rx_player_null_advance(p, 4 * PERIOD + 17);
        double t = start + fmod(i * (4 * PERIOD + 17) * inc, len);
        s = st(p);
        CHECK(fabs(s.position * 48000 - t) < 1e-3, "loop: %f, want %f", s.position * 48000, t);
    }

    rx_deck_clear_loop(p, 0);
    rx_deck_play_from(p, 0, dur - 0.5);
    rx_player_null_advance(p, 44100);
    s = st(p);
    CHECK(!s.playing && s.frame == n && s.position == dur, "stops at the end: %lld",
          (long long)s.frame);
    rx_player_destroy(p);
}

/* A drag: the deck follows the pointer, grains sound while it moves,
 * silence when it stops, and on release it resumes as it was. */
static void test_scrub(void)
{
    RXPlayer *p = make_player(48000);
    rx_deck_load(p, 0, make_track(2, SECONDS), NULL);
    rx_deck_play_from(p, 0, 5.0);
    rx_player_null_advance(p, 4800);

    rx_deck_scrub_begin(p, 0);
    RXDeckState s = st(p);
    CHECK(s.scrubbing && !s.playing, "scrubbing");

    static float buf[2 * 48000];
    double heard = 0.0;
    bool finite = true;
    for (int i = 0; i < 50; i++) {
        double t = 6.0 + i * 0.02;
        rx_deck_scrub_to(p, 0, t);
        CHECK(fabs(st(p).position - t) < 1e-9, "follows the pointer");
        render(p, buf, 800);
        for (int n = 0; n < 2 * 800; n++) {
            heard = fmax(heard, fabsf(buf[n]));
            finite &= isfinite(buf[n]);
        }
    }
    CHECK(finite, "no NaN or inf");
    CHECK(heard > 0.01, "grains are heard: %f", heard);
    CHECK(heard < 0.8, "and quieter than the track: %f", heard);

    /* A still pointer falls silent once the last grains end. */
    render(p, buf, 48000 / 5);
    render(p, buf, 4800);
    double still = 0.0;
    for (int n = 0; n < 2 * 4800; n++)
        still = fmax(still, fabsf(buf[n]));
    CHECK(still == 0.0, "silent when still: %f", still);

    rx_deck_scrub_end(p, 0);
    s = st(p);
    CHECK(!s.scrubbing && s.playing, "resumes: it was playing");
    CHECK(fabs(s.position - 6.98) < 1e-9, "from the drag position: %f", s.position);
    rx_player_null_advance(p, PERIOD);
    CHECK(st(p).playing && st(p).position > 6.98, "playing on");

    rx_deck_pause(p, 0);
    rx_deck_scrub_begin(p, 0);
    rx_deck_scrub_to(p, 0, 2.0);
    rx_deck_scrub_end(p, 0);
    rx_player_null_advance(p, PERIOD);
    s = st(p);
    CHECK(!s.playing && s.position == 2.0, "stays paused: it was paused");
    rx_player_destroy(p);
}

static void test_levels_and_limits(void)
{
    RXPlayer *p = make_player(RATE);
    RXAudio *a = make_track(1, 2.0);
    for (int64_t i = 0; i < a->nb_frames; i++)
        a->samples[i] = (i & 1) ? 1.0f : -1.0f;
    rx_deck_load(p, 0, a, NULL);
    rx_deck_play(p, 0);

    static float buf[2 * RATE / 2];
    render(p, buf, RATE / 2);
    float peak = 0.0f;
    bool mono = true;
    for (int n = 0; n < RATE / 2; n++) {
        peak = fmaxf(peak, fabsf(buf[2 * n]));
        mono &= buf[2 * n] == buf[2 * n + 1];
    }
    CHECK(mono, "a mono track plays on both sides");
    CHECK(peak < 1.0f && peak > 0.8f, "limited below full scale: %f", peak);
    RXDeckState s = st(p);
    CHECK(s.level_l > 0.9f && s.level_r > 0.9f, "meters: %f %f", s.level_l, s.level_r);

    rx_deck_set_volume(p, 0, 0.5f);
    render(p, buf, RATE / 10);
    render(p, buf, PERIOD);
    peak = 0.0f;
    for (int n = 0; n < 2 * PERIOD; n++)
        peak = fmaxf(peak, fabsf(buf[n]));
    CHECK(fabsf(peak - 0.5f) < 1e-6f, "deck volume: %f", peak);

    rx_player_set_master_volume(p, 0.0f);
    render(p, buf, PERIOD);
    render(p, buf, PERIOD);
    peak = 0.0f;
    for (int n = 0; n < 2 * PERIOD; n++)
        peak = fmaxf(peak, fabsf(buf[n]));
    CHECK(peak == 0.0f, "master volume 0: %f", peak);

    /* The deck meters are the channel's, before the master: they fall
     * back once the deck stops. */
    CHECK(st(p).level_l > 0.45f, "deck meter before the master: %f", st(p).level_l);
    rx_deck_pause(p, 0);
    render(p, buf, RATE / 2);
    s = st(p);
    CHECK(s.level_l < 0.25f, "meter released: %f", s.level_l);
    rx_player_destroy(p);
}

/* The ring drops commands rather than block when nothing consumes them,
 * and the owner's view keeps to what was accepted. */
static void test_ring_full(void)
{
    RXPlayer *p = make_player(48000);
    rx_deck_load(p, 0, make_track(2, SECONDS), NULL);
    enum RXLogLevel level = rx_log_level();
    rx_log_set_level(RX_LOG_ERROR);
    for (int i = 0; i < RX_CMD_RING + 10; i++)
        rx_deck_seek(p, 0, 1.0 + i * 0.001);
    rx_log_set_level(level);
    double owner = st(p).position;
    CHECK(owner < 1.0 + RX_CMD_RING * 0.001, "the dropped seeks are not reported: %f", owner);
    rx_player_null_advance(p, PERIOD);
    CHECK(st(p).position == owner, "the last accepted seek is where the deck is");
    rx_player_destroy(p);
}

static void sleep_ms(int ms)
{
    struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000 };
    nanosleep(&ts, NULL);
}

/* The null backend with its own thread: the clock runs in real time, the
 * position between callbacks is extrapolated, and buffers are released
 * across threads. Loose bounds: this runs on a loaded machine too. */
static void test_threaded(void)
{
    nb_released = 0;
    RXPlayerOptions opt;
    rx_player_options_default(&opt);
    opt.backend = RX_AUDIO_NULL;
    opt.rate = 48000;
    opt.release = on_release;
    RXPlayer *p = NULL;
    CHECK(rx_player_create(&p, &opt) == 0 && p, "create");
    if (!p)
        return;
    CHECK(rx_player_running(p), "running");

    RXAudio *a = make_track(2, SECONDS);
    rx_deck_load(p, 0, a, NULL);
    rx_deck_play_from(p, 0, 10.0);
    sleep_ms(300);
    RXDeckState s = st(p);
    CHECK(s.playing && s.position > 10.1 && s.position < 11.0, "plays in real time: %f",
          s.position);

    /* Extrapolated between callbacks: never backwards, finer than one. */
    double last = s.position;
    int steps = 0;
    for (int i = 0; i < 200; i++) {
        double t = st(p).position;
        CHECK(t >= last, "monotonic: %f after %f", t, last);
        steps += t > last;
        last = t;
        sleep_ms(1);
    }
    CHECK(steps > 100, "smooth: %d distinct positions", steps);

    rx_deck_set_loop(p, 0, 20.0, 20.5, true);
    rx_deck_seek(p, 0, 20.0);
    for (int i = 0; i < 100; i++) {
        double t = st(p).position;
        CHECK(t >= 20.0 && t <= 20.5, "inside the loop: %f", t);
        sleep_ms(5);
    }

    rx_deck_unload(p, 0);
    for (int i = 0; i < 200 && nb_released == 0; i++) {
        sleep_ms(5);
        st(p);
    }
    CHECK(nb_released == 1 && released[0] == a, "released from the other thread");
    rx_player_null_advance(p, 1000);
    rx_player_destroy(p);
}

int main(void)
{
    test_positions();
    test_seek();
    test_loop_exact();
    test_loop_fractional();
    test_loop_fold();
    test_loop_at_end();
    test_pos_arith();
    test_pause_resume();
    test_tempo_change();
    test_end();
    test_release();
    test_release_voices();
    test_long_track();
    test_scrub();
    test_levels_and_limits();
    test_ring_full();
    test_threaded();

    if (failures) {
        fprintf(stderr, "%d failures\n", failures);
        return 1;
    }
    printf("player engine: all tests passed\n");
    return 0;
}
