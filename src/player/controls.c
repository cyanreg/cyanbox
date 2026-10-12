/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* The deck's buttons: CUE, PLAY/PAUSE, hot cue pads, the MEMORY cluster,
 * loops, beat jump and QUANTIZE, with the semantics of a CDJ-3000 and of
 * rekordbox's Export mode. Everything runs on the owner thread; the deck
 * is driven only through the public rx_deck_* functions, and positions
 * are read back through rx_deck_state(), which already reports a seek
 * that the real-time thread has not applied yet.
 *
 * Sources: the CDJ-3000 operating instructions (cited as "manual p.N"),
 * and rbxport's observations of rekordbox 7 (src/lib/cues.ts,
 * src/lib/player.ts, src/views/player/useHotCues.ts and useMemoryCues.ts),
 * cited by file. Where neither says, the choice is marked as ours.
 *
 * The cue point and the current loop live here, not in the player: the
 * player only knows the loop it is told to wrap. Every edit of the cue
 * list goes through rx_track_add_cue/rx_track_remove_cue, attaches new
 * cues to the grid (musical when QUANTIZE is on and there is a grid, so
 * they follow grid corrections; absolute otherwise) and ends with
 * `changed` so the owner saves the track to the library. */

#include "rxbox/player.h"

#include <math.h>
#include <stdio.h>

#include "player_internal.h"

/* How close the playhead must be to a memory cue to be "on" it, and how
 * far beyond it to be before/after it (rbxport src/lib/cues.ts,
 * MEMORY_TOLERANCE_MS: rekordbox's own value is undocumented). */
#define MEMORY_TOLERANCE 0.020

/* How close a paused playhead must be to the cue point for CUE to preview
 * rather than set a new point. Ours: a CDJ is either standing on its cue
 * point or not, so this is only rounding, and it must stay well under the
 * 10 ms fine search, or a nudged pause position could not become a new
 * cue point. */
#define CUE_TOLERANCE 0.001

/* Fine jump, rbxport src/lib/player.ts FINE_JUMP_SECONDS. */
#define FINE_JUMP 0.010

/* rekordbox stores at most ten memory cues and loops together (rbxport
 * useMemoryCues.ts, from UiPlayer::eventMemoryCue), as many as a CDJ keeps
 * per track on a stick (manual p.85, "Up to 10 cue points/loop points"). */
#define MEMORY_CUE_LIMIT 10

/* Loop lengths a DJ can reach with /2 and x2, in beats. */
#define LOOP_BEATS_MIN (1.0 / 32.0)
#define LOOP_BEATS_MAX 512.0

/* A loop shorter than this (seconds, no grid) is not a loop. */
#define LOOP_MIN 0.005

/* Further than this between two ticks is a jump, not playback reaching a
 * point (for active memory loops). */
#define TICK_MAX_STEP 1.0

/* --------------------------------------------------------------- helpers */

static bool ready(const RxDeckCtl *c)
{
    return c && c->track && rx_deck_loaded(c->player, c->deck);
}

static RxDeckState state(const RxDeckCtl *c)
{
    RxDeckState s;
    rx_deck_state(c->player, c->deck, &s);
    return s;
}

static const RxGrid *grid(const RxDeckCtl *c)
{
    return c->track && rx_grid_valid(&c->track->grid) ? &c->track->grid : NULL;
}

/* QUANTIZE: the nearest beat, when it is on and the track has a grid
 * (manual p.61: no quantize without an analysed grid). */
static double snap(const RxDeckCtl *c, double t, double duration)
{
    const RxGrid *g = grid(c);
    if (c->quantize && g)
        t = rx_grid_quantize(g, t);
    return RX_CLAMP(t, 0.0, duration);
}

static void notify(RxDeckCtl *c)
{
    if (c->changed)
        c->changed(c->opaque, c->deck);
}

static bool inside_loop(const RxDeckCtl *c, double t)
{
    return c->loop_out > c->loop_in && t >= c->loop_in && t < c->loop_out;
}

/* Leaves the loop, keeping it for RELOOP. */
static void leave_loop(RxDeckCtl *c)
{
    if (!c->looping)
        return;
    c->looping = false;
    c->active_memory_loop = -1;
    rx_deck_loop_enable(c->player, c->deck, false);
}

