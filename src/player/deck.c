/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* One deck as the real-time thread sees it: the commands that change it
 * and the renderer that adds it to the mix.
 *
 * Everything here runs on the real-time thread and follows its rules: no
 * allocation, no locks, no logging, no system calls but the eventfd write
 * that wakes the deck's feed thread (non-blocking, as PipeWire's own data
 * thread wakes its main loop). The buffer is read,
 * never written; the owner keeps it alive until this deck has applied the
 * command that replaced it and publishes that it reads it no more (see
 * the release path in player.c). Voices keep positions, never a pointer,
 * so after the swap nothing reads the old buffer; a load or unload still
 * silences the de-click tail and the scrub grains at once, or they would
 * play the old track's positions out of the new one.
 *
 * A deck playing a stream reads its frames from the chunks its feed
 * thread puts in the stream's tables (feed.c) instead of a whole buffer, every voice through
 * fetch(): the interpolator's four frames, the de-click tail, the scrub
 * grains and MASTER TEMPO's read-ahead. Frames not decoded yet (after a
 * jump to a place far from the window and from every cue point) make the
 * main voice hold: silent, its position kept, the fade in of the jump kept
 * for when they come (a voice cut off by the hold fades in again); the
 * tail and the grains play silence there. After each callback the deck
 * tells the feed where it reads, and wakes it when it held or moved to a
 * new chunk of the window.
 *
 * Varispeed and resampling are one step: the read position advances by
 * (tempo + bend) * track_rate / device_rate per output frame and every
 * output frame is a 4-point cubic Hermite (Catmull-Rom) interpolation of
 * the track. At an integer position the interpolator returns the sample
 * itself, so a deck at tempo 1.0 on a device at the track's rate is a
 * bit-exact copy. Pitch follows tempo, as on a CDJ with MASTER TEMPO off.
 *
 * Loops wrap exactly at the out point: when a step crosses it, the
 * position moves back by the loop length and keeps the overshoot, so the
 * frame after the out point is the in point plus the same fraction. A
 * loop engages only when the playhead crosses its out point going
 * forward, or when it is switched on with the playhead less than one
 * length past it; a playhead further beyond it (after a jump) plays on,
 * which is what an active memory loop the deck has jumped past must do.
 *
 * Discontinuities a DJ triggers (start, pause, jump, scrub) are
 * crossfaded over 3 ms with a second "tail" voice that keeps playing from
 * the old position while fading out. A loop wrap is not faded: it is the
 * sample-exact behaviour a loop of whole beats needs.
 *
 * Scrubbing plays grains: 60 ms Hann-windowed snippets at the pointer,
 * one every 30 ms while the pointer moves, quieter and low-passed. A
 * still pointer starts no new grains and falls silent, like a stopped
 * platter.
 *
 * MASTER TEMPO keeps the varispeed path and puts the pitch back after it:
 * the deck's own sound (main voice and de-click tail) goes through a
 * Signalsmith Stretch shifter (stretch.h) at a factor of 1 / (tempo +
 * bend); the grains, which already play at the track's pitch, and the
 * volume come after it. The shifter runs whenever the button is on, at
 * tempo 1.0 too, so moving the slider never changes the timing; a stopped
 * deck feeds it silence, which Stretch passes through cheaply.
 *
 * The shifter's output is `latency` frames behind its input (120 ms at 48
 * kHz). Played straight, every jump, start and stop would be heard that
 * late, so the deck reads ahead instead: while it plays with the shifter,
 * the read position is `latency` frames ahead of what is heard, and every
 * command that moves the playhead or a loop, or switches the button,
 * restarts the shifter from the playhead the listener hears. The restart
 * renders the next `latency` frames from there, wrapping loops as playback
 * would, and hands them to Stretch's outputSeek, after which the next
 * output frame is the new position at full level. (Its plain seek(), with
 * the context ending at the target or a latency beyond it, keeps the old
 * sound for 40 ms and crossfades over the next 50: measured with sines.)
 * The old sound is not cut: the frames it would have played next are
 * captured first (the shifter's coming output if it was on, the dry
 * voices if it was off) and crossfaded out over the same 3 ms as any other
 * jump. Switching the button off steps the read position back to what is
 * heard and crossfades from the shifter to the dry path the same way;
 * stepping back rather than accepting a jump of 120 ms is what keeps the
 * music in time across the switch. A loop wrap is never a restart: it
 * stays sample-exact in the input and the shifter only delays it. Tempo,
 * bend and volume are not restarts either: the slider moves continuously,
 * a new tempo is heard after the delay, the volume at once.
 *
 * What is heard is known exactly: the read position of every frame fed is
 * kept in a ring, and the position the deck reports is the one fed
 * `latency` frames ago, across loop wraps and tempo changes. A restart
 * writes the frames it rendered ahead into the ring, so the report is the
 * target of a jump at once and never before it; a stopped deck that is
 * moved (a scrub) reports where it is. The factor for each block is the
 * inverse of the speed the analysed material was rendered at (the frame
 * `in_latency` back), so a tempo change is corrected when it is heard.
 *
 * The next track (CB_CMD_NEXT, cb_deck_queue_next): the owner may queue a
 * second buffer or stream, and the main voice, on stepping past the last
 * frame of the track it plays, goes on in the next one from its first
 * frame, keeping the fraction of the step it had left, so the two play as
 * one piece of audio: no fade, no silence, no command in between. The
 * interpolator reads across the change as within one track: while a next
 * is queued, the frames past the end of the track are the next's first
 * ones, and after the change the next's frames -2 and -1 are the old
 * track's last two (kept in `edge`), so a deck that resamples (another
 * device rate, a tempo off 1.0) puts no sample at the junction that the
 * two tracks played as one buffer would not have. The step is recomputed
 * for every frame, so a next at another rate plays at the right speed
 * from its first frame. The loop goes with the old track; a de-click
 * tail under way (a seek to the end, a jump just before it) plays out in
 * the old track (`old`), which the owner keeps while the deck publishes
 * its id; the scrub grains, quiet and short, stop.
 *
 * Every track has the owner's id (the serial of the command that brought
 * it, player_internal.h). The deck publishes the one it reads, so
 * the owner frees nothing the deck still reads, and the one heard: the
 * ring keeps it with every frame fed, so that with the shifter in the
 * change is reported when it is heard, `latency` after it was read. In
 * that delay, and in the callback before the owner hears of the move at
 * all, the owner still speaks of the old track: a seek, a loop or a next
 * carries the id of the track it was meant for, and the deck drops one
 * meant for a track it has left (its frames are the old track's; a next
 * queued after the old track is not the new one's). A command that
 * restarts the shifter inside the delay restarts from the start of the new
 * track, the old one's last frames being what the restart's crossfade lets
 * go of. A seek to the end of a track with a next queued goes on into it
 * when playing, and PLAY on a deck stopped at the end of its track takes
 * the next. */

#include "player_internal.h"

#include <math.h>
#include <unistd.h>

void cb_player_deck_init(CBDeckRT *d)
{
    *d = (CBDeckRT) {
        .tempo     = 1.0,
        .gain      = 1.0f,
        .gain_prev = 1.0f,
        .wake      = -1,
    };
}

double cb_player_deck_speed(const CBDeckRT *d)
{
    double speed = d->tempo + d->bend;
    return speed > 0.0 ? speed * d->src.rate : 0.0;
}

static int fade_frames(int rate)
{
    int n = (int)(rate * CB_FADE_SECONDS);
    return n > 0 ? n : 1;
}

static double increment(const CBDeckRT *d, int rate)
{
    return cb_player_deck_speed(d) / rate;
}

/* --------------------------------------------------------------- reading */

/* Catmull-Rom through x0..x1 at t in [0, 1). Exactly x0 at t = 0. */
static inline float hermite(float xm1, float x0, float x1, float x2, float t)
{
    float c1 = 0.5f * (x1 - xm1);
    float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
    float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
    return ((c3 * t + c2) * t + c1) * t + x0;
}

static inline bool loaded(const CBDeckRT *d)
{
    return d->src.samples || d->src.stream;
}

/* n frames of a track from frame i on, as stereo pairs (a mono track on both
 * sides, channels past the second ignored), silence outside it; false when
 * a stream does not have them decoded. */
static bool src_read(const CBSource *s, int64_t i, int n, float *out)
{
    if (s->stream)
        return cb_player_stream_read(s->stream, s->nb_frames, i, n, out);
    for (int k = 0; k < n; k++, i++) {
        if (!s->samples || i < 0 || i >= s->nb_frames) {
            out[2 * k] = out[2 * k + 1] = 0.0f;
            continue;
        }
        const float *x = s->samples + i * s->channels;
        out[2 * k]     = x[0];
        out[2 * k + 1] = x[s->channels > 1];
    }
    return true;
}

/* The interpolator's frames i - 1 to i + 2 of `s`, as stereo pairs: past
 * its end, the first frames of `after` (the track the deck plays on into;
 * NULL: silence), before its start, `before` (frames -2 and -1: the last
 * two of the track the deck came from; NULL: silence). */
static bool frames4(const CBSource *s, const CBSource *after, const float *before, int64_t i,
                    float *x)
{
    int64_t lo = i - 1;
    if (lo >= 0 && lo + 4 <= s->nb_frames)
        return src_read(s, lo, 4, x);

    int k = 0;
    for (; k < 4 && lo + k < 0; k++) {
        int64_t j = lo + k + 2;
        bool have = before && j >= 0;
        x[2 * k]     = have ? before[2 * j] : 0.0f;
        x[2 * k + 1] = have ? before[2 * j + 1] : 0.0f;
    }
    int in = (int)CB_CLAMP(s->nb_frames - (lo + k), (int64_t)0, (int64_t)(4 - k));
    if (in > 0 && !src_read(s, lo + k, in, x + 2 * k))
        return false;
    k += in;
    if (k == 4)
        return true;
    if (after)
        return src_read(after, lo + k - s->nb_frames, 4 - k, x + 2 * k);
    for (; k < 4; k++)
        x[2 * k] = x[2 * k + 1] = 0.0f;
    return true;
}

/* Track `s` at position p, read on into `after` and back into `before`
 * (frames4). False when a stream does not have the frames decoded (yet):
 * every voice reads through here, so all of them see the same frames, and
 * the same gaps. */
static bool fetch_from(CBDeckRT *d, const CBSource *s, const CBSource *after,
                       const float *before, CBPos p, float *l, float *r)
{
    int64_t i = p.i;
    float t = (float)p.f;

    int nc = s->channels;
    if (!s->stream && i >= 1 && i + 2 < s->nb_frames) {
        const float *x = s->samples + (i - 1) * nc;
        *l = hermite(x[0], x[nc], x[2 * nc], x[3 * nc], t);
        if (nc > 1)
            *r = hermite(x[1], x[nc + 1], x[2 * nc + 1], x[3 * nc + 1], t);
        else
            *r = *l;
        return true;
    }

    float x[8];
    if (!frames4(s, after, before, i, x)) {
        d->starved = true;
        return false;
    }
    *l = hermite(x[0], x[2], x[4], x[6], t);
    *r = hermite(x[1], x[3], x[5], x[7], t);
    return true;
}

/* The deck's track at p, as it plays on into the next. */
static bool fetch(CBDeckRT *d, CBPos p, float *l, float *r)
{
    return fetch_from(d, &d->src, d->next.id ? &d->next : NULL, d->has_edge ? d->edge : NULL, p,
                      l, r);
}

/* Where the de-click tail plays: the deck's track, or the one it moved on
 * from while the tail sounded, which went on into the deck's. */
static bool fetch_tail(CBDeckRT *d, float *l, float *r)
{
    if (d->tail_old)
        return fetch_from(d, &d->old, &d->src, NULL, d->tail_pos, l, r);
    return fetch(d, d->tail_pos, l, r);
}

/* ---------------------------------------------------------------- voices */

static void silence_voices(CBDeckRT *d)
{
    d->tail_left = 0;
    d->fade_in = 0;
    for (int i = 0; i < CB_GRAINS; i++)
        d->grains[i].len = 0;
    d->lp_l = d->lp_r = 0.0f;
}

/* Something is about to jump or stop: let the current sound fade out. */
static void start_tail(CBDeckRT *d, int rate)
{
    if (!d->playing)
        return;
    d->tail_pos  = d->pos;
    d->tail_inc  = increment(d, rate);
    d->tail_len  = fade_frames(rate);
    d->tail_left = d->tail_len;
    d->tail_old  = false;
}

static void start_fade_in(CBDeckRT *d, int rate)
{
    d->fade_len = fade_frames(rate);
    d->fade_in  = d->fade_len;
}

static CBPos clamp_frames(const CBDeckRT *d, double frames)
{
    if (!(frames > 0.0))
        frames = 0.0;
    if (frames > (double)d->src.nb_frames)
        frames = (double)d->src.nb_frames;
    return cb_pos_from(frames);
}

/* A loop switched on with the playhead less than one loop length past its
 * out point (LOOP OUT quantised to a beat just behind the playhead, a loop
 * halved around it, an active loop the interface noticed late): fold the
 * playhead back into the loop at the same phase, as if it had wrapped on
 * time. A playhead further on has left the loop behind and plays on. */
static void fold_into_loop(CBDeckRT *d)
{
    if (!d->loop_enabled || d->scrubbing || cb_pos_cmp(d->pos, d->loop_out) < 0)
        return;
    double len = cb_pos_get(d->loop_len);
    double over = cb_pos_get(cb_pos_sub(d->pos, d->loop_out));
    if (over >= len)
        return;
    d->pos = d->loop_in;
    cb_pos_add(&d->pos, over);
}

/* The end of the track: the main voice goes on in the next one, the frame
 * after the last, the fraction of its step kept (scaled when the rates
 * differ). */
static void take_next(CBDeckRT *d)
{
    CBPos over = cb_pos_sub(d->pos, (CBPos) { .i = d->src.nb_frames });
    double frames = cb_pos_get(over);
    if (!(frames >= 0.0))
        frames = 0.0;
    if (d->src.rate > 0 && d->next.rate != d->src.rate)
        frames *= (double)d->next.rate / d->src.rate;

    /* The old track's last two frames are the next's frames -2 and -1. A
     * stream has them decoded: the main voice has just played them, or
     * the deck jumped to the end, and silence will do for a jump. */
    if (!src_read(&d->src, d->src.nb_frames - 2, 2, d->edge))
        memset(d->edge, 0, sizeof(d->edge));
    d->has_edge = true;
    /* The tail's positions are the old track's: one under way plays out
     * there (the owner keeps the track while the deck says so). The
     * grains' are too: quiet and short, they stop. */
    if (d->tail_left > 0 && !d->tail_old) {
        d->old = d->src;
        d->tail_old = true;
    }
    for (int i = 0; i < CB_GRAINS; i++)
        d->grains[i].len = 0;

    d->src          = d->next;
    d->next         = (CBSource) { 0 };
    d->length_final = !d->src.stream;
    d->posted_chunk = -1;
    d->holding      = false;
    d->pos          = cb_pos_from(frames);
    d->loop_set     = false;
    d->loop_enabled = false;
}

static void seek(CBDeckRT *d, double frames, int rate)
{
    start_tail(d, rate);
    if (d->playing)
        start_fade_in(d, rate);
    d->pos = clamp_frames(d, frames);
    if (d->pos.i >= d->src.nb_frames) {
        if (d->next.id && d->playing)
            take_next(d);
        else
            d->playing = false;
    }
}

static bool can_play(const CBDeckRT *d)
{
    return !d->playing && loaded(d) && (d->pos.i < d->src.nb_frames || d->next.id);
}

static void play(CBDeckRT *d, int rate)
{
    if (!can_play(d))
        return;
    if (d->pos.i >= d->src.nb_frames)
        take_next(d);
    d->playing = true;
    start_fade_in(d, rate);
}

/* ---------------------------------------------------------- master tempo */

int cb_player_deck_mt_alloc(CBDeckRT *d, int rate)
{
    CBDeckMT *m = &d->mt;
    int ret = cb_stretch_create(&m->st, 2, rate);
    if (ret < 0)
        return ret;
    m->latency    = cb_stretch_latency(m->st);
    m->in_latency = cb_stretch_input_latency(m->st);

    /* The ring is read `latency` behind the newest frame, which a callback
     * moves on by up to a chunk before the next read. */
    int64_t ring = 1;
    while (ring < (int64_t)m->latency + CB_MT_CHUNK + 1)
        ring *= 2;
    m->hist_mask = ring - 1;

    bool ok = m->latency > 0;
    for (int c = 0; c < 2; c++) {
        m->in[c]    = calloc(CB_MT_CHUNK, sizeof(float));
        m->out[c]   = calloc(CB_MT_CHUNK, sizeof(float));
        m->xf[c]    = calloc(CB_MT_CHUNK, sizeof(float));
        m->ahead[c] = calloc(CB_MAX(m->latency, 1), sizeof(float));
        ok &= m->in[c] && m->out[c] && m->xf[c] && m->ahead[c];
    }
    m->hist       = calloc(ring, sizeof(*m->hist));
    m->hist_speed = calloc(ring, sizeof(*m->hist_speed));
    m->hist_id    = calloc(ring, sizeof(*m->hist_id));
    if (!ok || !m->hist || !m->hist_speed || !m->hist_id) {
        cb_player_deck_mt_free(d);
        return CB_ERROR(ENOMEM);
    }
    return 0;
}

void cb_player_deck_mt_free(CBDeckRT *d)
{
    CBDeckMT *m = &d->mt;
    if (m->st)
        cb_stretch_destroy(m->st);
    for (int c = 0; c < 2; c++) {
        free(m->in[c]);
        free(m->out[c]);
        free(m->xf[c]);
        free(m->ahead[c]);
    }
    free(m->hist);
    free(m->hist_speed);
    free(m->hist_id);
    *m = (CBDeckMT) { 0 };
}

CBPos cb_player_deck_heard(const CBDeckRT *d)
{
    /* With a restart due (a callback of no frames applies commands but
     * renders nothing), mt_begin has put the deck at the heard position
     * already and the ring is not filled yet. */
    const CBDeckMT *m = &d->mt;
    if (!m->on || m->restart)
        return d->pos;
    int64_t i = m->fed - m->latency;
    if (i < m->fill_until)
        return m->fill_pos;
    return m->hist[i & m->hist_mask];
}

uint64_t cb_player_deck_heard_id(const CBDeckRT *d)
{
    const CBDeckMT *m = &d->mt;
    if (!m->on || m->restart)
        return d->src.id;
    int64_t i = m->fed - m->latency;
    if (i < m->fill_until)
        return m->fill_id;
    return m->hist_id[i & m->hist_mask];
}

uint64_t cb_player_deck_old_id(const CBDeckRT *d)
{
    return d->tail_left > 0 && d->tail_old ? d->old.id : 0;
}

/* The frames fed from here on count as the deck's position and track. */
static void mt_fill_from_here(CBDeckRT *d)
{
    CBDeckMT *m = &d->mt;
    m->fill_until = m->fed;
    m->fill_pos   = d->pos;
    m->fill_id    = d->src.id;
}

/* The tempo + bend a frame plays at; 0 for a stopped deck. */
static float speed_ratio(const CBDeckRT *d)
{
    double s = d->tempo + d->bend;
    return d->playing && s > 0.0 ? (float)s : 0.0f;
}

/* The shifter's factor for material played at `speed`: its inverse. A
 * stopped deck is left alone, and the correction stops at two octaves
 * either way (a tempo of 0.25 or 4, the slider's own limit). */
static float pitch_factor(float speed)
{
    if (!(speed > 0.0f))
        return 1.0f;
    return 1.0f / CB_CLAMP(speed, 0.25f, 4.0f);
}

static void voice(CBDeckRT *d, float *l, float *r);

/* A command is about to move the playhead or a loop, or to switch the
 * shifter in or out. From here to the end of the callback the deck's
 * position is the one the listener hears (the read position is ahead of it
 * while the shifter plays), and the next render restarts the shifter from
 * it. The first such command of a callback records how the deck sounded:
 * whether the shifter was in, and if it was not, the dry voices as they
 * would have played on, rendered here from a copy of the deck. Here,
 * because what they read is the deck's now: by the render, a load may have
 * replaced it, and after a callback of no frames (manual mode applies the
 * commands before it renders), the owner may have freed it. */
static void mt_begin(CBDeckRT *d)
{
    CBDeckMT *m = &d->mt;
    if (!m->st || m->restart)
        return;
    /* Read before the restart is set: with it set, heard is the read
     * position (a restart due means the ring is not filled yet). */
    CBPos heard = cb_player_deck_heard(d);
    bool same_track = cb_player_deck_heard_id(d) == d->src.id;
    m->restart = true;
    m->was_on = m->on;
    if (m->on) {
        /* What is heard may still be the track before this one (the
         * deck moved on inside the shifter's delay): the restart then
         * starts this one, and the old track's last frames go with the
         * crossfade, which the shifter has already. */
        d->pos = same_track ? heard : (CBPos) { 0 };
    } else {
        CBDeckRT w = *d;
        m->old_len = CB_MIN(fade_frames(d->out_rate), CB_MT_CHUNK);
        for (int k = 0; k < m->old_len; k++)
            voice(&w, &m->out[0][k], &m->out[1][k]);
    }
}

/* The commands that matter to the shifter only while it is in. */
static void mt_touch(CBDeckRT *d)
{
    if (d->mt.on)
        mt_begin(d);
}

/* A stopped deck moved without a restart (a scrub): it is heard there. */
static void mt_moved(CBDeckRT *d)
{
    CBDeckMT *m = &d->mt;
    if (!m->on || m->restart)
        return;
    mt_fill_from_here(d);
}

/* A track change is a hard cut, with the shifter as without it: nothing of
 * the old track sounds on, and its buffer is not read again. */
static void mt_cut(CBDeckRT *d)
{
    CBDeckMT *m = &d->mt;
    if (!m->st)
        return;
    m->restart = false;
    m->xf_left = 0;
    if (m->on)
        cb_stretch_reset(m->st);
    m->fill_until = m->fed;
    m->fill_pos   = (CBPos) { 0 };
    m->fill_id    = d->src.id;
}

/* -------------------------------------------------------------- commands */

/* The track a load or a next carries; none for an unload, or a next that
 * drops the one queued (no id). */
static CBSource source_of(const CBCmd *cmd)
{
    if (cmd->type == CB_CMD_UNLOAD || !cmd->id)
        return (CBSource) { 0 };
    return (CBSource) {
        .samples   = cmd->samples,
        .stream    = cmd->stream,
        .channels  = cmd->channels,
        .rate      = cmd->rate,
        .nb_frames = cmd->nb_frames,
        .id        = cmd->id,
    };
}

void cb_player_deck_apply(CBDeckRT *d, const CBCmd *cmd, int rate)
{
    d->out_rate = rate;
    /* Meant for a track the deck has left (it moved on into the next
     * before the command came): its frames, or the track it was to follow,
     * are the old one's. Dropped, and counted like any other: the owner
     * reads the count. */
    if (cmd->track && cmd->track != d->src.id) {
        d->applied++;
        return;
    }

    switch (cmd->type) {
    case CB_CMD_LOAD:
    case CB_CMD_UNLOAD:
        /* The old buffer may be freed once this command is acknowledged
         * and the deck publishes the new track's id: d->src was the only
         * pointer to it, and the tail and the grains, positions in the
         * old track that must not go on in the new one, stop. A track
         * change on a playing deck is a hard cut, as on a CDJ. MASTER
         * TEMPO is the deck's and stays as it is. A load is a new start:
         * the track queued after the old one goes too (the owner releases
         * it). */
        silence_voices(d);
        d->src          = source_of(cmd);
        d->next         = (CBSource) { 0 };
        d->old          = (CBSource) { 0 };
        d->has_edge     = false;
        d->length_final = !d->src.stream;
        d->posted_chunk = -1;
        d->holding      = false;
        d->pos          = (CBPos) { 0 };
        d->playing      = false;
        d->bend         = 0.0;
        d->loop_set     = false;
        d->loop_enabled = false;
        d->scrubbing    = false;
        d->scrub_resume = false;
        mt_cut(d);
        break;
    case CB_CMD_NEXT:
        /* Replaces the queued track (the owner releases the old one once
         * this is acknowledged); nothing else changes until the end. */
        d->next = source_of(cmd);
        break;
    case CB_CMD_PLAY:
        if (d->scrubbing) {
            d->scrub_resume = true;
        } else {
            if (can_play(d))
                mt_touch(d);
            play(d, rate);
        }
        break;
    case CB_CMD_PAUSE:
        if (d->scrubbing) {
            d->scrub_resume = false;
        } else {
            if (d->playing)
                mt_touch(d);
            start_tail(d, rate);
            d->playing = false;
        }
        break;
    case CB_CMD_SEEK:
    case CB_CMD_PLAY_FROM:
        if (d->scrubbing) {
            d->pos = clamp_frames(d, cmd->a);
            mt_moved(d);
            if (cmd->type == CB_CMD_PLAY_FROM)
                d->scrub_resume = true;
            break;
        }
        mt_touch(d);
        seek(d, cmd->a, rate);
        if (cmd->type == CB_CMD_PLAY_FROM)
            play(d, rate);
        break;
    case CB_CMD_TEMPO:
        d->tempo = cmd->a;
        break;
    case CB_CMD_BEND:
        d->bend = cmd->a;
        break;
    case CB_CMD_VOLUME:
        d->gain = (float)cmd->a;
        break;
    case CB_CMD_LOOP_SET:
        /* Clamped like a seek: an out point at the end of the track comes
         * from seconds, and duration * rate can round a fraction of a frame
         * past nb_frames. A playhead landing exactly on nb_frames would then
         * not have crossed it, and the deck would stop instead of wrapping. */
        mt_touch(d);
        d->loop_in  = clamp_frames(d, cmd->a);
        d->loop_out = clamp_frames(d, cmd->b);
        d->loop_len = cb_pos_sub(d->loop_out, d->loop_in);
        /* The owner guarantees at least one frame; a shorter loop could
         * never be left by subtracting its length. */
        d->loop_set = cb_pos_get(d->loop_len) >= 1.0;
        d->loop_enabled = d->loop_set && cmd->flag;
        fold_into_loop(d);
        break;
    case CB_CMD_LOOP_ENABLE:
        mt_touch(d);
        d->loop_enabled = d->loop_set && cmd->flag;
        fold_into_loop(d);
        break;
    case CB_CMD_LOOP_CLEAR:
        mt_touch(d);
        d->loop_set = false;
        d->loop_enabled = false;
        break;
    case CB_CMD_SCRUB_BEGIN:
        if (d->scrubbing || !loaded(d))
            break;
        if (d->playing)
            mt_touch(d);
        d->scrub_resume = d->playing;
        start_tail(d, rate);
        d->playing    = false;
        d->scrubbing  = true;
        d->grain_from = cb_pos_get(d->pos);
        d->hop        = 0;
        break;
    case CB_CMD_SCRUB_TO:
        if (d->scrubbing) {
            d->pos = clamp_frames(d, cmd->a);
            mt_moved(d);
        } else {
            mt_touch(d);
            seek(d, cmd->a, rate);
        }
        break;
    case CB_CMD_SCRUB_END:
        if (!d->scrubbing)
            break;
        d->scrubbing = false;
        if (d->scrub_resume) {
            if (can_play(d))
                mt_touch(d);
            play(d, rate);
        }
        d->scrub_resume = false;
        break;
    case CB_CMD_MASTER_TEMPO:
        if (d->mt.st && d->mt.on != cmd->flag) {
            mt_begin(d);
            d->mt.on = cmd->flag;
        }
        break;
    }
    d->applied++;
}

/* ---------------------------------------------------------------- render */

/* One output frame of the main voice: advance, wrap, stop at the end. */
static void advance(CBDeckRT *d, double inc)
{
    CBPos prev = d->pos;
    cb_pos_add(&d->pos, inc);

    if (d->loop_enabled && cb_pos_cmp(prev, d->loop_out) < 0 &&
        cb_pos_cmp(d->pos, d->loop_out) >= 0) {
        do
            d->pos = cb_pos_sub(d->pos, d->loop_len);
        while (cb_pos_cmp(d->pos, d->loop_out) >= 0);
    }

    /* The end of the track: on into the next one when there is one, else
     * the deck pauses there (PLAY MODE SINGLE, the CDJ-3000's factory
     * setting, manual p.75). */
    if (d->pos.i >= d->src.nb_frames) {
        if (d->next.id) {
            take_next(d);
        } else {
            d->pos = (CBPos) { .i = d->src.nb_frames };
            d->playing = false;
        }
    }
}

/* The deck's own sound for one frame: the main voice, faded in after a
 * start or a jump, and the de-click tail. A main voice whose frames are
 * not decoded yet holds where it is, silent, its fade in kept for when
 * they come (a voice cut off by the hold fades in again); the tail plays
 * silence for frames that are not there. The step is the deck's as of
 * this frame: a next track at another rate changes it mid-callback. */
static void voice(CBDeckRT *d, float *l, float *r)
{
    *l = 0.0f;
    *r = 0.0f;

    if (d->playing) {
        if (fetch(d, d->pos, l, r)) {
            if (d->holding && !d->fade_in)
                start_fade_in(d, d->out_rate);
            d->holding = false;
            if (d->fade_in > 0) {
                float e = 1.0f - (float)d->fade_in / d->fade_len;
                *l *= e;
                *r *= e;
                d->fade_in--;
            }
            advance(d, increment(d, d->out_rate));
        } else {
            *l = *r = 0.0f;
            d->holding = true;
        }
    }

    if (d->tail_left > 0) {
        float sl, sr, e = (float)d->tail_left / d->tail_len;
        if (fetch_tail(d, &sl, &sr)) {
            *l += sl * e;
            *r += sr * e;
        }
        cb_pos_add(&d->tail_pos, d->tail_inc);
        d->tail_left--;
    }
}

static void spawn_grain(CBDeckRT *d, int glen)
{
    CBGrain *g = &d->grains[0];
    for (int i = 0; i < CB_GRAINS; i++) {
        if (!d->grains[i].len) {
            g = &d->grains[i];
            break;
        }
        if (d->grains[i].age > g->age)
            g = &d->grains[i];
    }
    *g = (CBGrain) { .pos = d->pos, .len = glen };
    d->grain_from = cb_pos_get(d->pos);
}

/* The grains' contribution for one output frame. */
static void grains(CBDeckRT *d, float *l, float *r, int glen, double ratio, float lp)
{
    if (d->scrubbing && --d->hop <= 0) {
        d->hop = glen / 2;
        /* A new grain only when the pointer has moved by more than a
         * millisecond of track since the last one. */
        if (fabs(cb_pos_get(d->pos) - d->grain_from) > d->src.rate * 0.001)
            spawn_grain(d, glen);
    }

    float gl = 0.0f, gr = 0.0f;
    bool any = false;
    for (int i = 0; i < CB_GRAINS; i++) {
        CBGrain *g = &d->grains[i];
        if (!g->len)
            continue;
        float w = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * g->age / g->len);
        float sl, sr;
        if (fetch(d, g->pos, &sl, &sr)) {
            gl += sl * w;
            gr += sr * w;
        }
        cb_pos_add(&g->pos, ratio);
        if (++g->age >= g->len)
            g->len = 0;
        any = true;
    }

    if (!any) {
        d->lp_l = d->lp_r = 0.0f;
        return;
    }
    d->lp_l += lp * (gl - d->lp_l);
    d->lp_r += lp * (gr - d->lp_r);
    *l += d->lp_l * CB_GRAIN_GAIN;
    *r += d->lp_r * CB_GRAIN_GAIN;
}

