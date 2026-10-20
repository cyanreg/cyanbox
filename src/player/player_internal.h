/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* The player's private structures, shared by player.c (engine, command
 * ring, published state), deck.c (the real-time side of one deck), feed.c
 * (the thread that runs a deck's stream), pipewire.c and
 * nullout.c (the two backends) and controls.c.
 *
 * Three groups of state live in CBPlayer, and each has one writer:
 *  - CBDeckOwner: written and read only by the owner thread (the thread
 *    that calls the public functions). It is what the owner has asked for:
 *    the loaded buffer, the loop, the tempo, and the position or playing
 *    state of a command the real-time thread has not applied yet.
 *  - CBDeckRT: written and read only by the real-time thread, which also
 *    applies the commands. The owner never touches it after creation.
 *  - CBDeckPub: written by the real-time thread, read by the owner through
 *    C11 atomics.
 * A deck playing a stream adds a fourth party, its feed thread: the owner
 * hands it the stream and the cue points under the feed's lock (an
 * CBDeckFeed), the feed puts its pipelines' chunks in the stream's tables
 * of pointers and the real-time thread reads them, and lets go of one it
 * took out only once a callback has finished since (the epoch), and the
 * real-time thread says where it reads through the stream's atomics. The
 * command ring is single-producer (owner) / single-consumer
 * (real time). Nothing on the real-time side allocates, locks, logs or
 * does I/O; everything it needs is allocated before it runs.
 *
 * Tracks have ids: the deck serial of the LOAD or NEXT command that
 * carried a track to the real-time side, unique on the deck, 0 for none.
 * The owner decides which buffers it may free, and which track the deck
 * plays, by id, never by timing: the real-time side publishes the id of
 * the track it reads (and of one its de-click tail still reads), and the
 * commands that only make sense for one track carry its id. */

#ifndef CYANBOX_PLAYER_INTERNAL_H
#define CYANBOX_PLAYER_INTERNAL_H

#include "cyanbox/player.h"

#include <math.h>
#include <pthread.h>
#include <stdalign.h>
#include <stdatomic.h>

#include "stretch.h"

/* Commands in flight. One UI frame sends a handful; a drag sends one per
 * pointer event. 1024 is several seconds of a stalled output. */
#define CB_CMD_RING 1024

/* The de-click fade: a jump, pause or start crossfades over this long. A
 * loop wrap does not (it is sample-exact). */
#define CB_FADE_SECONDS 0.003

/* Scrub grains: 60 ms Hann windows, a new one every 30 ms while the drag
 * moves, so overlapping windows sum to a constant level. */
#define CB_GRAIN_SECONDS 0.060
#define CB_GRAINS 3
#define CB_GRAIN_GAIN 0.5f
#define CB_GRAIN_LOWPASS_HZ 3000.0f

/* The level meters hold a peak and let it fall with this time constant. */
#define CB_LEVEL_RELEASE_SECONDS 0.3f

/* MASTER TEMPO renders a deck in pieces of at most this many frames, which
 * is what its scratch buffers hold. A callback has no bound of ours:
 * PipeWire hands it as many frames as its buffer has room for (pipewire.c,
 * on_process), so a longer one is cut up rather than refused. */
#define CB_MT_CHUNK 1024

/* A deck playing a stream (feed.c has the why): the window of
 * decoded frames is cut in chunks of CB_CHUNK frames and spans
 * CB_WINDOW_BEHIND seconds behind the read position and CB_WINDOW_AHEAD
 * ahead of it; each cue point keeps CB_CUE_CACHE_SECONDS decoded from
 * CB_CUE_CACHE_LEAD frames before it. */
#define CB_CHUNK             4096
#define CB_WINDOW_BEHIND     4.0
#define CB_WINDOW_AHEAD      8.0
#define CB_CUE_CACHE_SECONDS 0.5
#define CB_CUE_CACHE_LEAD    16

/* ------------------------------------------------------------- positions */