/* Moves the playhead, keeping the playing state. A target outside the
 * loop leaves it; inside it, the deck keeps looping. */
static void move_to(RxDeckCtl *c, double t)
{
    if (c->looping && !inside_loop(c, t))
        leave_loop(c);
    rx_deck_seek(c->player, c->deck, t);
    c->_last_position = t;
}

/* Engages [in, out) on the deck and remembers it as the current loop. */
static void engage_loop(RxDeckCtl *c, double in, double out)
{
    c->loop_in = in;
    c->loop_out = out;
    c->looping = true;
    rx_deck_set_loop(c->player, c->deck, in, out, true);
}

static double loop_beats(const RxDeckCtl *c, double in, double out)
{
    const RxGrid *g = grid(c);
    if (!g)
        return 0.0;
    return rx_grid_time_to_beat(g, out) - rx_grid_time_to_beat(g, in);
}

/* The memory loop with these points, or -1. */
static int find_memory_loop(const RxDeckCtl *c, double in, double out, bool active_only)
{
    if (!c->track || in < 0.0)
        return -1;
    for (int i = 0; i < c->track->nb_cues; i++) {
        const RxCue *cue = &c->track->cues[i];
        if (rx_cue_is_hot(cue) || !rx_cue_is_loop(cue) || (active_only && !cue->active))
            continue;
        if (fabs(cue->time - in) < 1e-6 && fabs(cue->loop_end - out) < 1e-6)
            return i;
    }
    return -1;
}

/* After a cue list edit: indices move (the list is sorted by time), so
 * the engaged memory loop is looked up again by its points. */
static void edited(RxDeckCtl *c)
{
    if (c->active_memory_loop >= 0)
        c->active_memory_loop = find_memory_loop(c, c->loop_in, c->loop_out, false);
    notify(c);
}

/* New cues follow grid corrections when they were placed by QUANTIZE
 * (musical: a beat index, and a loop length in beats), and stay where
 * they are otherwise. */
static void attach(const RxDeckCtl *c, RxCue *cue)
{
    rx_cue_attach_to_grid(cue, c->quantize ? grid(c) : NULL);
}

static int nb_memory_cues(const RxTrack *t)
{
    int n = 0;
    for (int i = 0; i < t->nb_cues; i++)
        n += !rx_cue_is_hot(&t->cues[i]);
    return n;
}

/* --------------------------------------------------------------- binding */

void rx_ctl_init(RxDeckCtl *c, RxPlayer *p, int deck, rx_ctl_changed_cb changed, void *opaque)
{
    *c = (RxDeckCtl) {
        .player              = p,
        .deck                = deck,
        .quantize            = true,
        .cue_point           = -1.0,
        .loop_in             = -1.0,
        .loop_out            = -1.0,
        .active_memory_loop  = -1,
        .jump_beats          = 4.0,     /* rbxport player.ts JUMP_SIZE_ID "4beats" */
        .last_hot_loop_beats = 4.0,
        .changed             = changed,
        .opaque              = opaque,
        ._hot_held           = -1,
    };
}

static void reset(RxDeckCtl *c)
{
    c->cue_point          = -1.0;
    c->cue_held           = false;
    c->loop_in            = -1.0;
    c->loop_out           = -1.0;
    c->looping            = false;
    c->active_memory_loop = -1;
    c->_last_position     = 0.0;
    c->_hot_held          = -1;
    c->_resume_after_hold = false;
}

/* Auto cue: the cue point goes to the memory cue nearest the start, or the
 * start (manual p.74, AUTO CUE LEVEL "MEMORY", the factory setting), and
 * the deck waits there. */
void rx_ctl_load(RxDeckCtl *c, RxTrack *track)
{
    reset(c);
    c->track = track;
    if (!track)
        return;

    int first = rx_track_memory_cue_nth(track, 1);
    c->cue_point = first >= 0 ? track->cues[first].time : 0.0;
    rx_deck_clear_loop(c->player, c->deck);
    rx_deck_seek(c->player, c->deck, c->cue_point);
    c->_last_position = c->cue_point;
}

