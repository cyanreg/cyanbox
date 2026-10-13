/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* One deck as the real-time thread sees it: the commands that change it
 * and the renderer that adds it to the mix.
 *
 * Everything here runs on the real-time thread and follows its rules: no
 * allocation, no locks, no logging, no system calls but the sem_post that
 * wakes the deck's feed thread (it never blocks). The buffer is read,
 * never written; the owner keeps it alive until this deck has applied the
 * command that replaced it (see the release path in player.c). Voices keep
 * positions, never a pointer, so after the swap nothing reads the old
 * buffer; a load or unload still silences the de-click tail and the scrub
 * grains at once, or they would play the old track's positions out of the
 * new one.
 *
 * A deck playing a packet store reads its frames from the window its feed
 * thread decodes (feed.c) instead of a whole buffer, every voice through
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
 * The next track (RX_CMD_NEXT, rx_deck_queue_next): the owner may queue a
 * second buffer or stream, and the main voice, on stepping past the last
 * frame of the track it plays, goes on in the next one from its first
 * frame, keeping the fraction of the step it had left, so the two play as
 * one piece of audio: no fade, no silence, no command in between. The
 * loop goes with the old track; so does the de-click tail, which would
 * otherwise read the old track's positions out of the new one. The deck's
 * track_gen counts the changes, and the ring keeps it with every frame fed
 * so that, with the shifter in, the change is reported when it is heard,
 * `latency` after it was read; a command that restarts the shifter inside
 * that delay restarts from the start of the new track, the old one's last
 * frames being what the restart's crossfade lets go of (its buffer may be
 * gone by then). A seek to the end of a track with a next queued goes on
 * into it when playing, and PLAY on a deck stopped at the end of its
 * track takes the next. The step is recomputed for every frame, so a next
 * at another rate plays at the right speed from its first frame. */

#include "player_internal.h"

#include <math.h>

void rx_player_deck_init(RXDeckRT *d)
{
    *d = (RXDeckRT) {
        .tempo     = 1.0,
        .gain      = 1.0f,
        .gain_prev = 1.0f,
    };
}

double rx_player_deck_speed(const RXDeckRT *d)
{
    double speed = d->tempo + d->bend;
    return speed > 0.0 ? speed * d->rate : 0.0;
}

static int fade_frames(int rate)
{
    int n = (int)(rate * RX_FADE_SECONDS);
    return n > 0 ? n : 1;
}

