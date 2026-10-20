/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* A deck playing a track as it decodes it: the stream the real-time thread
 * reads its frames from, and the deck's feed thread, which fills it.
 *
 * Why: decoding a whole track before it could play took 1.6 to 4.7 s for a
 * 4-minute FLAC or Opus (measured in the interface on this machine, the
 * decode being the whole of it), and 5.4 s and 450 MB of floats for a
 * 10-minute 96 kHz FLAC. A stream opens in a few milliseconds and decodes
 * as the deck plays: a window around the read position, CB_WINDOW_BEHIND
 * seconds behind and CB_WINDOW_AHEAD ahead, and CB_CUE_CACHE_SECONDS at
 * each cue point, so the jumps a DJ makes (cue, hot cue, a loop going back,
 * reloop) play at once while the window moves there. A loop shorter than
 * the window behind needs nothing.
 *
 * Two pipelines (cyanbox/audio.h) decode it, float at the file's rate, in
 * chunks of CB_CHUNK frames. The main one plays on through the window
 * ahead and is sought only when the window lacks a chunk it is not on its
 * way to; the other fetches what lies elsewhere: the target of a jump the
 * owner has just sent first, then the cue caches, then the window behind.
 * The feed moves no samples: it puts each chunk where the real-time side
 * looks for it, a table of pointers for the window (chunk c in slot
 * c % nb_slots) and one per cache (its first chunk's index, then its
 * chunks), and the real-time side reads the chunk itself. A chunk may be
 * in a cache and in the window at once: after a jump to a cue point, the
 * window takes the cache's chunks and the main pipeline starts after
 * them. The jump's cache is apart from the window because the window's
 * slots still hold the old window, which the main pipeline would refill
 * over the target before the real-time side got there.
 *
 * A chunk is reference counted. The real-time side takes no reference: it
 * loads a pointer and reads, never waits, never frees. The feed, which
 * holds the references, lets go of a chunk it took out of a table only
 * once the real-time side has finished a callback since (the player's
 * epoch, read after the exchange: a callback that loaded the old pointer
 * ends before the epoch passes what was read), so a callback reads every
 * chunk it found whole.
 *
 * The feed sleeps in poll() on its pipelines' eventfds and its own, which
 * the owner writes when it tells it something and the real-time side
 * after a callback that held for frames or entered a new chunk.
 *
 * A stream queued to follow the deck's (cb_deck_queue_next_stream) is the
 * feed's second: its main pipeline fills the window from where the deck
 * reads it, its start until the deck gets there, so the deck plays on
 * into it the frame after the first ends. Once the real-time side says it
 * reads it (the track id it publishes), the owner makes it the feed's
 * stream (cb_player_feed_set), and the feed goes on with it from the
 * chunks it has. */

#include "player_internal.h"

#include <poll.h>
#include <stdio.h>
#include <sys/eventfd.h>
#include <unistd.h>

/* The window starts this many frames before the read position: the
 * interpolator reads one frame back, the cue caches as many. */
#define LEAD CB_CUE_CACHE_LEAD
/* A feed with nothing to do looks again this often (ms): for the chunks
 * the real-time side has let go of since. */
#define IDLE_MS 50
/* Seconds the pipelines queue: the main one past the window, the other
 * past what it was asked for. */
#define MAIN_AHEAD 2.0
#define AUX_AHEAD  0.25

enum AuxJob { AUX_NONE, AUX_JUMP, AUX_CACHE, AUX_BEHIND };

/* ---------------------------------------------------------------- stream */