void rx_ctl_unload(RxDeckCtl *c)
{
    reset(c);
    c->track = NULL;
}

/* Active memory loops engage when playback reaches their in point (manual
 * p.56, "Using Active Loop"): the playhead moved forward, by less than a
 * jump, from at or before the in point to inside the loop. Jumps made
 * here reset the reference to their target, so landing exactly on an in
 * point and playing on counts as reaching it; a playhead that was put
 * past the in point does not. Not while another loop plays. */
static void engage_active_loops(RxDeckCtl *c, const RxDeckState *s)
{
    double last = c->_last_position, pos = s->position;
    if (c->looping || !s->playing || s->scrubbing || pos <= last || pos - last > TICK_MAX_STEP)
        return;

    for (int i = 0; i < c->track->nb_cues; i++) {
        const RxCue *cue = &c->track->cues[i];
        if (rx_cue_is_hot(cue) || !rx_cue_is_loop(cue) || !cue->active)
            continue;
        if (last <= cue->time && pos >= cue->time && pos < cue->loop_end) {
            engage_loop(c, cue->time, cue->loop_end);
            c->active_memory_loop = i;
            return;
        }
    }
}

void rx_ctl_tick(RxDeckCtl *c)
{
    if (!ready(c))
        return;
    RxDeckState s = state(c);
    engage_active_loops(c, &s);
    c->_last_position = s.position;
}

/* ------------------------------------------------------------- transport */

void rx_ctl_play_pause(RxDeckCtl *c)
{
    if (!ready(c))
        return;
    /* PLAY during a CUE preview: playback carries on when CUE is let go
     * (the CDJ cue sampler; ours where the manual is silent). */
    if (c->cue_held) {
        c->_resume_after_hold = true;
        return;
    }
    if (state(c).playing)
        rx_deck_pause(c->player, c->deck);
    else
        rx_deck_play(c->player, c->deck);
}

void rx_ctl_cue_press(RxDeckCtl *c)
{
    if (!ready(c))
        return;
    RxDeckState s = state(c);

    /* Playing: back to the cue point and pause (manual p.51, "Jumping to
     * a cue point"). */
    if (s.playing) {
        double cue = c->cue_point >= 0.0 ? c->cue_point : 0.0;
        rx_deck_pause(c->player, c->deck);
        move_to(c, cue);
        return;
    }

    /* Paused on the cue point: play while held (manual p.52, "Cue Point
     * Sampler"). */
    if (c->cue_point >= 0.0 && fabs(s.position - c->cue_point) < CUE_TOLERANCE) {
        c->cue_held = true;
        c->_resume_after_hold = false;
        rx_deck_play(c->player, c->deck);
        return;
    }

    /* Paused elsewhere: that becomes the cue point (manual p.51, "Setting
     * a cue point ... During pause"), on the beat with QUANTIZE, and the
     * paused playhead moves onto it so the next press previews from it. */
    c->cue_point = snap(c, s.position, s.duration);
    if (c->cue_point != s.position)
        move_to(c, c->cue_point);
}

void rx_ctl_cue_release(RxDeckCtl *c)
{
    if (!c || !c->cue_held)
        return;
    c->cue_held = false;
    if (c->_resume_after_hold) {
        c->_resume_after_hold = false;
        return;
    }
    if (!ready(c))
        return;
    /* End of the preview: back to the cue point, paused. */
    rx_deck_pause(c->player, c->deck);
    move_to(c, c->cue_point);
}

/* LOOP IN/CUE during playback sets the cue point (manual p.51); this is
 * the same without the loop, for a "set cue" key. */
void rx_ctl_set_cue_here(RxDeckCtl *c)
{
    if (!ready(c))
        return;
    RxDeckState s = state(c);
    c->cue_point = snap(c, s.position, s.duration);
    if (!s.playing && c->cue_point != s.position)
        move_to(c, c->cue_point);
}

/* -------------------------------------------------------------- hot cues */

