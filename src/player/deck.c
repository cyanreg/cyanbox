/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* One deck as the real-time thread sees it: the commands that change it
 * and the renderer that adds it to the mix.
 *
 * Everything here runs on the real-time thread and follows its rules: no
 * allocation, no locks, no logging, no system calls. The buffer is read,
 * never written; the owner keeps it alive until this deck has applied the
 * command that replaced it (see the release path in player.c). Voices keep
 * positions, never a pointer, so after the swap nothing reads the old
 * buffer; a load or unload still silences the de-click tail and the scrub
 * grains at once, or they would play the old track's positions out of the
 * new one.
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
 * platter. */

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

/* The track at position p; a mono track plays on both sides, and channels
 * past the second are ignored. */
static void fetch(const RXDeckRT *d, RXPos p, float *l, float *r)
{
    int64_t i = p.i;
    float t = (float)p.f;
    int nc = d->channels;

    if (i >= 1 && i + 2 < d->nb_frames) {
        const float *s = d->samples + (i - 1) * nc;
        *l = hermite(s[0], s[nc], s[2 * nc], s[3 * nc], t);
        if (nc > 1)
            *r = hermite(s[1], s[nc + 1], s[2 * nc + 1], s[3 * nc + 1], t);
        else
            *r = *l;
        return;
    }

    *l = hermite(at(d, i - 1, 0), at(d, i, 0), at(d, i + 1, 0), at(d, i + 2, 0), t);
    if (nc > 1)
        *r = hermite(at(d, i - 1, 1), at(d, i, 1), at(d, i + 1, 1), at(d, i + 2, 1), t);
    else
        *r = *l;
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

static void seek(RXDeckRT *d, double frames, int rate)
{
    start_tail(d, rate);
    if (d->playing)
        start_fade_in(d, rate);
    d->pos = clamp_frames(d, frames);
    if (d->pos.i >= d->nb_frames)
        d->playing = false;
}

static void play(RXDeckRT *d, int rate)
{
    if (d->playing || !d->samples || d->pos.i >= d->nb_frames)
        return;
    d->playing = true;
    start_fade_in(d, rate);
}

/* -------------------------------------------------------------- commands */

void rx_player_deck_apply(RXDeckRT *d, const RXCmd *cmd, int rate)
{
    switch (cmd->type) {
    case RX_CMD_LOAD:
    case RX_CMD_UNLOAD:
        /* The old buffer may be freed as soon as this command is
         * acknowledged; d->samples is the only pointer to it. The tail and
         * the grains are positions in the old track and must not go on in
         * the new one. A track change on a playing deck is a hard cut, as
         * on a CDJ. */
        silence_voices(d);
        d->samples      = cmd->type == RX_CMD_LOAD ? cmd->samples : NULL;
        d->channels     = cmd->type == RX_CMD_LOAD ? cmd->channels : 0;
        d->rate         = cmd->type == RX_CMD_LOAD ? cmd->rate : 0;
        d->nb_frames    = cmd->type == RX_CMD_LOAD ? cmd->nb_frames : 0;
        d->pos          = (RXPos) { 0 };
        d->playing      = false;
        d->bend         = 0.0;
        d->loop_set     = false;
        d->loop_enabled = false;
        d->scrubbing    = false;
        d->scrub_resume = false;
        break;
    case RX_CMD_PLAY:
        if (d->scrubbing)
            d->scrub_resume = true;
        else
            play(d, rate);
        break;
    case RX_CMD_PAUSE:
        if (d->scrubbing) {
            d->scrub_resume = false;
        } else {
            start_tail(d, rate);
            d->playing = false;
        }
        break;
    case RX_CMD_SEEK:
    case RX_CMD_PLAY_FROM:
        if (d->scrubbing) {
            d->pos = clamp_frames(d, cmd->a);
            if (cmd->type == RX_CMD_PLAY_FROM)
                d->scrub_resume = true;
            break;
        }
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
        d->loop_enabled = d->loop_set && cmd->flag;
        fold_into_loop(d);
        break;
    case RX_CMD_LOOP_CLEAR:
        d->loop_set = false;
        d->loop_enabled = false;
        break;
    case RX_CMD_SCRUB_BEGIN:
        if (d->scrubbing || !d->samples)
            break;
        d->scrub_resume = d->playing;
        start_tail(d, rate);
        d->playing    = false;
        d->scrubbing  = true;
        d->grain_from = rx_pos_get(d->pos);
        d->hop        = 0;
        break;
    case RX_CMD_SCRUB_TO:
        if (d->scrubbing)
            d->pos = clamp_frames(d, cmd->a);
        else
            seek(d, cmd->a, rate);
        break;
    case RX_CMD_SCRUB_END:
        if (!d->scrubbing)
            break;
        d->scrubbing = false;
        if (d->scrub_resume)
            play(d, rate);
        d->scrub_resume = false;
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

    /* The end of the track pauses the deck there (PLAY MODE SINGLE, the
     * CDJ-3000's factory setting, manual p.75). */
    if (d->pos.i >= d->nb_frames) {
        d->pos = (RXPos) { .i = d->nb_frames };
        d->playing = false;
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
        fetch(d, g->pos, &sl, &sr);
        gl += sl * w;
        gr += sr * w;
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

void rx_player_deck_render(RXDeckRT *d, float *out, int frames, int rate)
{
    if (frames <= 0)
        return;

    float decay = expf(-frames / (RX_LEVEL_RELEASE_SECONDS * rate));
    float peak_l = 0.0f, peak_r = 0.0f;

    if (d->samples) {
        double inc   = increment(d, rate);
        double ratio = (double)d->rate / rate;
        int    glen  = (int)(rate * RX_GRAIN_SECONDS);
        float  lp    = 1.0f - expf(-2.0f * (float)M_PI * RX_GRAIN_LOWPASS_HZ / rate);
        float  g0    = d->gain_prev;
        float  dg    = (d->gain - g0) / frames;
        bool   grain_work = d->scrubbing;
        for (int i = 0; i < RX_GRAINS; i++)
            grain_work |= d->grains[i].len > 0;

        for (int n = 0; n < frames; n++) {
            float l = 0.0f, r = 0.0f;

            if (d->playing) {
                fetch(d, d->pos, &l, &r);
                if (d->fade_in > 0) {
                    float e = 1.0f - (float)d->fade_in / d->fade_len;
                    l *= e;
                    r *= e;
                    d->fade_in--;
                }
                advance(d, inc);
            }

            if (d->tail_left > 0) {
                float sl, sr, e = (float)d->tail_left / d->tail_len;
                fetch(d, d->tail_pos, &sl, &sr);
                l += sl * e;
                r += sr * e;
                rx_pos_add(&d->tail_pos, d->tail_inc);
                d->tail_left--;
            }

            if (grain_work)
                grains(d, &l, &r, glen, ratio, lp);

            float g = dg != 0.0f ? g0 + dg * (n + 1) : g0;
            l *= g;
            r *= g;
            out[2 * n + 0] += l;
            out[2 * n + 1] += r;
            peak_l = fmaxf(peak_l, fabsf(l));
            peak_r = fmaxf(peak_r, fabsf(r));
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
