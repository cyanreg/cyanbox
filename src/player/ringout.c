/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* The ring backend (CB_AUDIO_RING): the mix kept as 16-bit stereo frames
 * in a ring that another thread takes as it needs them
 * (cb_player_ring_take), never a device: with --pro-link the mixer's
 * LINK CUE, which asks for 7 ms of audio every 7 ms. libswresample turns
 * the float mix into 16 bits (triangular dither), writing into the ring
 * itself; it allocates on its first conversion, which the open makes, so
 * the mixing thread never does.
 *
 * One thread mixes, waking every period on an absolute monotonic
 * schedule as the null backend does. While someone takes (a take in the
 * last RING_IDLE_US), it mixes only while the ring holds less than
 * RING_TARGET frames: the taker's clock paces the decks, so a mixer's
 * clock and ours never drift apart, and what is taken is at most
 * RING_TARGET frames old. While no one takes, it mixes every period and
 * throws the audio away, so the decks run as on a device. */

#include "player_internal.h"

#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>

#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>

/* Frames the ring holds (a power of two), what it is kept at while taken,
 * and how long after the last take it counts as not taken: longer than a
 * mixer goes without asking while it holds enough (the DJM-A9: 58 ms as
 * its LINK CUE starts), which must neither drop what the ring kept nor
 * let the decks run on. */
#define RING_FRAMES  4096
#define RING_TARGET  1024
#define RING_IDLE_US 150000

struct CBRingOut {
    pthread_t   thread;
    bool        started;
    atomic_bool stop;
    float      *mix;
    int         period;
    SwrContext *swr;
    int16_t     ring[RING_FRAMES * 2];
    atomic_uint head, tail;         /* frames written and taken, wrapping */
    atomic_llong taken_us;          /* cb_time_us of the last take, 0 never */
};

/* n frames of the mix into the ring from frame `at`, around its end. */
static void to_ring(CBRingOut *r, unsigned at, int n)
{
    int done = 0;
    while (done < n) {
        unsigned i = (at + (unsigned)done) % RING_FRAMES;
        int k = (int)CB_MIN((unsigned)(n - done), RING_FRAMES - i);
        const uint8_t *in[1] = { (const uint8_t *)(r->mix + done * 2) };
        uint8_t *out[1] = { (uint8_t *)(r->ring + i * 2) };
        swr_convert(r->swr, out, k, in, k);
        done += k;
    }
}

static void *ring_thread(void *arg)
{
    CBPlayer *p = arg;
    CBRingOut *r = p->ring_out;
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    int64_t rem = 0;

    while (!atomic_load_explicit(&r->stop, memory_order_relaxed)) {
        int64_t taken = atomic_load_explicit(&r->taken_us, memory_order_relaxed);
        if (taken && cb_time_us() - taken < RING_IDLE_US) {
            unsigned head = atomic_load_explicit(&r->head, memory_order_relaxed);
            while (head - atomic_load_explicit(&r->tail, memory_order_acquire) + (unsigned)r->period
                   <= RING_TARGET) {
                cb_player_mix(p, r->mix, r->period);
                to_ring(r, head, r->period);
                head += (unsigned)r->period;
                atomic_store_explicit(&r->head, head, memory_order_release);
            }
        } else {
            cb_player_mix(p, r->mix, r->period);
        }

        /* Next deadline: period / rate seconds later, the remainder carried
         * so the schedule does not drift; after a long stall (a suspended
         * machine) the schedule starts again rather than catching up. */
        int rate = cb_player_rate(p);
        int64_t ns = (int64_t)r->period * 1000000000 + rem;
        rem = ns % rate;
        next.tv_nsec += ns / rate;
        while (next.tv_nsec >= 1000000000) {
            next.tv_nsec -= 1000000000;
            next.tv_sec++;
        }
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec > next.tv_sec + 1)
            next = now;
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
    }
    return NULL;
}