static void call_hot_cue(RxDeckCtl *c, const RxCue *cue)
{
    /* A pad pressed during a CUE preview takes over: playback continues
     * after CUE is released (ours). */
    if (c->cue_held)
        c->_resume_after_hold = true;

    double t = cue->time;
    if (rx_cue_is_loop(cue) && cue->loop_end > t) {
        /* A hot loop plays as a loop from its in point; RELOOP/EXIT
         * leaves it (manual p.59). One that ends before it starts (an
         * imported loop entry with no end) is called as a hot cue: the
         * deck would refuse the loop. */
        rx_deck_play_from(c->player, c->deck, t);
        engage_loop(c, t, cue->loop_end);
        c->active_memory_loop = -1;
        double beats = loop_beats(c, t, cue->loop_end);
        if (beats > 0.0)
            c->last_hot_loop_beats = beats;
    } else {
        /* A call leaves a playing loop at once (rbxport useHotCues.ts) and
         * plays from the cue, also from pause (manual p.59, "Playback
         * starts from the Hot Cue point"). It does not become the cue
         * point. */
        leave_loop(c);
        rx_deck_play_from(c->player, c->deck, t);
    }
    c->_last_position = t;
}

void rx_ctl_hot_cue_press(RxDeckCtl *c, int slot)
{
    if (!ready(c) || slot < 0 || slot >= RX_HOT_CUE_SLOTS)
        return;
    c->_hot_held = slot;

    int idx = rx_track_hot_cue(c->track, slot);
    if (idx >= 0) {
        call_hot_cue(c, &c->track->cues[idx]);
        return;
    }

    /* An empty pad stores the playhead, or the playing loop as a hot loop
     * (manual p.58, "If you do this during loop playback, a loop is set
     * instead"). Setting leaves the transport alone. */
    RxDeckState s = state(c);
    RxCue cue;
    if (c->looping && c->loop_out > c->loop_in) {
        rx_cue_init(&cue, c->loop_in);
        cue.loop_end = c->loop_out;
    } else {
        rx_cue_init(&cue, snap(c, s.position, s.duration));
    }
    cue.hot = slot;
    attach(c, &cue);

    int ret = rx_track_add_cue(c->track, &cue);
    if (ret < 0) {
        rx_err("player: cannot store hot cue %c: %s", 'A' + slot, strerror(-ret));
        return;
    }
    edited(c);
}

void rx_ctl_hot_cue_release(RxDeckCtl *c, int slot)
{
    /* Pads are not gates in Export mode (rbxport useHotCues.ts: rekordbox's
     * "Gate Cue" preference is off by default), so a release only ends
     * the press. */
    if (c && c->_hot_held == slot)
        c->_hot_held = -1;
}

void rx_ctl_hot_cue_delete(RxDeckCtl *c, int slot)
{
    if (!c || !c->track || slot < 0 || slot >= RX_HOT_CUE_SLOTS)
        return;
    int idx = rx_track_hot_cue(c->track, slot);
    if (idx < 0)
        return;
    if (rx_track_remove_cue(c->track, idx) < 0)
        return;
    edited(c);
}

void rx_ctl_hot_cue_set(RxDeckCtl *c, int slot, const char *name, const char *color)
{
    if (!c || !c->track || slot < 0 || slot >= RX_HOT_CUE_SLOTS)
        return;
    int idx = rx_track_hot_cue(c->track, slot);
    if (idx < 0)
        return;

    /* Adding into an occupied slot replaces it (model.h), which keeps the
     * edit inside the cue list functions. */
    RxCue cue = c->track->cues[idx];
    if (name)
        snprintf(cue.name, sizeof(cue.name), "%s", name);
    if (color)
        snprintf(cue.color, sizeof(cue.color), "%s", color);
    if (rx_track_add_cue(c->track, &cue) < 0)
        return;
    edited(c);
}

/* ---------------------------------------------------------------- memory */

/* MEMORY stores the cue point, never the playhead, or the playing loop
 * (manual p.52 and p.57; rbxport useMemoryCues.ts: rekordbox reads the
 * deck's cue time). Nothing is stored when the same point is already
 * there or the list is full. */
