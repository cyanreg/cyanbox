/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

#ifndef RXBOX_PLAYER_H
#define RXBOX_PLAYER_H

/* Playback: RX_DECKS decks (one: rxbox prepares tracks, it is not a mixer)
 * mixed into one PipeWire stream.
 *
 * Thread model. The owner thread (the interface's main thread) calls every
 * function here. Commands go to the real-time thread through a lock-free
 * single-producer/single-consumer ring; the real-time thread never
 * allocates, locks, or touches files, and publishes positions through
 * atomics that rx_deck_state() reads. A deck plays a track's packet store
 * (RXPackets: the file's packets in memory, rxbox/audio.h), which a thread
 * of the deck's own decodes around the playhead and at the cue points as
 * it plays, so a track is ready as soon as its first packets are read; or
 * a whole decoded track (RXAudio, native rate, interleaved float). Where
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
 * Loading hands an RXAudio or an RXPackets to the player, which owns it
 * until the deck is unloaded or loaded again; the old one is released only
 * after the real-time thread (and the deck's decoding thread) have
 * acknowledged the swap, so freeing is always safe. A second one may be
 * queued to follow (rx_deck_queue_next): the deck plays on into it at the
 * end of its track, sample-exactly, as one piece of audio.
 *
 * Backends: PipeWire (pw_stream, 48 kHz float stereo, target node from
 * options), or the null backend, which runs the same callback on a timer
 * thread and discards the audio: for tests and headless runs. Nothing
 * here ever picks an output device by itself other than the PipeWire
 * default; tests always use RX_AUDIO_NULL or a null sink. */

#include "rxbox/model.h"
#include "rxbox/audio.h"

#define RX_DECKS 1
/* Cue points a deck keeps decoded (rx_deck_set_cue_points): the start, the
 * cue point, the loop in point, eight hot cues and ten memory cues, and a
 * few to spare. */
#define RX_DECK_CUE_POINTS 24

enum RXAudioBackend {
    RX_AUDIO_PIPEWIRE = 0,
    RX_AUDIO_NULL,
};

typedef struct RXPlayerOptions {
    enum RXAudioBackend backend;
    const char *sink;           /* PipeWire node name to connect to; NULL = default */
    int         rate;           /* 0 = 48000 */
    int         period;         /* frames per callback; 0 = 256. RX_AUDIO_NULL only:
                                 * < 0 is manual mode, no thread; callbacks of
                                 * -period frames run from rx_player_null_advance */
    const char *app_name;       /* "rxbox" */
    /* Called on the owner thread when the player lets go of a buffer it was
     * given, once the real-time thread can no longer read it (or at
     * destroy). NULL: rx_audio_free() and free(), so the RXAudio struct
     * itself must then come from malloc. Packet stores are always freed by
     * the player (rx_packets_free). */
    void      (*release)(void *opaque, RXAudio *audio);
    void       *release_opaque;
} RXPlayerOptions;
void rx_player_options_default(RXPlayerOptions *o);

typedef struct RXPlayer RXPlayer;

int  rx_player_create(RXPlayer **p, const RXPlayerOptions *opt);
void rx_player_destroy(RXPlayer *p);
int  rx_player_rate(const RXPlayer *p);
enum RXAudioBackend rx_player_backend(const RXPlayer *p);
/* The PipeWire node the stream was told to connect to; NULL = the default. */
const char *rx_player_sink(const RXPlayer *p);
/* True once the stream is running; the error string when it is not
 * (NULL while running). */
bool rx_player_running(const RXPlayer *p);
const char *rx_player_error(const RXPlayer *p);
void rx_player_set_master_volume(RXPlayer *p, float gain);   /* 0..1 */
/* Advances the null backend by `frames` synchronously (tests only; no-op
 * on PipeWire and on a null backend with its own thread, period >= 0).
 * Before each callback it waits for a deck playing a packet store to have
 * the audio around its playhead decoded, as a device on a machine that
 * keeps up would find it, so the tests see the same output every run. */
void rx_player_null_advance(RXPlayer *p, int frames);

/* ------------------------------------------------------------------- decks */

/* Takes ownership of `audio` on success (see RXPlayerOptions.release); on
 * error the caller keeps it. The grid is copied (NULL = none). */
int  rx_deck_load(RXPlayer *p, int deck, RXAudio *audio, const RXGrid *grid);
/* The same for a packet store, which the deck decodes as it plays: from
 * here on the deck's thread holds it (the player frees it). */
int  rx_deck_load_packets(RXPlayer *p, int deck, RXPackets *pk, const RXGrid *grid);
/* The places a jump may go to on the deck (the start, the cue point, the
 * loop in point, the hot and memory cues), at most RX_DECK_CUE_POINTS, in
 * seconds. A deck playing a packet store keeps half a second decoded at
 * each, so a cue, a hot cue or a loop going back plays at once while the
 * decoder moves there. Cheap to call with the same points again (every
 * frame: the controls do). */