/* The next n frames the deck would have played as it sounded before this
 * callback's restart, into m->out: the shifter's coming output if it was
 * in (fed silence; what comes out in the next few milliseconds was decided
 * by the blocks already analysed), the dry voices if it was not (mt_begin
 * rendered them). A crossfade still under way carries on inside it. */
static void mt_old_sound(CBDeckRT *d, int n)
{
    CBDeckMT *m = &d->mt;
    if (m->was_on) {
        for (int c = 0; c < 2; c++)
            memset(m->in[c], 0, sizeof(float) * n);
        const float *in[2] = { m->in[0], m->in[1] };
        cb_stretch_process(m->st, in, m->out, n);
    }

    int at = m->xf_len - m->xf_left;
    for (int k = 0; k < n && k < m->xf_left; k++) {
        float e = 1.0f - (float)(m->xf_left - k) / m->xf_len;
        for (int c = 0; c < 2; c++)
            m->out[c][k] = m->xf[c][at + k] * (1.0f - e) + m->out[c][k] * e;
    }
}

/* The restart the commands of this callback asked for (see mt_begin and
 * the file comment). */
static void mt_restart(CBDeckRT *d, int rate)
{
    CBDeckMT *m = &d->mt;
    int n = m->was_on ? CB_MIN(fade_frames(rate), CB_MT_CHUNK) : m->old_len;
    mt_old_sound(d, n);
    for (int c = 0; c < 2; c++)
        memcpy(m->xf[c], m->out[c], sizeof(float) * n);
    m->xf_len = m->xf_left = n;
    m->restart = false;

    /* The crossfade is the de-click now; a dry fade on top of it would
     * dip the level. */
    d->tail_left = 0;
    d->fade_in   = 0;
    cb_stretch_reset(m->st);
    if (!m->on)
        return;

    if (!d->playing) {
        mt_fill_from_here(d);
        return;
    }

    /* Read ahead: the next `latency` frames from the playhead, entered in
     * the ring as if they had been fed already. */
    for (int k = 0; k < m->latency; k++) {
        int64_t i = (m->fed - m->latency + k) & m->hist_mask;
        m->hist[i]       = d->pos;
        m->hist_speed[i] = speed_ratio(d);
        m->hist_id[i]    = d->src.id;
        voice(d, &m->ahead[0][k], &m->ahead[1][k]);
    }
    m->fill_until = m->fed - m->latency;
    cb_stretch_set_pitch(m->st, pitch_factor(m->hist_speed[(m->fed - m->in_latency) &
                                                           m->hist_mask]));
    const float *ahead[2] = { m->ahead[0], m->ahead[1] };
    cb_stretch_seek(m->st, ahead, m->latency);
}

