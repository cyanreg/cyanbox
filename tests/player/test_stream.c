/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* A deck playing a packet store (cyanbox/player.h, src/player/feed.c) on the
 * null backend in manual mode, with its feed thread decoding for real.
 *
 * The track: 60 s at 44.1 kHz, a WAV written here and, when the ffmpeg CLI
 * is there, the same as FLAC (the store's two routes: libavformat's
 * packets and cyanbox's FLAC frame walk). The device runs at the track's
 * rate and the deck at tempo 1.0, where the interpolator returns the
 * track's samples themselves, so every output frame can be checked
 * against cb_decode's at the place the deck should be playing.
 *
 * "Settled" below means the test waited for the feed to have decoded
 * everything around the playhead before a callback (what
 * cb_player_null_advance does); without it, a callback finds whatever the
 * feed has done so far, as on a real device.
 *
 * Checked: the window refilled while 30 s play through it; a seek far
 * away holds, silent, and plays on from there once decoded; a loop
 * shorter than the window behind wraps sample-exactly; a long loop and a
 * hot cue jump are served at once from the cue caches, unsettled; the
 * real-time side allocates nothing (a counting malloc around the mix)
 * through seeks, holds, loops, scrubs and MASTER TEMPO; and a track
 * queued to follow plays on from the frame after the last, sample-exact
 * on both sides of the change. */

#include "cyanbox/player.h"

#include <math.h>
#include <spawn.h>
#include <stdio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "src/player/player_internal.h"

#define RATE    44100
#define SECONDS 60
#define PERIOD  256

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

/* ------------------------------------------------- counting allocations */

/* The mix runs on the test's thread; the feed's allocations, on its own,
 * are not counted. AddressSanitizer has its own malloc: no counting there. */
static _Thread_local bool counting;
static int allocations;

#ifndef __SANITIZE_ADDRESS__
extern void *__libc_malloc(size_t size);
extern void *__libc_calloc(size_t n, size_t size);
extern void *__libc_realloc(void *p, size_t size);
extern void *__libc_memalign(size_t align, size_t size);

void *malloc(size_t size)
{
    if (counting)
        allocations++;
    return __libc_malloc(size);
}

void *calloc(size_t n, size_t size)
{
    if (counting)
        allocations++;
    return __libc_calloc(n, size);
}

void *realloc(void *p, size_t size)
{
    if (counting)
        allocations++;
    return __libc_realloc(p, size);
}

void *aligned_alloc(size_t align, size_t size)
{
    if (counting)
        allocations++;
    return __libc_memalign(align, size);
}
#endif

/* ---------------------------------------------------------------- media */

extern char **environ;

/* A stereo 16-bit WAV of n frames at RATE, its header written. */
static FILE *wav_open(const char *path, int64_t n)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return NULL;
    uint32_t data = (uint32_t)(n * 4);
    uint8_t h[44] = "RIFF....WAVEfmt \x10\0\0\0\x01\0\x02\0....\0\0\0\0\x04\0\x10\0data....";
    uint32_t v[] = { 36 + data, RATE, RATE * 4, data };
    memcpy(h + 4, &v[0], 4);
    memcpy(h + 24, &v[1], 4);
    memcpy(h + 28, &v[2], 4);
    memcpy(h + 40, &v[3], 4);
    fwrite(h, 1, 44, f);
    return f;
}

/* A 60 s stereo WAV, 16-bit: a ramp on the left, its negative on the right,
 * each frame different from its neighbours, all well below the limiter. */
static int write_wav(const char *path)
{
    int64_t n = (int64_t)RATE * SECONDS;
    FILE *f = wav_open(path, n);
    if (!f)
        return -1;
    for (int64_t i = 0; i < n; i++) {
        int16_t s[2];
        s[0] = (int16_t)((i * 7) % 20000 - 10000);
        s[1] = (int16_t)-s[0];
        fwrite(s, 2, 2, f);
    }
    return fclose(f) ? -1 : 0;
}

/* Frames [from, from + n) of one signal: a 441 Hz cosine at half scale on
 * the left, its sine on the right, so frame RATE (a whole number of
 * cycles) is a crest of the left. */