static double increment(const RXDeckRT *d, int rate)
{
    return rx_player_deck_speed(d) / rate;
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

/* A sample, silence outside the track. */
static inline float at(const RXDeckRT *d, int64_t i, int ch)
{
    if (i < 0 || i >= d->nb_frames)
        return 0.0f;
    return d->samples[i * d->channels + ch];
}

static inline bool loaded(const RXDeckRT *d)
{
    return d->samples || d->stream;
}

/* The track at position p; a mono track plays on both sides, and channels
 * past the second are ignored. False when a stream does not have the frames
 * decoded (yet): every voice reads through here, so all of them see the
 * same frames, and the same gaps. */
static bool fetch(RXDeckRT *d, RXPos p, float *l, float *r)
{
    int64_t i = p.i;
    float t = (float)p.f;

    if (d->stream) {
        float x[8];
        if (!rx_player_stream_read(d->stream, d->nb_frames, i - 1, 4, x)) {
            d->starved = true;
            return false;
        }
        *l = hermite(x[0], x[2], x[4], x[6], t);
        *r = hermite(x[1], x[3], x[5], x[7], t);
        return true;
    }

    int nc = d->channels;
    if (i >= 1 && i + 2 < d->nb_frames) {
        const float *s = d->samples + (i - 1) * nc;
        *l = hermite(s[0], s[nc], s[2 * nc], s[3 * nc], t);
        if (nc > 1)
            *r = hermite(s[1], s[nc + 1], s[2 * nc + 1], s[3 * nc + 1], t);
        else
            *r = *l;
        return true;
    }

    *l = hermite(at(d, i - 1, 0), at(d, i, 0), at(d, i + 1, 0), at(d, i + 2, 0), t);
    if (nc > 1)
        *r = hermite(at(d, i - 1, 1), at(d, i, 1), at(d, i + 1, 1), at(d, i + 2, 1), t);
    else
        *r = *l;
    return true;
}

/* ---------------------------------------------------------------- voices */

static void silence_voices(RXDeckRT *d)
{
    d->tail_left = 0;
    d->fade_in = 0;
    for (int i = 0; i < RX_GRAINS; i++)
        d->grains[i].len = 0;
    d->lp_l = d->lp_r = 0.0f;
}

/* Something is about to jump or stop: let the current sound fade out. */
static void start_tail(RXDeckRT *d, int rate)
{
    if (!d->playing)
        return;
    d->tail_pos  = d->pos;
    d->tail_inc  = increment(d, rate);
    d->tail_len  = fade_frames(rate);
    d->tail_left = d->tail_len;
}

static void start_fade_in(RXDeckRT *d, int rate)
{
    d->fade_len = fade_frames(rate);
    d->fade_in  = d->fade_len;
}

static RXPos clamp_frames(const RXDeckRT *d, double frames)
{
    if (!(frames > 0.0))
        frames = 0.0;
    if (frames > (double)d->nb_frames)
        frames = (double)d->nb_frames;
    return rx_pos_from(frames);
}

/* A loop switched on with the playhead less than one loop length past its
 * out point (LOOP OUT quantised to a beat just behind the playhead, a loop
 * halved around it, an active loop the interface noticed late): fold the
 * playhead back into the loop at the same phase, as if it had wrapped on
 * time. A playhead further on has left the loop behind and plays on. */
static void fold_into_loop(RXDeckRT *d)
{
    if (!d->loop_enabled || d->scrubbing || rx_pos_cmp(d->pos, d->loop_out) < 0)
        return;
    double len = rx_pos_get(d->loop_len);
    double over = rx_pos_get(rx_pos_sub(d->pos, d->loop_out));
    if (over >= len)
        return;
    d->pos = d->loop_in;
    rx_pos_add(&d->pos, over);
}

/* The end of the track: the main voice goes on in the next one, the frame
 * after the last, the fraction of its step kept (scaled when the rates
 * differ). */
static void take_next(RXDeckRT *d)
{
    RXPos over = rx_pos_sub(d->pos, (RXPos) { .i = d->nb_frames });
    double frames = rx_pos_get(over);
    if (!(frames >= 0.0))
        frames = 0.0;
    if (d->rate > 0 && d->next_rate != d->rate)
        frames *= (double)d->next_rate / d->rate;

    d->samples      = d->next_samples;
    d->stream       = d->next_stream;
    d->channels     = d->next_channels;
    d->rate         = d->next_rate;
    d->nb_frames    = d->next_nb_frames;
    d->next_samples = NULL;
    d->next_stream  = NULL;
    d->has_next     = false;
    d->length_final = !d->stream;
    d->posted_chunk = -1;
    d->holding      = false;
    d->pos          = rx_pos_from(frames);
    d->loop_set     = false;
    d->loop_enabled = false;
    /* The tail's positions are the old track's. */
    d->tail_left    = 0;
    d->track_gen++;
}

static void seek(RXDeckRT *d, double frames, int rate)
{
    start_tail(d, rate);
    if (d->playing)
        start_fade_in(d, rate);
    d->pos = clamp_frames(d, frames);
    if (d->pos.i >= d->nb_frames) {
        if (d->has_next && d->playing)
            take_next(d);
        else
            d->playing = false;
    }
}

static bool can_play(const RXDeckRT *d)
{
    return !d->playing && loaded(d) && (d->pos.i < d->nb_frames || d->has_next);
}

static void play(RXDeckRT *d, int rate)
{
    if (!can_play(d))
        return;
    if (d->pos.i >= d->nb_frames)
        take_next(d);
    d->playing = true;
    start_fade_in(d, rate);
}

/* ---------------------------------------------------------- master tempo */

int rx_player_deck_mt_alloc(RXDeckRT *d, int rate)
{
    RXDeckMT *m = &d->mt;
    int ret = rx_stretch_create(&m->st, 2, rate);
    if (ret < 0)
        return ret;
    m->latency    = rx_stretch_latency(m->st);
    m->in_latency = rx_stretch_input_latency(m->st);

    /* The ring is read `latency` behind the newest frame, which a callback
     * moves on by up to a chunk before the next read. */
    int64_t ring = 1;
    while (ring < (int64_t)m->latency + RX_MT_CHUNK + 1)
        ring *= 2;
    m->hist_mask = ring - 1;

    bool ok = m->latency > 0;
    for (int c = 0; c < 2; c++) {
        m->in[c]    = calloc(RX_MT_CHUNK, sizeof(float));
        m->out[c]   = calloc(RX_MT_CHUNK, sizeof(float));
        m->xf[c]    = calloc(RX_MT_CHUNK, sizeof(float));
        m->ahead[c] = calloc(RX_MAX(m->latency, 1), sizeof(float));
        ok &= m->in[c] && m->out[c] && m->xf[c] && m->ahead[c];
    }
    m->hist       = calloc(ring, sizeof(*m->hist));
    m->hist_speed = calloc(ring, sizeof(*m->hist_speed));
    m->hist_gen   = calloc(ring, sizeof(*m->hist_gen));
    m->was        = calloc(1, sizeof(*m->was));
    if (!ok || !m->hist || !m->hist_speed || !m->hist_gen || !m->was) {
        rx_player_deck_mt_free(d);
        return RX_ERROR(ENOMEM);
    }
    return 0;
}

void rx_player_deck_mt_free(RXDeckRT *d)
{
    RXDeckMT *m = &d->mt;
    if (m->st)
        rx_stretch_destroy(m->st);
    for (int c = 0; c < 2; c++) {
        free(m->in[c]);
        free(m->out[c]);
        free(m->xf[c]);
        free(m->ahead[c]);
    }
    free(m->hist);
    free(m->hist_speed);
    free(m->hist_gen);
    free(m->was);
    *m = (RXDeckMT) { 0 };
}

RXPos rx_player_deck_heard(const RXDeckRT *d)
{
    /* With a restart due (a callback of no frames applies commands but
     * renders nothing), mt_begin has put the deck at the heard position
     * already and the ring is not filled yet. */
    const RXDeckMT *m = &d->mt;
    if (!m->on || m->restart)
        return d->pos;
    int64_t i = m->fed - m->latency;
    if (i < m->fill_until)
        return m->fill_pos;
    return m->hist[i & m->hist_mask];
}

uint32_t rx_player_deck_heard_gen(const RXDeckRT *d)
{
    const RXDeckMT *m = &d->mt;
    if (!m->on || m->restart)
        return d->track_gen;
    int64_t i = m->fed - m->latency;
    if (i < m->fill_until)
        return m->fill_gen;
    return m->hist_gen[i & m->hist_mask];
}

/* The frames fed from here on count as the deck's position and track. */
static void mt_fill_from_here(RXDeckRT *d)
{
    RXDeckMT *m = &d->mt;
    m->fill_until = m->fed;
    m->fill_pos   = d->pos;
    m->fill_gen   = d->track_gen;
}

/* The tempo + bend a frame plays at; 0 for a stopped deck. */
static float speed_ratio(const RXDeckRT *d)
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
    return 1.0f / RX_CLAMP(speed, 0.25f, 4.0f);
}

