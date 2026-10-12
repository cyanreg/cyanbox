/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* The player's private structures, shared by player.c (engine, command
 * ring, published state), deck.c (the real-time side of one deck),
 * pipewire.c and nullout.c (the two backends) and controls.c.
 *
 * Three groups of state live in RXPlayer, and each has one writer:
 *  - RXDeckOwner: written and read only by the owner thread (the thread
 *    that calls the public functions). It is what the owner has asked for:
 *    the loaded buffer, the loop, the tempo, and the position or playing
 *    state of a command the real-time thread has not applied yet.
 *  - RXDeckRT: written and read only by the real-time thread, which also
 *    applies the commands. The owner never touches it after creation.
 *  - RXDeckPub: written by the real-time thread, read by the owner through
 *    C11 atomics. The only shared mutable memory besides the ring.
 * The command ring is single-producer (owner) / single-consumer (real
 * time). Nothing on the real-time side allocates, locks, logs or does
 * I/O; everything it needs is allocated before it runs. */

#ifndef RXBOX_PLAYER_INTERNAL_H
#define RXBOX_PLAYER_INTERNAL_H

#include "rxbox/player.h"

#include <math.h>
#include <stdalign.h>
#include <stdatomic.h>

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
};

/* A command is copied into the ring by value. A load carries the buffer's
 * fields rather than the RXAudio pointer, so the real-time side never reads
 * the owner's struct. */
typedef struct RXCmd {
    uint8_t      type;
    int8_t       deck;
    bool         flag;
    double       a, b;
    const float *samples;
    int          channels;
    int          rate;
    int64_t      nb_frames;
} RXCmd;

/* ------------------------------------------------------- real-time decks */

typedef struct RXGrain {
    RXPos pos;
    int   age, len;         /* len == 0: idle */
} RXGrain;

typedef struct RXDeckRT {
    /* The buffer: owned by the owner side, read-only here. */
    const float *samples;
    int          channels;
    int          rate;
    int64_t      nb_frames;

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
    _Atomic uint64_t serial;
    atomic_uint      level_l, level_r;  /* float bits */
} RXDeckPub;

typedef struct RXDeckSnap {
    double   frames;
    double   speed;
    int64_t  time_us, span_us;
    bool     playing;
    uint64_t serial;
} RXDeckSnap;

/* ------------------------------------------------------------ owner side */

typedef struct RXDeckOwner {
    RXAudio *audio;             /* loaded buffer, NULL when empty */
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
} RXDeckOwner;

/* A buffer the owner let go of, freed once its deck has applied `serial`. */
typedef struct RXRelease {
    RXAudio *audio;
    int      deck;
    uint64_t serial;
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
/* Applies one command for this deck (real-time thread). */
void rx_player_deck_apply(RXDeckRT *d, const RXCmd *cmd, int rate);
/* Adds the deck's output to `out` with its gain and updates its meters. */
void rx_player_deck_render(RXDeckRT *d, float *out, int frames, int rate);
/* Track frames per second of output the deck advances by. */
double rx_player_deck_speed(const RXDeckRT *d);

/* -------------------------------------------------------------- backends */

int  rx_player_pw_open(RXPlayer *p);
void rx_player_pw_close(RXPlayer *p);

int  rx_player_null_open(RXPlayer *p);
void rx_player_null_close(RXPlayer *p);
/* Manual mode only: runs the mix for `frames` frames on the calling thread. */
void rx_player_null_run(RXPlayer *p, int frames);

#endif /* RXBOX_PLAYER_INTERNAL_H */