/* A position in track frames: an integer frame and a fraction in [0, 1).
 * A double alone loses resolution as the frame count grows, and a constant
 * increment rounds the same way on every step, so the error would add up
 * linearly over an hour-long track. Kept apart, the fraction stays at full
 * precision and a loop wrap (subtracting the loop length) is exact. */
typedef struct CBPos {
    int64_t i;
    double  f;
} CBPos;

static inline CBPos cb_pos_from(double frames)
{
    double fl = floor(frames);
    return (CBPos) { .i = (int64_t)fl, .f = frames - fl };
}

static inline double cb_pos_get(CBPos p)
{
    return (double)p.i + p.f;
}

static inline void cb_pos_add(CBPos *p, double d)
{
    double fl = floor(d);
    p->i += (int64_t)fl;
    p->f += d - fl;
    if (p->f >= 1.0) {
        p->f -= 1.0;
        p->i++;
    }
}

static inline CBPos cb_pos_sub(CBPos a, CBPos b)
{
    CBPos r = { .i = a.i - b.i, .f = a.f - b.f };
    if (r.f < 0.0) {
        r.f += 1.0;
        r.i--;
        /* A difference within 2^-54 below zero plus the borrow rounds to
         * exactly 1.0: that is the next whole frame, and cb_pos_cmp needs
         * the fraction in [0, 1) to order positions by value. */
        if (r.f >= 1.0) {
            r.f -= 1.0;
            r.i++;
        }
    }
    return r;
}

static inline int cb_pos_cmp(CBPos a, CBPos b)
{
    if (a.i != b.i)
        return a.i < b.i ? -1 : 1;
    return (a.f > b.f) - (a.f < b.f);
}

/* -------------------------------------------------------------- commands */

enum CBCmdType {
    CB_CMD_LOAD = 0,
    CB_CMD_UNLOAD,
    CB_CMD_PLAY,
    CB_CMD_PAUSE,
    CB_CMD_SEEK,            /* a = frames */
    CB_CMD_PLAY_FROM,       /* a = frames */
    CB_CMD_TEMPO,           /* a = ratio */
    CB_CMD_LOOP_SET,        /* a = in, b = out (frames), flag = enabled */
    CB_CMD_LOOP_ENABLE,     /* flag */
    CB_CMD_LOOP_CLEAR,
    CB_CMD_VOLUME,          /* a = gain */
    CB_CMD_BEND,            /* a = ratio offset */
    CB_CMD_SCRUB_BEGIN,
    CB_CMD_SCRUB_TO,        /* a = frames */
    CB_CMD_SCRUB_END,
    CB_CMD_MASTER_VOLUME,   /* a = gain; deck = -1 */
    CB_CMD_MASTER_TEMPO,    /* flag */
    CB_CMD_NEXT,            /* the track to follow this one, as a load; neither
                               samples nor stream: none */
};

/* A command is copied into the ring by value. A load (and a next) carries
 * the buffer's fields rather than the CBAudio pointer, so the real-time
 * side never reads the owner's struct; or the stream; and the track's
 * id. A seek, a loop or a next carries the id of
 * the track the owner meant it for (`track`), and the deck drops it when
 * it no longer reads that track: it moved on into the next before the
 * command came, and the frames, or the order, are the old track's. */
typedef struct CBCmd {
    uint8_t      type;
    int8_t       deck;
    bool         flag;
    double       a, b;
    uint64_t     track;     /* the track meant; 0 any */
    uint64_t     id;        /* LOAD, NEXT: the track carried */
    const float *samples;
    CBStream    *stream;
    int          channels;
    int          rate;
    int64_t      nb_frames;
} CBCmd;

/* --------------------------------------------------------------- streams */

/* A chunk out of a table, held until the epoch is past `epoch`. */
typedef struct CBRetired {
    CBChunk *c;
    uint64_t epoch;
} CBRetired;

/* A track as a deck plays it (feed.c): its pipelines' chunks, where the
 * real-time side looks for them. Made and freed by the owner (or a job's
 * thread); while loaded, the feed thread alone fills it and the real-time
 * thread alone reads it, through the atomics. */
