/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* The player's private structures, shared by player.c (engine, command
 * ring, published state), deck.c (the real-time side of one deck), feed.c
 * (the thread that decodes a deck's packet store), pipewire.c and
 * nullout.c (the two backends) and controls.c.
 *
 * Three groups of state live in RXPlayer, and each has one writer:
 *  - RXDeckOwner: written and read only by the owner thread (the thread
 *    that calls the public functions). It is what the owner has asked for:
 *    the loaded buffer, the loop, the tempo, and the position or playing
 *    state of a command the real-time thread has not applied yet.
 *  - RXDeckRT: written and read only by the real-time thread, which also
 *    applies the commands. The owner never touches it after creation.
 *  - RXDeckPub: written by the real-time thread, read by the owner through
 *    C11 atomics.
 * A deck playing a packet store adds a fourth party, its feed thread: the
 * owner hands it the stream and the cue points under the feed's lock (an
 * RXDeckFeed), the feed writes decoded frames into the stream's slots and
 * the real-time thread reads them, both under the slots' sequence locks,
 * and the real-time thread says where it reads through the stream's
 * atomics. The command ring is single-producer (owner) / single-consumer
 * (real time). Nothing on the real-time side allocates, locks, logs or
 * does I/O; everything it needs is allocated before it runs. */

#ifndef RXBOX_PLAYER_INTERNAL_H
#define RXBOX_PLAYER_INTERNAL_H

#include "rxbox/player.h"

#include <math.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdalign.h>
#include <stdatomic.h>

#include "stretch.h"

/* Commands in flight. One UI frame sends a handful; a drag sends one per
 * pointer event. 1024 is several seconds of a stalled output. */
#define RX_CMD_RING 1024

/* The de-click fade: a jump, pause or start crossfades over this long. A
 * loop wrap does not (it is sample-exact). */
#define RX_FADE_SECONDS 0.003

/* Scrub grains: 60 ms Hann windows, a new one every 30 ms while the drag
 * moves, so overlapping windows sum to a constant level. */
#define RX_GRAIN_SECONDS 0.060
#define RX_GRAINS 3
#define RX_GRAIN_GAIN 0.5f
#define RX_GRAIN_LOWPASS_HZ 3000.0f

/* The level meters hold a peak and let it fall with this time constant. */
#define RX_LEVEL_RELEASE_SECONDS 0.3f

/* MASTER TEMPO renders a deck in pieces of at most this many frames, which
 * is what its scratch buffers hold. A callback has no bound of ours:
 * PipeWire hands it as many frames as its buffer has room for (pipewire.c,
 * on_process), so a longer one is cut up rather than refused. */
#define RX_MT_CHUNK 1024

/* A deck playing from a packet store (feed.c has the why): the window of
 * decoded frames is cut in chunks of RX_CHUNK frames and spans
 * RX_WINDOW_BEHIND seconds behind the read position and RX_WINDOW_AHEAD
 * ahead of it; each cue point keeps RX_CUE_CACHE_SECONDS decoded from
 * RX_CUE_CACHE_LEAD frames before it. */
#define RX_CHUNK             4096
#define RX_WINDOW_BEHIND     4.0
#define RX_WINDOW_AHEAD      8.0
#define RX_CUE_CACHE_SECONDS 0.5
#define RX_CUE_CACHE_LEAD    16

/* ------------------------------------------------------------- positions */

/* A position in track frames: an integer frame and a fraction in [0, 1).
 * A double alone loses resolution as the frame count grows, and a constant
 * increment rounds the same way on every step, so the error would add up
 * linearly over an hour-long track. Kept apart, the fraction stays at full
 * precision and a loop wrap (subtracting the loop length) is exact. */
typedef struct RXPos {
    int64_t i;
    double  f;
} RXPos;

static inline RXPos rx_pos_from(double frames)
{
    double fl = floor(frames);
    return (RXPos) { .i = (int64_t)fl, .f = frames - fl };
}

static inline double rx_pos_get(RXPos p)
{
    return (double)p.i + p.f;
}

static inline void rx_pos_add(RXPos *p, double d)
{
    double fl = floor(d);
    p->i += (int64_t)fl;
    p->f += d - fl;
    if (p->f >= 1.0) {
        p->f -= 1.0;
        p->i++;
    }
}

static inline RXPos rx_pos_sub(RXPos a, RXPos b)
{
    RXPos r = { .i = a.i - b.i, .f = a.f - b.f };
    if (r.f < 0.0) {
        r.f += 1.0;
        r.i--;
        /* A difference within 2^-54 below zero plus the borrow rounds to
         * exactly 1.0: that is the next whole frame, and rx_pos_cmp needs
         * the fraction in [0, 1) to order positions by value. */
        if (r.f >= 1.0) {
            r.f -= 1.0;
            r.i++;
        }
    }
    return r;
}

