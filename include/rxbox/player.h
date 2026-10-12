/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

#ifndef RXBOX_PLAYER_H
#define RXBOX_PLAYER_H

/* Playback: two decks mixed into one PipeWire stream.
 *
 * Thread model. The owner thread (the interface's main thread) calls every
 * function here. Commands go to the real-time thread through a lock-free
 * single-producer/single-consumer ring; the real-time thread never
 * allocates, locks, or touches files, and publishes positions through
 * atomics that rx_deck_state() reads. A deck plays a whole track decoded
 * in memory (RxAudio, native rate, interleaved float); the real-time
 * thread resamples to the device rate with a cubic interpolator, which is
 * also how tempo changes play (pitch follows tempo, as a CDJ without
 * master tempo). Loops wrap sample-exactly inside the callback.
 *
 * Loading hands an RxAudio to the player, which owns it until the deck is
 * unloaded or loaded again; the old buffer is released only after the
 * real-time thread has acknowledged the swap, so freeing is always safe.
 *
 * Backends: PipeWire (pw_stream, 48 kHz float stereo, target node from
 * options), or the null backend, which runs the same callback on a timer
 * thread and discards the audio: for tests and headless runs. Nothing
 * here ever picks an output device by itself other than the PipeWire
 * default; tests always use RX_AUDIO_NULL or a null sink. */

#include "rxbox/model.h"
#include "rxbox/audio.h"

#define RX_DECKS 1

enum RxAudioBackend {
    RX_AUDIO_PIPEWIRE = 0,
    RX_AUDIO_NULL,
};

typedef struct RxPlayerOptions {
    enum RxAudioBackend backend;
    const char *sink;           /* PipeWire node name to connect to; NULL = default */
    int         rate;           /* 0 = 48000 */
    int         period;         /* frames per callback; 0 = 256 */
    const char *app_name;       /* "rxbox" */
} RxPlayerOptions;
void rx_player_options_default(RxPlayerOptions *o);

typedef struct RxPlayer RxPlayer;

int  rx_player_create(RxPlayer **p, const RxPlayerOptions *opt);
void rx_player_destroy(RxPlayer *p);
int  rx_player_rate(const RxPlayer *p);
/* True once the stream is running; the error string when it is not. */
bool rx_player_running(const RxPlayer *p);
const char *rx_player_error(const RxPlayer *p);
void rx_player_set_master_volume(RxPlayer *p, float gain);   /* 0..1 */
/* Advances the null backend by `frames` synchronously (tests only; no-op
 * on PipeWire). */
void rx_player_null_advance(RxPlayer *p, int frames);

/* ------------------------------------------------------------------- decks */

/* Takes ownership of `audio`. The grid is copied (NULL = none). */
int  rx_deck_load(RxPlayer *p, int deck, RxAudio *audio, const RxGrid *grid);
void rx_deck_unload(RxPlayer *p, int deck);
bool rx_deck_loaded(const RxPlayer *p, int deck);
/* The buffer a deck plays (valid until unload; read-only). */
const RxAudio *rx_deck_audio(const RxPlayer *p, int deck);

void rx_deck_play(RxPlayer *p, int deck);
void rx_deck_pause(RxPlayer *p, int deck);
/* Seeks to `seconds` (clamped to the track). Takes effect at the next
 * callback; rx_deck_state reports the new position immediately. */
void rx_deck_seek(RxPlayer *p, int deck, double seconds);
/* Plays from `seconds` (seek + play in one command, for cue/hot cue). */
void rx_deck_play_from(RxPlayer *p, int deck, double seconds);
/* 1.0 = original speed; the tempo slider's ratio. */
void rx_deck_set_tempo(RxPlayer *p, int deck, double ratio);
/* Reserved: pitch-preserving tempo. v1 ignores it and reports false. */
bool rx_deck_set_master_tempo(RxPlayer *p, int deck, bool on);
/* Loop between in and out seconds. `enabled` starts looping right away. */
void rx_deck_set_loop(RxPlayer *p, int deck, double in, double out, bool enabled);
void rx_deck_loop_enable(RxPlayer *p, int deck, bool enabled);
void rx_deck_clear_loop(RxPlayer *p, int deck);
void rx_deck_set_volume(RxPlayer *p, int deck, float gain);     /* 0..1 */
/* Pitch bend: a temporary speed offset (+-, e.g. 0.02) while held. */
void rx_deck_set_bend(RxPlayer *p, int deck, double ratio_offset);
/* Scrubbing: while a drag is in progress the deck follows the pointer and
 * plays short grains at the drag position (quiet, filtered) so the ear can
 * find a beat; on end it stays paused there, or resumes if it was playing. */