void rx_ctl_memory_set(RxDeckCtl *c)
{
    if (!ready(c))
        return;

    RxCue cue;
    if (c->looping && c->loop_out > c->loop_in) {
        rx_cue_init(&cue, c->loop_in);
        cue.loop_end = c->loop_out;
    } else if (c->cue_point >= 0.0) {
        rx_cue_init(&cue, c->cue_point);
    } else {
        return;
    }
    cue.hot = -1;

    int at = rx_track_memory_cue_at(c->track, cue.time, MEMORY_TOLERANCE);
    if (at >= 0) {
        const RxCue *old = &c->track->cues[at];
        bool loop = rx_cue_is_loop(&cue);
        bool same = rx_cue_is_loop(old) == loop &&
                    (!loop || fabs(old->loop_end - cue.loop_end) < MEMORY_TOLERANCE);
        if (same)
            return;
    }
    if (nb_memory_cues(c->track) >= MEMORY_CUE_LIMIT) {
        rx_info("player: a track holds at most %d memory cues and loops", MEMORY_CUE_LIMIT);
        return;
    }

    attach(c, &cue);
    int ret = rx_track_add_cue(c->track, &cue);
    if (ret < 0) {
        rx_err("player: cannot store memory cue: %s", strerror(-ret));
        return;
    }
    edited(c);
}

/* Calling a memory cue moves there and makes it the cue point, keeping
 * the playing state (manual p.52; rbxport useMemoryCues.ts). A memory
 * loop is called as the loop (manual p.57). */
static void call_memory_cue(RxDeckCtl *c, int idx)
{
    const RxCue *cue = &c->track->cues[idx];
    double t = cue->time;
    c->cue_point = t;

    if (rx_cue_is_loop(cue) && cue->loop_end > t) {
        rx_deck_seek(c->player, c->deck, t);
        engage_loop(c, t, cue->loop_end);
        c->active_memory_loop = cue->active ? idx : -1;
        c->_last_position = t;
        return;
    }
    move_to(c, t);
}

void rx_ctl_memory_prev(RxDeckCtl *c)
{
    if (!ready(c))
        return;
    int idx = rx_track_memory_cue_before(c->track, state(c).position, MEMORY_TOLERANCE);
    if (idx >= 0)
        call_memory_cue(c, idx);
}

void rx_ctl_memory_next(RxDeckCtl *c)
{
    if (!ready(c))
        return;
    int idx = rx_track_memory_cue_after(c->track, state(c).position, MEMORY_TOLERANCE);
    if (idx >= 0)
        call_memory_cue(c, idx);
}

/* DELETE removes the memory cue the playhead stands on, and nothing away
 * from one (manual p.53; rbxport cues.ts memoryCueAt). */
void rx_ctl_memory_delete(RxDeckCtl *c)
{
    if (!ready(c))
        return;
    int idx = rx_track_memory_cue_at(c->track, state(c).position, MEMORY_TOLERANCE);
    if (idx < 0)
        return;
    if (idx == c->active_memory_loop)
        c->active_memory_loop = -1;
    if (rx_track_remove_cue(c->track, idx) < 0)
        return;
    edited(c);
}

void rx_ctl_memory_call(RxDeckCtl *c, int cue_index)
{
    if (!ready(c) || cue_index < 0 || cue_index >= c->track->nb_cues)
        return;
    call_memory_cue(c, cue_index);
}

void rx_ctl_memory_set_active(RxDeckCtl *c, int cue_index, bool active)
{
    if (!c || !c->track || cue_index < 0 || cue_index >= c->track->nb_cues)
        return;
    RxCue cue = c->track->cues[cue_index];
    if (rx_cue_is_hot(&cue) || !rx_cue_is_loop(&cue) || cue.active == active)
        return;

    /* Memory cues are not unique by slot, so an edit is a remove and an
     * add. */
    cue.active = active;
    if (rx_track_remove_cue(c->track, cue_index) < 0)
        return;
    if (rx_track_add_cue(c->track, &cue) < 0)
        rx_err("player: cannot store memory loop");
    if (!active && c->active_memory_loop == cue_index)
        c->active_memory_loop = -1;
    edited(c);
}

/* ----------------------------------------------------------------- loops */

/* LOOP IN: a new in point at the playhead, on the beat with QUANTIZE; it
 * is also the cue point (the button is LOOP IN/CUE, manual p.51). A loop
 * that was playing is left: the next OUT closes the new one. */
