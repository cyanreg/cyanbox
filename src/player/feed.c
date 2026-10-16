/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* A deck playing a packet store: the stream the real-time thread reads its
 * frames from, and the deck's feed thread, which decodes them.
 *
 * Why: decoding a whole track before it could play took 1.6 to 4.7 s for a
 * 4-minute FLAC or Opus (measured in the interface on this machine, the
 * decode being the whole of it), and 5.4 s and 450 MB of floats for a
 * 10-minute 96 kHz FLAC. A packet store holds the file's packets
 * (cyanbox/audio.h): opened in a few milliseconds, it is what the deck loads,
 * and this file decodes it as the deck plays, a window around the read
 * position, CB_WINDOW_BEHIND seconds behind and CB_WINDOW_AHEAD ahead, and
 * CB_CUE_CACHE_SECONDS at each cue point, so the jumps a DJ makes (cue, hot
 * cue, a loop going back, reloop) play at once while the feed moves its
 * window there. A loop shorter than the window behind needs nothing.
 *
 * The window is cut in chunks of CB_CHUNK frames, chunk c in slot
 * c % nb_slots. There are slots for the whole window and two more, so the
 * chunks of one window never share a slot, and a slot is only written with
 * a chunk that has left the window. Slots and caches are sequence locks
 * (CBSlot): the real-time side copies and checks, never waits; a copy that
 * raced a write counts as frames not there.
 *
 * The feed's work, one piece at a time, reading the read position again
 * before each: the target of a jump the owner has just sent (cb_deck_seek,
 * a hot cue, a click on the waveform), a chunk's worth into the jump slot
 * kept for it, unless the window or a cue cache has it; the chunks from
 * just behind the read position to the end of the window ahead, nearest
 * first; the cue caches; the window behind; then the rest of the store's
 * index, a few packets at a time. A chunk is a few hundred microseconds of
 * decoding (a millisecond or two after a jump, with the codec's pre-roll).
 * The owner tells the feed of a jump as it sends it, and the real-time
 * side applies it at its next callback, up to a period later: measured
 * with 256-frame callbacks at 48 kHz, a jump far into a FLAC, MP3 or AAC
 * track then plays from the next callback as with the whole track in
 * memory (median 1.9 ms, worst 7.3), an Opus one, whose 640 ms of pre-roll
 * take longer, a callback after that (median 7.2 ms, worst 12.7). The
 * jump slot is apart from the window because the window's slots still hold
 * the old window, which the feed would otherwise refill over the target
 * before the real-time side got there. The real-time side wakes the feed
 * (sem_post, which neither blocks nor allocates) whenever it held for
 * frames, and whenever its read position enters a new chunk; an idle feed
 * also looks every IDLE_MS.
 *
 * The real-time side reads every frame through cb_player_stream_read: the
 * interpolator's four frames, the de-click tail, the grains and master
 * tempo's read-ahead (deck.c). Frames that are not there: the main voice
 * holds, silent, and keeps its position; the others play silence.
 *
 * A stream queued to follow the deck's (cb_deck_queue_next) is the feed's
 * second: once the first has nothing left to do, the feed decodes the
 * window ahead from the start of the second, with a decoder of its own,
 * so the deck plays on into it the frame after the first ends. Once the
 * real-time side says it reads it (the track id it publishes), the owner
 * makes it the feed's stream (cb_player_feed_set), and the feed goes on
 * with that decoder from the chunks it has. */

#include "player_internal.h"

#include <stdio.h>
#include <time.h>
#include <unistd.h>

/* The window starts this many frames before the read position: the
 * interpolator reads one frame back, the cue caches as many. */
#define LEAD CB_CUE_CACHE_LEAD
/* Packets the feed indexes per piece of work once everything else is done. */
#define INDEX_STEP 64
/* An idle feed looks again this often, whatever wakes it (ms). */
#define IDLE_MS 50

/* ---------------------------------------------------------------- stream */

int cb_player_stream_create(CBStream **out, CBPackets *pk)
{
    *out = NULL;
    int rate = cb_packets_rate(pk);
    int ch = CB_MIN(cb_packets_channels(pk), 2);
    if (rate <= 0 || ch <= 0)
        return CB_ERROR(EINVAL);

    CBStream *s = calloc(1, sizeof(*s));
    if (!s)
        return CB_ERROR(ENOMEM);
    int64_t window = (int64_t)ceil((CB_WINDOW_BEHIND + CB_WINDOW_AHEAD) * rate) + LEAD;
    s->nb_slots  = (int)((window + CB_CHUNK - 1) / CB_CHUNK) + 2;
    s->cache_len = LEAD + (int64_t)ceil(CB_CUE_CACHE_SECONDS * rate);
    s->rate      = rate;
    s->channels  = ch;

    /* Not zeroed: nothing is read before it is written, and pages never
     * written (caches of points a track does not have) stay unallocated. */
    size_t chunk = (size_t)CB_CHUNK * ch, cache = (size_t)s->cache_len * ch;
    s->slots = calloc((size_t)s->nb_slots, sizeof(*s->slots));
    s->mem = malloc(sizeof(float) * (chunk * s->nb_slots + cache * (CB_DECK_CUE_POINTS + 1)));
    if (!s->slots || !s->mem) {
        free(s->slots);
        free(s->mem);
        free(s);
        return CB_ERROR(ENOMEM);
    }
    for (int i = 0; i < s->nb_slots; i++) {
        atomic_init(&s->slots[i].seq, 0);
        atomic_init(&s->slots[i].first, -1);
        atomic_init(&s->slots[i].len, 0);
        s->slots[i].data = s->mem + chunk * i;
    }
    for (int i = 0; i < CB_DECK_CUE_POINTS + 1; i++) {
        atomic_init(&s->caches[i].seq, 0);
        atomic_init(&s->caches[i].first, -1);
        atomic_init(&s->caches[i].len, 0);
        s->caches[i].data = s->mem + chunk * s->nb_slots + cache * i;
    }
    atomic_init(&s->playhead, 0);
    atomic_init(&s->starved, false);
    atomic_init(&s->hint, -1);
    s->pk = pk;
    *out = s;
    return 0;
}

void cb_player_stream_free(CBStream *s)
{
    if (!s)
        return;
    cb_packets_free(s->pk);
    free(s->slots);
    free(s->mem);
    free(s);
}

/* ------------------------------------------------------------- real time */

/* Copies n frames from frame `off` of a slot that should hold `first` on,
 * as stereo pairs. */
static bool slot_copy(const CBSlot *cs, int64_t first, int64_t off, int n, int ch, float *out)
{
    CBSlot *sl = (CBSlot *)cs;
    unsigned q = atomic_load_explicit(&sl->seq, memory_order_acquire);
    if ((q & 1) || atomic_load_explicit(&sl->first, memory_order_relaxed) != first ||
        off + n > atomic_load_explicit(&sl->len, memory_order_relaxed))
        return false;
    const float *x = sl->data + off * ch;
    if (ch > 1) {
        for (int k = 0; k < 2 * n; k++)
            out[k] = x[k];
    } else {
        for (int k = 0; k < n; k++)
            out[2 * k] = out[2 * k + 1] = x[k];
    }
    atomic_thread_fence(memory_order_acquire);
    return atomic_load_explicit(&sl->seq, memory_order_relaxed) == q;
}

/* The cue caches and the jump slot (slot_copy checks again what is read
 * here outside its sequence lock). */
static bool cache_copy(const CBStream *s, int64_t i, int n, float *out)
{
    for (int k = 0; k < CB_DECK_CUE_POINTS + 1; k++) {
        CBSlot *sl = (CBSlot *)&s->caches[k];
        int64_t first = atomic_load_explicit(&sl->first, memory_order_relaxed);
        if (first < 0 || i < first || i + n > first + s->cache_len)
            continue;
        if (slot_copy(sl, first, i - first, n, s->channels, out))
            return true;
    }
    return false;
}

bool cb_player_stream_read(const CBStream *s, int64_t nb_frames, int64_t i, int n, float *out)
{
    while (n > 0) {
        if (i < 0 || i >= nb_frames) {
            out[0] = out[1] = 0.0f;
            out += 2;
            i++;
            n--;
            continue;
        }
        int64_t c = i / CB_CHUNK, off = i - c * CB_CHUNK;
        int take = (int)CB_MIN((int64_t)n, CB_MIN(CB_CHUNK - off, nb_frames - i));
        if (!slot_copy(&s->slots[c % s->nb_slots], c * CB_CHUNK, off, take, s->channels, out) &&
            !cache_copy(s, i, take, out))
            return false;
        out += 2 * take;
        i += take;
        n -= take;
    }
    return true;
}

/* ------------------------------------------------------------------ feed */

/* What the feed thread keeps for the stream it fills, and for the one
 * queued to follow it. */
typedef struct Work {
    CBDeckFeed      *f;
    CBStream        *s;
    CBPacketDecoder *dec;
    uint64_t         points_gen;
    int64_t          cache_first[CB_DECK_CUE_POINTS];  /* -1 unused */
    bool             cache_done[CB_DECK_CUE_POINTS];
    bool             error_said;
    CBStream        *next_s;
    CBPacketDecoder *next_dec;
    bool             next_error_said;
} Work;

static void write_begin(CBSlot *sl)
{
    unsigned q = atomic_load_explicit(&sl->seq, memory_order_relaxed);
    atomic_store_explicit(&sl->seq, q + 1, memory_order_relaxed);
    atomic_store_explicit(&sl->first, -1, memory_order_relaxed);
    atomic_thread_fence(memory_order_release);
}

static void write_end(CBSlot *sl, int64_t first, int64_t len)
{
    atomic_store_explicit(&sl->first, first, memory_order_relaxed);
    atomic_store_explicit(&sl->len, len, memory_order_relaxed);
    unsigned q = atomic_load_explicit(&sl->seq, memory_order_relaxed);
    atomic_store_explicit(&sl->seq, q + 1, memory_order_release);
}

/* Only this thread writes `first`. */
static bool holds(const CBSlot *sl, int64_t first)
{
    return atomic_load_explicit(&((CBSlot *)sl)->first, memory_order_relaxed) == first;
}

static void fill_with(CBStream *s, CBPacketDecoder *dec, bool *error_said, CBSlot *sl,
                      int64_t first, int64_t n)
{
    write_begin(sl);
    int64_t got = cb_packets_decode(dec, first, n, sl->data, s->channels);
    if (got < 0) {
        /* Silence rather than a deck waiting for ever. */
        memset(sl->data, 0, sizeof(float) * (size_t)n * s->channels);
        if (!*error_said)
            cb_err("player: cannot decode the track at %.1f s: %s", (double)first / s->rate,
                   strerror((int)-got));
        *error_said = true;
    }
    write_end(sl, first, n);
}

/* Into the deck's stream. */
static void fill(Work *w, CBSlot *sl, int64_t first, int64_t n)
{
    atomic_store_explicit(&w->f->settled, -1, memory_order_relaxed);
    fill_with(w->s, w->dec, &w->error_said, sl, first, n);
}

static void forget_cache(Work *w, int k)
{
    if (w->cache_first[k] < 0)
        return;
    write_begin(&w->s->caches[k]);
    write_end(&w->s->caches[k], -1, 0);
    w->cache_first[k] = -1;
}

/* New cue points: caches already at one of them stay, the others go to the
 * points without one. */
static void take_points(Work *w, const int64_t *points, int nb)
{
    int64_t want[CB_DECK_CUE_POINTS];
    int nw = 0;
    for (int i = 0; i < nb && nw < CB_DECK_CUE_POINTS; i++) {
        int64_t first = CB_MAX(points[i] - LEAD, (int64_t)0);
        bool dup = false;
        for (int j = 0; j < nw; j++)
            dup |= want[j] == first;
        if (!dup)
            want[nw++] = first;
    }
    bool served[CB_DECK_CUE_POINTS] = { 0 };
    for (int k = 0; k < CB_DECK_CUE_POINTS; k++) {
        int j = 0;
        while (j < nw && (served[j] || want[j] != w->cache_first[k]))
            j++;
        if (j < nw && w->cache_first[k] >= 0)
            served[j] = true;
        else
            forget_cache(w, k);
    }
    for (int j = 0, k = 0; j < nw; j++) {
        if (served[j])
            continue;
        while (k < CB_DECK_CUE_POINTS && w->cache_first[k] >= 0)
            k++;
        if (k == CB_DECK_CUE_POINTS)
            break;
        w->cache_first[k] = want[j];
        w->cache_done[k] = false;
    }
}

/* One piece of work; false when there was none left. */
static bool step(Work *w)
{
    CBStream *s = w->s;
    bool exact;
    int64_t len = cb_packets_length(s->pk, &exact);
    int64_t at = atomic_load_explicit(&s->playhead, memory_order_relaxed);
    int64_t ph = CB_CLAMP(at, (int64_t)0, len);
    int64_t ahead = (int64_t)(CB_WINDOW_AHEAD * s->rate);
    int64_t behind = (int64_t)(CB_WINDOW_BEHIND * s->rate);

    /* A jump the owner just sent: a chunk's worth at its target first, in
     * the jump slot (the window's slots are the old window's until the
     * real-time side gets there), so that it is decoded by the time the
     * real-time side applies the jump, up to a callback later. Not when the
     * window or a cue cache has it already. */
    int64_t hint = atomic_exchange_explicit(&s->hint, -1, memory_order_relaxed);
    if (hint >= 0 && hint < len) {
        int64_t first = CB_MAX(hint - LEAD, (int64_t)0);
        float probe[2 * 2 * LEAD];
        if (!cb_player_stream_read(s, len, first, 2 * LEAD, probe)) {
            fill(w, &s->caches[CB_DECK_CUE_POINTS], first, CB_MIN(s->cache_len, CB_CHUNK + LEAD));
            return true;
        }
    }

    int64_t c0 = CB_MAX(ph - LEAD, (int64_t)0) / CB_CHUNK;
    int64_t c1 = len > 0 ? CB_MIN(ph + ahead, len - 1) / CB_CHUNK : -1;
    for (int64_t c = c0; c <= c1; c++) {
        CBSlot *sl = &s->slots[c % s->nb_slots];
        if (!holds(sl, c * CB_CHUNK)) {
            fill(w, sl, c * CB_CHUNK, CB_CHUNK);
            return true;
        }
    }
    for (int k = 0; k < CB_DECK_CUE_POINTS; k++) {
        if (w->cache_first[k] >= 0 && !w->cache_done[k]) {
            fill(w, &s->caches[k], w->cache_first[k], s->cache_len);
            w->cache_done[k] = true;
            return true;
        }
    }
    for (int64_t c = CB_MAX(ph - behind, (int64_t)0) / CB_CHUNK; c < c0; c++) {
        CBSlot *sl = &s->slots[c % s->nb_slots];
        if (!holds(sl, c * CB_CHUNK)) {
            fill(w, sl, c * CB_CHUNK, CB_CHUNK);
            return true;
        }
    }

    atomic_store_explicit(&w->f->settled, at, memory_order_release);
    if (!exact) {
        int ret = cb_packets_index(s->pk, INDEX_STEP);
        if (ret < 0)
            cb_warn("player: the track's index ends early: %s", strerror(-ret));
        return true;
    }
    return false;
}

/* The stream queued to follow, once the deck's has nothing left: the
 * window ahead from its first frame, a chunk at a time, so the deck
 * plays on into it at once. The rest of it is decoded once it is the
 * deck's stream, from these chunks on. */
static bool step_next(Work *w)
{
    CBStream *s = w->next_s;
    if (!s || !w->next_dec)
        return false;
    int64_t len = cb_packets_length(s->pk, NULL);
    int64_t ahead = (int64_t)(CB_WINDOW_AHEAD * s->rate);
    int64_t c1 = len > 0 ? CB_MIN(ahead, len - 1) / CB_CHUNK : -1;
    for (int64_t c = 0; c <= c1; c++) {
        CBSlot *sl = &s->slots[c % s->nb_slots];
        if (!holds(sl, c * CB_CHUNK)) {
            fill_with(s, w->next_dec, &w->next_error_said, sl, c * CB_CHUNK, CB_CHUNK);
            return true;
        }
    }
    return false;
}

static void wait_wake(CBDeckFeed *f, bool busy)
{
    if (busy) {
        while (!sem_trywait(&f->wake))
            ;
        return;
    }
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += IDLE_MS * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_nsec -= 1000000000L;
        ts.tv_sec++;
    }
    while (sem_timedwait(&f->wake, &ts) && errno == EINTR)
        ;
}