static inline int rx_pos_cmp(RXPos a, RXPos b)
{
    if (a.i != b.i)
        return a.i < b.i ? -1 : 1;
    return (a.f > b.f) - (a.f < b.f);
}

/* -------------------------------------------------------------- commands */

enum RXCmdType {
    RX_CMD_LOAD = 0,
    RX_CMD_UNLOAD,
    RX_CMD_PLAY,
    RX_CMD_PAUSE,
    RX_CMD_SEEK,            /* a = frames */
    RX_CMD_PLAY_FROM,       /* a = frames */
    RX_CMD_TEMPO,           /* a = ratio */
    RX_CMD_LOOP_SET,        /* a = in, b = out (frames), flag = enabled */
    RX_CMD_LOOP_ENABLE,     /* flag */
    RX_CMD_LOOP_CLEAR,
    RX_CMD_VOLUME,          /* a = gain */
    RX_CMD_BEND,            /* a = ratio offset */
    RX_CMD_SCRUB_BEGIN,
    RX_CMD_SCRUB_TO,        /* a = frames */
    RX_CMD_SCRUB_END,
    RX_CMD_MASTER_VOLUME,   /* a = gain; deck = -1 */
    RX_CMD_MASTER_TEMPO,    /* flag */
};

typedef struct RXStream RXStream;

/* A command is copied into the ring by value. A load carries the buffer's
 * fields rather than the RXAudio pointer, so the real-time side never reads
 * the owner's struct; or the stream a packet store plays through. */
typedef struct RXCmd {
    uint8_t      type;
    int8_t       deck;
    bool         flag;
    double       a, b;
    const float *samples;
    RXStream    *stream;
    int          channels;
    int          rate;
    int64_t      nb_frames;
} RXCmd;

/* --------------------------------------------------------------- streams */

/* Decoded frames the feed thread publishes and the real-time thread reads,
 * under a sequence lock: the writer makes `seq` odd, writes `first` and the
 * frames, then makes `seq` even with release order; a reader that finds the
 * same even `seq` before and after copying, and the `first` it wanted,
 * copied whole frames. Neither ever waits. */
typedef struct RXSlot {
    atomic_uint     seq;
    _Atomic int64_t first;      /* track frame of data[0], -1 when it holds nothing */
    _Atomic int64_t len;        /* frames it holds from there */
    float          *data;       /* channels interleaved */
} RXSlot;

/* A packet store as a deck plays it. Made and freed by the owner; while
 * loaded, the feed thread alone decodes into it and the real-time thread
 * alone reads it, and the two talk through the atomics. */
struct RXStream {
    RXPackets *pk;              /* the feed thread's while loaded */
    int        channels;        /* in the window: the track's first one or two */
    int        rate;
    int        nb_slots;        /* the window: chunk c in slot c % nb_slots */
    RXSlot    *slots;
    int64_t    cache_len;       /* frames a cue cache holds at most */
    /* The cue caches, and last the jump slot: the target of the jump the
     * owner sent last (`hint`), outside the window, whose slots the old
     * window still holds until the real-time side gets there */
    RXSlot     caches[RX_DECK_CUE_POINTS + 1];
    float     *mem;
    /* Real time -> feed */
    _Atomic int64_t playhead;   /* the read position */
    atomic_bool     starved;    /* the deck holds for frames the window lacks */
    /* Owner -> feed: where a jump just sent will read, before the real-time
     * side gets to it; -1 none */
    _Atomic int64_t hint;
};

/* One deck's feed thread (feed.c). The owner hands it the stream to fill and
 * the cue points under `lock`, and wakes it; it says which stream
 * generation it has moved to, after which it no longer touches the streams
 * before. */
typedef struct RXDeckFeed {
    pthread_t thread;
    bool      started;
    sem_t     wake;
    bool      sem_ok;
    pthread_mutex_t lock;
    bool      lock_ok;
    /* under lock */
    RXStream *stream;
    uint64_t  gen;
    bool      quit;
    int64_t   points[RX_DECK_CUE_POINTS];
    int       nb_points;
    uint64_t  points_gen;
    /* published by the feed thread */
    _Atomic uint64_t acked;     /* the generation it works on */
    _Atomic int64_t  settled;   /* the read position everything is decoded around, or -1 */
} RXDeckFeed;

/* ------------------------------------------------------- real-time decks */

typedef struct RXGrain {
    RXPos pos;
    int   age, len;         /* len == 0: idle */
} RXGrain;

/* MASTER TEMPO on the real-time side (deck.c has the why). Everything is
 * allocated by rx_player_create; st == NULL means the player has none. */
