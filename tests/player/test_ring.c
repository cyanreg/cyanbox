/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* The ring backend (RX_AUDIO_RING) in real time, as LINK CUE takes it:
 * 336 frames every 7 ms come back as the deck's audio in 16 bits and the
 * deck moves by what was taken; taken at half that pace it moves at half
 * speed (the taker's clock paces the decks); with no one taking it runs
 * on the clock; paused, what is taken is silence. About four seconds. */

#include "rxbox/player.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "rxbox/common.h"

#define RATE 48000

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

static void on_release(void *opaque, RXAudio *audio)
{
    free(audio->samples);
    free(audio);
}

/* A minute of a constant level, left 0.5 and right -0.25. */
static RXAudio *make_track(void)
{
    RXAudio *a = calloc(1, sizeof(*a));
    if (!a)
        abort();
    a->rate = RATE;
    a->channels = 2;
    a->nb_frames = 60 * RATE;
    a->samples = malloc((size_t)a->nb_frames * 2 * sizeof(float));
    if (!a->samples)
        abort();
    for (int64_t i = 0; i < a->nb_frames; i++) {
        a->samples[i * 2] = 0.5f;
        a->samples[i * 2 + 1] = -0.25f;
    }
    return a;
}

static double position(RXPlayer *p)
{
    RXDeckState s;
    rx_deck_state(p, 0, &s);
    return s.position;
}

static void sleep_us(int64_t us)
{
    struct timespec ts = { .tv_sec = us / 1000000, .tv_nsec = (us % 1000000) * 1000 };
    nanosleep(&ts, NULL);
}

/* Takes 336 frames every `every_us` for `seconds`: how many frames came
 * back, and how many of those were the track's level (left 0.5, right
 * -0.25, in 16 bits). */
static int64_t take_for(RXPlayer *p, int64_t every_us, double seconds, int64_t *level)
{
    static int16_t buf[336 * 2];
    int64_t got = 0, end = rx_time_us() + (int64_t)(seconds * 1e6), next = rx_time_us();
    *level = 0;
    while (rx_time_us() < end) {
        int n = rx_player_ring_take(p, buf, 336);
        got += n;
        for (int i = 0; i < n; i++)
            *level += abs(buf[i * 2] - 16384) <= 2 && abs(buf[i * 2 + 1] + 8192) <= 2;
        next += every_us;
        int64_t wait = next - rx_time_us();
        if (wait > 0)
            sleep_us(wait);
    }
    return got;
}

int main(void)
{
    RXPlayerOptions opt;
    rx_player_options_default(&opt);
    opt.backend = RX_AUDIO_RING;
    opt.rate = RATE;
    opt.release = on_release;
    RXPlayer *p = NULL;
    int ret = rx_player_create(&p, &opt);
    CHECK(ret == 0 && p, "create: %d", ret);
    if (!p)
        return 1;
    CHECK(rx_player_running(p) && rx_player_backend(p) == RX_AUDIO_RING, "running");
    CHECK(rx_deck_load(p, 0, make_track(), NULL) == 0, "load");
    rx_deck_play(p, 0);
    sleep_us(100000);

    /* 7 ms at a time, as a mixer asks: the deck's audio, and the deck moved
     * by what was taken, a ring's worth (21 ms) or so more. */
    double from = position(p);
    int64_t level = 0;
    int64_t got = take_for(p, 7000, 1.0, &level);
    double moved = position(p) - from;
    CHECK(got > 0.9 * RATE && level == got, "taken %lld frames, %lld at the level",
          (long long)got, (long long)level);
    CHECK(fabs(moved - (double)got / RATE) < 0.06, "moved %.3f s for %.3f s taken", moved,
          (double)got / RATE);

    /* Taken at half that pace: the deck at half speed. */
    sleep_us(100000);
    from = position(p);
    got = take_for(p, 14000, 1.0, &level);
    moved = position(p) - from;
    CHECK(moved > 0.4 && moved < 0.6 && fabs(moved - (double)got / RATE) < 0.06,
          "half pace: moved %.3f s for %.3f s taken", moved, (double)got / RATE);

    /* No one taking: the clock. */
    from = position(p);
    sleep_us(500000);
    moved = position(p) - from;
    CHECK(moved > 0.4 && moved < 0.6, "untaken: moved %.3f s in 0.5 s", moved);

    /* Paused: silence. */
    rx_deck_pause(p, 0);
    sleep_us(100000);
    got = take_for(p, 7000, 0.3, &level);
    CHECK(got > 0 && level == 0, "paused: %lld frames, %lld at the level", (long long)got,
          (long long)level);

    rx_player_destroy(p);
    if (failures)
        fprintf(stderr, "test_ring: %d failures\n", failures);
    else
        printf("test_ring: ok\n");
    return failures ? 1 : 0;
}