int cb_stream_open(const char *path, CBStream **out)
{
    *out = NULL;
    CBStream *s = calloc(1, sizeof(*s));
    if (!s)
        return CB_ERROR(ENOMEM);
    CBPipelineOptions o = {
        .format = { .sample = CB_SAMPLE_FLT },
        .chunk = CB_CHUNK, .ahead = MAIN_AHEAD,
    };
    int ret = cb_pipeline_open(&s->main, path, &o);
    o.ahead = AUX_AHEAD;
    if (ret >= 0)
        ret = cb_pipeline_open(&s->aux, path, &o);
    if (ret < 0) {
        cb_stream_free(s);
        return ret;
    }
    s->rate = cb_pipeline_rate(s->main);
    s->channels = cb_pipeline_channels(s->main);
    int64_t window = (int64_t)ceil((CB_WINDOW_BEHIND + CB_WINDOW_AHEAD) * s->rate) + LEAD;
    s->nb_slots = (int)((window + CB_CHUNK - 1) / CB_CHUNK) + 2;
    s->cache_len = LEAD + (int64_t)ceil(CB_CUE_CACHE_SECONDS * s->rate);
    s->cpc = (int)((s->cache_len + CB_CHUNK - 1) / CB_CHUNK) + 1;
    s->slots = calloc((size_t)s->nb_slots, sizeof(*s->slots));
    s->cache = calloc((size_t)(CB_DECK_CUE_POINTS + 1) * s->cpc, sizeof(*s->cache));
    if (!s->slots || !s->cache) {
        cb_stream_free(s);
        return CB_ERROR(ENOMEM);
    }
    for (int i = 0; i < s->nb_slots; i++)
        atomic_init(&s->slots[i], NULL);
    for (int i = 0; i < (CB_DECK_CUE_POINTS + 1) * s->cpc; i++)
        atomic_init(&s->cache[i], NULL);
    for (int e = 0; e <= CB_DECK_CUE_POINTS; e++)
        atomic_init(&s->cache_first[e], -1);
    for (int k = 0; k < CB_DECK_CUE_POINTS; k++)
        s->want_first[k] = -1;
    atomic_init(&s->length, CB_MAX(cb_pipeline_length(s->main), (int64_t)0));
    atomic_init(&s->exact, false);
    atomic_init(&s->playhead, 0);
    atomic_init(&s->hint, -1);
    *out = s;
    return 0;
}

void cb_stream_free(CBStream *s)
{
    if (!s)
        return;
    cb_pipeline_free(&s->main);
    cb_pipeline_free(&s->aux);
    for (int i = 0; s->slots && i < s->nb_slots; i++) {
        CBChunk *c = atomic_load_explicit(&s->slots[i], memory_order_relaxed);
        cb_chunk_unref(&c);
    }
    for (int i = 0; s->cache && i < (CB_DECK_CUE_POINTS + 1) * s->cpc; i++) {
        CBChunk *c = atomic_load_explicit(&s->cache[i], memory_order_relaxed);
        cb_chunk_unref(&c);
    }
    for (int i = 0; i < s->nb_retired; i++)
        cb_chunk_unref(&s->retired[i].c);
    cb_chunk_unref(&s->held);
    free(s->retired);
    free(s->slots);
    free(s->cache);
    free(s);
}

int cb_stream_rate(const CBStream *s)
{
    return s->rate;
}

int cb_stream_channels(const CBStream *s)
{
    return s->channels;
}

int64_t cb_stream_length(const CBStream *s, bool *exact)
{
    CBStream *m = (CBStream *)s;
    bool e = atomic_load_explicit(&m->exact, memory_order_acquire);
    if (exact)
        *exact = e;
    return atomic_load_explicit(&m->length, memory_order_relaxed);
}

/* ------------------------------------------------------------- real time */

/* The chunk of index c in a cache, or NULL. */
static const CBChunk *cache_at(const CBStream *cs, int64_t c)
{
    CBStream *s = (CBStream *)cs;
    for (int e = 0; e <= CB_DECK_CUE_POINTS; e++) {
        int64_t f = atomic_load(&s->cache_first[e]);
        if (f < 0 || c < f || c >= f + s->cpc)
            continue;
        CBChunk *k = atomic_load(&s->cache[e * s->cpc + (c - f)]);
        if (k && k->index == c)
            return k;
    }
    return NULL;
}