struct CBStream {
    int         rate, channels;
    CBPipeline *main, *aux;     /* the feed's */
    /* The window: chunk c in slots[c % nb_slots] */
    int         nb_slots;
    _Atomic(CBChunk *) *slots;
    /* The cue caches, and last the jump's: cache e holds the chunks from
     * cache_first[e] (-1: none) on, cpc of them, in cache[e * cpc ...] */
    int         cpc;
    int64_t     cache_len;      /* frames a cue cache holds from LEAD before its point */
    _Atomic(CBChunk *) *cache;
    _Atomic int64_t cache_first[CB_DECK_CUE_POINTS + 1];
    /* The container's length until the last chunk says (exact) */
    _Atomic int64_t length;
    atomic_bool     exact;
    /* Real time -> feed: the read position */
    _Atomic int64_t playhead;
    /* Owner -> feed: where a jump just sent will read, before the real-time
     * side gets to it; -1 none */
    _Atomic int64_t hint;

    /* The feed's */
    CBChunk    *held;           /* the main pipeline's next, past the window */
    int64_t     main_next;      /* the chunk the main pipeline gives next */
    int         main_seeks;     /* the main pipeline sought, counted (tests) */
    int         aux_job, aux_cue;
    int64_t     aux_next, aux_to;
    int64_t     want_first[CB_DECK_CUE_POINTS];    /* frames; -1 unused */
    bool        cache_done[CB_DECK_CUE_POINTS];
    bool        error_said;
    CBRetired  *retired;
    int         nb_retired, cap_retired;
};

/* One deck's feed thread (feed.c). The owner hands it the stream to fill,
 * the stream queued to follow it (whose window it fills too) and the cue
 * points under `lock`, and wakes it; it says which generation of the two
 * it has moved to, after which it no longer touches the streams of the
 * generations before. */
typedef struct CBDeckFeed {
    pthread_t thread;
    bool      started;
    int       wakefd;           /* an eventfd: the owner and the real-time side write it */
    const _Atomic uint64_t *epoch;  /* the player's */
    pthread_mutex_t lock;
    bool      lock_ok;
    /* under lock */
    CBStream *stream;
    CBStream *next;
    uint64_t  gen;
    bool      quit;
    int64_t   points[CB_DECK_CUE_POINTS];
    int       nb_points;
    uint64_t  points_gen;
    /* published by the feed thread */
    _Atomic uint64_t acked;     /* the generation it works on */
    _Atomic int64_t  settled;   /* the read position everything is decoded around, or -1 */
} CBDeckFeed;

/* ------------------------------------------------------- real-time decks */

typedef struct CBGrain {
    CBPos pos;
    int   age, len;         /* len == 0: idle */
} CBGrain;

/* A track as the real-time side reads it: a buffer (the owner's, read-only
 * here) or a stream, and its id; id 0 is none. */
typedef struct CBSource {
    const float *samples;
    CBStream    *stream;
    int          channels;
    int          rate;
    int64_t      nb_frames;
    uint64_t     id;
} CBSource;

/* MASTER TEMPO on the real-time side (deck.c has the why). Everything is
 * allocated by cb_player_create; st == NULL means the player has none. */
typedef struct CBDeckMT {
    CBStretch *st;
    int     latency;        /* frames from the shifter's input to its output */
    int     in_latency;     /* frames from its newest input to the block it analyses */
    float  *in[2], *out[2]; /* CB_MT_CHUNK frames: varispeed in, shifted out */
    float  *ahead[2];       /* latency frames: what a restart starts from */
    float  *xf[2];          /* CB_MT_CHUNK frames: the old sound of a crossfade */
    int     xf_len, xf_left;
    /* The read position, speed ratio (0 when stopped) and track (its id) of
     * every frame fed, a ring of hist_mask + 1: what is heard now was fed
     * `latency` ago. */
    CBPos    *hist;
    float    *hist_speed;
    uint64_t *hist_id;
    int64_t hist_mask;
    int64_t fed;            /* frames fed so far */
    int64_t fill_until;     /* frames fed before this one count as fill_pos, fill_id */
    CBPos   fill_pos;
    uint64_t fill_id;
    bool    on;             /* the shifter is in the deck's path */
    /* A restart is due at the next render. was_on: the shifter was in
     * before the first command that asked for it; when it was not, out[]
     * holds old_len frames of the dry voices as they would have played on,
     * rendered by that command. */
    bool    restart, was_on;
    int     old_len;
} CBDeckMT;