static int write_cos_wav(const char *path, int64_t from, int64_t n)
{
    FILE *f = wav_open(path, n);
    if (!f)
        return -1;
    for (int64_t i = 0; i < n; i++) {
        double ph = 2.0 * M_PI * 441.0 * (double)(from + i) / RATE;
        int16_t s[2] = { (int16_t)lrint(16384.0 * cos(ph)), (int16_t)lrint(16384.0 * sin(ph)) };
        fwrite(s, 2, 2, f);
    }
    return fclose(f) ? -1 : 0;
}

static bool to_flac(const char *wav, const char *flac)
{
    const char *argv[] = { "ffmpeg", "-v", "error", "-nostdin", "-y", "-i", wav, "-c:a", "flac",
                           flac, NULL };
    pid_t pid;
    if (posix_spawnp(&pid, argv[0], NULL, NULL, (char *const *)argv, environ))
        return false;
    int status;
    return waitpid(pid, &status, 0) >= 0 && WIFEXITED(status) && !WEXITSTATUS(status);
}

/* -------------------------------------------------------------- helpers */

static CBAudio ref;

static CBPlayer *make_player_at(int rate)
{
    CBPlayerOptions opt;
    cb_player_options_default(&opt);
    opt.backend = CB_AUDIO_NULL;
    opt.rate = rate;
    opt.period = -PERIOD;
    CBPlayer *p = NULL;
    int ret = cb_player_create(&p, &opt);
    CHECK(ret == 0 && p, "create: %d", ret);
    if (!p)
        exit(1);
    return p;
}

static CBPlayer *make_player(void)
{
    return make_player_at(RATE);
}

static CBPlayer *load_at(const char *path, int rate)
{
    CBPlayer *p = make_player_at(rate);
    CBPackets *pk = NULL;
    int ret = cb_packets_open(path, &pk);
    CHECK(ret >= 0, "open %s: %d", path, ret);
    if (ret < 0)
        exit(1);
    ret = cb_deck_load_packets(p, 0, pk, NULL);
    CHECK(ret == 0, "load: %d", ret);
    return p;
}

static CBPlayer *load(const char *path)
{
    return load_at(path, RATE);
}

static CBDeckState st(CBPlayer *p)
{
    CBDeckState s;
    cb_deck_state(p, 0, &s);
    return s;
}

/* The commands applied (and the owner's side of a change of track they
 * made, which tells the feed), then everything around the playhead
 * decoded. */
static void settle(CBPlayer *p)
{
    cb_player_mix(p, NULL, 0);
    st(p);
    CHECK(cb_player_feed_wait(&p->feed[0], p->rt[0].src.stream, 10000000), "the feed is stuck");
}

/* Renders n frames in callbacks, settling before each when asked. */
static void render(CBPlayer *p, float *out, int n, bool settled)
{
    static float scratch[2 * PERIOD];
    while (n > 0) {
        int k = CB_MIN(n, PERIOD);
        if (settled)
            settle(p);
        cb_player_mix(p, out ? out : scratch, k);
        if (out)
            out += 2 * k;
        n -= k;
    }
}

/* How many of n output frames differ from the track from frame `at` on. */
static int64_t mismatches(const float *out, int64_t n, int64_t at)
{
    int64_t bad = 0;
    for (int64_t i = 0; i < n; i++) {
        int64_t f = at + i;
        bad += out[2 * i] != ref.samples[2 * f] || out[2 * i + 1] != ref.samples[2 * f + 1];
    }
    return bad;
}

static bool silent(const float *out, int64_t n)
{
    for (int64_t i = 0; i < 2 * n; i++)
        if (out[i] != 0.0f)
            return false;
    return true;
}

/* ---------------------------------------------------------------- tests */

static void test_play_through(const char *path)
{
    CBPlayer *p = load(path);
    cb_deck_play(p, 0);
    int n = RATE * 30, fade = (int)(RATE * CB_FADE_SECONDS);
    float *out = malloc(sizeof(float) * 2 * n);
    render(p, out, n, true);
    /* after the start's fade in */
    int64_t bad = mismatches(out + 2 * fade, n - fade, fade);
    CHECK(bad == 0, "%s: 30 s through the window: %lld frames differ", path, (long long)bad);
    CBDeckState s = st(p);
    CHECK(s.playing && !s.waiting && s.frame == n, "%s: at %lld after 30 s", path,
          (long long)s.frame);
    CHECK(fabs(s.duration - SECONDS) < 1e-9, "%s: duration %f", path, s.duration);

    /* What the window holds: no more than its span, however long the
     * track. */
    const CBStream *sm = p->rt[0].src.stream;
    int held = 0;
    for (int i = 0; i < sm->nb_slots; i++)
        held += atomic_load(&sm->slots[i].first) >= 0;
    CHECK(held <= sm->nb_slots && (int64_t)held * CB_CHUNK <=
          (int64_t)((CB_WINDOW_BEHIND + CB_WINDOW_AHEAD) * RATE) + 3 * CB_CHUNK,
          "%s: %d chunks decoded", path, held);
    free(out);
    cb_player_destroy(p);
}