/* The chunk of index c, in the window or a cache, or NULL. */
static const CBChunk *chunk_at(const CBStream *cs, int64_t c)
{
    CBStream *s = (CBStream *)cs;
    CBChunk *k = atomic_load(&s->slots[c % s->nb_slots]);
    return k && k->index == c ? k : cache_at(s, c);
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
        const CBChunk *k = chunk_at(s, c);
        if (!k)
            return false;
        /* Silence past the end of the track's last chunk */
        int have = (int)CB_CLAMP(k->nb - off, (int64_t)0, (int64_t)take);
        if (have < take && !k->last)
            return false;
        const float *x = (const float *)k->data[0] + off * s->channels;
        if (s->channels > 1) {
            for (int f = 0; f < 2 * have; f++)
                out[f] = x[f];
        } else {
            for (int f = 0; f < have; f++)
                out[2 * f] = out[2 * f + 1] = x[f];
        }
        for (int f = 2 * have; f < 2 * take; f++)
            out[f] = 0.0f;
        out += 2 * take;
        i += take;
        n -= take;
    }
    return true;
}

/* ------------------------------------------------------------------ feed */

/* A chunk out of a table: let go of once the epoch is past `epoch`. */
static void retire(CBStream *s, CBChunk *c, uint64_t epoch)
{
    if (!c)
        return;
    if (s->nb_retired == s->cap_retired) {
        int cap = s->cap_retired ? 2 * s->cap_retired : 64;
        CBRetired *r = realloc(s->retired, (size_t)cap * sizeof(*r));
        if (!r) {
            /* Freeing it now could pull it from under the real-time side;
             * keeping it is the safe failure. */
            cb_err("player: out of memory, keeping a chunk");
            return;
        }
        s->retired = r;
        s->cap_retired = cap;
    }
    s->retired[s->nb_retired++] = (CBRetired) { .c = c, .epoch = epoch };
}

static void reclaim(const CBDeckFeed *f, CBStream *s)
{
    uint64_t epoch = atomic_load(f->epoch);
    int k = 0;
    for (int i = 0; i < s->nb_retired; i++) {
        if (s->retired[i].epoch < epoch)
            cb_chunk_unref(&s->retired[i].c);
        else
            s->retired[k++] = s->retired[i];
    }
    s->nb_retired = k;
}

/* Puts chunk c (the caller's reference, now the table's; NULL empties) at
 * `at`, unless the chunk there has its index already. */
static void place(const CBDeckFeed *f, CBStream *s, _Atomic(CBChunk *) *at, CBChunk *c)
{
    CBChunk *old = atomic_load_explicit(at, memory_order_relaxed);
    if (c && old && old->index == c->index) {
        cb_chunk_unref(&c);
        return;
    }
    if (!c && !old)
        return;
    old = atomic_exchange(at, c);
    retire(s, old, atomic_load(f->epoch));
}

static bool holds(const CBStream *s, int64_t c)
{
    CBChunk *k = atomic_load_explicit(&((CBStream *)s)->slots[c % s->nb_slots],
                                      memory_order_relaxed);
    return k && k->index == c;
}

/* A chunk the pipelines gave: the track's length, the container's until
 * the last chunk says (or, when the container says less, as far as the
 * chunks went); an error said once. */
static void heard(CBStream *s, const CBChunk *c)
{
    if (c->err < 0 && !s->error_said) {
        cb_err("player: cannot decode the track at %.1f s: %s",
               (double)c->index * CB_CHUNK / s->rate, strerror(-c->err));
        s->error_said = true;
    }
    if (atomic_load_explicit(&s->exact, memory_order_relaxed))
        return;
    int64_t end = c->index * CB_CHUNK + c->nb;
    if (c->last || end > atomic_load_explicit(&s->length, memory_order_relaxed))
        atomic_store_explicit(&s->length, end, memory_order_relaxed);
    if (c->last)
        atomic_store_explicit(&s->exact, true, memory_order_release);
}

/* The window's last chunk for the read position ph. */
static int64_t window_end(const CBStream *s, int64_t ph)
{
    int64_t len = cb_stream_length(s, NULL);
    int64_t last = len > 0 ? (len - 1) / CB_CHUNK : 0;
    return CB_MIN((ph + (int64_t)(CB_WINDOW_AHEAD * s->rate)) / CB_CHUNK, last);
}

