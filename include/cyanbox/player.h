/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

#ifndef CYANBOX_PLAYER_H
#define CYANBOX_PLAYER_H

/* Playback: CB_DECKS decks (one: cyanbox prepares tracks, it is not a mixer)
 * mixed into one PipeWire stream.
 *
 * Thread model. The owner thread (the interface's main thread) calls every
 * function here. Commands go to the real-time thread through a lock-free
 * single-producer/single-consumer ring; the real-time thread never
 * allocates, locks, or touches files, and publishes positions through
 * atomics that cb_deck_state() reads. A deck plays a track's packet store
 * (CBPackets: the file's packets in memory, cyanbox/audio.h), which a thread
 * of the deck's own decodes around the playhead and at the cue points as
 * it plays, so a track is ready as soon as its first packets are read; or
 * a whole decoded track (CBAudio, native rate, interleaved float). Where
 * the decoded audio does not reach yet (just after a jump far away), a
 * playing deck holds, silent, until it does. The real-time thread
 * resamples to the device rate with a cubic interpolator, which is also
 * how tempo changes play: pitch follows tempo, as on a CDJ with MASTER
 * TEMPO off. With it on, the deck's output is shifted back by
 * 1 / (tempo + bend) (Signalsmith Stretch), reading ahead by the
 * shifter's delay so that jumps, starts and stops are still heard at once
 * and the position reported is the one heard. Loops wrap sample-exactly
 * inside the callback.
 *
 * Loading hands an CBAudio or an CBPackets to the player, which owns it
 * until the deck is unloaded or loaded again; the old one is released only
 * after the real-time thread (and the deck's decoding thread) have
 * acknowledged the swap, so freeing is always safe. A second one may be
 * queued to follow (cb_deck_queue_next): the deck plays on into it at the
 * end of its track, sample-exactly, as one piece of audio.
 *
 * Backends: PipeWire (pw_stream, 48 kHz float stereo, target node from
 * options), the null backend, which runs the same callback on a timer
 * thread and discards the audio: for tests and headless runs, or the ring
 * backend, which keeps it for another thread to take (with --pro-link, the
 * mixer's LINK CUE; src/player/ringout.c). Nothing
 * here ever picks an output device by itself other than the PipeWire
 * default; tests always use CB_AUDIO_NULL or a null sink. */

#include "cyanbox/model.h"
#include "cyanbox/audio.h"

#define CB_DECKS 1
/* Cue points a deck keeps decoded (cb_deck_set_cue_points): the start, the
 * cue point, the loop in point, eight hot cues and ten memory cues, and a
 * few to spare. */
#define CB_DECK_CUE_POINTS 24

enum CBAudioBackend {
    CB_AUDIO_PIPEWIRE = 0,
    CB_AUDIO_NULL,
    /* No device: the mix kept for another thread to take as it needs it
     * (cb_player_ring_take: PRO DJ LINK's LINK CUE), paced by it while it
     * takes, by the clock while not. */
    CB_AUDIO_RING,
};

typedef struct CBPlayerOptions {
    enum CBAudioBackend backend;
    const char *sink;           /* PipeWire node name to connect to; NULL = default */
    int         rate;           /* 0 = 48000 */
    int         period;         /* frames per callback; 0 = 256. CB_AUDIO_NULL only:
                                 * < 0 is manual mode, no thread; callbacks of
                                 * -period frames run from cb_player_null_advance */
    const char *app_name;       /* "cyanbox" */
    /* Called on the owner thread when the player lets go of a buffer it was
     * given, once the real-time thread can no longer read it (or at
     * destroy). NULL: cb_audio_free() and free(), so the CBAudio struct
     * itself must then come from malloc. Packet stores are always freed by
     * the player (cb_packets_free). */
    void      (*release)(void *opaque, CBAudio *audio);
    void       *release_opaque;
} CBPlayerOptions;
void cb_player_options_default(CBPlayerOptions *o);

typedef struct CBPlayer CBPlayer;

int  cb_player_create(CBPlayer **p, const CBPlayerOptions *opt);
void cb_player_destroy(CBPlayer *p);
int  cb_player_rate(const CBPlayer *p);
enum CBAudioBackend cb_player_backend(const CBPlayer *p);
/* The PipeWire node the stream was told to connect to; NULL = the default. */
const char *cb_player_sink(const CBPlayer *p);
/* True once the stream is running; the error string when it is not
 * (NULL while running). */
bool cb_player_running(const CBPlayer *p);
const char *cb_player_error(const CBPlayer *p);
void cb_player_set_master_volume(CBPlayer *p, float gain);   /* 0..1 */
/* Advances the null backend by `frames` synchronously (tests only; no-op
 * on PipeWire and on a null backend with its own thread, period >= 0).
 * Before each callback it waits for a deck playing a packet store to have
 * the audio around its playhead decoded, as a device on a machine that
 * keeps up would find it, so the tests see the same output every run. */