void rx_deck_scrub_begin(RxPlayer *p, int deck);
void rx_deck_scrub_to(RxPlayer *p, int deck, double seconds);
void rx_deck_scrub_end(RxPlayer *p, int deck);

typedef struct RxDeckState {
    bool   loaded;
    bool   playing;
    bool   scrubbing;
    double position;        /* seconds on the track's timeline */
    double duration;
    double tempo;           /* ratio */
    bool   loop_enabled;
    double loop_in, loop_out;   /* < 0 when no loop is set */
    float  level_l, level_r;    /* recent peak, 0..1 */
    int64_t frame;          /* position in track frames */
    uint64_t serial;        /* bumps on every command the RT thread applied */
} RxDeckState;

void rx_deck_state(const RxPlayer *p, int deck, RxDeckState *out);

/* ---------------------------------------------------------------- controls */

/* CDJ/rekordbox button semantics on top of a deck: the cue point, hot cue
 * pads, memory cues, loops, beat jump and quantize. Owns no audio; edits
 * the track's cue list in place and tells the owner through `changed` so
 * the sidecar can be saved. All calls from the owner thread; call
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
 *  - QUANTIZE snaps cue, hot cue and loop points to the nearest beat. */

typedef void (*rx_ctl_changed_cb)(void *opaque, int deck);

typedef struct RxDeckCtl {
    RxPlayer *player;
    int       deck;
    RxTrack  *track;            /* the library's track; NULL when empty */
    bool      quantize;
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
} RxDeckCtl;

void rx_ctl_init(RxDeckCtl *c, RxPlayer *p, int deck, rx_ctl_changed_cb changed, void *opaque);
/* After rx_deck_load: binds the track and applies auto-cue (position at
 * the first memory cue when there is one, else the start). */
void rx_ctl_load(RxDeckCtl *c, RxTrack *track);
void rx_ctl_unload(RxDeckCtl *c);
void rx_ctl_tick(RxDeckCtl *c);

void rx_ctl_play_pause(RxDeckCtl *c);
void rx_ctl_cue_press(RxDeckCtl *c);
void rx_ctl_cue_release(RxDeckCtl *c);
void rx_ctl_set_cue_here(RxDeckCtl *c);

void rx_ctl_hot_cue_press(RxDeckCtl *c, int slot);
void rx_ctl_hot_cue_release(RxDeckCtl *c, int slot);
void rx_ctl_hot_cue_delete(RxDeckCtl *c, int slot);
/* Edits the stored hot cue (name/colour) without moving it. */
void rx_ctl_hot_cue_set(RxDeckCtl *c, int slot, const char *name, const char *color);

void rx_ctl_memory_set(RxDeckCtl *c);
void rx_ctl_memory_prev(RxDeckCtl *c);
void rx_ctl_memory_next(RxDeckCtl *c);
void rx_ctl_memory_delete(RxDeckCtl *c);
void rx_ctl_memory_call(RxDeckCtl *c, int cue_index);      /* a specific memory cue */
void rx_ctl_memory_set_active(RxDeckCtl *c, int cue_index, bool active);

void rx_ctl_loop_in(RxDeckCtl *c);
void rx_ctl_loop_out(RxDeckCtl *c);
void rx_ctl_exit_reloop(RxDeckCtl *c);
void rx_ctl_auto_loop(RxDeckCtl *c, double beats);          /* 0.25 .. 64 */
void rx_ctl_loop_half(RxDeckCtl *c);
void rx_ctl_loop_double(RxDeckCtl *c);
void rx_ctl_loop_exit(RxDeckCtl *c);

void rx_ctl_beat_jump(RxDeckCtl *c, double beats);         /* +-, 0 = fine nudge of 10 ms */
void rx_ctl_nudge(RxDeckCtl *c, double seconds);
void rx_ctl_set_quantize(RxDeckCtl *c, bool on);
void rx_ctl_seek(RxDeckCtl *c, double seconds);            /* a click on the waveform */
/* Grid edits go through here so the deck and the cues stay in step:
 * applies to the track, resolves musical cues, pushes the grid to the deck
 * and reports `changed`. */
void rx_ctl_grid_changed(RxDeckCtl *c);

#endif /* RXBOX_PLAYER_H */
