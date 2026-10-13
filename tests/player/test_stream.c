/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* A deck playing a packet store (rxbox/player.h, src/player/feed.c) on the
 * null backend in manual mode, with its feed thread decoding for real.
 *
 * The track: 60 s at 44.1 kHz, a WAV written here and, when the ffmpeg CLI
 * is there, the same as FLAC (the store's two routes: libavformat's
 * packets and rxbox's FLAC frame walk). The device runs at the track's
 * rate and the deck at tempo 1.0, where the interpolator returns the
 * track's samples themselves, so every output frame can be checked
 * against rx_decode's at the place the deck should be playing.
 *
 * "Settled" below means the test waited for the feed to have decoded
 * everything around the playhead before a callback (what
 * rx_player_null_advance does); without it, a callback finds whatever the
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

#include "rxbox/player.h"

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

/* A 60 s stereo WAV, 16-bit: a ramp on the left, its negative on the right,
 * each frame different from its neighbours, all well below the limiter. */
static int write_wav(const char *path)
{
    int64_t n = (int64_t)RATE * SECONDS;
    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;
    uint32_t data = (uint32_t)(n * 4);
    uint8_t h[44] = "RIFF....WAVEfmt \x10\0\0\0\x01\0\x02\0....\0\0\0\0\x04\0\x10\0data....";
    uint32_t v[] = { 36 + data, RATE, RATE * 4, data };
    memcpy(h + 4, &v[0], 4);
    memcpy(h + 24, &v[1], 4);
    memcpy(h + 28, &v[2], 4);
    memcpy(h + 40, &v[3], 4);
    fwrite(h, 1, 44, f);
    for (int64_t i = 0; i < n; i++) {
        int16_t s[2];
        s[0] = (int16_t)((i * 7) % 20000 - 10000);
        s[1] = (int16_t)-s[0];
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

static RXAudio ref;

static RXPlayer *make_player(void)
{
    RXPlayerOptions opt;
    rx_player_options_default(&opt);
    opt.backend = RX_AUDIO_NULL;
    opt.rate = RATE;
    opt.period = -PERIOD;
    RXPlayer *p = NULL;
    int ret = rx_player_create(&p, &opt);
    CHECK(ret == 0 && p, "create: %d", ret);
    if (!p)
        exit(1);
    return p;
}

static RXPlayer *load(const char *path)
{
    RXPlayer *p = make_player();
    RXPackets *pk = NULL;
    int ret = rx_packets_open(path, &pk);
    CHECK(ret >= 0, "open %s: %d", path, ret);
    if (ret < 0)
        exit(1);
    ret = rx_deck_load_packets(p, 0, pk, NULL);
    CHECK(ret == 0, "load: %d", ret);
    return p;
}

static RXDeckState st(RXPlayer *p)
{
    RXDeckState s;
    rx_deck_state(p, 0, &s);
    return s;
}

/* The commands applied (and the owner's side of a change of track they
 * made, which tells the feed), then everything around the playhead
 * decoded. */
static void settle(RXPlayer *p)
{
    rx_player_mix(p, NULL, 0);
    st(p);
    CHECK(rx_player_feed_wait(&p->feed[0], p->rt[0].stream, 10000000), "the feed is stuck");
}

/* Renders n frames in callbacks, settling before each when asked. */
static void render(RXPlayer *p, float *out, int n, bool settled)
{
    static float scratch[2 * PERIOD];
    while (n > 0) {
        int k = RX_MIN(n, PERIOD);
        if (settled)
            settle(p);
        rx_player_mix(p, out ? out : scratch, k);
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
    RXPlayer *p = load(path);
    rx_deck_play(p, 0);
    int n = RATE * 30, fade = (int)(RATE * RX_FADE_SECONDS);
    float *out = malloc(sizeof(float) * 2 * n);
    render(p, out, n, true);
    /* after the start's fade in */
    int64_t bad = mismatches(out + 2 * fade, n - fade, fade);
    CHECK(bad == 0, "%s: 30 s through the window: %lld frames differ", path, (long long)bad);
    RXDeckState s = st(p);
    CHECK(s.playing && !s.waiting && s.frame == n, "%s: at %lld after 30 s", path,
          (long long)s.frame);
    CHECK(fabs(s.duration - SECONDS) < 1e-9, "%s: duration %f", path, s.duration);

    /* What the window holds: no more than its span, however long the
     * track. */
    const RXStream *sm = p->rt[0].stream;
    int held = 0;
    for (int i = 0; i < sm->nb_slots; i++)
        held += atomic_load(&sm->slots[i].first) >= 0;
    CHECK(held <= sm->nb_slots && (int64_t)held * RX_CHUNK <=
          (int64_t)((RX_WINDOW_BEHIND + RX_WINDOW_AHEAD) * RATE) + 3 * RX_CHUNK,
          "%s: %d chunks decoded", path, held);
    free(out);
    rx_player_destroy(p);
}

static void test_far_seek(const char *path)
{
    RXPlayer *p = load(path);
    rx_deck_play(p, 0);
    render(p, NULL, RATE, true);

    /* A seek tells the feed at once, which usually has the target decoded
     * before the next callback: whether it does or the deck waits a
     * callback, the deck plays on from exactly 50 s, after the fade in.
     * The callbacks here run at the pace of a device, so the feed is given
     * the time it has on one, whatever else the machine does. */
    int fade = (int)(RATE * RX_FADE_SECONDS);
    rx_deck_seek(p, 0, 50.0);
    int n = RATE;
    float *x = malloc(sizeof(float) * 2 * n);
    for (int done = 0; done < n; done += PERIOD) {
        rx_player_mix(p, x + 2 * done, RX_MIN(n - done, PERIOD));
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
    rx_deck_play_from(p, 0, 40.0);
    render(p, NULL, PERIOD, true);
    rx_deck_set_loop(p, 0, 10.0, 40.0 + (double)PERIOD * 2 / RATE, true);
    int64_t at = st(p).frame;
    float out[2 * 4 * PERIOD];
    pthread_mutex_lock(&p->feed[0].lock);
    render(p, out, 4 * PERIOD, false);
    int64_t wrap = 40 * RATE + 2 * PERIOD - at;
    RXDeckState s = st(p);
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
    rx_player_destroy(p);
}

static void test_loops(const char *path)
{
    RXPlayer *p = load(path);

    /* Two seconds, wrapping inside the window behind: sample-exact */
    rx_deck_play_from(p, 0, 11.0);
    rx_deck_set_loop(p, 0, 10.0, 12.0, true);
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
    rx_deck_clear_loop(p, 0);
    double points[] = { 0.0, 5.0 };
    rx_deck_set_cue_points(p, 0, points, 2);
    rx_deck_play_from(p, 0, 24.0);
    rx_deck_set_loop(p, 0, 5.0, 25.0, true);
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
    rx_player_destroy(p);
}

static void test_hot_cue(const char *path)
{
    RXPlayer *p = load(path);
    double points[] = { 0.0, 3.0, 40.0, 55.5 };
    rx_deck_set_cue_points(p, 0, points, 4);
    rx_deck_play_from(p, 0, 20.0);
    render(p, NULL, RATE / 2, true);

    /* From the cache: plays at once, no waiting for the feed */
    int fade = (int)(RATE * RX_FADE_SECONDS);
    for (int c = 1; c < 4; c++) {
        rx_deck_play_from(p, 0, points[c]);
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
    rx_player_destroy(p);
}

static void test_no_allocation(const char *path)
{
    /* The counter counts (allocations in a library: the compiler may drop
     * a malloc it sees freed unused) */
    counting = true;
    char *a = rx_strdup("x"), *b = rx_asprintf("%d", 2);
    counting = false;
#ifndef __SANITIZE_ADDRESS__
    CHECK(allocations >= 2, "the counting malloc saw %d of 2", allocations);
#endif
    free(a);
    free(b);
    allocations = 0;

    RXPlayer *p = load(path);
    double points[] = { 0.0, 2.0, 30.0 };
    rx_deck_set_cue_points(p, 0, points, 3);
    rx_deck_play(p, 0);
    render(p, NULL, RATE, true);
    rx_deck_set_master_tempo(p, 0, true);
    render(p, NULL, RATE / 4, true);

    /* Everything the owner sends is queued; the callbacks run counted */
    for (int round = 0; round < 20; round++) {
        rx_deck_seek(p, 0, (round * 7919 % 58) + 0.5);         /* far: holds */
        counting = true;
        render(p, NULL, PERIOD * 4, false);
        counting = false;
        settle(p);
        rx_deck_play_from(p, 0, 30.0);                          /* the cache */
        rx_deck_set_loop(p, 0, 30.0, 30.25, true);
        rx_deck_set_tempo(p, 0, 1.0 + round * 0.01);
        counting = true;
        render(p, NULL, RATE / 2, false);
        counting = false;
        rx_deck_clear_loop(p, 0);
        rx_deck_scrub_begin(p, 0);
        rx_deck_scrub_to(p, 0, 31.0);
        rx_deck_set_master_tempo(p, 0, round & 1);
        counting = true;
        render(p, NULL, PERIOD * 8, false);
        counting = false;
        rx_deck_scrub_end(p, 0);
        settle(p);
    }
    CHECK(allocations == 0, "%s: %d allocations on the real-time side", path, allocations);
    rx_player_destroy(p);
}

/* Waits for the feed to have the first two chunks of the stream queued to
 * follow the deck's, which it decodes once the deck's has nothing left. */
static void settle_next(RXPlayer *p)
{
    static float probe[2 * 2 * RX_CHUNK];
    RXStream *s = p->own[0].next_stream;
    int64_t t0 = rx_time_us();
    while (!rx_player_stream_read(s, rx_packets_length(s->pk, NULL), 0, 2 * RX_CHUNK, probe)) {
        if (rx_time_us() - t0 > 10000000) {
            CHECK(false, "the start of the next track is not decoded");
            return;
        }
        sem_post(&p->feed[0].wake);
        usleep(50);
    }
}

/* Renders n frames in callbacks, the owner's side (rx_deck_state: a
 * change of track is noticed and the feed told) and the feed settled
 * before each. */
static void render_synced(RXPlayer *p, float *out, int64_t n)
{
    for (int64_t done = 0; done < n; done += PERIOD) {
        st(p);
        settle(p);
        rx_player_mix(p, out + 2 * done, PERIOD);
    }
}

/* The track queued to follow (rx_deck_queue_next_packets): a second copy
 * of the track, queued while the first plays its last half second, plays
 * on from the frame after the first's last, every output frame the
 * track's own on both sides of the change, with no silence; the state is
 * the next's once the change is heard, with nothing queued; the first's
 * stream is let go of. Then, stopped at the end of a track with nothing
 * queued, PLAY takes a track queued after the stop; and a queued track
 * dropped (rx_deck_clear_next) leaves the deck stopping at its end. */
static void test_next(const char *path)
{
    RXPlayer *p = load(path);
    RXPackets *pk = NULL;
    CHECK(rx_deck_queue_next_packets(p, 0, NULL, NULL) == RX_ERROR(EINVAL), "no packets");
    CHECK(rx_packets_open(path, &pk) >= 0, "open the next");
    CHECK(rx_deck_queue_next_packets(p, 0, pk, NULL) == 0, "queue the next");
    CHECK(st(p).has_next, "a next is queued");

    int64_t total = (int64_t)RATE * SECONDS;
    int64_t before = RATE / 2;                  /* of the first track */
    int64_t n = 345 * PERIOD;                   /* 2 s, in whole callbacks */
    float *out = calloc((size_t)2 * n, sizeof(float));
    if (!out)
        abort();
    rx_deck_play_from(p, 0, (double)(total - before) / RATE);
    settle(p);
    settle_next(p);
    unsigned adv0 = st(p).advanced;
    render_synced(p, out, n);
    RXDeckState s = st(p);
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
    rx_deck_seek(p, 0, (double)SECONDS);
    render(p, NULL, PERIOD, true);
    s = st(p);
    CHECK(!s.playing && s.position == SECONDS, "stopped at the end: %d at %f", s.playing,
          s.position);
    CHECK(rx_packets_open(path, &pk) >= 0, "open a third");
    CHECK(rx_deck_queue_next_packets(p, 0, pk, NULL) == 0, "queue after the stop");
    settle(p);
    settle_next(p);
    adv0 = s.advanced;
    rx_deck_play(p, 0);
    render_synced(p, out, n);
    s = st(p);
    CHECK(s.advanced == adv0 + 1 && s.playing, "PLAY took the next: %u then %u, playing %d",
          adv0, s.advanced, s.playing);
    CHECK(fabs(s.position * RATE - (double)n) < 0.5, "position %f s", s.position);
    CHECK(mismatches(out + 2 * PERIOD, n - PERIOD, PERIOD) == 0, "played from its first frame");

    /* Dropped: the deck stops at its end. */
    CHECK(rx_packets_open(path, &pk) >= 0, "open a fourth");
    CHECK(rx_deck_queue_next_packets(p, 0, pk, NULL) == 0, "queue a fourth");
    rx_deck_clear_next(p, 0);
    CHECK(!st(p).has_next, "dropped");
    rx_deck_play_from(p, 0, SECONDS - 0.1);
    render(p, NULL, RATE / 4, true);
    s = st(p);
    CHECK(!s.playing && s.position == SECONDS && s.advanced == adv0 + 1,
          "stops at the end with nothing queued: %d at %f, advanced %u", s.playing, s.position,
          s.advanced);
    free(out);
    rx_player_destroy(p);
}

/* Loads replacing loads while the feed works, and a teardown with a
 * stream in the release list: no crash, nothing left (LeakSanitizer). */
static void test_reload(const char *path)
{
    RXPlayer *p = make_player();
    for (int i = 0; i < 5; i++) {
        RXPackets *pk = NULL;
        CHECK(rx_packets_open(path, &pk) >= 0, "open");
        CHECK(rx_deck_load_packets(p, 0, pk, NULL) == 0, "load %d", i);
        rx_deck_play_from(p, 0, i * 7.0);
        render(p, NULL, PERIOD * (i + 1), i & 1);
    }
    rx_deck_unload(p, 0);
    CHECK(!rx_deck_loaded(p, 0), "unloaded");
    render(p, NULL, PERIOD, false);
    rx_player_destroy(p);
}

int main(void)
{
    const char *root = getenv("RX_TEST_DIR");
    char *dir = rx_path_join(root ? root : "test-media", "stream");
    char *wav = dir ? rx_path_join(dir, "ramp.wav") : NULL;
    char *flac = dir ? rx_path_join(dir, "ramp.flac") : NULL;
    if (!wav || !flac || rx_mkdir_p(dir) < 0 || write_wav(wav) < 0) {
        fprintf(stderr, "cannot write the test media in %s\n", dir ? dir : "?");
        return 1;
    }
    if (rx_decode(wav, 0, 0, &ref) < 0 || ref.nb_frames != (int64_t)RATE * SECONDS) {
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
        test_reload(paths[i]);
    }

    rx_audio_free(&ref);
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