static int64_t window_start(const CBStream *s, int64_t ph)
{
    return CB_MAX(ph - (int64_t)(CB_WINDOW_BEHIND * s->rate), (int64_t)0) / CB_CHUNK;
}

/* Empties cache e and starts it at chunk `first` (-1: none). */
static void cache_reset(const CBDeckFeed *f, CBStream *s, int e, int64_t first)
{
    atomic_store(&s->cache_first[e], -1);
    for (int i = 0; i < s->cpc; i++)
        place(f, s, &s->cache[e * s->cpc + i], NULL);
    atomic_store(&s->cache_first[e], first);
}

/* New cue points: caches already at one of them stay, the others go to the
 * points without one. */
static void take_points(const CBDeckFeed *f, CBStream *s, const int64_t *points, int nb)
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
        while (j < nw && (served[j] || want[j] != s->want_first[k]))
            j++;
        if (j < nw && s->want_first[k] >= 0) {
            served[j] = true;
        } else if (s->want_first[k] >= 0) {
            s->want_first[k] = -1;
            s->cache_done[k] = false;
            cache_reset(f, s, k, -1);
        }
    }
    for (int j = 0, k = 0; j < nw; j++) {
        if (served[j])
            continue;
        while (k < CB_DECK_CUE_POINTS && s->want_first[k] >= 0)
            k++;
        if (k == CB_DECK_CUE_POINTS)
            break;
        s->want_first[k] = want[j];
        s->cache_done[k] = false;
    }
    if (s->aux_job == AUX_CACHE)
        s->aux_job = AUX_NONE;
}

/* Chunk c from a cache into the window, when one has it: true then. */
static bool share(const CBDeckFeed *f, CBStream *s, int64_t c)
{
    const CBChunk *k = cache_at(s, c);
    if (k)
        place(f, s, &s->slots[c % s->nb_slots], cb_chunk_ref((CBChunk *)k));
    return k;
}

/* What the main pipeline made, into the window, and where it should be
 * for the read position ph: true while the window lacks a chunk. */
static bool fill_window(const CBDeckFeed *f, CBStream *s, int64_t ph)
{
    while (1) {
        CBChunk *k = s->held ? s->held : cb_pipeline_pop(s->main);
        s->held = NULL;
        if (!k)
            break;
        heard(s, k);
        s->main_next = k->index + 1;
        if (k->index > window_end(s, ph)) {
            /* kept until the window gets there */
            s->held = k;
            break;
        }
        if (k->index >= window_start(s, ph))
            place(f, s, &s->slots[k->index % s->nb_slots], k);
        else
            cb_chunk_unref(&k);
    }

    /* Everything a cache has first, then the main pipeline to the first
     * chunk still missing (not one the other pipeline is on its way to) */
    bool exact, busy = false;
    int64_t len = cb_stream_length(s, &exact);
    int64_t c0 = CB_MAX(ph - LEAD, (int64_t)0) / CB_CHUNK;
    int64_t c1 = exact && len <= 0 ? -1 : window_end(s, ph);
    int64_t want = -1;
    for (int64_t c = c0; c <= c1; c++) {
        if (holds(s, c) || share(f, s, c))
            continue;
        busy = true;
        if (want < 0 && !(s->aux_job == AUX_JUMP && c >= s->aux_next && c < s->aux_to))
            want = c;
    }
    if (want >= 0 && want != s->main_next) {
        cb_chunk_unref(&s->held);
        cb_pipeline_seek(s->main, want);
        s->main_next = want;
        s->main_seeks++;
    }
    return busy;
}

/* Whether the real-time side would find frames [i, i + n) now. */
static bool readable(const CBStream *s, int64_t i, int64_t n)
{
    for (int64_t c = i / CB_CHUNK; n > 0 && c <= (i + n - 1) / CB_CHUNK; c++)
        if (!chunk_at(s, c))
            return false;
    return true;
}