typedef struct RXDeckMT {
    RXStretch *st;
    int     latency;        /* frames from the shifter's input to its output */
    int     in_latency;     /* frames from its newest input to the block it analyses */
    float  *in[2], *out[2]; /* RX_MT_CHUNK frames: varispeed in, shifted out */
    float  *ahead[2];       /* latency frames: what a restart starts from */
    float  *xf[2];          /* RX_MT_CHUNK frames: the old sound of a crossfade */
    int     xf_len, xf_left;
    /* The read position and speed ratio (0 when stopped) of every frame fed,
     * a ring of hist_mask + 1: what is heard now was fed `latency` ago. */
    RXPos  *hist;
    float  *hist_speed;
    int64_t hist_mask;
    int64_t fed;            /* frames fed so far */
    int64_t fill_until;     /* frames fed before this one count as fill_pos */
    RXPos   fill_pos;
    bool    on;             /* the shifter is in the deck's path */
    /* A restart is due at the next render. was_on and *was are the deck as
     * it sounded before the first command that asked for it. */
    bool    restart, was_on;
    struct RXDeckRT *was;
} RXDeckMT;

typedef struct RXDeckRT {
    /* The buffer: owned by the owner side, read-only here. Or the stream. */
    const float *samples;
    RXStream    *stream;
    int          channels;
    int          rate;
    int64_t      nb_frames;
    bool         length_final;  /* a stream whose index is complete: nb_frames is its count */
    sem_t       *wake;          /* the deck's feed thread */
    int64_t      posted_chunk;  /* the read position's chunk the feed was last woken for */
    bool         starved;       /* this callback missed frames not decoded yet */
    bool         holding;       /* the main voice holds for them */
    int          out_rate;      /* the device's, as of the last callback */

    RXPos  pos;
    bool   playing;
    double tempo, bend;
    float  gain, gain_prev;     /* volume, ramped over one callback */

    bool   loop_set, loop_enabled;
    RXPos  loop_in, loop_out, loop_len;

    /* De-click: the main voice fades in after a start or a jump while the
     * tail voice fades out from where the deck was. */
    int    fade_in, fade_len;
    RXPos  tail_pos;
    double tail_inc;
    int    tail_left, tail_len;

    /* Scrubbing: the deck follows the pointer and plays grains. */
    bool    scrubbing, scrub_resume;
    double  grain_from;         /* frames: where the last grain started */
    int     hop;                /* output frames until the next grain may start */
    RXGrain grains[RX_GRAINS];
    float   lp_l, lp_r;

    float    level_l, level_r;
    uint64_t applied;           /* commands applied to this deck */

    RXDeckMT mt;
} RXDeckRT;

/* ------------------------------------------------------- published state */

/* Written by the real-time thread at the end of every callback, read by
 * rx_deck_state(). Position, playing state and serial have to agree with
 * each other (the owner uses the serial to decide whether a seek it sent
 * has been applied), so they form a sequence lock: the writer makes `seq`
 * odd, stores, then makes it even with release order; the reader retries
 * until it sees the same even value before and after its loads. The writer
 * never waits. The meters are single values and need no such care. */
typedef struct RXDeckPub {
    alignas(64) atomic_uint seq;
    _Atomic int64_t  frame_i;
    _Atomic uint64_t frame_f;       /* double bits */
    _Atomic uint64_t speed;         /* double bits: track frames per second, 0 when stopped */
    _Atomic int64_t  time_us;       /* CLOCK_MONOTONIC at the callback */
    _Atomic int64_t  span_us;       /* duration of the block it rendered */
    atomic_bool      playing;
    atomic_bool      waiting;       /* playing, holding for the feed */
    _Atomic uint64_t serial;
    atomic_uint      level_l, level_r;  /* float bits */
} RXDeckPub;

typedef struct RXDeckSnap {
    double   frames;
    double   speed;
    int64_t  time_us, span_us;
    bool     playing;
    bool     waiting;
    uint64_t serial;
} RXDeckSnap;

/* ------------------------------------------------------------ owner side */

typedef struct RXDeckOwner {
    RXAudio  *audio;            /* loaded buffer, NULL when empty */
    RXStream *stream;           /* or the loaded stream */
    int       rate;             /* of what is loaded */
    int64_t   nb_frames;        /* its length (a stream's as last read) */
    int64_t   points[RX_DECK_CUE_POINTS];   /* the cue points the feed has, in frames */
    int       nb_points;
    RXGrid   grid;              /* copy of the grid given at load */
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
} RXDeckOwner;

/* A buffer or stream the owner let go of, freed once its deck has applied
 * `serial` and the deck's feed has moved to generation `gen`. */
typedef struct RXRelease {
    RXAudio  *audio;
    RXStream *stream;
    int       deck;
    uint64_t  serial;
    uint64_t  gen;
} RXRelease;

/* Backend state. */
enum RXOutState {
    RX_OUT_CONNECTING = 0,
    RX_OUT_PAUSED,
    RX_OUT_RUNNING,
    RX_OUT_UNCONNECTED,
    RX_OUT_ERROR,
};