void cb_player_null_advance(CBPlayer *p, int frames);
/* CB_AUDIO_RING: the next `frames` frames of the mix as 16-bit stereo,
 * interleaved, at the player's rate, into `out`, zeros for what is not
 * mixed yet: how many were. The first take after a while (50 ms) drops
 * what the ring held, and the mix catches up within a period. Any
 * thread, one at a time. */
int  cb_player_ring_take(CBPlayer *p, int16_t *out, int frames);

/* ------------------------------------------------------------------- decks */

/* Takes ownership of `audio` on success (see CBPlayerOptions.release); on
 * error the caller keeps it. The grid is copied (NULL = none). */
int  cb_deck_load(CBPlayer *p, int deck, CBAudio *audio, const CBGrid *grid);
/* The same for a packet store, which the deck decodes as it plays: from
 * here on the deck's thread holds it (the player frees it). */
int  cb_deck_load_packets(CBPlayer *p, int deck, CBPackets *pk, const CBGrid *grid);
/* The places a jump may go to on the deck (the start, the cue point, the
 * loop in point, the hot and memory cues), at most CB_DECK_CUE_POINTS, in
 * seconds. A deck playing a packet store keeps half a second decoded at
 * each, so a cue, a hot cue or a loop going back plays at once while the
 * decoder moves there. Cheap to call with the same points again (every
 * frame: the controls do). */
void cb_deck_set_cue_points(CBPlayer *p, int deck, const double *seconds, int nb);
void cb_deck_unload(CBPlayer *p, int deck);
bool cb_deck_loaded(const CBPlayer *p, int deck);
/* The track to follow the loaded one, taken like a load (the player owns
 * it from here, on success; -ENOENT when nothing is loaded): when the
 * deck plays past the last frame of its track it goes on in this one from
 * its first frame, the step's fraction kept, with no fade, silence or
 * command in between (the two play as one piece of audio); the loop goes
 * with the old track, the tempo and MASTER TEMPO stay. A deck playing a
 * packet store has the start of the next decoded ahead of time, and the
 * interpolator reads across the change, so a deck that resamples plays
 * the two as one signal too. The owner learns of the move from
 * CBDeckState.advanced, after which the state (duration, position) is the
 * next track's and what was loaded is released. Queueing again replaces
 * the queued track; a load, an unload or cb_deck_clear_next drops it.
 *
 * The deck may move on at any callback, and with MASTER TEMPO on it reads
 * the next the shifter's delay (120 ms) before the move is heard: so
 * dropping or replacing the queued track can come too late. Then the deck
 * plays the queued track all the same, and the move is reported as any
 * other (advanced); a replacement is let go of, unplayed. While the deck
 * may still move on into a track (queued, read and not yet heard, or
 * dropped and the drop not yet applied), has_next stays true: a caller
 * that needs to know which track the deck moves on into drops the queued
 * one and queues another only once has_next is false. Queueing while the
 * deck reads the queued track already, the move not heard yet, fails with
 * -EBUSY. Seeks, loops and cue points meant for the old track are not
 * applied to the new one: from the read to the heard move they do
 * nothing. */
int  cb_deck_queue_next(CBPlayer *p, int deck, CBAudio *audio, const CBGrid *grid);
int  cb_deck_queue_next_packets(CBPlayer *p, int deck, CBPackets *pk, const CBGrid *grid);
void cb_deck_clear_next(CBPlayer *p, int deck);
bool cb_deck_has_next(const CBPlayer *p, int deck);
/* The buffer a deck plays (valid until unload; read-only); NULL for a
 * packet store. */
const CBAudio *cb_deck_audio(const CBPlayer *p, int deck);

void cb_deck_play(CBPlayer *p, int deck);
void cb_deck_pause(CBPlayer *p, int deck);
/* Seeks to `seconds` (clamped to the track). Takes effect at the next
 * callback; cb_deck_state reports the new position immediately. */
void cb_deck_seek(CBPlayer *p, int deck, double seconds);
/* Plays from `seconds` (seek + play in one command, for cue/hot cue). */
void cb_deck_play_from(CBPlayer *p, int deck, double seconds);
/* 1.0 = original speed; the tempo slider's ratio. */
void cb_deck_set_tempo(CBPlayer *p, int deck, double ratio);
/* MASTER TEMPO: the deck keeps its pitch while the tempo changes (the
 * varispeed output is shifted back by 1/speed with Signalsmith Stretch on
 * the real-time thread). A property of the deck that survives loads.
 * Returns false when the player has no shifter (creation failed), in which
 * case the deck stays as a CDJ with MASTER TEMPO off. */