typedef struct CBDeckRT {
    CBSource     src;           /* the track the main voice plays */
    bool         length_final;  /* a stream whose last chunk came: nb_frames is its length */
    int          wake;          /* the deck's feed thread's eventfd; -1 none */
    int64_t      posted_chunk;  /* the read position's chunk the feed was last woken for */
    bool         starved;       /* this callback missed frames not decoded yet */
    bool         holding;       /* the main voice holds for them */
    int          out_rate;      /* the device's, as of the last callback */

    /* The track queued to follow this one (CB_CMD_NEXT), taken over the
     * frame after the last one of this; id 0 none. Its first frames are
     * this one's frames past the end, and after the change the old one's
     * last two are the new one's frames -2 and -1 (edge, stereo, when
     * has_edge): the interpolator reads across the change as within one
     * track. */
    CBSource     next;
    float        edge[4];
    bool         has_edge;
    /* The track the deck moved on from while its de-click tail still reads
     * it (tail_old): the owner keeps it until the deck says it is done. */
    CBSource     old;

    CBPos  pos;
    bool   playing;
    double tempo, bend;
    float  gain, gain_prev;     /* volume, ramped over one callback */

    bool   loop_set, loop_enabled;
    CBPos  loop_in, loop_out, loop_len;

    /* De-click: the main voice fades in after a start or a jump while the
     * tail voice fades out from where the deck was. */
    int    fade_in, fade_len;
    CBPos  tail_pos;
    double tail_inc;
    int    tail_left, tail_len;
    bool   tail_old;            /* the tail reads `old`, not `src` */

    /* Scrubbing: the deck follows the pointer and plays grains. */
    bool    scrubbing, scrub_resume;
    double  grain_from;         /* frames: where the last grain started */
    int     hop;                /* output frames until the next grain may start */
    CBGrain grains[CB_GRAINS];
    float   lp_l, lp_r;

    float    level_l, level_r;
    uint64_t applied;           /* commands applied to this deck */

    CBDeckMT mt;
} CBDeckRT;

/* ------------------------------------------------------- published state */

/* Written by the real-time thread at the end of every callback, read by
 * cb_deck_state(). Position, playing state and serial have to agree with
 * each other (the owner uses the serial to decide whether a seek it sent
 * has been applied), so they form a sequence lock: the writer makes `seq`
 * odd, stores, then makes it even with release order; the reader retries
 * until it sees the same even value before and after its loads. The writer
 * never waits. The meters are single values and need no such care. */
typedef struct CBDeckPub {
    alignas(64) atomic_uint seq;
    _Atomic int64_t  frame_i;
    _Atomic uint64_t frame_f;       /* double bits */
    _Atomic uint64_t speed;         /* double bits: track frames per second, 0 when stopped */
    _Atomic int64_t  time_us;       /* CLOCK_MONOTONIC at the callback */
    _Atomic int64_t  span_us;       /* duration of the block it rendered */
    atomic_bool      playing;
    atomic_bool      waiting;       /* playing, holding for the feed */
    _Atomic uint64_t serial;
    _Atomic uint64_t heard_id;      /* the track the position is in (heard, like it) */
    _Atomic uint64_t read_id;       /* the track the deck reads */
    _Atomic uint64_t old_id;        /* one its de-click tail still reads, 0 none */
    atomic_uint      level_l, level_r;  /* float bits */
} CBDeckPub;

typedef struct CBDeckSnap {
    double   frames;
    double   speed;
    int64_t  time_us, span_us;
    bool     playing;
    bool     waiting;
    uint64_t serial;
    uint64_t heard_id, read_id, old_id;
} CBDeckSnap;

/* ------------------------------------------------------------ owner side */

/* A track the owner holds: its buffer, or its stream, the copy of the grid
 * given with it, and its id (0: none). */