static void *feed_main(void *arg)
{
    CBDeckFeed *f = arg;
    Work w = { .f = f };
    for (int k = 0; k < CB_DECK_CUE_POINTS; k++)
        w.cache_first[k] = -1;

    bool busy = false;
    while (1) {
        wait_wake(f, busy);

        int64_t points[CB_DECK_CUE_POINTS];
        pthread_mutex_lock(&f->lock);
        bool quit = f->quit;
        CBStream *s = f->stream;
        CBStream *next = f->next;
        uint64_t gen = f->gen;
        bool new_points = f->points_gen != w.points_gen;
        int nb = f->nb_points;
        memcpy(points, f->points, sizeof(points[0]) * nb);
        w.points_gen = f->points_gen;
        pthread_mutex_unlock(&f->lock);
        if (quit)
            break;

        if (s != w.s) {
            /* The stream queued to follow, now the deck's: its decoder
             * and the chunks it has go on. */
            CBPacketDecoder *dec = NULL;
            if (s && s == w.next_s) {
                dec = w.next_dec;
                w.next_dec = NULL;
                w.next_s = NULL;
            }
            cb_packets_decoder_free(w.dec);
            w.dec = dec;
            w.s = s;
            w.error_said = false;
            for (int k = 0; k < CB_DECK_CUE_POINTS; k++)
                w.cache_first[k] = -1;
            new_points = true;
            if (s && !w.dec) {
                int ret = cb_packets_decoder(s->pk, &w.dec);
                if (ret < 0)
                    cb_err("player: no decoder for the track: %s", strerror(-ret));
            }
            atomic_store_explicit(&f->settled, -1, memory_order_relaxed);
        }
        if (next != w.next_s) {
            cb_packets_decoder_free(w.next_dec);
            w.next_dec = NULL;
            w.next_s = next;
            w.next_error_said = false;
            if (next) {
                int ret = cb_packets_decoder(next->pk, &w.next_dec);
                if (ret < 0)
                    cb_err("player: no decoder for the next track: %s", strerror(-ret));
            }
        }
        /* From here on the streams before this generation are not touched. */
        atomic_store_explicit(&f->acked, gen, memory_order_release);
        if (!w.s || !w.dec) {
            busy = step_next(&w);
            continue;
        }
        if (new_points)
            take_points(&w, points, nb);
        busy = step(&w);
        if (!busy)
            busy = step_next(&w);
    }
    cb_packets_decoder_free(w.dec);
    cb_packets_decoder_free(w.next_dec);
    return NULL;
}