static void aux_start(CBStream *s, int job, int cue, int64_t from, int64_t to)
{
    s->aux_job = job;
    s->aux_cue = cue;
    s->aux_next = from;
    s->aux_to = to;
    cb_pipeline_seek(s->aux, from);
}

/* What the other pipeline fetches for the read position ph: true while it
 * has something to do. */
static bool fill_rest(const CBDeckFeed *f, CBStream *s, int64_t ph)
{
    int64_t len = cb_stream_length(s, NULL);

    /* A jump the owner just sent, to a place nothing holds: before what the
     * other pipeline was doing, which it takes up again after */
    int64_t hint = atomic_exchange_explicit(&s->hint, -1, memory_order_relaxed);
    if (hint >= 0 && hint < len) {
        int64_t first = CB_MAX(hint - LEAD, (int64_t)0);
        if (!readable(s, first, CB_MIN(2 * LEAD, len - first))) {
            int64_t c = first / CB_CHUNK;
            cache_reset(f, s, CB_DECK_CUE_POINTS, c);
            aux_start(s, AUX_JUMP, CB_DECK_CUE_POINTS, c, c + s->cpc);
        }
    }

    while (s->aux_job != AUX_NONE) {
        CBChunk *k = cb_pipeline_pop(s->aux);
        if (!k)
            return true;
        heard(s, k);
        if (k->index != s->aux_next) {
            cb_chunk_unref(&k);
            continue;
        }
        s->aux_next++;
        bool done = k->last || s->aux_next >= s->aux_to;
        if (s->aux_job == AUX_BEHIND) {
            /* only into the window as it is now */
            if (k->index >= window_start(s, ph) && k->index <= window_end(s, ph))
                place(f, s, &s->slots[k->index % s->nb_slots], k);
            else
                cb_chunk_unref(&k);
        } else {
            int e = s->aux_cue;
            int64_t first = atomic_load_explicit(&s->cache_first[e], memory_order_relaxed);
            if (first >= 0 && k->index >= first && k->index < first + s->cpc)
                place(f, s, &s->cache[e * s->cpc + (k->index - first)], k);
            else
                cb_chunk_unref(&k);
        }
        if (done) {
            if (s->aux_job == AUX_CACHE)
                s->cache_done[s->aux_cue] = true;
            s->aux_job = AUX_NONE;
        }
    }

    /* The cue caches */
    for (int k = 0; k < CB_DECK_CUE_POINTS; k++) {
        if (s->want_first[k] < 0 || s->cache_done[k] || s->want_first[k] >= len)
            continue;
        int64_t c = s->want_first[k] / CB_CHUNK;
        int64_t to = (s->want_first[k] + s->cache_len - 1) / CB_CHUNK + 1;
        if (atomic_load_explicit(&s->cache_first[k], memory_order_relaxed) != c)
            cache_reset(f, s, k, c);
        aux_start(s, AUX_CACHE, k, c, CB_MIN(to, c + s->cpc));
        return true;
    }
    /* The window behind: what the caches have, then the rest */
    int64_t c0 = CB_MAX(ph - LEAD, (int64_t)0) / CB_CHUNK, from = -1;
    for (int64_t c = window_start(s, ph); c < c0; c++)
        if (!holds(s, c) && !share(f, s, c) && from < 0)
            from = c;
    if (from >= 0) {
        aux_start(s, AUX_BEHIND, 0, from, c0);
        return true;
    }
    return false;
}

/* ------------------------------------------------------------ the thread */

static void poke(int fd)
{
    uint64_t one = 1;
    ssize_t w = write(fd, &one, sizeof(one));
    (void)w;
}