static void test_far_seek(const char *path)
{
    CBPlayer *p = load(path);
    cb_deck_play(p, 0);
    render(p, NULL, RATE, true);

    /* A seek tells the feed at once, which usually has the target decoded
     * before the next callback: whether it does or the deck waits a
     * callback, the deck plays on from exactly 50 s, after the fade in.
     * The callbacks here run at the pace of a device, so the feed is given
     * the time it has on one, whatever else the machine does. */
    int fade = (int)(RATE * CB_FADE_SECONDS);
    cb_deck_seek(p, 0, 50.0);
    int n = RATE;
    float *x = malloc(sizeof(float) * 2 * n);
    for (int done = 0; done < n; done += PERIOD) {
        cb_player_mix(p, x + 2 * done, CB_MIN(n - done, PERIOD));
        usleep(PERIOD * 1000000 / RATE);
    }
    int64_t lost = 50 * RATE + n - st(p).frame;
    CHECK(lost >= 0 && lost < RATE / 4, "%s: %lld frames late after the seek", path,
          (long long)lost);
    render(p, NULL, PERIOD, true);
    int64_t from = st(p).frame;
    render(p, x, n, true);
    CHECK(mismatches(x, n, from) == 0, "%s: after the seek, from %lld", path, (long long)from);

    /* A loop wrapping back to where nothing is decoded (no cue point, no
     * seek told the feed): the deck holds there, silent, its position kept,
     * then plays on from there, fading in. The feed is kept from its work
     * (its lock held: it blocks on it when woken) while the callbacks run,
     * so the wrap always finds the frames missing, as on a machine where
     * the feed is a callback late. */
    cb_deck_play_from(p, 0, 40.0);
    render(p, NULL, PERIOD, true);
    cb_deck_set_loop(p, 0, 10.0, 40.0 + (double)PERIOD * 2 / RATE, true);
    int64_t at = st(p).frame;
    float out[2 * 4 * PERIOD];
    pthread_mutex_lock(&p->feed[0].lock);
    render(p, out, 4 * PERIOD, false);
    int64_t wrap = 40 * RATE + 2 * PERIOD - at;
    CBDeckState s = st(p);
    CHECK(s.playing && s.waiting && s.frame == 10 * RATE, "%s: after the wrap at %lld, waiting %d",
          path, (long long)s.frame, s.waiting);
    CHECK(mismatches(out, wrap, at) == 0, "%s: before the wrap", path);
    CHECK(silent(out + 2 * wrap, 4 * PERIOD - wrap), "%s: sound while holding", path);
    pthread_mutex_unlock(&p->feed[0].lock);
    settle(p);
    render(p, x, n, true);
    CHECK(mismatches(x + 2 * fade, n - fade, 10 * RATE + fade) == 0, "%s: on from the wrap", path);
    CHECK(!st(p).waiting && st(p).frame == 10 * RATE + n, "%s: at %lld", path,
          (long long)st(p).frame);
    free(x);
    cb_player_destroy(p);
}