void rx_deck_set_cue_points(RXPlayer *p, int deck, const double *seconds, int nb);
void rx_deck_unload(RXPlayer *p, int deck);
bool rx_deck_loaded(const RXPlayer *p, int deck);
/* The track to follow the loaded one, taken like a load (the player owns
 * it from here, on success; -ENOENT when nothing is loaded): when the
 * deck plays past the last frame of its track it goes on in this one from
 * its first frame, the step's fraction kept, with no fade, silence or
 * command in between (the two play as one piece of audio); the loop goes
 * with the old track, the tempo and MASTER TEMPO stay. A deck playing a
 * packet store has the start of the next decoded ahead of time. The
 * owner learns of the move from RXDeckState.advanced, after which the
 * state (duration, position) is the next track's and what was loaded is
 * released. Queueing again replaces the queued track; a load, an unload
 * or rx_deck_clear_next drops it. */
int  rx_deck_queue_next(RXPlayer *p, int deck, RXAudio *audio, const RXGrid *grid);
int  rx_deck_queue_next_packets(RXPlayer *p, int deck, RXPackets *pk, const RXGrid *grid);
void rx_deck_clear_next(RXPlayer *p, int deck);
bool rx_deck_has_next(const RXPlayer *p, int deck);
/* The buffer a deck plays (valid until unload; read-only); NULL for a
 * packet store. */
const RXAudio *rx_deck_audio(const RXPlayer *p, int deck);

void rx_deck_play(RXPlayer *p, int deck);
void rx_deck_pause(RXPlayer *p, int deck);
/* Seeks to `seconds` (clamped to the track). Takes effect at the next
 * callback; rx_deck_state reports the new position immediately. */
void rx_deck_seek(RXPlayer *p, int deck, double seconds);
/* Plays from `seconds` (seek + play in one command, for cue/hot cue). */
void rx_deck_play_from(RXPlayer *p, int deck, double seconds);
/* 1.0 = original speed; the tempo slider's ratio. */
void rx_deck_set_tempo(RXPlayer *p, int deck, double ratio);
/* MASTER TEMPO: the deck keeps its pitch while the tempo changes (the
 * varispeed output is shifted back by 1/speed with Signalsmith Stretch on
 * the real-time thread). A property of the deck that survives loads.
 * Returns false when the player has no shifter (creation failed), in which
 * case the deck stays as a CDJ with MASTER TEMPO off. */
bool rx_deck_set_master_tempo(RXPlayer *p, int deck, bool on);
/* Loop between in and out seconds. `enabled` starts looping right away. */
void rx_deck_set_loop(RXPlayer *p, int deck, double in, double out, bool enabled);
void rx_deck_loop_enable(RXPlayer *p, int deck, bool enabled);
void rx_deck_clear_loop(RXPlayer *p, int deck);
void rx_deck_set_volume(RXPlayer *p, int deck, float gain);     /* 0..1 */
/* Pitch bend: a temporary speed offset (+-, e.g. 0.02) while held. */
void rx_deck_set_bend(RXPlayer *p, int deck, double ratio_offset);
/* Scrubbing: while a drag is in progress the deck follows the pointer and
 * plays short grains at the drag position (quiet, filtered) so the ear can
 * find a beat; on end it stays paused there, or resumes if it was playing. */
void rx_deck_scrub_begin(RXPlayer *p, int deck);
void rx_deck_scrub_to(RXPlayer *p, int deck, double seconds);
void rx_deck_scrub_end(RXPlayer *p, int deck);

typedef struct RXDeckState {
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
                               (rx_deck_queue_next): from then on the state is that
                               track's. With MASTER TEMPO on, when the move is heard. */
    bool   has_next;        /* a track is queued to follow */
} RXDeckState;

void rx_deck_state(const RXPlayer *p, int deck, RXDeckState *out);

/* ---------------------------------------------------------------- controls */

/* CDJ/rekordbox button semantics on top of a deck: the cue point, hot cue
 * pads, memory cues, loops, beat jump and quantize. Owns no audio; edits
 * the track's cue list in place and tells the owner through `changed` so
 * the library can be saved. All calls from the owner thread; call
 * rx_ctl_tick() once per interface frame.
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

typedef void (*rx_ctl_changed_cb)(void *opaque, int deck);

/* Where a load leaves the deck (My Settings' AUTO CUE and AUTO CUE LEVEL). */
typedef enum RXAutoCue {
    RX_AUTO_CUE_MEMORY = 0,     /* the first memory cue, else the start (level MEMORY,
                                   the factory setting) */
    RX_AUTO_CUE_OFF,            /* the start */
    RX_AUTO_CUE_LEVEL,          /* a level in dB: the grid's first beat, else the start */
} RXAutoCue;