bool cb_deck_set_master_tempo(CBPlayer *p, int deck, bool on);
/* Loop between in and out seconds. `enabled` starts looping right away. */
void cb_deck_set_loop(CBPlayer *p, int deck, double in, double out, bool enabled);
void cb_deck_loop_enable(CBPlayer *p, int deck, bool enabled);
void cb_deck_clear_loop(CBPlayer *p, int deck);
void cb_deck_set_volume(CBPlayer *p, int deck, float gain);     /* 0..1 */
/* Pitch bend: a temporary speed offset (+-, e.g. 0.02) while held. */
void cb_deck_set_bend(CBPlayer *p, int deck, double ratio_offset);
/* Scrubbing: while a drag is in progress the deck follows the pointer and
 * plays short grains at the drag position (quiet, filtered) so the ear can
 * find a beat; on end it stays paused there, or resumes if it was playing. */
void cb_deck_scrub_begin(CBPlayer *p, int deck);
void cb_deck_scrub_to(CBPlayer *p, int deck, double seconds);
void cb_deck_scrub_end(CBPlayer *p, int deck);

typedef struct CBDeckState {
    bool   loaded;
    bool   playing;
    bool   waiting;         /* playing, but holding (silent) until the audio at the playhead
                               is decoded: just after a jump far from where the deck was */
    bool   scrubbing;
    double position;        /* seconds on the track's timeline */
    double duration;
    double tempo;           /* ratio */
    bool   master_tempo;    /* pitch kept while the tempo changes */
    bool   loop_enabled;
    double loop_in, loop_out;   /* < 0 when no loop is set */
    float  level_l, level_r;    /* recent peak, 0..1 */
    int64_t frame;          /* position in track frames */
    uint64_t serial;        /* bumps on every command the RT thread applied */
    unsigned advanced;      /* bumps each time the deck moved on to a queued track
                               (cb_deck_queue_next): from then on the state is that
                               track's. With MASTER TEMPO on, when the move is heard. */
    bool   has_next;        /* the deck may still move on into a queued track (queued,
                               or dropped and the drop not applied yet: see
                               cb_deck_queue_next) */
} CBDeckState;

void cb_deck_state(const CBPlayer *p, int deck, CBDeckState *out);

/* ---------------------------------------------------------------- controls */

/* CDJ/rekordbox button semantics on top of a deck: the cue point, hot cue
 * pads, memory cues, loops, beat jump and quantize. Owns no audio; edits
 * the track's cue list in place and tells the owner through `changed` so
 * the library can be saved. All calls from the owner thread; call
 * cb_ctl_tick() once per interface frame.
 *
 * The rules (CDJ-3000 manual, rekordbox Export mode):
 *  - PLAY/PAUSE toggles.
 *  - CUE while playing: back to the cue point and pause. CUE while paused
 *    at the cue point: play while held, return and pause on release.
 *    CUE while paused elsewhere: set the cue point there (quantised).
 *  - Hot cue pad on an empty slot: store the current position (quantised),
 *    or the active loop as a hot loop. On a set slot: jump there and play
 *    (a hot loop starts looping). Shift/delete clears the slot.
 *  - MEMORY stores the cue point, or the active loop, as a memory cue/loop.
 *    CALL previous/next moves to the memory cue before/after the playhead
 *    (20 ms tolerance) and makes it the cue point, keeping the playing
 *    state. DELETE removes the memory cue the playhead is on.
 *  - LOOP IN sets the in point (quantised); LOOP OUT sets the out point and
 *    starts looping. EXIT/RELOOP leaves the loop, or returns to the last
 *    loop. Auto beat loop N starts an N-beat loop at the playhead
 *    (quantised, measured on the grid). /2 and x2 change the length from
 *    the in point. An active memory loop engages when the playhead reaches
 *    its in point.
 *  - Beat jump moves +-N beats on the grid (quantised when QUANTIZE is on),
 *    keeping the playing state.
 *  - QUANTIZE snaps cue, hot cue and loop points to the nearest beat, or to
 *    the nearest fraction of a beat the QUANTIZE BEAT VALUE gives
 *    (quantize_beats: 1/2, 1/4, 1/8).
 *  - A load cues by AUTO CUE (auto_cue): at the first memory cue, at the
 *    start, or at the grid's first beat. */

typedef void (*cb_ctl_changed_cb)(void *opaque, int deck);

/* Where a load leaves the deck (My Settings' AUTO CUE and AUTO CUE LEVEL). */
typedef enum CBAutoCue {
    CB_AUTO_CUE_MEMORY = 0,     /* the first memory cue, else the start (level MEMORY,
                                   the factory setting) */
    CB_AUTO_CUE_OFF,            /* the start */
    CB_AUTO_CUE_LEVEL,          /* a level in dB: the grid's first beat, else the start */
} CBAutoCue;