static void test_loops(const char *path)
{
    CBPlayer *p = load(path);

    /* Two seconds, wrapping inside the window behind: sample-exact */
    cb_deck_play_from(p, 0, 11.0);
    cb_deck_set_loop(p, 0, 10.0, 12.0, true);
    render(p, NULL, PERIOD, true);
    int64_t at = st(p).frame;
    int n = 3 * RATE;
    float *x = malloc(sizeof(float) * 2 * n);
    render(p, x, n, true);
    int64_t bad = 0;
    for (int64_t i = 0; i < n; i++) {
        int64_t f = at + i;
        while (f >= 12 * RATE)
            f -= 2 * RATE;
        bad += mismatches(x + 2 * i, 1, f);
    }
    CHECK(bad == 0, "%s: a 2 s loop: %lld frames differ", path, (long long)bad);

    /* Twenty seconds: the in point has left the window when the deck
     * wraps, and the cue cache there plays it while the feed catches up.
     * Nothing waits for the feed from a second before the wrap on. */
    cb_deck_clear_loop(p, 0);
    double points[] = { 0.0, 5.0 };
    cb_deck_set_cue_points(p, 0, points, 2);
    cb_deck_play_from(p, 0, 24.0);
    cb_deck_set_loop(p, 0, 5.0, 25.0, true);
    render(p, NULL, PERIOD, true);
    at = st(p).frame;
    n = RATE + RATE / 4;
    render(p, x, n, false);
    bad = 0;
    for (int64_t i = 0; i < n; i++) {
        int64_t f = at + i;
        if (f >= 25 * RATE)
            f -= 20 * RATE;
        bad += mismatches(x + 2 * i, 1, f);
    }
    CHECK(bad == 0, "%s: a 20 s loop from the cache: %lld frames differ", path, (long long)bad);
    CHECK(!st(p).waiting, "%s: waiting after the wrap", path);
    free(x);
    cb_player_destroy(p);
}

static void test_hot_cue(const char *path)
{
    CBPlayer *p = load(path);
    double points[] = { 0.0, 3.0, 40.0, 55.5 };
    cb_deck_set_cue_points(p, 0, points, 4);
    cb_deck_play_from(p, 0, 20.0);
    render(p, NULL, RATE / 2, true);

    /* From the cache: plays at once, no waiting for the feed */
    int fade = (int)(RATE * CB_FADE_SECONDS);
    for (int c = 1; c < 4; c++) {
        cb_deck_play_from(p, 0, points[c]);
        int n = RATE / 4;
        float *x = malloc(sizeof(float) * 2 * n);
        render(p, x, n, false);
        int64_t from = (int64_t)floor(points[c] * RATE);
        CHECK(mismatches(x + 2 * fade, n - fade, from + fade) == 0,
              "%s: hot cue at %.1f s: %lld frames differ", path, points[c],
              (long long)mismatches(x + 2 * fade, n - fade, from + fade));
        CHECK(st(p).frame == from + n && !st(p).waiting, "%s: at %lld, want %lld", path,
              (long long)st(p).frame, (long long)(from + n));
        free(x);
    }
    cb_player_destroy(p);
}

static void test_no_allocation(const char *path)
{
    /* The counter counts (allocations in a library: the compiler may drop
     * a malloc it sees freed unused) */
    counting = true;
    char *a = cb_strdup("x"), *b = cb_asprintf("%d", 2);
    counting = false;
#ifndef __SANITIZE_ADDRESS__
    CHECK(allocations >= 2, "the counting malloc saw %d of 2", allocations);
#endif
    free(a);
    free(b);
    allocations = 0;

    CBPlayer *p = load(path);
    double points[] = { 0.0, 2.0, 30.0 };
    cb_deck_set_cue_points(p, 0, points, 3);
    cb_deck_play(p, 0);
    render(p, NULL, RATE, true);
    cb_deck_set_master_tempo(p, 0, true);
    render(p, NULL, RATE / 4, true);

    /* Everything the owner sends is queued; the callbacks run counted */
    for (int round = 0; round < 20; round++) {
        cb_deck_seek(p, 0, (round * 7919 % 58) + 0.5);         /* far: holds */
        counting = true;
        render(p, NULL, PERIOD * 4, false);
        counting = false;
        settle(p);
        cb_deck_play_from(p, 0, 30.0);                          /* the cache */
        cb_deck_set_loop(p, 0, 30.0, 30.25, true);
        cb_deck_set_tempo(p, 0, 1.0 + round * 0.01);
        counting = true;
        render(p, NULL, RATE / 2, false);
        counting = false;
        cb_deck_clear_loop(p, 0);
        cb_deck_scrub_begin(p, 0);
        cb_deck_scrub_to(p, 0, 31.0);
        cb_deck_set_master_tempo(p, 0, round & 1);
        counting = true;
        render(p, NULL, PERIOD * 8, false);
        counting = false;
        cb_deck_scrub_end(p, 0);
        settle(p);
    }
    CHECK(allocations == 0, "%s: %d allocations on the real-time side", path, allocations);
    cb_player_destroy(p);
}