typedef struct CBOwnTrack {
    CBAudio  *audio;
    CBStream *stream;
    int       rate;
    int64_t   nb_frames;        /* its length (a stream's as last read) */
    CBGrid    grid;
    uint64_t  id;
} CBOwnTrack;

typedef struct CBDeckOwner {
    /* The track the state describes (the one heard); the one queued to
     * follow it (cb_deck_queue_next); and the one the real-time side reads
     * already while cur is still what is heard (it moved on into the next,
     * or into one dropped too late, inside MASTER TEMPO's delay or before
     * its callback published the move). See sync_deck in player.c. */
    CBOwnTrack cur, next, ahead;
    unsigned   advanced;        /* moves into a next, counted when heard */
    int64_t    points[CB_DECK_CUE_POINTS];  /* the cue points the feed has, in frames */
    int        nb_points;
    /* What the deck's feed was last told to decode, and the generation
     * that told it. */
    CBStream  *feed_stream, *feed_next;
    uint64_t   feed_gen;
    uint64_t sent;              /* commands pushed for this deck */
    /* The latest position and playing state the owner asked for, valid
     * while the deck's applied serial is below the command's serial. */
    uint64_t pos_serial, play_serial;
    double   pos_frames;
    bool     playing;
    double   tempo, bend;
    float    volume;
    bool     loop_enabled;
    double   loop_in, loop_out; /* seconds, < 0 none */
    bool     scrubbing, scrub_resume;
    bool     master_tempo;      /* the deck's button: survives loads */
} CBDeckOwner;

/* A track the owner let go of, freed once its deck has applied `serial`
 * and reads it no more (neither its main voice nor its de-click tail: the
 * published ids) and, a stream, once the deck's feed has moved to
 * generation `gen` and is not told to decode it. A next dropped when the
 * deck may have moved on into it already keeps `follows`, the id of the
 * track it was queued after, until the deck shows it did not (then 0): if
 * it did, it is the deck's track again (sync_deck). */
typedef struct CBRelease {
    CBOwnTrack t;
    int        deck;
    uint64_t   serial;
    uint64_t   gen;
    uint64_t   follows;
} CBRelease;

/* Backend state. */
enum CBOutState {
    CB_OUT_CONNECTING = 0,
    CB_OUT_PAUSED,
    CB_OUT_RUNNING,
    CB_OUT_UNCONNECTED,
    CB_OUT_ERROR,
};

typedef struct CBPwOut CBPwOut;
typedef struct CBNullOut CBNullOut;
typedef struct CBRingOut CBRingOut;

struct CBPlayer {
    /* Command ring: head written by the owner, tail by the real-time
     * thread, each on its own cache line. */
    alignas(64) atomic_size_t head;
    alignas(64) atomic_size_t tail;
    alignas(64) CBCmd ring[CB_CMD_RING];

    /* Real-time side. */
    CBDeckRT rt[CB_DECKS];
    float    master_gain, master_prev;

    /* The decks' feed threads, and the callbacks finished, after which
     * they let go of the chunks they took out of the tables before. */
    CBDeckFeed feed[CB_DECKS];
    alignas(64) _Atomic uint64_t epoch;

    /* Published. */
    CBDeckPub   pub[CB_DECKS];
    atomic_int  device_rate;
    atomic_int  out_state;          /* enum CBOutState */
    atomic_bool error_set;
    char        error[256];         /* written once, before out_state = ERROR */

    /* Owner side. */
    CBPlayerOptions opt;            /* strings below are copies */
    char           *sink, *app_name;
    int             period;
    bool            manual;         /* null backend driven by cb_player_null_advance */
    CBDeckOwner     own[CB_DECKS];
    float           master_volume;
    CBRelease      *releases;
    int             nb_releases, releases_cap;
    bool            ring_full_warned;
    bool            master_tempo_ok;    /* every deck has a shifter */

    CBPwOut   *pw;
    CBNullOut *null;
    CBRingOut *ring_out;
};

/* ---------------------------------------------------------------- engine */