int cb_player_feed_start(CBDeckFeed *f)
{
    *f = (CBDeckFeed) { 0 };
    atomic_init(&f->acked, 0);
    atomic_init(&f->settled, -1);
    if (sem_init(&f->wake, 0, 0))
        return CB_ERROR(errno);
    f->sem_ok = true;
    int ret = pthread_mutex_init(&f->lock, NULL);
    if (ret)
        return CB_ERROR(ret);
    f->lock_ok = true;
    ret = pthread_create(&f->thread, NULL, feed_main, f);
    if (ret)
        return CB_ERROR(ret);
    f->started = true;
    return 0;
}

void cb_player_feed_stop(CBDeckFeed *f)
{
    if (f->started) {
        pthread_mutex_lock(&f->lock);
        f->quit = true;
        pthread_mutex_unlock(&f->lock);
        sem_post(&f->wake);
        pthread_join(f->thread, NULL);
        f->started = false;
    }
    if (f->lock_ok)
        pthread_mutex_destroy(&f->lock);
    if (f->sem_ok)
        sem_destroy(&f->wake);
    f->lock_ok = f->sem_ok = false;
}

uint64_t cb_player_feed_set(CBDeckFeed *f, CBStream *s, CBStream *next)
{
    pthread_mutex_lock(&f->lock);
    if (s != f->stream) {
        /* The points were the old track's. */
        f->nb_points = 0;
        f->points_gen++;
    }
    f->stream = s;
    f->next = next;
    uint64_t gen = ++f->gen;
    pthread_mutex_unlock(&f->lock);
    sem_post(&f->wake);
    return gen;
}