/* Waits for the feed to have the first two chunks of the stream queued to
 * follow the deck's, which it decodes once the deck's has nothing left. */
static void settle_next(CBPlayer *p)
{
    static float probe[2 * 2 * CB_CHUNK];
    CBStream *s = p->own[0].next.stream;
    int64_t t0 = cb_time_us();
    while (!cb_player_stream_read(s, cb_packets_length(s->pk, NULL), 0, 2 * CB_CHUNK, probe)) {
        if (cb_time_us() - t0 > 10000000) {
            CHECK(false, "the start of the next track is not decoded");
            return;
        }
        sem_post(&p->feed[0].wake);
        usleep(50);
    }
}

/* Renders n frames in callbacks, the owner's side (cb_deck_state: a
 * change of track is noticed and the feed told) and the feed settled
 * before each. */
static void render_synced(CBPlayer *p, float *out, int64_t n)
{
    for (int64_t done = 0; done < n; done += PERIOD) {
        st(p);
        settle(p);
        cb_player_mix(p, out + 2 * done, PERIOD);
    }
}

/* The track queued to follow (cb_deck_queue_next_packets): a second copy
 * of the track, queued while the first plays its last half second, plays
 * on from the frame after the first's last, every output frame the
 * track's own on both sides of the change, with no silence; the state is
 * the next's once the change is heard, with nothing queued; the first's
 * stream is let go of. Then, stopped at the end of a track with nothing
 * queued, PLAY takes a track queued after the stop; and a queued track
 * dropped (cb_deck_clear_next) leaves the deck stopping at its end. */
static void test_next(const char *path)
{
    CBPlayer *p = load(path);
    CBPackets *pk = NULL;
    CHECK(cb_deck_queue_next_packets(p, 0, NULL, NULL) == CB_ERROR(EINVAL), "no packets");
    CHECK(cb_packets_open(path, &pk) >= 0, "open the next");
    CHECK(cb_deck_queue_next_packets(p, 0, pk, NULL) == 0, "queue the next");
    CHECK(st(p).has_next, "a next is queued");

    int64_t total = (int64_t)RATE * SECONDS;
    int64_t before = RATE / 2;                  /* of the first track */
    int64_t n = 345 * PERIOD;                   /* 2 s, in whole callbacks */
    float *out = calloc((size_t)2 * n, sizeof(float));
    if (!out)
        abort();
    cb_deck_play_from(p, 0, (double)(total - before) / RATE);
    settle(p);
    settle_next(p);
    unsigned adv0 = st(p).advanced;
    render_synced(p, out, n);
    CBDeckState s = st(p);
    CHECK(s.advanced == adv0 + 1, "moved on once: %u then %u", adv0, s.advanced);
    CHECK(s.playing && !s.has_next, "playing the next, nothing queued: %d %d", s.playing,
          s.has_next);
    CHECK(fabs(s.position * RATE - (double)(n - before)) < 0.5, "position in the next: %f s",
          s.position);
    /* Past the start's fade in. */
    CHECK(mismatches(out + 2 * PERIOD, before - PERIOD, total - before + PERIOD) == 0,
          "the first track's last frames");
    CHECK(mismatches(out + 2 * before, n - before, 0) == 0, "the next from its first frame");
    CHECK(!silent(out + 2 * (before - 64), 128), "no silence at the change");
    for (int i = 0; i < 200 && p->nb_releases; i++) {
        usleep(100);
        st(p);
    }
    CHECK(p->nb_releases == 0, "the first track's stream released: %d pending", p->nb_releases);

    /* Stopped at the end with nothing queued; PLAY takes a track queued
     * after the stop, fading in as any start does. */
    cb_deck_seek(p, 0, (double)SECONDS);
    render(p, NULL, PERIOD, true);
    s = st(p);
    CHECK(!s.playing && s.position == SECONDS, "stopped at the end: %d at %f", s.playing,
          s.position);
    CHECK(cb_packets_open(path, &pk) >= 0, "open a third");
    CHECK(cb_deck_queue_next_packets(p, 0, pk, NULL) == 0, "queue after the stop");
    settle(p);
    settle_next(p);
    adv0 = s.advanced;
    cb_deck_play(p, 0);
    render_synced(p, out, n);
    s = st(p);
    CHECK(s.advanced == adv0 + 1 && s.playing, "PLAY took the next: %u then %u, playing %d",
          adv0, s.advanced, s.playing);
    CHECK(fabs(s.position * RATE - (double)n) < 0.5, "position %f s", s.position);
    CHECK(mismatches(out + 2 * PERIOD, n - PERIOD, PERIOD) == 0, "played from its first frame");

    /* Dropped: still pending until the deck has applied the drop (it might
     * have moved on into it before), then gone; the deck stops at its
     * end. */
    CHECK(cb_packets_open(path, &pk) >= 0, "open a fourth");
    CHECK(cb_deck_queue_next_packets(p, 0, pk, NULL) == 0, "queue a fourth");
    cb_deck_clear_next(p, 0);
    CHECK(st(p).has_next, "dropped, not applied yet");
    render(p, NULL, PERIOD, true);
    CHECK(!st(p).has_next, "dropped");
    cb_deck_play_from(p, 0, SECONDS - 0.1);
    render(p, NULL, RATE / 4, true);
    s = st(p);
    CHECK(!s.playing && s.position == SECONDS && s.advanced == adv0 + 1,
          "stops at the end with nothing queued: %d at %f, advanced %u", s.playing, s.position,
          s.advanced);
    free(out);
    cb_player_destroy(p);
}