int cb_player_ring_open(CBPlayer *p)
{
    CBRingOut *r = p->ring_out;
    if (!r) {
        if (!(r = calloc(1, sizeof(*r))))
            return CB_ERROR(ENOMEM);
        p->ring_out = r;
        r->period = CB_MIN(p->period, RING_TARGET / 2);
        atomic_init(&r->stop, false);
        atomic_init(&r->head, 0);
        atomic_init(&r->tail, 0);
        atomic_init(&r->taken_us, 0);
        r->mix = calloc(2 * (size_t)r->period, sizeof(*r->mix));
        if (!r->mix)
            return CB_ERROR(ENOMEM);
        AVChannelLayout st = AV_CHANNEL_LAYOUT_STEREO;
        if (swr_alloc_set_opts2(&r->swr, &st, AV_SAMPLE_FMT_S16, 48000, &st, AV_SAMPLE_FMT_FLT,
                                48000, 0, NULL) < 0 ||
            av_opt_set_int(r->swr, "dither_method", SWR_DITHER_TRIANGULAR, 0) < 0 ||
            swr_init(r->swr) < 0)
            return CB_ERROR(ENOMEM);
        /* its buffers, sized by a first conversion here */
        int16_t warm[2 * RING_TARGET];
        const uint8_t *in[1] = { (const uint8_t *)r->mix };
        uint8_t *out[1] = { (uint8_t *)warm };
        swr_convert(r->swr, out, r->period, in, r->period);
    }
    /* Opened again: what the ring kept is old, and the first take after a
     * while drops it. */
    atomic_store_explicit(&r->stop, false, memory_order_relaxed);
    int err = pthread_create(&r->thread, NULL, ring_thread, p);
    if (err)
        return CB_ERROR(err);
    r->started = true;
    atomic_store_explicit(&p->out_state, CB_OUT_RUNNING, memory_order_release);
    return 0;
}

void cb_player_ring_stop(CBPlayer *p)
{
    CBRingOut *r = p->ring_out;
    if (!r || !r->started)
        return;
    atomic_store_explicit(&r->stop, true, memory_order_relaxed);
    pthread_join(r->thread, NULL);
    r->started = false;
    /* What it kept is dropped: until it runs again a take is silence. */
    atomic_store_explicit(&r->tail, atomic_load_explicit(&r->head, memory_order_relaxed),
                          memory_order_release);
}

void cb_player_ring_close(CBPlayer *p)
{
    CBRingOut *r = p->ring_out;
    if (!r)
        return;
    cb_player_ring_stop(p);
    swr_free(&r->swr);
    free(r->mix);
    free(r);
    p->ring_out = NULL;
}

int cb_player_ring_take(CBPlayer *p, int16_t *out, int frames)
{
    CBRingOut *r = p->ring_out;
    if (!r || frames <= 0) {
        if (frames > 0)
            memset(out, 0, (size_t)frames * 2 * sizeof(*out));
        return 0;
    }
    int64_t now = cb_time_us();
    int64_t before = atomic_exchange_explicit(&r->taken_us, now, memory_order_relaxed);
    unsigned tail = atomic_load_explicit(&r->tail, memory_order_relaxed);
    unsigned head = atomic_load_explicit(&r->head, memory_order_acquire);
    /* The first take after a while: what the ring kept is old. */
    if (!before || now - before >= RING_IDLE_US)
        tail = head;
    unsigned have = head - tail;
    int n = (int)CB_MIN(have, (unsigned)frames);
    for (int i = 0; i < n; i++) {
        unsigned at = (tail + (unsigned)i) % RING_FRAMES;
        out[i * 2] = r->ring[at * 2];
        out[i * 2 + 1] = r->ring[at * 2 + 1];
    }
    if (n < frames)
        memset(out + n * 2, 0, (size_t)(frames - n) * 2 * sizeof(*out));
    atomic_store_explicit(&r->tail, tail + (unsigned)n, memory_order_release);
    return n;
}