/* A command is about to move the playhead or a loop, or to switch the
 * shifter in or out. From here to the end of the callback the deck's
 * position is the one the listener hears (the read position is ahead of it
 * while the shifter plays), and the next render restarts the shifter from
 * it. The first such command of a callback records how the deck sounded:
 * whether the shifter was in, and the dry voices if it was not. */
static void mt_begin(RXDeckRT *d)
{
    RXDeckMT *m = &d->mt;
    if (!m->st || m->restart)
        return;
    /* Read before the restart is set: with it set, heard is the read
     * position (a restart due means the ring is not filled yet). */
    RXPos heard = rx_player_deck_heard(d);
    bool same_track = rx_player_deck_heard_gen(d) == d->track_gen;
    m->restart = true;
    m->was_on = m->on;
    if (m->on) {
        /* What is heard may still be the track before this one (the
         * deck moved on inside the shifter's delay): the restart then
         * starts this one, and the old track's last frames go with the
         * crossfade. Its buffer may be released already. */
        d->pos = same_track ? heard : (RXPos) { 0 };
    } else {
        *m->was = *d;
    }
}

/* The commands that matter to the shifter only while it is in. */
static void mt_touch(RXDeckRT *d)
{
    if (d->mt.on)
        mt_begin(d);
}

/* A stopped deck moved without a restart (a scrub): it is heard there. */
static void mt_moved(RXDeckRT *d)
{
    RXDeckMT *m = &d->mt;
    if (!m->on || m->restart)
        return;
    mt_fill_from_here(d);
}

/* A track change is a hard cut, with the shifter as without it: nothing of
 * the old track sounds on, and its buffer is not read again. */