void rx_ctl_loop_in(RxDeckCtl *c)
{
    if (!ready(c))
        return;
    RxDeckState s = state(c);
    double t = snap(c, s.position, s.duration);

    if (c->looping || c->loop_out >= 0.0)
        rx_deck_clear_loop(c->player, c->deck);
    c->looping = false;
    c->active_memory_loop = -1;
    c->loop_in = t;
    c->loop_out = -1.0;
    c->cue_point = t;
    if (!s.playing && t != s.position)
        move_to(c, t);
}

/* LOOP OUT closes the loop at the playhead (on the beat with QUANTIZE) and
 * loops at once (manual p.54). The in point is the one LOOP IN left
 * pending, else the cue point, which on a CDJ is the same button's other
 * half. During a loop, OUT moves the out point to the playhead (a
 * simplified OUT ADJUST, manual p.55). An out point quantised onto the
 * in point becomes the next beat. */
void rx_ctl_loop_out(RxDeckCtl *c)
{
    if (!ready(c))
        return;
    RxDeckState s = state(c);

    double in;
    if (c->looping || (c->loop_in >= 0.0 && c->loop_out < 0.0))
        in = c->loop_in;
    else
        in = c->cue_point;
    if (in < 0.0 || s.position < in)
        return;

    double out = snap(c, s.position, s.duration);
    if (out - in < LOOP_MIN) {
        const RxGrid *g = grid(c);
        if (!c->quantize || !g)
            return;
        out = RX_MIN(rx_grid_beat_after(g, in), s.duration);
        if (out - in < LOOP_MIN)
            return;
    }

    c->active_memory_loop = -1;
    engage_loop(c, in, out);
}

void rx_ctl_loop_exit(RxDeckCtl *c)
{
    if (ready(c))
        leave_loop(c);
}

/* RELOOP/EXIT: leaves a playing loop; otherwise goes back to the last
 * loop's in point and loops again (manual p.56), keeping the playing
 * state. */
void rx_ctl_exit_reloop(RxDeckCtl *c)
{
    if (!ready(c))
        return;
    if (c->looping) {
        leave_loop(c);
        return;
    }
    if (c->loop_in < 0.0 || c->loop_out <= c->loop_in)
        return;
    rx_deck_seek(c->player, c->deck, c->loop_in);
    engage_loop(c, c->loop_in, c->loop_out);
    c->active_memory_loop = find_memory_loop(c, c->loop_in, c->loop_out, true);
    c->_last_position = c->loop_in;
}

/* Auto beat loop: N beats on the grid from the playhead (on the beat with
 * QUANTIZE; manual p.54-55). During a loop the same size again leaves it
 * and another size resizes it from its in point (rekordbox's beat loop
 * buttons; ours where the manual is silent). Needs a grid: without one
 * there is no beat to measure. */
void rx_ctl_auto_loop(RxDeckCtl *c, double beats)
{
    if (!ready(c) || !(beats > 0.0))
        return;
    const RxGrid *g = grid(c);
    if (!g) {
        rx_debug("player: beat loop needs a beat grid");
        return;
    }
    beats = RX_CLAMP(beats, LOOP_BEATS_MIN, LOOP_BEATS_MAX);

    RxDeckState s = state(c);
    double in;
    if (c->looping) {
        if (fabs(loop_beats(c, c->loop_in, c->loop_out) - beats) < 1e-6) {
            leave_loop(c);
            return;
        }
        in = c->loop_in;
    } else {
        in = snap(c, s.position, s.duration);
    }

    double out = RX_MIN(rx_grid_offset_beats(g, in, beats), s.duration);
    if (out - in < LOOP_MIN)
        return;
    c->active_memory_loop = -1;
    c->last_hot_loop_beats = beats;
    engage_loop(c, in, out);
}

/* /2 and x2 keep the in point (manual p.56). On the grid when there is
 * one, so a loop over a tempo change still halves into whole beats. */