typedef struct RXDeckCtl {
    RXPlayer *player;
    int       deck;
    RXTrack  *track;            /* the library's track; NULL when empty */
    bool      quantize;
    double    quantize_beats;   /* what QUANTIZE snaps to, in beats: 1 (default), 0.5,
                                   0.25 or 0.125; anything else is a whole beat */
    RXAutoCue auto_cue;         /* applied by rx_ctl_load; RX_AUTO_CUE_MEMORY by default */
    double    cue_point;        /* seconds; < 0 none */
    bool      cue_held;         /* CUE is being held for a preview */
    double    loop_in, loop_out;/* the current/last loop; < 0 none */
    bool      looping;
    int       active_memory_loop;   /* index of the engaged memory loop, -1 */
    double    jump_beats;       /* beat jump size (4 default) */
    double    last_hot_loop_beats;  /* length of the last auto loop (for the pad lamps) */
    rx_ctl_changed_cb changed;
    void     *opaque;
    /* private */
    double    _last_position;
    int       _hot_held;        /* slot held during a paused pad press, or -1 */
    bool      _resume_after_hold;
} RXDeckCtl;

void rx_ctl_init(RXDeckCtl *c, RXPlayer *p, int deck, rx_ctl_changed_cb changed, void *opaque);
/* After rx_deck_load: binds the track and applies auto-cue (c->auto_cue:
 * the cue point and the paused playhead at the first memory cue, the
 * start, or the grid's first beat). */
void rx_ctl_load(RXDeckCtl *c, RXTrack *track);
/* The deck moved on to `track` by itself (rx_deck_queue_next,
 * RXDeckState.advanced): binds it without touching the playhead, which
 * plays on from its start; the cue point goes where a load's auto cue
 * would put it. */
void rx_ctl_continue(RXDeckCtl *c, RXTrack *track);
void rx_ctl_unload(RXDeckCtl *c);
void rx_ctl_tick(RXDeckCtl *c);

void rx_ctl_play_pause(RXDeckCtl *c);
void rx_ctl_cue_press(RXDeckCtl *c);
void rx_ctl_cue_release(RXDeckCtl *c);
void rx_ctl_set_cue_here(RXDeckCtl *c);

void rx_ctl_hot_cue_press(RXDeckCtl *c, int slot);
void rx_ctl_hot_cue_release(RXDeckCtl *c, int slot);
void rx_ctl_hot_cue_delete(RXDeckCtl *c, int slot);
/* Edits the stored hot cue (name/colour) without moving it. */
void rx_ctl_hot_cue_set(RXDeckCtl *c, int slot, const char *name, const char *color);

void rx_ctl_memory_set(RXDeckCtl *c);
void rx_ctl_memory_prev(RXDeckCtl *c);
void rx_ctl_memory_next(RXDeckCtl *c);
void rx_ctl_memory_delete(RXDeckCtl *c);
void rx_ctl_memory_call(RXDeckCtl *c, int cue_index);      /* a specific memory cue */
void rx_ctl_memory_set_active(RXDeckCtl *c, int cue_index, bool active);

void rx_ctl_loop_in(RXDeckCtl *c);
void rx_ctl_loop_out(RXDeckCtl *c);
void rx_ctl_exit_reloop(RXDeckCtl *c);
void rx_ctl_auto_loop(RXDeckCtl *c, double beats);          /* 0.25 .. 64 */
void rx_ctl_loop_half(RXDeckCtl *c);
void rx_ctl_loop_double(RXDeckCtl *c);
void rx_ctl_loop_exit(RXDeckCtl *c);

void rx_ctl_beat_jump(RXDeckCtl *c, double beats);         /* +-, 0 = fine nudge of 10 ms (-0.0 back) */
void rx_ctl_nudge(RXDeckCtl *c, double seconds);
void rx_ctl_set_quantize(RXDeckCtl *c, bool on);
/* `seconds` where a cue set there lands: on the QUANTIZE grid (the nearest
 * beat or fraction of one) when it is on and the track has a grid, else as
 * it is; clamped to the track. */
double rx_ctl_quantized(const RXDeckCtl *c, double seconds);
void rx_ctl_seek(RXDeckCtl *c, double seconds);            /* a click on the waveform */
/* Grid edits go through here so the deck and the cues stay in step:
 * applies to the track, resolves musical cues, pushes the grid to the deck
 * and reports `changed`. */
void rx_ctl_grid_changed(RXDeckCtl *c);

#endif /* RXBOX_PLAYER_H */