/* Sleeps until something wakes the feed, or IDLE_MS. */
static void wait_wake(CBDeckFeed *f, const CBStream *s, const CBStream *next)
{
    struct pollfd p[4];
    nfds_t n = 0;
    p[n++] = (struct pollfd) { .fd = f->wakefd, .events = POLLIN };
    if (s) {
        p[n++] = (struct pollfd) { .fd = cb_pipeline_fd(s->main), .events = POLLIN };
        p[n++] = (struct pollfd) { .fd = cb_pipeline_fd(s->aux), .events = POLLIN };
    }
    if (next)
        p[n++] = (struct pollfd) { .fd = cb_pipeline_fd(next->main), .events = POLLIN };
    if (poll(p, n, IDLE_MS) <= 0)
        return;
    for (nfds_t i = 0; i < n; i++) {
        uint64_t v;
        if (p[i].revents & POLLIN) {
            ssize_t r = read(p[i].fd, &v, sizeof(v));
            (void)r;
        }
    }
}

static void *feed_main(void *arg)
{
    CBDeckFeed *f = arg;
    CBStream *s = NULL, *next = NULL;
    uint64_t points_gen = 0;
    while (1) {
        wait_wake(f, s, next);

        int64_t points[CB_DECK_CUE_POINTS];
        pthread_mutex_lock(&f->lock);
        bool quit = f->quit;
        CBStream *now = f->stream, *now_next = f->next;
        uint64_t gen = f->gen;
        bool new_points = f->points_gen != points_gen;
        int nb = f->nb_points;
        memcpy(points, f->points, sizeof(points[0]) * nb);
        points_gen = f->points_gen;
        pthread_mutex_unlock(&f->lock);
        if (quit)
            break;
        if (now != s)
            atomic_store_explicit(&f->settled, -1, memory_order_relaxed);
        s = now;
        next = now_next != now ? now_next : NULL;
        /* From here on the streams before this generation are not touched. */
        atomic_store_explicit(&f->acked, gen, memory_order_release);

        if (s) {
            reclaim(f, s);
            if (new_points)
                take_points(f, s, points, nb);
            int64_t at = atomic_load_explicit(&s->playhead, memory_order_relaxed);
            int64_t ph = CB_CLAMP(at, (int64_t)0, cb_stream_length(s, NULL));
            bool busy = fill_window(f, s, ph);
            busy |= fill_rest(f, s, ph);
            atomic_store_explicit(&f->settled, busy ? -1 : at, memory_order_release);
        }
        if (next) {
            reclaim(f, next);
            int64_t at = atomic_load_explicit(&next->playhead, memory_order_relaxed);
            fill_window(f, next, CB_CLAMP(at, (int64_t)0, cb_stream_length(next, NULL)));
        }
    }
    return NULL;
}

int cb_player_feed_start(CBDeckFeed *f, const _Atomic uint64_t *epoch)
{
    *f = (CBDeckFeed) { .wakefd = -1, .epoch = epoch };
    atomic_init(&f->acked, 0);
    atomic_init(&f->settled, -1);
    int ret = pthread_mutex_init(&f->lock, NULL);
    if (ret)
        return CB_ERROR(ret);
    f->lock_ok = true;
    if ((f->wakefd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)) < 0)
        return CB_ERROR(errno);
    ret = pthread_create(&f->thread, NULL, feed_main, f);
    if (ret)
        return CB_ERROR(ret);
    f->started = true;
    return 0;
}

/* Safe on a feed never started (zeroed) or started part way. */
void cb_player_feed_stop(CBDeckFeed *f)
{
    if (f->started) {
        pthread_mutex_lock(&f->lock);
        f->quit = true;
        pthread_mutex_unlock(&f->lock);
        poke(f->wakefd);
        pthread_join(f->thread, NULL);
        f->started = false;
    }
    if (f->lock_ok) {
        pthread_mutex_destroy(&f->lock);
        if (f->wakefd >= 0)
            close(f->wakefd);
    }
    f->wakefd = -1;
    f->lock_ok = false;
}

void cb_player_feed_poke(CBDeckFeed *f)
{
    if (f->lock_ok && f->wakefd >= 0)
        poke(f->wakefd);
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
    cb_player_feed_poke(f);
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
    cb_player_feed_poke(f);
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
        cb_player_feed_poke(f);
        usleep(50);
    }
    return true;
}