/* The mix callback both backends call: applies the queued commands, renders
 * every deck into `out` (interleaved stereo float, `frames` frames at the
 * device rate) and publishes the state. Real-time safe. */
void cb_player_mix(CBPlayer *p, float *out, int frames);

/* The grid copy kept for a deck (owner thread). */
int  cb_player_deck_set_grid(CBPlayer *p, int deck, const CBGrid *grid);
const CBGrid *cb_player_deck_grid(const CBPlayer *p, int deck);

/* ---------------------------------------------------------------- deck.c */

void cb_player_deck_init(CBDeckRT *d);
/* Owner thread: the deck's master tempo shifter and buffers, for a device at
 * `rate`, and their release (safe on a deck without them). */
int  cb_player_deck_mt_alloc(CBDeckRT *d, int rate);
void cb_player_deck_mt_free(CBDeckRT *d);
/* Applies one command for this deck (real-time thread). */
void cb_player_deck_apply(CBDeckRT *d, const CBCmd *cmd, int rate);
/* Adds the deck's output to `out` with its gain and updates its meters. */
void cb_player_deck_render(CBDeckRT *d, float *out, int frames, int rate);
/* After a callback: tells a stream deck's feed where it reads and whether
 * it went short, waking it when that matters. */
void cb_player_deck_signal(CBDeckRT *d);
/* Track frames per second of output the deck advances by. */
double cb_player_deck_speed(const CBDeckRT *d);
/* The position the listener hears: the read position, or with master tempo
 * on, the position of what the shifter is putting out now; and the track
 * (its id) it is in, which with the shifter on is the one before the
 * deck's for its delay after a change of track. */
CBPos    cb_player_deck_heard(const CBDeckRT *d);
uint64_t cb_player_deck_heard_id(const CBDeckRT *d);
/* The track the de-click tail still reads when it is not the deck's; 0. */
uint64_t cb_player_deck_old_id(const CBDeckRT *d);

/* ---------------------------------------------------------------- feed.c */

/* Owner thread: starts and stops a deck's feed thread, and wakes it. */
int  cb_player_feed_start(CBDeckFeed *f, const _Atomic uint64_t *epoch);
void cb_player_feed_stop(CBDeckFeed *f);
void cb_player_feed_poke(CBDeckFeed *f);
/* Owner thread: the stream the feed fills (NULL: none) and the one whose
 * start it fills once that has nothing left (NULL: none), returning the
 * generation that tells it (a new stream drops the cue points); the cue
 * points it keeps decoded. */
uint64_t cb_player_feed_set(CBDeckFeed *f, CBStream *s, CBStream *next);
void cb_player_feed_points(CBDeckFeed *f, const int64_t *frames, int nb);
/* Owner thread, manual mode: waits (up to `timeout_us`) until the feed has
 * decoded everything around the read position the stream says; at once
 * when the feed does not fill that stream. */
bool cb_player_feed_wait(CBDeckFeed *f, const CBStream *s, int64_t timeout_us);
/* Real-time thread: frames [i, i + n) of the stream as stereo pairs into
 * out (a mono track on both sides, silence outside [0, nb_frames)); false
 * when some of them are not decoded. */
bool cb_player_stream_read(const CBStream *s, int64_t nb_frames, int64_t i, int n, float *out);

/* -------------------------------------------------------------- backends */

int  cb_player_pw_open(CBPlayer *p);
void cb_player_pw_close(CBPlayer *p);

int  cb_player_null_open(CBPlayer *p);
void cb_player_null_close(CBPlayer *p);
/* Manual mode only: runs the mix for `frames` frames on the calling thread. */
void cb_player_null_run(CBPlayer *p, int frames);

/* The ring outlives its thread: cb_player_ring_stop joins the thread and
 * keeps the ring for cb_player_ring_take (another thread's), which a
 * later open starts mixing into again; cb_player_ring_close frees it, when
 * the player goes. */
int  cb_player_ring_open(CBPlayer *p);
void cb_player_ring_stop(CBPlayer *p);
void cb_player_ring_close(CBPlayer *p);

#endif /* CYANBOX_PLAYER_INTERNAL_H */