static void resize_loop(RxDeckCtl *c, double factor)
{
    if (!ready(c) || c->loop_in < 0.0 || c->loop_out <= c->loop_in)
        return;
    RxDeckState s = state(c);
    const RxGrid *g = grid(c);
    double out;
    if (g) {
        double beats = loop_beats(c, c->loop_in, c->loop_out) * factor;
        if (beats < LOOP_BEATS_MIN - 1e-9 || beats > LOOP_BEATS_MAX + 1e-9)
            return;
        out = rx_grid_offset_beats(g, c->loop_in, beats);
        c->last_hot_loop_beats = beats;
    } else {
        out = c->loop_in + (c->loop_out - c->loop_in) * factor;
    }
    out = RX_MIN(out, s.duration);
    if (out - c->loop_in < LOOP_MIN)
        return;

    c->loop_out = out;
    c->active_memory_loop = -1;
    rx_deck_set_loop(c->player, c->deck, c->loop_in, out, c->looping);
}

void rx_ctl_loop_half(RxDeckCtl *c)
{
    resize_loop(c, 0.5);
}

void rx_ctl_loop_double(RxDeckCtl *c)
{
    resize_loop(c, 2.0);
}

/* ----------------------------------------------------------------- jumps */

/* Beat jump: +-N beats, keeping the playing state (manual p.62). With
 * QUANTIZE the jump lands on the grid, as rekordbox's does: the playhead
 * is snapped to the nearest beat, then moved N whole grid beats, beat by
 * beat through every section crossed. Without it the jump is N beats of
 * the tempo at the playhead, in seconds, from the exact position (rbxport
 * player.ts jumpSeconds). During a loop the loop moves N beats with the
 * playhead (manual p.62, Loop Move). 0 is the 10 ms fine jump, forward,
 * or back for -0.0. */
void rx_ctl_beat_jump(RxDeckCtl *c, double beats)
{
    if (!ready(c) || !isfinite(beats))
        return;
    if (beats == 0.0) {
        rx_ctl_nudge(c, signbit(beats) ? -FINE_JUMP : FINE_JUMP);
        return;
    }

    const RxGrid *g = grid(c);
    if (!g)
        return;
    RxDeckState s = state(c);
    bool on_grid = c->quantize;
    double step = beats * 60.0 / rx_grid_bpm_at(g, s.position);
    double target = s.position + step;
    if (on_grid)
        target = rx_grid_offset_beats(g, rx_grid_quantize(g, s.position), beats);

    if (c->looping) {
        double in  = on_grid ? rx_grid_offset_beats(g, c->loop_in, beats) : c->loop_in + step;
        double out = on_grid ? rx_grid_offset_beats(g, c->loop_out, beats) : c->loop_out + step;
        if (in < 0.0 || out > s.duration)
            return;
        /* From the loop's last half beat the snapped playhead lands on
         * the moved out point, which inside a loop is its in point. */
        if (target >= out)
            target -= out - in;
        target = RX_CLAMP(target, in, out);
        rx_deck_seek(c->player, c->deck, target);
        c->active_memory_loop = -1;
        engage_loop(c, in, out);
        c->_last_position = target;
        return;
    }

    move_to(c, RX_CLAMP(target, 0.0, s.duration));
}

void rx_ctl_nudge(RxDeckCtl *c, double seconds)
{
    if (!ready(c) || !isfinite(seconds))
        return;
    RxDeckState s = state(c);
    move_to(c, RX_CLAMP(s.position + seconds, 0.0, s.duration));
}

void rx_ctl_set_quantize(RxDeckCtl *c, bool on)
{
    if (c)
        c->quantize = on;
}

void rx_ctl_seek(RxDeckCtl *c, double seconds)
{
    if (!ready(c) || !isfinite(seconds))
        return;
    RxDeckState s = state(c);
    move_to(c, RX_CLAMP(seconds, 0.0, s.duration));
}

/* After a grid edit on the track: musical cues move with the grid, the
 * deck gets the new grid, and the owner saves. The cue point and the
 * current loop are positions, not cues, and stay where they are. */
void rx_ctl_grid_changed(RxDeckCtl *c)
{
    if (!c || !c->track)
        return;
    rx_track_resolve_cues(c->track);
    if (rx_deck_loaded(c->player, c->deck) &&
        rx_player_deck_set_grid(c->player, c->deck, &c->track->grid) < 0)
        rx_err("player: out of memory copying the beat grid");
    if (c->active_memory_loop >= 0)
        c->active_memory_loop = -1;
    notify(c);
}