/* A track and its next cut from one signal on a crest, on a device at
 * another rate (48 kHz) and at tempos off 1.0 (1.06, 0.94): the
 * interpolator reads the next's first frames past the end of the track,
 * and the old one's last before the next's start, so the two play frame
 * for frame as the signal does as one stream. Reading silence there
 * instead put a frame pulled toward zero at every gapless change of a
 * deck that resamples: a click. */
static void test_next_junction(const char *whole, const char *first, const char *second)
{
    const int rates[] = { RATE, 48000 };
    const double tempos[] = { 1.0, 1.06, 0.94 };
    for (int ri = 0; ri < 2; ri++) {
        int rate = rates[ri];
        int n = rate / 4 / PERIOD * PERIOD;
        float *one = calloc((size_t)2 * n, sizeof(float));
        float *out = calloc((size_t)2 * n, sizeof(float));
        if (!one || !out)
            abort();
        for (int ti = 0; ti < 3; ti++) {
            double tempo = tempos[ti];
            CBPlayer *p = load_at(whole, rate);
            cb_deck_set_tempo(p, 0, tempo);
            cb_deck_play_from(p, 0, 0.9);
            render(p, one, n, true);
            cb_player_destroy(p);

            p = load_at(first, rate);
            CBPackets *pk = NULL;
            CHECK(cb_packets_open(second, &pk) >= 0, "open %s", second);
            CHECK(cb_deck_queue_next_packets(p, 0, pk, NULL) == 0, "queue %s", second);
            cb_deck_set_tempo(p, 0, tempo);
            cb_deck_play_from(p, 0, 0.9);
            settle(p);
            settle_next(p);
            render(p, out, n, true);
            CBDeckState s = st(p);
            CHECK(s.advanced == 1 && fabs(s.duration - 1.0) < 1e-9,
                  "%d Hz, tempo %.2f: moved on %u, %f s", rate, tempo, s.advanced, s.duration);

            double worst = 0.0;
            for (int k = 0; k < 2 * n; k++)
                worst = fmax(worst, fabs((double)out[k] - one[k]));
            /* The output frame whose read position crossed the cut. */
            int at = (int)ceil(0.1 * RATE / (tempo * RATE / rate));
            CHECK(worst < 1e-6, "%d Hz, tempo %.2f: %g from the signal played whole", rate, tempo,
                  worst);
            printf("stream next junction at %d Hz, tempo %.2f: %.4f (whole %.4f), largest "
                   "difference %g\n", rate, tempo, out[2 * at], one[2 * at], worst);
            cb_player_destroy(p);
        }
        free(one);
        free(out);
    }
}

/* With MASTER TEMPO on, the deck reads the shifter's delay ahead of what
 * is heard, so it reads the next while the old track is still heard. The
 * owner hands the feed the stream the deck reads as soon as the deck
 * reads it, and manual mode waits for the feed of the stream the deck
 * reads: no advance waits on a feed still filling the old one (each one in
 * that delay waited out the 10 s timeout). The move is heard once, and the
 * next plays on from its start. */