static void mt_cut(RXDeckRT *d)
{
    RXDeckMT *m = &d->mt;
    if (!m->st)
        return;
    m->restart = false;
    m->xf_left = 0;
    if (m->on)
        rx_stretch_reset(m->st);
    m->fill_until = m->fed;
    m->fill_pos   = (RXPos) { 0 };
    m->fill_gen   = d->track_gen;
}

/* -------------------------------------------------------------- commands */

void rx_player_deck_apply(RXDeckRT *d, const RXCmd *cmd, int rate)
{
    d->out_rate = rate;
    switch (cmd->type) {
    case RX_CMD_LOAD:
    case RX_CMD_UNLOAD:
        /* The old buffer may be freed as soon as this command is
         * acknowledged; d->samples is the only pointer to it. The tail and
         * the grains are positions in the old track and must not go on in
         * the new one. A track change on a playing deck is a hard cut, as
         * on a CDJ. MASTER TEMPO is the deck's and stays as it is. A load
         * is a new start: the track queued after the old one goes too
         * (the owner releases it). */
        silence_voices(d);
        mt_cut(d);
        d->samples      = cmd->type == RX_CMD_LOAD ? cmd->samples : NULL;
        d->stream       = cmd->type == RX_CMD_LOAD ? cmd->stream : NULL;
        d->channels     = cmd->type == RX_CMD_LOAD ? cmd->channels : 0;
        d->rate         = cmd->type == RX_CMD_LOAD ? cmd->rate : 0;
        d->nb_frames    = cmd->type == RX_CMD_LOAD ? cmd->nb_frames : 0;
        d->length_final = !d->stream;
        d->posted_chunk = -1;
        d->holding      = false;
        d->pos          = (RXPos) { 0 };
        d->playing      = false;
        d->bend         = 0.0;
        d->loop_set     = false;
        d->loop_enabled = false;
        d->scrubbing    = false;
        d->scrub_resume = false;
        d->has_next     = false;
        d->next_samples = NULL;
        d->next_stream  = NULL;
        break;
    case RX_CMD_NEXT:
        /* Replaces the queued track (the owner releases the old one once
         * this is acknowledged); nothing else changes until the end. */
        d->next_samples   = cmd->samples;
        d->next_stream    = cmd->stream;
        d->next_channels  = cmd->channels;
        d->next_rate      = cmd->rate;
        d->next_nb_frames = cmd->nb_frames;
        d->has_next       = (cmd->samples || cmd->stream) && cmd->rate > 0;
        break;
    case RX_CMD_PLAY:
        if (d->scrubbing) {
            d->scrub_resume = true;
        } else {
            if (can_play(d))
                mt_touch(d);
            play(d, rate);
        }
        break;
    case RX_CMD_PAUSE:
        if (d->scrubbing) {
            d->scrub_resume = false;
        } else {
            if (d->playing)
                mt_touch(d);
            start_tail(d, rate);
            d->playing = false;
        }
        break;
    case RX_CMD_SEEK:
    case RX_CMD_PLAY_FROM:
        if (d->scrubbing) {
            d->pos = clamp_frames(d, cmd->a);
            mt_moved(d);
            if (cmd->type == RX_CMD_PLAY_FROM)
                d->scrub_resume = true;
            break;
        }
        mt_touch(d);
        seek(d, cmd->a, rate);
        if (cmd->type == RX_CMD_PLAY_FROM)
            play(d, rate);
        break;
    case RX_CMD_TEMPO:
        d->tempo = cmd->a;
        break;
    case RX_CMD_BEND:
        d->bend = cmd->a;
        break;
    case RX_CMD_VOLUME:
        d->gain = (float)cmd->a;
        break;
    case RX_CMD_LOOP_SET:
        /* Clamped like a seek: an out point at the end of the track comes
         * from seconds, and duration * rate can round a fraction of a frame
         * past nb_frames. A playhead landing exactly on nb_frames would then
         * not have crossed it, and the deck would stop instead of wrapping. */
        mt_touch(d);
        d->loop_in  = clamp_frames(d, cmd->a);
        d->loop_out = clamp_frames(d, cmd->b);
        d->loop_len = rx_pos_sub(d->loop_out, d->loop_in);
        /* The owner guarantees at least one frame; a shorter loop could
         * never be left by subtracting its length. */
        d->loop_set = rx_pos_get(d->loop_len) >= 1.0;
        d->loop_enabled = d->loop_set && cmd->flag;
        fold_into_loop(d);
        break;
    case RX_CMD_LOOP_ENABLE:
        mt_touch(d);
        d->loop_enabled = d->loop_set && cmd->flag;
        fold_into_loop(d);
        break;
    case RX_CMD_LOOP_CLEAR:
        mt_touch(d);
        d->loop_set = false;
        d->loop_enabled = false;
        break;
    case RX_CMD_SCRUB_BEGIN:
        if (d->scrubbing || !loaded(d))
            break;
        if (d->playing)
            mt_touch(d);
        d->scrub_resume = d->playing;
        start_tail(d, rate);
        d->playing    = false;
        d->scrubbing  = true;
        d->grain_from = rx_pos_get(d->pos);
        d->hop        = 0;
        break;
    case RX_CMD_SCRUB_TO:
        if (d->scrubbing) {
            d->pos = clamp_frames(d, cmd->a);
            mt_moved(d);
        } else {
            mt_touch(d);
            seek(d, cmd->a, rate);
        }
        break;
    case RX_CMD_SCRUB_END:
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
    case RX_CMD_MASTER_TEMPO:
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
static void advance(RXDeckRT *d, double inc)
{
    RXPos prev = d->pos;
    rx_pos_add(&d->pos, inc);

    if (d->loop_enabled && rx_pos_cmp(prev, d->loop_out) < 0 &&
        rx_pos_cmp(d->pos, d->loop_out) >= 0) {
        do
            d->pos = rx_pos_sub(d->pos, d->loop_len);
        while (rx_pos_cmp(d->pos, d->loop_out) >= 0);
    }

    /* The end of the track: on into the next one when there is one, else
     * the deck pauses there (PLAY MODE SINGLE, the CDJ-3000's factory
     * setting, manual p.75). */
    if (d->pos.i >= d->nb_frames) {
        if (d->has_next) {
            take_next(d);
        } else {
            d->pos = (RXPos) { .i = d->nb_frames };
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
static void voice(RXDeckRT *d, float *l, float *r)
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
        if (fetch(d, d->tail_pos, &sl, &sr)) {
            *l += sl * e;
            *r += sr * e;
        }
        rx_pos_add(&d->tail_pos, d->tail_inc);
        d->tail_left--;
    }
}

static void spawn_grain(RXDeckRT *d, int glen)
{
    RXGrain *g = &d->grains[0];
    for (int i = 0; i < RX_GRAINS; i++) {
        if (!d->grains[i].len) {
            g = &d->grains[i];
            break;
        }
        if (d->grains[i].age > g->age)
            g = &d->grains[i];
    }
    *g = (RXGrain) { .pos = d->pos, .len = glen };
    d->grain_from = rx_pos_get(d->pos);
}

/* The grains' contribution for one output frame. */
static void grains(RXDeckRT *d, float *l, float *r, int glen, double ratio, float lp)
{
    if (d->scrubbing && --d->hop <= 0) {
        d->hop = glen / 2;
        /* A new grain only when the pointer has moved by more than a
         * millisecond of track since the last one. */
        if (fabs(rx_pos_get(d->pos) - d->grain_from) > d->rate * 0.001)
            spawn_grain(d, glen);
    }

    float gl = 0.0f, gr = 0.0f;
    bool any = false;
    for (int i = 0; i < RX_GRAINS; i++) {
        RXGrain *g = &d->grains[i];
        if (!g->len)
            continue;
        float w = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * g->age / g->len);
        float sl, sr;
        if (fetch(d, g->pos, &sl, &sr)) {
            gl += sl * w;
            gr += sr * w;
        }
        rx_pos_add(&g->pos, ratio);
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
    *l += d->lp_l * RX_GRAIN_GAIN;
    *r += d->lp_r * RX_GRAIN_GAIN;
}

/* The next n frames the deck would have played as it sounded before this
 * callback's restart, into m->out: the shifter's coming output if it was
 * in (fed silence; what comes out in the next few milliseconds was decided
 * by the blocks already analysed), the dry voices if it was not. A
 * crossfade still under way carries on inside it. */
static void mt_old_sound(RXDeckRT *d, int n, int rate)
{
    RXDeckMT *m = &d->mt;
    if (m->was_on) {
        for (int c = 0; c < 2; c++)
            memset(m->in[c], 0, sizeof(float) * n);
        const float *in[2] = { m->in[0], m->in[1] };
        rx_stretch_process(m->st, in, m->out, n);
    } else {
        RXDeckRT *w = m->was;
        w->out_rate = rate;
        for (int k = 0; k < n; k++)
            voice(w, &m->out[0][k], &m->out[1][k]);
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
static void mt_restart(RXDeckRT *d, int rate)
{
    RXDeckMT *m = &d->mt;
    int n = RX_MIN(fade_frames(rate), RX_MT_CHUNK);
    mt_old_sound(d, n, rate);
    for (int c = 0; c < 2; c++)
        memcpy(m->xf[c], m->out[c], sizeof(float) * n);
    m->xf_len = m->xf_left = n;
    m->restart = false;

    /* The crossfade is the de-click now; a dry fade on top of it would
     * dip the level. */
    d->tail_left = 0;
    d->fade_in   = 0;
    rx_stretch_reset(m->st);
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
        m->hist_gen[i]   = d->track_gen;
        voice(d, &m->ahead[0][k], &m->ahead[1][k]);
    }
    m->fill_until = m->fed - m->latency;
    rx_stretch_set_pitch(m->st, pitch_factor(m->hist_speed[(m->fed - m->in_latency) &
                                                           m->hist_mask]));
    const float *ahead[2] = { m->ahead[0], m->ahead[1] };
    rx_stretch_seek(m->st, ahead, m->latency);
}

/* Feeds the next n frames of the deck through the shifter, into m->out. */
static void mt_feed(RXDeckRT *d, int n)
{
    RXDeckMT *m = &d->mt;
    for (int k = 0; k < n; k++) {
        int64_t i = (m->fed + k) & m->hist_mask;
        m->hist[i]       = d->pos;
        m->hist_speed[i] = speed_ratio(d);
        m->hist_gen[i]   = d->track_gen;
        voice(d, &m->in[0][k], &m->in[1][k]);
    }
    m->fed += n;

    rx_stretch_set_pitch(m->st, pitch_factor(m->hist_speed[(m->fed - m->in_latency) &
                                                           m->hist_mask]));
    const float *in[2] = { m->in[0], m->in[1] };
    rx_stretch_process(m->st, in, m->out, n);
}

void rx_player_deck_render(RXDeckRT *d, float *out, int frames, int rate)
{
    if (frames <= 0)
        return;
    d->out_rate = rate;

    /* A stream's length is the container's until its index is complete. */
    if (d->stream && !d->length_final) {
        bool exact;
        int64_t len = rx_packets_length(d->stream->pk, &exact);
        if (exact) {
            d->nb_frames = len;
            d->length_final = true;
        }
    }

    RXDeckMT *m = &d->mt;
    if (m->restart)
        mt_restart(d, rate);

    float decay = expf(-frames / (RX_LEVEL_RELEASE_SECONDS * rate));
    float peak_l = 0.0f, peak_r = 0.0f;

    if (loaded(d)) {
        double ratio = (double)d->rate / rate;
        int    glen  = (int)(rate * RX_GRAIN_SECONDS);
        float  lp    = 1.0f - expf(-2.0f * (float)M_PI * RX_GRAIN_LOWPASS_HZ / rate);
        float  g0    = d->gain_prev;
        float  dg    = (d->gain - g0) / frames;
        bool   grain_work = d->scrubbing;
        for (int i = 0; i < RX_GRAINS; i++)
            grain_work |= d->grains[i].len > 0;

        /* In pieces the scratch buffers hold; the dry path goes frame by
         * frame and is the same either way. */
        for (int done = 0; done < frames;) {
            int len = RX_MIN(frames - done, RX_MT_CHUNK);
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
                out[2 * n + 0] += l;
                out[2 * n + 1] += r;
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

void rx_player_deck_signal(RXDeckRT *d)
{
    RXStream *s = d->stream;
    if (!s)
        return;
    int64_t at = d->pos.i;
    atomic_store_explicit(&s->playhead, at, memory_order_relaxed);
    atomic_store_explicit(&s->starved, d->starved, memory_order_relaxed);
    /* sem_post neither blocks nor allocates (it is even async-signal-safe):
     * a futex wake when the feed sleeps. */
    int64_t c = at / RX_CHUNK;
    if (d->starved || c != d->posted_chunk) {
        if (d->wake)
            sem_post(d->wake);
        d->posted_chunk = c;
    }
    d->starved = false;
}