void cb_player_feed_points(CBDeckFeed *f, const int64_t *frames, int nb)
{
    nb = CB_CLAMP(nb, 0, CB_DECK_CUE_POINTS);
    pthread_mutex_lock(&f->lock);
    memcpy(f->points, frames, sizeof(frames[0]) * nb);
    f->nb_points = nb;
    f->points_gen++;
    pthread_mutex_unlock(&f->lock);
    sem_post(&f->wake);
}

bool cb_player_feed_wait(CBDeckFeed *f, const CBStream *s, int64_t timeout_us)
{
    pthread_mutex_lock(&f->lock);
    uint64_t gen = f->gen;
    bool ours = f->stream == s;
    pthread_mutex_unlock(&f->lock);
    /* A stream the feed does not fill is never settled: nothing to wait
     * for (the owner gives the feed the stream the deck reads as soon as
     * it knows of it). */
    if (!ours)
        return true;
    int64_t want = atomic_load_explicit(&((CBStream *)s)->playhead, memory_order_relaxed);
    int64_t t0 = cb_time_us();
    while (atomic_load_explicit(&f->acked, memory_order_acquire) != gen ||
           atomic_load_explicit(&f->settled, memory_order_acquire) != want) {
        if (cb_time_us() - t0 > timeout_us)
            return false;
        sem_post(&f->wake);
        usleep(50);
    }
    return true;
}