static void test_next_master_tempo(const char *path)
{
    CBPlayer *p = load(path);
    CHECK(cb_deck_set_master_tempo(p, 0, true), "master tempo");
    CBPackets *pk = NULL;
    CHECK(cb_packets_open(path, &pk) >= 0, "open the next");
    CHECK(cb_deck_queue_next_packets(p, 0, pk, NULL) == 0, "queue the next");
    cb_deck_play_from(p, 0, SECONDS - 0.5);
    unsigned adv0 = st(p).advanced;
    int slow = 0, steps = RATE / PERIOD;
    for (int i = 0; i < steps; i++) {
        int64_t t0 = cb_time_us();
        cb_player_null_advance(p, PERIOD);
        slow += cb_time_us() - t0 > 1000000;
    }
    CBDeckState s = st(p);
    CHECK(!slow, "%s: %d advances waited a second or more", path, slow);
    double want = (double)steps * PERIOD / RATE - 0.5;
    CHECK(s.advanced == adv0 + 1 && s.playing && !s.has_next && fabs(s.position - want) < 0.01,
          "%s: moved on %u times, playing %d at %f s (want %f)", path, s.advanced - adv0,
          s.playing, s.position, want);
    cb_player_destroy(p);
}

/* Loads replacing loads while the feed works, and a teardown with a
 * stream in the release list: no crash, nothing left (LeakSanitizer). */
static void test_reload(const char *path)
{
    CBPlayer *p = make_player();
    for (int i = 0; i < 5; i++) {
        CBPackets *pk = NULL;
        CHECK(cb_packets_open(path, &pk) >= 0, "open");
        CHECK(cb_deck_load_packets(p, 0, pk, NULL) == 0, "load %d", i);
        cb_deck_play_from(p, 0, i * 7.0);
        render(p, NULL, PERIOD * (i + 1), i & 1);
    }
    cb_deck_unload(p, 0);
    CHECK(!cb_deck_loaded(p, 0), "unloaded");
    render(p, NULL, PERIOD, false);
    cb_player_destroy(p);
}

int main(void)
{
    const char *root = getenv("CB_TEST_DIR");
    char *dir = cb_path_join(root ? root : "test-media", "stream");
    char *wav = dir ? cb_path_join(dir, "ramp.wav") : NULL;
    char *flac = dir ? cb_path_join(dir, "ramp.flac") : NULL;
    if (!wav || !flac || cb_mkdir_p(dir) < 0 || write_wav(wav) < 0) {
        fprintf(stderr, "cannot write the test media in %s\n", dir ? dir : "?");
        return 1;
    }
    if (cb_decode(wav, 0, 0, &ref) < 0 || ref.nb_frames != (int64_t)RATE * SECONDS) {
        fprintf(stderr, "cannot decode %s\n", wav);
        return 1;
    }

    const char *paths[2] = { wav, flac };
    int nb = to_flac(wav, flac) ? 2 : 1;
    if (nb == 1)
        fprintf(stderr, "no ffmpeg: the FLAC route is not tested\n");
    for (int i = 0; i < nb; i++) {
        test_play_through(paths[i]);
        test_far_seek(paths[i]);
        test_loops(paths[i]);
        test_hot_cue(paths[i]);
        test_no_allocation(paths[i]);
        test_next(paths[i]);
        test_next_master_tempo(paths[i]);
        test_reload(paths[i]);
    }

    /* One signal, whole and cut in two on a crest. */
    char *whole = cb_path_join(dir, "cos.wav");
    char *first = cb_path_join(dir, "cos_a.wav");
    char *second = cb_path_join(dir, "cos_n.wav");
    if (whole && first && second && write_cos_wav(whole, 0, 2 * RATE) >= 0 &&
        write_cos_wav(first, 0, RATE) >= 0 && write_cos_wav(second, RATE, RATE) >= 0)
        test_next_junction(whole, first, second);
    else
        CHECK(false, "cannot write the cosine in %s", dir);

    cb_audio_free(&ref);
    free(whole);
    free(first);
    free(second);
    free(wav);
    free(flac);
    free(dir);
    if (failures) {
        fprintf(stderr, "%d failures\n", failures);
        return 1;
    }
    printf("player streams: all tests passed\n");
    return 0;
}