/* Feeds the next n frames of the deck through the shifter, into m->out. */
static void mt_feed(CBDeckRT *d, int n)
{
    CBDeckMT *m = &d->mt;
    for (int k = 0; k < n; k++) {
        int64_t i = (m->fed + k) & m->hist_mask;
        m->hist[i]       = d->pos;
        m->hist_speed[i] = speed_ratio(d);
        m->hist_id[i]    = d->src.id;
        voice(d, &m->in[0][k], &m->in[1][k]);
    }
    m->fed += n;

    cb_stretch_set_pitch(m->st, pitch_factor(m->hist_speed[(m->fed - m->in_latency) &
                                                           m->hist_mask]));
    const float *in[2] = { m->in[0], m->in[1] };
    cb_stretch_process(m->st, in, m->out, n);
}

void cb_player_deck_render(CBDeckRT *d, const CBMixOut *o, int frames, int rate)
{
    if (frames <= 0)
        return;
    d->out_rate = rate;

    /* A stream's length is the container's until its last chunk came. */
    if (d->src.stream && !d->length_final) {
        bool exact;
        d->src.nb_frames = cb_stream_length(d->src.stream, &exact);
        d->length_final = exact;
    }

    CBDeckMT *m = &d->mt;
    if (m->restart)
        mt_restart(d, rate);

    float decay = expf(-frames / (CB_LEVEL_RELEASE_SECONDS * rate));
    float peak_l = 0.0f, peak_r = 0.0f;

    if (loaded(d)) {
        double ratio = (double)d->src.rate / rate;
        int    glen  = (int)(rate * CB_GRAIN_SECONDS);
        float  lp    = 1.0f - expf(-2.0f * (float)M_PI * CB_GRAIN_LOWPASS_HZ / rate);
        float  g0    = d->gain_prev;
        float  dg    = (d->gain - g0) / frames;
        bool   grain_work = d->scrubbing;
        for (int i = 0; i < CB_GRAINS; i++)
            grain_work |= d->grains[i].len > 0;

        /* In pieces the scratch buffers hold; the dry path goes frame by
         * frame and is the same either way. */
        for (int done = 0; done < frames;) {
            int len = CB_MIN(frames - done, CB_MT_CHUNK);
            if (m->on)
                mt_feed(d, len);

            for (int k = 0; k < len; k++) {
                float l, r;
                if (m->on) {
                    l = m->out[0][k];
                    r = m->out[1][k];
                } else {
                    voice(d, &l, &r);
                }

                if (m->xf_left > 0) {
                    float e = 1.0f - (float)m->xf_left / m->xf_len;
                    int i = m->xf_len - m->xf_left;
                    l = l * e + m->xf[0][i] * (1.0f - e);
                    r = r * e + m->xf[1][i] * (1.0f - e);
                    m->xf_left--;
                }

                if (grain_work)
                    grains(d, &l, &r, glen, ratio, lp);

                int n = done + k;
                float g = dg != 0.0f ? g0 + dg * (n + 1) : g0;
                l *= g;
                r *= g;
                o->ch[0][(size_t)n * o->stride] += l;
                o->ch[1][(size_t)n * o->stride] += r;
                peak_l = fmaxf(peak_l, fabsf(l));
                peak_r = fmaxf(peak_r, fabsf(r));
            }
            done += len;
        }
    }

    d->gain_prev = d->gain;
    d->level_l = fmaxf(peak_l, d->level_l * decay);
    d->level_r = fmaxf(peak_r, d->level_r * decay);
    /* A meter falling forever would end in denormals. */
    if (d->level_l < 1e-6f)
        d->level_l = 0.0f;
    if (d->level_r < 1e-6f)
        d->level_r = 0.0f;
}

void cb_player_deck_signal(CBDeckRT *d)
{
    CBStream *s = d->src.stream;
    if (!s)
        return;
    int64_t at = d->pos.i;
    atomic_store_explicit(&s->playhead, at, memory_order_relaxed);
    /* An eventfd's counter: the write neither blocks nor allocates. */
    int64_t c = at / CB_CHUNK;
    if (d->starved || c != d->posted_chunk) {
        if (d->wake >= 0) {
            uint64_t one = 1;
            ssize_t w = write(d->wake, &one, sizeof(one));
            (void)w;
        }
        d->posted_chunk = c;
    }
    d->starved = false;
}
