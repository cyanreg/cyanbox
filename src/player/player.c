/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* The playback engine: the owner-side half of every cb_deck_* function,
 * the command ring to the real-time thread, the mix callback both backends
 * run, and the state the real-time thread publishes back.
 *
 * Owner -> real time: a fixed single-producer/single-consumer ring of
 * small command structs. The owner writes the slot, then publishes the new
 * head with release order; the real-time thread reads the head with
 * acquire order, so the slot contents are visible, applies every command
 * at the start of a callback, and publishes the new tail with release
 * order so the owner may reuse those slots. A full ring drops the command
 * (with one warning) rather than block the owner or the device.
 *
 * Real time -> owner: per deck, the position, playing state and the
 * number of commands applied (its serial), under a sequence lock (see
 * CBDeckPub). The owner keeps what it last asked for (a seek, a play) with
 * the deck serial of that command, and cb_deck_state() reports its own
 * value until the published serial reaches it: a seek is visible at once,
 * and a stale position from before the seek never shows.
 *
 * Buffer release: every track the owner hands the deck has an id (the
 * deck serial of the command that carried it), and the deck publishes,
 * with the serial, the id of the track it reads and of one its de-click
 * tail still reads. A track let go of (loaded over, unloaded, dropped from
 * the queue, moved on from) waits in a pending list with the serial of
 * the command that let go of it, and is freed once a published serial is
 * at least that and neither published id is its own: the command took it
 * out of the deck's hands (the deck had no other pointer to it), and the
 * deck no longer plays it. Both are published with release order after
 * the fact, so once the owner reads them with acquire order, freeing is
 * safe. No second ring is needed and nothing can overflow. The pending
 * list is drained by every owner-side call, cb_deck_state() included.
 *
 * Packet stores: a deck loaded with one plays a stream (feed.c), which the
 * deck's feed thread decodes into. The owner tells the feed which stream
 * to fill (the one the deck reads) and which one's start (the one queued
 * after it), with a generation number; a stream let go of waits in the
 * same pending list until the feed is not told to fill it and has moved
 * to the generation that told it so. A stream's length is the container's
 * until the store's index is complete, then the count: both sides read it
 * from the store (two atomics) rather than being told. Manual mode waits
 * for the feeds before each callback (cb_player_null_advance).
 *
 * MASTER TEMPO: each deck's pitch shifter is made here on the owner thread
 * and handed to the real-time side with the rest of CBDeckRT; after that
 * only the real-time thread touches it (deck.c). The position published is
 * the one the listener hears (cb_player_deck_heard): with the shifter on,
 * that of the frame fed the shifter's delay ago, not the read position;
 * and with it the id of the track that frame was in.
 *
 * The next track (cb_deck_queue_next): a second buffer or stream the owner
 * hands the deck, which the real-time side moves on to by itself at the
 * end of the one it plays (deck.c). The owner keeps it beside the loaded
 * one and learns of the move from the ids, in two steps (sync_deck): when
 * the deck reads it, the feed is told to fill it, and when it is heard
 * (with the shifter on, its delay later), it becomes the track the state
 * describes and what was loaded is let go of. The owner cannot know, when
 * it drops or replaces the queued track, whether the deck has moved on
 * into it already (a callback may be doing so while it asks, or have done
 * so inside the shifter's delay): so a next carries the id of the track
 * it follows, and the deck drops one that no longer follows its own (as it
 * drops a seek or a loop meant for a track it has left); a dropped next
 * waits in the pending list like any track let go of, and if the deck
 * turns out to read it, it is the deck's track again. */

#include "cyanbox/player.h"

#include <math.h>
#include <sched.h>
#include <stdio.h>
#include <time.h>

#include "player_internal.h"

static uint64_t dbits(double v)
{
    uint64_t u;
    memcpy(&u, &v, sizeof(u));
    return u;
}

static double bitsd(uint64_t u)
{
    double v;
    memcpy(&v, &u, sizeof(v));
    return v;
}

static unsigned fbits(float v)
{
    unsigned u;
    memcpy(&u, &v, sizeof(u));
    return u;
}

static float bitsf(unsigned u)
{
    float v;
    memcpy(&v, &u, sizeof(v));
    return v;
}

static int64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/* ------------------------------------------------------- published state */

/* Real-time side. Sequence lock writer: odd while writing. The release
 * fence after the odd store keeps the data stores from moving above it;
 * the final release store orders them before the even value. */
static void publish(CBDeckPub *pub, const CBDeckRT *d, int64_t time_us, int64_t span_us)
{
    CBPos pos = cb_player_deck_heard(d);
    /* A deck holding for its feed does not move: no speed to extrapolate */
    bool waiting = d->playing && d->holding;
    unsigned seq = atomic_load_explicit(&pub->seq, memory_order_relaxed);
    atomic_store_explicit(&pub->seq, seq + 1, memory_order_relaxed);
    atomic_thread_fence(memory_order_release);

    atomic_store_explicit(&pub->frame_i, pos.i, memory_order_relaxed);
    atomic_store_explicit(&pub->frame_f, dbits(pos.f), memory_order_relaxed);
    atomic_store_explicit(&pub->speed,
                          dbits(d->playing && !waiting ? cb_player_deck_speed(d) : 0.0),
                          memory_order_relaxed);
    atomic_store_explicit(&pub->time_us, time_us, memory_order_relaxed);
    atomic_store_explicit(&pub->span_us, span_us, memory_order_relaxed);
    atomic_store_explicit(&pub->playing, d->playing, memory_order_relaxed);
    atomic_store_explicit(&pub->waiting, waiting, memory_order_relaxed);
    atomic_store_explicit(&pub->serial, d->applied, memory_order_relaxed);
    atomic_store_explicit(&pub->heard_id, cb_player_deck_heard_id(d), memory_order_relaxed);
    atomic_store_explicit(&pub->read_id, d->src.id, memory_order_relaxed);
    atomic_store_explicit(&pub->old_id, cb_player_deck_old_id(d), memory_order_relaxed);

    atomic_store_explicit(&pub->seq, seq + 2, memory_order_release);

    atomic_store_explicit(&pub->level_l, fbits(d->level_l), memory_order_relaxed);
    atomic_store_explicit(&pub->level_r, fbits(d->level_r), memory_order_relaxed);
}

/* Owner side. Sequence lock reader: an even value, the loads, an acquire
 * fence, the same value again. The acquire load of an even value
 * synchronises with the writer's final release store, which is what makes
 * a serial and the ids read here a safe acknowledgement for freeing a
 * buffer. */
static void snapshot(const CBDeckPub *cpub, CBDeckSnap *s)
{
    CBDeckPub *pub = (CBDeckPub *)cpub;
    int tries = 0;
    while (1) {
        unsigned s1 = atomic_load_explicit(&pub->seq, memory_order_acquire);
        if (!(s1 & 1)) {
            int64_t  fi = atomic_load_explicit(&pub->frame_i, memory_order_relaxed);
            uint64_t ff = atomic_load_explicit(&pub->frame_f, memory_order_relaxed);
            uint64_t sp = atomic_load_explicit(&pub->speed, memory_order_relaxed);
            *s = (CBDeckSnap) {
                .time_us  = atomic_load_explicit(&pub->time_us, memory_order_relaxed),
                .span_us  = atomic_load_explicit(&pub->span_us, memory_order_relaxed),
                .playing  = atomic_load_explicit(&pub->playing, memory_order_relaxed),
                .waiting  = atomic_load_explicit(&pub->waiting, memory_order_relaxed),
                .serial   = atomic_load_explicit(&pub->serial, memory_order_relaxed),
                .heard_id = atomic_load_explicit(&pub->heard_id, memory_order_relaxed),
                .read_id  = atomic_load_explicit(&pub->read_id, memory_order_relaxed),
                .old_id   = atomic_load_explicit(&pub->old_id, memory_order_relaxed),
            };
            atomic_thread_fence(memory_order_acquire);
            if (atomic_load_explicit(&pub->seq, memory_order_relaxed) == s1) {
                s->frames = (double)fi + bitsd(ff);
                s->speed  = bitsd(sp);
                return;
            }
        }
        /* The writer is a dozen stores long; if it was preempted in the
         * middle, let it run. */
        if (++tries > 64)
            sched_yield();
    }
}

/* ---------------------------------------------------------------- mixing */

/* Linear below 0.8, then a tanh knee that reaches 1.0 only at infinity:
 * two full-scale decks summed stay below full scale and the knee is
 * smooth, so nothing ever wraps or clips hard. */
static inline float soft_clip(float x)
{
    const float knee = 0.8f;
    float a = fabsf(x);
    if (a <= knee)
        return x;
    return copysignf(knee + (1.0f - knee) * tanhf((a - knee) / (1.0f - knee)), x);
}

void cb_player_mix(CBPlayer *p, float *out, int frames)
{
    int rate = atomic_load_explicit(&p->device_rate, memory_order_relaxed);
    int64_t time_us = now_us();

    size_t tail = atomic_load_explicit(&p->tail, memory_order_relaxed);
    size_t head = atomic_load_explicit(&p->head, memory_order_acquire);
    while (tail != head) {
        const CBCmd *cmd = &p->ring[tail & (CB_CMD_RING - 1)];
        if (cmd->type == CB_CMD_MASTER_VOLUME)
            p->master_gain = (float)cmd->a;
        else if (cmd->deck >= 0 && cmd->deck < CB_DECKS)
            cb_player_deck_apply(&p->rt[cmd->deck], cmd, rate);
        tail++;
    }
    atomic_store_explicit(&p->tail, tail, memory_order_release);

    if (frames > 0) {
        memset(out, 0, sizeof(*out) * 2 * frames);
        for (int i = 0; i < CB_DECKS; i++)
            cb_player_deck_render(&p->rt[i], out, frames, rate);

        float g0 = p->master_prev;
        float dg = (p->master_gain - g0) / frames;
        for (int n = 0; n < frames; n++) {
            float g = dg != 0.0f ? g0 + dg * (n + 1) : g0;
            out[2 * n + 0] = soft_clip(out[2 * n + 0] * g);
            out[2 * n + 1] = soft_clip(out[2 * n + 1] * g);
        }
        p->master_prev = p->master_gain;
    }

    for (int i = 0; i < CB_DECKS; i++) {
        cb_player_deck_signal(&p->rt[i]);
        publish(&p->pub[i], &p->rt[i], time_us, rate > 0 ? (int64_t)frames * 1000000 / rate : 0);
    }
}

/* ------------------------------------------------------------ owner side */

static void release_audio(CBPlayer *p, CBAudio *audio)
{
    if (p->opt.release) {
        p->opt.release(p->opt.release_opaque, audio);
        return;
    }
    cb_audio_free(audio);
    free(audio);
}

static void release_track(CBPlayer *p, CBOwnTrack *t)
{
    if (t->audio)
        release_audio(p, t->audio);
    cb_player_stream_free(t->stream);
    free(t->grid.sections);
    *t = (CBOwnTrack) { 0 };
}

/* The deck lets go of track t (which is emptied): into the pending list
 * until the deck has applied every command sent so far and reads it no
 * more. `follows`: a next dropped while the deck may have moved on into
 * it already, the id of the track it was queued after; else 0. */
static void defer_release(CBPlayer *p, int deck, CBOwnTrack *t, uint64_t follows)
{
    if (!t->id && !t->audio && !t->stream) {
        free(t->grid.sections);
        *t = (CBOwnTrack) { 0 };
        return;
    }
    if (p->nb_releases == p->releases_cap) {
        int cap = p->releases_cap ? 2 * p->releases_cap : 8;
        CBRelease *r = realloc(p->releases, sizeof(*r) * cap);
        if (!r) {
            /* Freeing now could pull the buffer from under the real-time
             * thread; leaking it is the safe failure. */
            cb_err("player: out of memory, leaking a released track buffer");
            *t = (CBOwnTrack) { 0 };
            return;
        }
        p->releases = r;
        p->releases_cap = cap;
    }
    p->releases[p->nb_releases++] = (CBRelease) {
        .t       = *t,
        .deck    = deck,
        .serial  = p->own[deck].sent,
        .gen     = p->own[deck].feed_gen,
        .follows = follows,
    };
    *t = (CBOwnTrack) { 0 };
}

/* The feed fills the stream the deck reads and, while the deck may still
 * move on into it, the start of the one queued to follow. A stream it is
 * no longer told to fill waits for the generation that told it so. */
static void feed_sync(CBPlayer *p, int deck)
{
    CBDeckOwner *o = &p->own[deck];
    CBStream *s = o->ahead.id ? o->ahead.stream : o->cur.stream;
    CBStream *n = o->ahead.id ? NULL : o->next.stream;
    if (s == o->feed_stream && n == o->feed_next)
        return;
    uint64_t gen = cb_player_feed_set(&p->feed[deck], s, n);
    for (int i = 0; i < p->nb_releases; i++) {
        CBRelease *r = &p->releases[i];
        if (r->deck == deck && r->t.stream &&
            (r->t.stream == o->feed_stream || r->t.stream == o->feed_next))
            r->gen = gen;
    }
    /* A new stream drops the feed's cue points: they are sent again. */
    if (s != o->feed_stream)
        o->nb_points = 0;
    o->feed_stream = s;
    o->feed_next   = n;
    o->feed_gen    = gen;
}

/* The deck lets go of the track queued to follow, which may have been
 * taken already when `taken` says so (cb_deck_clear_next, a queue over it:
 * the owner asks without knowing whether the deck has moved on into it). */
static void drop_next(CBPlayer *p, int deck, bool taken)
{
    CBDeckOwner *o = &p->own[deck];
    if (!o->next.id)
        return;
    defer_release(p, deck, &o->next, taken ? o->cur.id : 0);
    feed_sync(p, deck);
}

/* No pending next can be the deck's any more: it has left the track they
 * followed (a load, an unload, a move into another). */
static void forget_dropped(CBPlayer *p, int deck)
{
    for (int i = 0; i < p->nb_releases; i++) {
        if (p->releases[i].deck == deck)
            p->releases[i].follows = 0;
    }
}

/* The owner's side of what the real-time side did by itself: moving on
 * into the next, from one snapshot, in two steps.
 *  - The deck reads another track than cur: the next queued, or one
 *    dropped from the queue after the deck had taken it (the command that
 *    dropped it came too late, and the deck dropped that command). That
 *    track is `ahead` now. Whatever else is queued followed cur, which the
 *    deck has left: the deck dropped its command, and it never plays. The
 *    feed fills the track read.
 *  - The deck's sound is that track's (heard: with MASTER TEMPO on, the
 *    shifter's delay later): it becomes cur, the state is its own and
 *    `advanced` counts the move; the track before is let go of, and so
 *    are its loop and a position asked for in it (the deck dropped those
 *    commands too).
 * Nothing here goes by timing: a track is the deck's when the deck says it
 * reads it, and a dropped next is let go of for good once the deck shows
 * it applied the drop reading another track. */
static void sync_deck(CBPlayer *p, int deck, const CBDeckSnap *s)
{
    CBDeckOwner *o = &p->own[deck];

    for (int i = 0; i < p->nb_releases; i++) {
        CBRelease *r = &p->releases[i];
        if (r->deck == deck && r->follows && s->serial >= r->serial && s->read_id != r->t.id)
            r->follows = 0;
    }

    if (o->cur.id && !o->ahead.id && s->read_id && s->read_id != o->cur.id) {
        if (o->next.id == s->read_id) {
            o->ahead = o->next;
            o->next = (CBOwnTrack) { 0 };
        } else {
            for (int i = 0; i < p->nb_releases; i++) {
                CBRelease *r = &p->releases[i];
                if (r->deck == deck && r->follows == o->cur.id && r->t.id == s->read_id) {
                    o->ahead = r->t;
                    *r = p->releases[--p->nb_releases];
                    break;
                }
            }
        }
        if (o->ahead.id) {
            drop_next(p, deck, false);
            forget_dropped(p, deck);
            feed_sync(p, deck);
        }
    }

    if (o->ahead.id && s->heard_id == o->ahead.id) {
        defer_release(p, deck, &o->cur, 0);
        o->cur   = o->ahead;
        o->ahead = (CBOwnTrack) { 0 };
        o->advanced++;
        o->nb_points    = 0;
        o->loop_enabled = false;
        o->loop_in      = -1.0;
        o->loop_out     = -1.0;
        o->pos_serial   = 0;
        feed_sync(p, deck);
    }
}

/* A pending track the deck can no longer reach. */
static bool releasable(CBPlayer *p, const CBRelease *r, const CBDeckSnap *s)
{
    if (s->serial < r->serial || s->read_id == r->t.id || s->old_id == r->t.id)
        return false;
    if (!r->t.stream)
        return true;
    /* A stream also waits for its feed to have moved on */
    const CBDeckOwner *o = &p->own[r->deck];
    return r->t.stream != o->feed_stream && r->t.stream != o->feed_next &&
           atomic_load_explicit(&p->feed[r->deck].acked, memory_order_acquire) >= r->gen;
}

static void drain_releases(CBPlayer *p)
{
    CBDeckSnap snaps[CB_DECKS];
    for (int d = 0; d < CB_DECKS; d++) {
        snapshot(&p->pub[d], &snaps[d]);
        sync_deck(p, d, &snaps[d]);
    }
    for (int i = 0; i < p->nb_releases;) {
        CBRelease *r = &p->releases[i];
        if (!releasable(p, r, &snaps[r->deck])) {
            i++;
            continue;
        }
        release_track(p, &r->t);
        *r = p->releases[--p->nb_releases];
    }
}

/* Pushes a command as it is. A caller that decides what to send from the
 * owner's view of the deck (which track is loaded, whether one is queued)
 * syncs that view first (drain_releases) and pushes with this, so that
 * no sync between the decision and the push changes the view under it. */
static int push_now(CBPlayer *p, CBCmd cmd)
{
    size_t head = atomic_load_explicit(&p->head, memory_order_relaxed);
    size_t tail = atomic_load_explicit(&p->tail, memory_order_acquire);
    if (head - tail >= CB_CMD_RING) {
        if (!p->ring_full_warned)
            cb_warn("player: command queue full (is the audio output running?), dropping commands");
        p->ring_full_warned = true;
        return CB_ERROR(EAGAIN);
    }
    p->ring_full_warned = false;

    p->ring[head & (CB_CMD_RING - 1)] = cmd;
    atomic_store_explicit(&p->head, head + 1, memory_order_release);
    if (cmd.deck >= 0)
        p->own[cmd.deck].sent++;
    return 0;
}

/* The pending list drained (which never sends a command), then the push. */
static int push(CBPlayer *p, CBCmd cmd)
{
    drain_releases(p);
    return push_now(p, cmd);
}

/* A command that carries a track (push_now: the caller has synced): the
 * track's id is the command's deck serial. */
static int push_track(CBPlayer *p, CBCmd cmd, uint64_t *id)
{
    cmd.id = p->own[cmd.deck].sent + 1;
    int ret = push_now(p, cmd);
    if (ret == 0)
        *id = cmd.id;
    return ret;
}

/* Most commands are a type, a deck and at most one value. */
static int send(CBPlayer *p, enum CBCmdType type, int deck, double a)
{
    return push(p, (CBCmd) {
        .type = type,
        .deck = deck,
        .a    = a,
    });
}

/* A stream's length is the container's until its index is complete; the
 * real-time side takes the count at the same time (deck.c). */
static void refresh_length(CBDeckOwner *o)
{
    if (o->cur.stream)
        o->cur.nb_frames = cb_packets_length(o->cur.stream->pk, NULL);
}

static CBDeckOwner *loaded_deck(CBPlayer *p, int deck)
{
    if (!p || deck < 0 || deck >= CB_DECKS || !p->own[deck].cur.id)
        return NULL;
    refresh_length(&p->own[deck]);
    return &p->own[deck];
}

/* The same, synced, for a command that is only meant for the track loaded
 * (a seek, a loop, the cue points; pushed with push_now): none while the
 * deck reads the next already and the move is not heard yet. The deck
 * would drop the command; not sending it keeps the owner's state from
 * showing what never comes. */
static CBDeckOwner *track_deck(CBPlayer *p, int deck)
{
    if (!p)
        return NULL;
    drain_releases(p);
    CBDeckOwner *o = loaded_deck(p, deck);
    return o && !o->ahead.id ? o : NULL;
}

static double duration_of(const CBDeckOwner *o)
{
    return (double)o->cur.nb_frames / o->cur.rate;
}

/* Seconds to frames, clamped to the track. */
static double to_frames(const CBDeckOwner *o, double seconds)
{
    double t = CB_CLAMP(seconds, 0.0, duration_of(o));
    return t * o->cur.rate;
}

/* The deck lets go of what it holds: the track loaded, the one it reads
 * ahead of it and the one queued after it. */
static void drop_loaded(CBPlayer *p, int deck)
{
    CBDeckOwner *o = &p->own[deck];
    drop_next(p, deck, false);
    defer_release(p, deck, &o->ahead, 0);
    defer_release(p, deck, &o->cur, 0);
    forget_dropped(p, deck);
    o->nb_points = 0;
}

/* A next still pending: queued, read already, or dropped while the deck
 * may have moved on into it. */
static bool next_pending(const CBPlayer *p, int deck)
{
    const CBDeckOwner *o = &p->own[deck];
    if (o->next.id || o->ahead.id)
        return true;
    for (int i = 0; i < p->nb_releases; i++) {
        const CBRelease *r = &p->releases[i];
        if (r->deck == deck && r->follows && r->follows == o->cur.id)
            return true;
    }
    return false;
}

static void owner_reset(CBDeckOwner *o)
{
    o->pos_frames   = 0.0;
    o->playing      = false;
    o->bend         = 0.0;
    o->loop_enabled = false;
    o->loop_in      = -1.0;
    o->loop_out     = -1.0;
    o->scrubbing    = false;
    o->scrub_resume = false;
}

/* ------------------------------------------------------------ the player */

void cb_player_options_default(CBPlayerOptions *o)
{
    *o = (CBPlayerOptions) {
        .backend  = CB_AUDIO_PIPEWIRE,
        .rate     = 48000,
        .period   = 256,
        .app_name = "cyanbox",
    };
}

int cb_player_create(CBPlayer **out, const CBPlayerOptions *opt)
{
    int ret = 0;
    CBPlayerOptions def;
    if (!opt) {
        cb_player_options_default(&def);
        opt = &def;
    }

    *out = NULL;
    if (opt->rate < 0 || opt->rate > 768000)
        return CB_ERROR(EINVAL);
    if (opt->period < -65536 || opt->period > 65536 ||
        (opt->backend != CB_AUDIO_NULL && opt->period < 0))
        return CB_ERROR(EINVAL);

    size_t size = CB_ALIGN(sizeof(CBPlayer), 64);
    CBPlayer *p = aligned_alloc(64, size);
    if (!p)
        return CB_ERROR(ENOMEM);
    memset(p, 0, size);

    p->opt = *opt;
    p->sink = cb_strdup(opt->sink);
    p->app_name = cb_strdup(opt->app_name ? opt->app_name : "cyanbox");
    if ((opt->sink && !p->sink) || !p->app_name) {
        ret = CB_ERROR(ENOMEM);
        goto fail;
    }
    p->opt.sink = p->sink;
    p->opt.app_name = p->app_name;
    if (!p->opt.rate)
        p->opt.rate = 48000;
    p->manual = opt->backend == CB_AUDIO_NULL && opt->period < 0;
    p->period = opt->period ? abs(opt->period) : 256;

    atomic_init(&p->head, 0);
    atomic_init(&p->tail, 0);
    atomic_init(&p->device_rate, p->opt.rate);
    atomic_init(&p->out_state, CB_OUT_CONNECTING);
    atomic_init(&p->error_set, false);
    p->master_gain = p->master_prev = 1.0f;
    p->master_volume = 1.0f;
    for (int i = 0; i < CB_DECKS; i++) {
        cb_player_deck_init(&p->rt[i]);
        CBDeckOwner *o = &p->own[i];
        owner_reset(o);
        o->tempo  = 1.0;
        o->volume = 1.0f;
        atomic_init(&p->pub[i].seq, 0);
    }

    /* Without a shifter the player still plays, as a CDJ with MASTER TEMPO
     * off; cb_deck_set_master_tempo then says no. */
    p->master_tempo_ok = true;
    for (int i = 0; i < CB_DECKS && p->master_tempo_ok; i++) {
        int err = cb_player_deck_mt_alloc(&p->rt[i], p->opt.rate);
        if (err < 0) {
            cb_warn("player: no MASTER TEMPO, the pitch shifter could not be set up: %s",
                    strerror(-err));
            p->master_tempo_ok = false;
        }
    }
    if (!p->master_tempo_ok) {
        for (int i = 0; i < CB_DECKS; i++)
            cb_player_deck_mt_free(&p->rt[i]);
    }

    for (int i = 0; i < CB_DECKS; i++) {
        ret = cb_player_feed_start(&p->feed[i]);
        if (ret < 0)
            goto fail;
        p->rt[i].wake = &p->feed[i].wake;
    }

    if (opt->backend == CB_AUDIO_NULL)
        ret = cb_player_null_open(p);
    else if (opt->backend == CB_AUDIO_RING)
        ret = cb_player_ring_open(p);
    else
        ret = cb_player_pw_open(p);
    if (ret < 0)
        goto fail;

    *out = p;
    return 0;

fail:
    cb_player_destroy(p);
    return ret;
}

void cb_player_destroy(CBPlayer *p)
{
    if (!p)
        return;

    /* The backend's thread is joined first, then the feeds; after that
     * nothing reads the buffers and every one of them can go. */
    if (p->null)
        cb_player_null_close(p);
    if (p->ring_out)
        cb_player_ring_close(p);
    if (p->pw)
        cb_player_pw_close(p);
    for (int i = 0; i < CB_DECKS; i++)
        cb_player_feed_stop(&p->feed[i]);

    for (int i = 0; i < p->nb_releases; i++)
        release_track(p, &p->releases[i].t);
    free(p->releases);
    for (int i = 0; i < CB_DECKS; i++) {
        release_track(p, &p->own[i].cur);
        release_track(p, &p->own[i].next);
        release_track(p, &p->own[i].ahead);
        cb_player_deck_mt_free(&p->rt[i]);
    }
    free(p->sink);
    free(p->app_name);
    free(p);
}

int cb_player_rate(const CBPlayer *p)
{
    return atomic_load_explicit(&p->device_rate, memory_order_relaxed);
}

enum CBAudioBackend cb_player_backend(const CBPlayer *p)
{
    return p->opt.backend;
}

const char *cb_player_sink(const CBPlayer *p)
{
    return p->sink;
}

bool cb_player_running(const CBPlayer *p)
{
    return p && atomic_load_explicit(&p->out_state, memory_order_acquire) == CB_OUT_RUNNING;
}

const char *cb_player_error(const CBPlayer *p)
{
    if (!p)
        return "no player";
    /* The acquire load pairs with the backend's release store of ERROR,
     * after which the message is never written again. */
    switch (atomic_load_explicit(&p->out_state, memory_order_acquire)) {
    case CB_OUT_RUNNING:
        return NULL;
    case CB_OUT_CONNECTING:
        return "connecting to the audio server";
    case CB_OUT_PAUSED:
        return "audio stream paused (not linked to a sink yet)";
    case CB_OUT_UNCONNECTED:
        return "audio stream disconnected";
    case CB_OUT_ERROR:
    default:
        return p->error[0] ? p->error : "audio stream error";
    }
}

void cb_player_set_master_volume(CBPlayer *p, float gain)
{
    if (!p || !isfinite(gain))
        return;
    gain = CB_CLAMP(gain, 0.0f, 1.0f);
    if (send(p, CB_CMD_MASTER_VOLUME, -1, gain) == 0)
        p->master_volume = gain;
}

/* Manual mode: before a callback, each deck playing a stream waits for its
 * feed to have decoded everything around where it reads (see
 * cb_player_null_advance). Here, the owner is also the real-time thread. */
static void feeds_settle(CBPlayer *p)
{
    /* The commands applied, and the feeds told where the decks are; then
     * the owner's side of a change of track they may have made (a PLAY
     * at the end of a track with a next queued: sync_deck gives the feed
     * the stream the deck reads now). */
    cb_player_mix(p, NULL, 0);
    drain_releases(p);
    bool any = false;
    for (int i = 0; i < CB_DECKS; i++)
        any |= p->rt[i].src.stream || p->own[i].feed_stream;
    if (!any)
        return;
    for (int i = 0; i < CB_DECKS; i++) {
        CBStream *s = p->rt[i].src.stream;
        if (s && !cb_player_feed_wait(&p->feed[i], s, 10000000))
            cb_warn("player: deck %d: the feed did not catch up in 10 s", i);
    }
}

void cb_player_null_advance(CBPlayer *p, int frames)
{
    if (!p || !p->manual || frames <= 0)
        return;
    while (frames > 0) {
        int len = CB_MIN(frames, p->period);
        feeds_settle(p);
        cb_player_null_run(p, len);
        frames -= len;
    }
    drain_releases(p);
}

/* ----------------------------------------------------------------- decks */

static void copy_grid(CBGrid *dst, const CBGrid *src)
{
    free(dst->sections);
    *dst = (CBGrid) { .beats_per_bar = 4 };
    if (!src || src->nb_sections <= 0 || !src->sections)
        return;
    dst->sections = malloc(sizeof(*dst->sections) * src->nb_sections);
    if (!dst->sections)
        return;
    memcpy(dst->sections, src->sections, sizeof(*dst->sections) * src->nb_sections);
    dst->nb_sections   = src->nb_sections;
    dst->beats_per_bar = src->beats_per_bar;
}

int cb_player_deck_set_grid(CBPlayer *p, int deck, const CBGrid *grid)
{
    if (!p || deck < 0 || deck >= CB_DECKS)
        return CB_ERROR(EINVAL);
    copy_grid(&p->own[deck].cur.grid, grid);
    if (grid && grid->nb_sections > 0 && !p->own[deck].cur.grid.sections)
        return CB_ERROR(ENOMEM);
    return 0;
}

const CBGrid *cb_player_deck_grid(const CBPlayer *p, int deck)
{
    if (!p || deck < 0 || deck >= CB_DECKS)
        return NULL;
    return &p->own[deck].cur.grid;
}

/* A load: pushed with the track's id, then what the deck held goes. */
static int load(CBPlayer *p, int deck, CBOwnTrack t, const CBGrid *grid)
{
    CBGrid copy = { 0 };
    copy_grid(&copy, grid);
    if (grid && grid->nb_sections > 0 && !copy.sections)
        return CB_ERROR(ENOMEM);

    drain_releases(p);
    CBStream *s = t.stream;
    int ret = push_track(p, (CBCmd) {
        .type      = CB_CMD_LOAD,
        .deck      = deck,
        .samples   = t.audio ? t.audio->samples : NULL,
        .stream    = s,
        .channels  = t.audio ? t.audio->channels : s->channels,
        .rate      = t.rate,
        .nb_frames = t.nb_frames,
    }, &t.id);
    if (ret < 0) {
        free(copy.sections);
        return ret;
    }

    CBDeckOwner *o = &p->own[deck];
    drop_loaded(p, deck);
    t.grid = copy;
    o->cur = t;
    /* The feed starts decoding at once: the real-time side reads nothing
     * of the stream before it applies the load. */
    feed_sync(p, deck);
    owner_reset(o);
    o->pos_serial = o->play_serial = o->sent;
    return 0;
}

int cb_deck_load(CBPlayer *p, int deck, CBAudio *audio, const CBGrid *grid)
{
    if (!p || deck < 0 || deck >= CB_DECKS || !audio)
        return CB_ERROR(EINVAL);
    if (audio->channels < 1 || audio->rate <= 0 || audio->nb_frames < 0 ||
        (audio->nb_frames > 0 && !audio->samples))
        return CB_ERROR(EINVAL);
    return load(p, deck, (CBOwnTrack) {
        .audio     = audio,
        .rate      = audio->rate,
        .nb_frames = audio->nb_frames,
    }, grid);
}

int cb_deck_load_packets(CBPlayer *p, int deck, CBPackets *pk, const CBGrid *grid)
{
    if (!p || deck < 0 || deck >= CB_DECKS || !pk)
        return CB_ERROR(EINVAL);

    CBStream *s;
    int ret = cb_player_stream_create(&s, pk);
    if (ret < 0)
        return ret;
    ret = load(p, deck, (CBOwnTrack) {
        .stream    = s,
        .rate      = s->rate,
        .nb_frames = cb_packets_length(pk, NULL),
    }, grid);
    if (ret < 0) {
        /* The caller keeps the packets. */
        s->pk = NULL;
        cb_player_stream_free(s);
    }
    return ret;
}

/* The track to follow: pushed as a load is, with the id of the track it
 * follows, then kept beside the loaded one; what was queued before goes
 * to the pending list, where it stays the deck's if the deck had moved
 * on into it already. */
static int queue_next(CBPlayer *p, int deck, CBOwnTrack t, const CBGrid *grid)
{
    drain_releases(p);
    CBDeckOwner *o = loaded_deck(p, deck);
    if (!o)
        return CB_ERROR(ENOENT);
    if (o->ahead.id)
        return CB_ERROR(EBUSY);

    CBGrid copy = { 0 };
    copy_grid(&copy, grid);
    if (grid && grid->nb_sections > 0 && !copy.sections)
        return CB_ERROR(ENOMEM);

    CBStream *s = t.stream;
    int ret = push_track(p, (CBCmd) {
        .type      = CB_CMD_NEXT,
        .deck      = deck,
        .track     = o->cur.id,
        .samples   = t.audio ? t.audio->samples : NULL,
        .stream    = s,
        .channels  = t.audio ? t.audio->channels : s->channels,
        .rate      = t.rate,
        .nb_frames = t.nb_frames,
    }, &t.id);
    if (ret < 0) {
        free(copy.sections);
        return ret;
    }

    drop_next(p, deck, true);
    t.grid = copy;
    o->next = t;
    feed_sync(p, deck);
    return 0;
}

int cb_deck_queue_next(CBPlayer *p, int deck, CBAudio *audio, const CBGrid *grid)
{
    if (!p || deck < 0 || deck >= CB_DECKS || !audio)
        return CB_ERROR(EINVAL);
    if (audio->channels < 1 || audio->rate <= 0 || audio->nb_frames < 0 ||
        (audio->nb_frames > 0 && !audio->samples))
        return CB_ERROR(EINVAL);
    return queue_next(p, deck, (CBOwnTrack) {
        .audio     = audio,
        .rate      = audio->rate,
        .nb_frames = audio->nb_frames,
    }, grid);
}

int cb_deck_queue_next_packets(CBPlayer *p, int deck, CBPackets *pk, const CBGrid *grid)
{
    if (!p || deck < 0 || deck >= CB_DECKS || !pk)
        return CB_ERROR(EINVAL);
    if (!loaded_deck(p, deck))
        return CB_ERROR(ENOENT);
    CBStream *s;
    int ret = cb_player_stream_create(&s, pk);
    if (ret < 0)
        return ret;
    ret = queue_next(p, deck, (CBOwnTrack) {
        .stream    = s,
        .rate      = s->rate,
        .nb_frames = cb_packets_length(pk, NULL),
    }, grid);
    if (ret < 0) {
        /* The caller keeps the packets. */
        s->pk = NULL;
        cb_player_stream_free(s);
    }
    return ret;
}

void cb_deck_clear_next(CBPlayer *p, int deck)
{
    if (!p || deck < 0 || deck >= CB_DECKS)
        return;
    drain_releases(p);
    CBDeckOwner *o = &p->own[deck];
    /* Read already (ahead): too late, the deck plays it. */
    if (!o->next.id)
        return;
    CBCmd cmd = {
        .type  = CB_CMD_NEXT,
        .deck  = deck,
        .track = o->cur.id,
    };
    if (push_now(p, cmd) < 0)
        return;
    drop_next(p, deck, true);
}

bool cb_deck_has_next(const CBPlayer *cp, int deck)
{
    if (!cp || deck < 0 || deck >= CB_DECKS)
        return false;
    CBPlayer *p = (CBPlayer *)cp;
    drain_releases(p);
    return next_pending(p, deck);
}

void cb_deck_set_cue_points(CBPlayer *p, int deck, const double *seconds, int nb)
{
    CBDeckOwner *o = track_deck(p, deck);
    if (!o || !o->cur.stream)
        return;
    int64_t frames[CB_DECK_CUE_POINTS];
    int n = 0;
    for (int i = 0; i < nb && n < CB_DECK_CUE_POINTS; i++) {
        if (isfinite(seconds[i]) && seconds[i] >= 0.0)
            frames[n++] = (int64_t)floor(CB_MIN(seconds[i], duration_of(o)) * o->cur.rate);
    }
    if (n == o->nb_points && !memcmp(frames, o->points, sizeof(frames[0]) * n))
        return;
    memcpy(o->points, frames, sizeof(frames[0]) * n);
    o->nb_points = n;
    cb_player_feed_points(&p->feed[deck], frames, n);
}

void cb_deck_unload(CBPlayer *p, int deck)
{
    CBDeckOwner *o = loaded_deck(p, deck);
    if (!o || send(p, CB_CMD_UNLOAD, deck, 0.0) < 0)
        return;
    drop_loaded(p, deck);
    feed_sync(p, deck);
    owner_reset(o);
    o->pos_serial = o->play_serial = o->sent;
}

bool cb_deck_loaded(const CBPlayer *p, int deck)
{
    return p && deck >= 0 && deck < CB_DECKS && p->own[deck].cur.id;
}

const CBAudio *cb_deck_audio(const CBPlayer *p, int deck)
{
    if (!p || deck < 0 || deck >= CB_DECKS)
        return NULL;
    return p->own[deck].cur.audio;
}

/* The owner's view of the playing state right now. */
static bool owner_playing(CBPlayer *p, int deck)
{
    CBDeckSnap s;
    snapshot(&p->pub[deck], &s);
    const CBDeckOwner *o = &p->own[deck];
    return s.serial < o->play_serial ? o->playing : s.playing;
}

static void set_playing(CBPlayer *p, int deck, enum CBCmdType type, bool playing)
{
    CBDeckOwner *o = loaded_deck(p, deck);
    if (!o || send(p, type, deck, 0.0) < 0)
        return;
    if (o->scrubbing) {
        o->scrub_resume = playing;
        return;
    }
    o->playing = playing;
    o->play_serial = o->sent;
}

void cb_deck_play(CBPlayer *p, int deck)
{
    set_playing(p, deck, CB_CMD_PLAY, true);
}

void cb_deck_pause(CBPlayer *p, int deck)
{
    set_playing(p, deck, CB_CMD_PAUSE, false);
}

static void seek_cmd(CBPlayer *p, int deck, double seconds, enum CBCmdType type)
{
    CBDeckOwner *o = track_deck(p, deck);
    if (!o || !isfinite(seconds))
        return;
    double frames = to_frames(o, seconds);
    CBCmd cmd = {
        .type  = type,
        .deck  = deck,
        .a     = frames,
        .track = o->cur.id,
    };
    if (push_now(p, cmd) < 0)
        return;
    o->pos_frames = frames;
    o->pos_serial = o->sent;
    /* The feed hears of it now and decodes there while the real-time side
     * has yet to apply it: a callback's head start, usually enough for
     * the deck not to wait at all */
    if (o->cur.stream) {
        atomic_store_explicit(&o->cur.stream->hint, (int64_t)frames, memory_order_relaxed);
        sem_post(&p->feed[deck].wake);
    }
    if (type == CB_CMD_PLAY_FROM) {
        if (o->scrubbing) {
            o->scrub_resume = true;
        } else {
            o->playing = frames < (double)o->cur.nb_frames;
            o->play_serial = o->sent;
        }
    } else if (frames >= (double)o->cur.nb_frames && !o->scrubbing && !o->next.id) {
        /* A seek to the very end stops the deck there (with a next track
         * queued, a playing deck goes on into it). */
        o->playing = false;
        o->play_serial = o->sent;
    }
}

void cb_deck_seek(CBPlayer *p, int deck, double seconds)
{
    seek_cmd(p, deck, seconds, CB_CMD_SEEK);
}

void cb_deck_play_from(CBPlayer *p, int deck, double seconds)
{
    seek_cmd(p, deck, seconds, CB_CMD_PLAY_FROM);
}

void cb_deck_set_tempo(CBPlayer *p, int deck, double ratio)
{
    if (!p || deck < 0 || deck >= CB_DECKS || !isfinite(ratio))
        return;
    /* Wider than any CDJ range (+-100 %), and never backwards. */
    ratio = CB_CLAMP(ratio, 0.0, 4.0);
    if (send(p, CB_CMD_TEMPO, deck, ratio) == 0)
        p->own[deck].tempo = ratio;
}

bool cb_deck_set_master_tempo(CBPlayer *p, int deck, bool on)
{
    if (!p || deck < 0 || deck >= CB_DECKS || (on && !p->master_tempo_ok))
        return false;
    CBCmd cmd = {
        .type = CB_CMD_MASTER_TEMPO,
        .deck = deck,
        .flag = on,
    };
    if (push(p, cmd) < 0)
        return false;
    p->own[deck].master_tempo = on;
    return true;
}

void cb_deck_set_loop(CBPlayer *p, int deck, double in, double out, bool enabled)
{
    CBDeckOwner *o = track_deck(p, deck);
    if (!o || !isfinite(in) || !isfinite(out))
        return;
    in  = CB_CLAMP(in, 0.0, duration_of(o));
    out = CB_CLAMP(out, 0.0, duration_of(o));
    /* At least one frame long, or the loop could never be left. */
    if ((out - in) * o->cur.rate < 1.0)
        return;
    CBCmd cmd = {
        .type  = CB_CMD_LOOP_SET,
        .deck  = deck,
        .a     = in * o->cur.rate,
        .b     = out * o->cur.rate,
        .flag  = enabled,
        .track = o->cur.id,
    };
    if (push_now(p, cmd) < 0)
        return;
    o->loop_in = in;
    o->loop_out = out;
    o->loop_enabled = enabled;
}

void cb_deck_loop_enable(CBPlayer *p, int deck, bool enabled)
{
    CBDeckOwner *o = track_deck(p, deck);
    if (!o || o->loop_in < 0.0)
        return;
    CBCmd cmd = {
        .type  = CB_CMD_LOOP_ENABLE,
        .deck  = deck,
        .flag  = enabled,
        .track = o->cur.id,
    };
    if (push_now(p, cmd) == 0)
        o->loop_enabled = enabled;
}

void cb_deck_clear_loop(CBPlayer *p, int deck)
{
    CBDeckOwner *o = track_deck(p, deck);
    if (!o)
        return;
    CBCmd cmd = {
        .type  = CB_CMD_LOOP_CLEAR,
        .deck  = deck,
        .track = o->cur.id,
    };
    if (push_now(p, cmd) < 0)
        return;
    o->loop_enabled = false;
    o->loop_in = o->loop_out = -1.0;
}

void cb_deck_set_volume(CBPlayer *p, int deck, float gain)
{
    if (!p || deck < 0 || deck >= CB_DECKS || !isfinite(gain))
        return;
    gain = CB_CLAMP(gain, 0.0f, 1.0f);
    if (send(p, CB_CMD_VOLUME, deck, gain) == 0)
        p->own[deck].volume = gain;
}

void cb_deck_set_bend(CBPlayer *p, int deck, double ratio_offset)
{
    if (!p || deck < 0 || deck >= CB_DECKS || !isfinite(ratio_offset))
        return;
    ratio_offset = CB_CLAMP(ratio_offset, -1.0, 1.0);
    if (send(p, CB_CMD_BEND, deck, ratio_offset) == 0)
        p->own[deck].bend = ratio_offset;
}

void cb_deck_scrub_begin(CBPlayer *p, int deck)
{
    CBDeckOwner *o = loaded_deck(p, deck);
    if (!o || o->scrubbing)
        return;
    bool was_playing = owner_playing(p, deck);
    if (send(p, CB_CMD_SCRUB_BEGIN, deck, 0.0) < 0)
        return;
    o->scrubbing = true;
    o->scrub_resume = was_playing;
    o->playing = false;
    o->play_serial = o->sent;
}

void cb_deck_scrub_to(CBPlayer *p, int deck, double seconds)
{
    seek_cmd(p, deck, seconds, CB_CMD_SCRUB_TO);
}

void cb_deck_scrub_end(CBPlayer *p, int deck)
{
    CBDeckOwner *o = loaded_deck(p, deck);
    if (!o || !o->scrubbing || send(p, CB_CMD_SCRUB_END, deck, 0.0) < 0)
        return;
    o->scrubbing = false;
    o->playing = o->scrub_resume && o->pos_frames < (double)o->cur.nb_frames;
    o->play_serial = o->sent;
}

void cb_deck_state(const CBPlayer *cp, int deck, CBDeckState *out)
{
    *out = (CBDeckState) {
        .tempo    = 1.0,
        .loop_in  = -1.0,
        .loop_out = -1.0,
    };
    if (!cp || deck < 0 || deck >= CB_DECKS)
        return;

    /* Logically const: the only thing changed is the release list, which
     * the owner thread alone touches. */
    CBPlayer *p = (CBPlayer *)cp;
    drain_releases(p);

    CBDeckOwner *o = &p->own[deck];
    const CBDeckPub *pub = &p->pub[deck];
    CBDeckSnap s;
    snapshot(pub, &s);
    /* The same snapshot for the owner's fields and the report: a change
     * of track between the two would put the new track's position
     * against the old one's length. */
    sync_deck(p, deck, &s);

    out->tempo        = o->tempo;
    out->master_tempo = o->master_tempo;
    out->serial       = s.serial;
    out->advanced     = o->advanced;
    out->has_next     = next_pending(p, deck);
    out->level_l      = bitsf(atomic_load_explicit(&pub->level_l, memory_order_relaxed));
    out->level_r      = bitsf(atomic_load_explicit(&pub->level_r, memory_order_relaxed));
    if (!o->cur.id)
        return;
    refresh_length(o);

    double frames = s.frames;
    bool playing = s.playing;
    bool pending = false;
    if (s.serial < o->pos_serial) {
        frames = o->pos_frames;
        pending = true;
    }
    if (s.serial < o->play_serial) {
        playing = o->playing;
        pending = true;
    }

    double end = (double)o->cur.nb_frames;
    /* Between callbacks the playhead moves on: extrapolate from the
     * callback's time stamp by at most the length of the block it rendered,
     * which is where the next callback will publish. A late callback then
     * holds the playhead still instead of making it step back, and a
     * stalled stream does not run away. Never past a loop's out point or
     * the end. The manual null backend is exact instead. */
    if (!pending && playing && !o->scrubbing && !p->manual && s.speed > 0.0) {
        double dt = CB_CLAMP((now_us() - s.time_us) * 1e-6, 0.0, s.span_us * 1e-6);
        double limit = end;
        double out_frames = o->loop_out * o->cur.rate;
        if (o->loop_enabled && frames < out_frames)
            limit = out_frames;
        frames = CB_MIN(frames + dt * s.speed, limit);
    }
    frames = CB_CLAMP(frames, 0.0, end);

    out->loaded       = true;
    out->playing      = playing;
    out->waiting      = playing && !pending && s.waiting;
    out->scrubbing    = o->scrubbing;
    out->position     = frames / o->cur.rate;
    out->duration     = duration_of(o);
    out->frame        = (int64_t)floor(frames);
    out->loop_enabled = o->loop_enabled;
    out->loop_in      = o->loop_in;
    out->loop_out     = o->loop_out;
}