typedef struct CBDeckCtl {
    CBPlayer *player;
    int       deck;
    CBTrack  *track;            /* the library's track; NULL when empty */
    bool      quantize;
    double    quantize_beats;   /* what QUANTIZE snaps to, in beats: 1 (default), 0.5,
                                   0.25 or 0.125; anything else is a whole beat */
    CBAutoCue auto_cue;         /* applied by cb_ctl_load; CB_AUTO_CUE_MEMORY by default */
    double    cue_point;        /* seconds; < 0 none */
    bool      cue_held;         /* CUE is being held for a preview */
    double    loop_in, loop_out;/* the current/last loop; < 0 none */
    bool      looping;
    int       active_memory_loop;   /* index of the engaged memory loop, -1 */
    double    jump_beats;       /* beat jump size (4 default) */
    double    last_hot_loop_beats;  /* length of the last auto loop (for the pad lamps) */
    cb_ctl_changed_cb changed;
    void     *opaque;
    /* private */
    double    _last_position;
    int       _hot_held;        /* slot held during a paused pad press, or -1 */
    bool      _resume_after_hold;
} CBDeckCtl;

void cb_ctl_init(CBDeckCtl *c, CBPlayer *p, int deck, cb_ctl_changed_cb changed, void *opaque);
/* After cb_deck_load: binds the track and applies auto-cue (c->auto_cue:
 * the cue point and the paused playhead at the first memory cue, the
 * start, or the grid's first beat). */
void cb_ctl_load(CBDeckCtl *c, CBTrack *track);
/* The deck moved on to `track` by itself (cb_deck_queue_next,
 * CBDeckState.advanced): binds it without touching the playhead, which
 * plays on from its start; the cue point goes where a load's auto cue
 * would put it. */
void cb_ctl_continue(CBDeckCtl *c, CBTrack *track);
void cb_ctl_unload(CBDeckCtl *c);
void cb_ctl_tick(CBDeckCtl *c);

void cb_ctl_play_pause(CBDeckCtl *c);
void cb_ctl_cue_press(CBDeckCtl *c);
void cb_ctl_cue_release(CBDeckCtl *c);
void cb_ctl_set_cue_here(CBDeckCtl *c);

void cb_ctl_hot_cue_press(CBDeckCtl *c, int slot);
void cb_ctl_hot_cue_release(CBDeckCtl *c, int slot);
void cb_ctl_hot_cue_delete(CBDeckCtl *c, int slot);
/* Edits the stored hot cue (name/colour) without moving it. */
void cb_ctl_hot_cue_set(CBDeckCtl *c, int slot, const char *name, const char *color);

void cb_ctl_memory_set(CBDeckCtl *c);
void cb_ctl_memory_prev(CBDeckCtl *c);
void cb_ctl_memory_next(CBDeckCtl *c);
void cb_ctl_memory_delete(CBDeckCtl *c);
void cb_ctl_memory_call(CBDeckCtl *c, int cue_index);      /* a specific memory cue */
void cb_ctl_memory_set_active(CBDeckCtl *c, int cue_index, bool active);

void cb_ctl_loop_in(CBDeckCtl *c);
void cb_ctl_loop_out(CBDeckCtl *c);
void cb_ctl_exit_reloop(CBDeckCtl *c);
void cb_ctl_auto_loop(CBDeckCtl *c, double beats);          /* 0.25 .. 64 */
void cb_ctl_loop_half(CBDeckCtl *c);
void cb_ctl_loop_double(CBDeckCtl *c);
void cb_ctl_loop_exit(CBDeckCtl *c);

void cb_ctl_beat_jump(CBDeckCtl *c, double beats);         /* +-, 0 = fine nudge of 10 ms (-0.0 back) */
void cb_ctl_nudge(CBDeckCtl *c, double seconds);
void cb_ctl_set_quantize(CBDeckCtl *c, bool on);
/* `seconds` where a cue set there lands: on the QUANTIZE grid (the nearest
 * beat or fraction of one) when it is on and the track has a grid, else as
 * it is; clamped to the track. */
double cb_ctl_quantized(const CBDeckCtl *c, double seconds);
void cb_ctl_seek(CBDeckCtl *c, double seconds);            /* a click on the waveform */
/* Grid edits go through here so the deck and the cues stay in step:
 * applies to the track, resolves musical cues, pushes the grid to the deck
 * and reports `changed`. */
void cb_ctl_grid_changed(CBDeckCtl *c);

#endif /* CYANBOX_PLAYER_H */