typedef struct RXPwOut RXPwOut;
typedef struct RXNullOut RXNullOut;

struct RXPlayer {
    /* Command ring: head written by the owner, tail by the real-time
     * thread, each on its own cache line. */
    alignas(64) atomic_size_t head;
    alignas(64) atomic_size_t tail;
    alignas(64) RXCmd ring[RX_CMD_RING];

    /* Real-time side. */
    RXDeckRT rt[RX_DECKS];
    float    master_gain, master_prev;

    /* The decks' feed threads. */
    RXDeckFeed feed[RX_DECKS];

    /* Published. */
    RXDeckPub   pub[RX_DECKS];
    atomic_int  device_rate;
    atomic_int  out_state;          /* enum RXOutState */
    atomic_bool error_set;
    char        error[256];         /* written once, before out_state = ERROR */

    /* Owner side. */
    RXPlayerOptions opt;            /* strings below are copies */
    char           *sink, *app_name;
    int             period;
    bool            manual;         /* null backend driven by rx_player_null_advance */
    RXDeckOwner     own[RX_DECKS];
    float           master_volume;
    RXRelease      *releases;
    int             nb_releases, releases_cap;
    bool            ring_full_warned;
    bool            master_tempo_ok;    /* every deck has a shifter */

    RXPwOut   *pw;
    RXNullOut *null;
};

/* ---------------------------------------------------------------- engine */

/* The mix callback both backends call: applies the queued commands, renders
 * every deck into `out` (interleaved stereo float, `frames` frames at the
 * device rate) and publishes the state. Real-time safe. */
void rx_player_mix(RXPlayer *p, float *out, int frames);

/* The grid copy kept for a deck (owner thread). */
int  rx_player_deck_set_grid(RXPlayer *p, int deck, const RXGrid *grid);
const RXGrid *rx_player_deck_grid(const RXPlayer *p, int deck);

/* ---------------------------------------------------------------- deck.c */

void rx_player_deck_init(RXDeckRT *d);
/* Owner thread: the deck's master tempo shifter and buffers, for a device at
 * `rate`, and their release (safe on a deck without them). */
int  rx_player_deck_mt_alloc(RXDeckRT *d, int rate);
void rx_player_deck_mt_free(RXDeckRT *d);
/* Applies one command for this deck (real-time thread). */
void rx_player_deck_apply(RXDeckRT *d, const RXCmd *cmd, int rate);
/* Adds the deck's output to `out` with its gain and updates its meters. */
void rx_player_deck_render(RXDeckRT *d, float *out, int frames, int rate);
/* After a callback: tells a stream deck's feed where it reads and whether
 * it went short, waking it when that matters. */
void rx_player_deck_signal(RXDeckRT *d);
/* Track frames per second of output the deck advances by. */
double rx_player_deck_speed(const RXDeckRT *d);
/* The position the listener hears: the read position, or with master tempo
 * on, the position of what the shifter is putting out now. */
RXPos rx_player_deck_heard(const RXDeckRT *d);

/* ---------------------------------------------------------------- feed.c */

/* Owner thread: a stream for pk (which it then owns), and its release. */
int  rx_player_stream_create(RXStream **out, RXPackets *pk);
void rx_player_stream_free(RXStream *s);
/* Owner thread: starts and stops a deck's feed thread. */
int  rx_player_feed_start(RXDeckFeed *f);
void rx_player_feed_stop(RXDeckFeed *f);
/* Owner thread: the stream the feed fills (NULL: none), returning its
 * generation; the cue points it keeps decoded. */
uint64_t rx_player_feed_set(RXDeckFeed *f, RXStream *s);
void rx_player_feed_points(RXDeckFeed *f, const int64_t *frames, int nb);
/* Owner thread, manual mode: waits (up to `timeout_us`) until the feed has
 * decoded everything around the read position the stream says. */
bool rx_player_feed_wait(RXDeckFeed *f, const RXStream *s, int64_t timeout_us);
/* Real-time thread: frames [i, i + n) of the stream as stereo pairs into
 * out (a mono track on both sides, silence outside [0, nb_frames)); false
 * when some of them are not decoded. */
bool rx_player_stream_read(const RXStream *s, int64_t nb_frames, int64_t i, int n, float *out);

/* -------------------------------------------------------------- backends */

int  rx_player_pw_open(RXPlayer *p);
void rx_player_pw_close(RXPlayer *p);

int  rx_player_null_open(RXPlayer *p);
void rx_player_null_close(RXPlayer *p);
/* Manual mode only: runs the mix for `frames` frames on the calling thread. */
void rx_player_null_run(RXPlayer *p, int frames);

#endif /* RXBOX_PLAYER_INTERNAL_H */
