/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* The playback engine: the owner-side half of every rx_deck_* function,
 * the command ring to the real-time thread, the mix callback both backends
 * run, and the state the real-time thread publishes back.
 *
 * Owner -> real time: a fixed single-producer/single-consumer ring of
 * small command structs. The owner writes the slot, then publishes the new
 * head with release order; the real-time thread reads the head with
 * acquire order, so the slot contents are visible, applies every command
 * at the start of a callback, and publishes the new tail with release
 * order so the owner may reuse those slots. A full ring drops the command
 * (with one warning) rather than block the owner or the device.
 *
 * Real time -> owner: per deck, the position, playing state and the
 * number of commands applied (its serial), under a sequence lock (see
 * RXDeckPub). The owner keeps what it last asked for (a seek, a play) with
 * the deck serial of that command, and rx_deck_state() reports its own
 * value until the published serial reaches it: a seek is visible at once,
 * and a stale position from before the seek never shows.
 *
 * Buffer release: loading or unloading hands the old buffer to a pending
 * list together with the deck serial of the command that replaced it.
 * Applying that command is the real-time thread's last use of the buffer
 * (it replaces the deck's only pointer to it), and the serial is
 * published with release order after it, so once the owner reads a serial
 * at least that high with acquire order, freeing is safe. No second ring
 * is needed and nothing can overflow. The pending list is drained by every
 * owner-side call, rx_deck_state() included. */

#include "rxbox/player.h"

#include <math.h>
#include <sched.h>
#include <stdio.h>
#include <time.h>

#include "player_internal.h"

static uint64_t dbits(double v)
{
    uint64_t u;
    memcpy(&u, &v, sizeof(u));
    return u;
}

static double bitsd(uint64_t u)
{
    double v;
    memcpy(&v, &u, sizeof(v));
    return v;
}

static unsigned fbits(float v)
{
    unsigned u;
    memcpy(&u, &v, sizeof(u));
    return u;
}

static float bitsf(unsigned u)
{
    float v;
    memcpy(&v, &u, sizeof(v));
    return v;
}

static int64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/* ------------------------------------------------------- published state */

/* Real-time side. Sequence lock writer: odd while writing. The release
 * fence after the odd store keeps the data stores from moving above it;
 * the final release store orders them before the even value. */
static void publish(RXDeckPub *pub, const RXDeckRT *d, int64_t time_us, int64_t span_us)
{
    unsigned seq = atomic_load_explicit(&pub->seq, memory_order_relaxed);
    atomic_store_explicit(&pub->seq, seq + 1, memory_order_relaxed);
    atomic_thread_fence(memory_order_release);

    atomic_store_explicit(&pub->frame_i, d->pos.i, memory_order_relaxed);
    atomic_store_explicit(&pub->frame_f, dbits(d->pos.f), memory_order_relaxed);
    atomic_store_explicit(&pub->speed, dbits(d->playing ? rx_player_deck_speed(d) : 0.0),
                          memory_order_relaxed);
    atomic_store_explicit(&pub->time_us, time_us, memory_order_relaxed);
    atomic_store_explicit(&pub->span_us, span_us, memory_order_relaxed);
    atomic_store_explicit(&pub->playing, d->playing, memory_order_relaxed);
    atomic_store_explicit(&pub->serial, d->applied, memory_order_relaxed);

    atomic_store_explicit(&pub->seq, seq + 2, memory_order_release);

    atomic_store_explicit(&pub->level_l, fbits(d->level_l), memory_order_relaxed);
    atomic_store_explicit(&pub->level_r, fbits(d->level_r), memory_order_relaxed);
}

/* Owner side. Sequence lock reader: an even value, the loads, an acquire
 * fence, the same value again. The acquire load of an even value
 * synchronises with the writer's final release store, which is what makes
 * a serial read here a safe acknowledgement for freeing a buffer. */
static void snapshot(const RXDeckPub *cpub, RXDeckSnap *s)
{
    RXDeckPub *pub = (RXDeckPub *)cpub;
    int tries = 0;
    while (1) {
        unsigned s1 = atomic_load_explicit(&pub->seq, memory_order_acquire);
        if (!(s1 & 1)) {
            int64_t  fi = atomic_load_explicit(&pub->frame_i, memory_order_relaxed);
            uint64_t ff = atomic_load_explicit(&pub->frame_f, memory_order_relaxed);
            uint64_t sp = atomic_load_explicit(&pub->speed, memory_order_relaxed);
            *s = (RXDeckSnap) {
                .time_us = atomic_load_explicit(&pub->time_us, memory_order_relaxed),
                .span_us = atomic_load_explicit(&pub->span_us, memory_order_relaxed),
                .playing = atomic_load_explicit(&pub->playing, memory_order_relaxed),
                .serial  = atomic_load_explicit(&pub->serial, memory_order_relaxed),
            };
            atomic_thread_fence(memory_order_acquire);
            if (atomic_load_explicit(&pub->seq, memory_order_relaxed) == s1) {
                s->frames = (double)fi + bitsd(ff);
                s->speed  = bitsd(sp);
                return;
            }
        }
        /* The writer is a dozen stores long; if it was preempted in the
         * middle, let it run. */
        if (++tries > 64)
            sched_yield();
    }
}

/* ---------------------------------------------------------------- mixing */

/* Linear below 0.8, then a tanh knee that reaches 1.0 only at infinity:
 * two full-scale decks summed stay below full scale and the knee is
 * smooth, so nothing ever wraps or clips hard. */
static inline float soft_clip(float x)
{
    const float knee = 0.8f;
    float a = fabsf(x);
    if (a <= knee)
        return x;
    return copysignf(knee + (1.0f - knee) * tanhf((a - knee) / (1.0f - knee)), x);
}

void rx_player_mix(RXPlayer *p, float *out, int frames)
{
    int rate = atomic_load_explicit(&p->device_rate, memory_order_relaxed);
    int64_t time_us = now_us();

    size_t tail = atomic_load_explicit(&p->tail, memory_order_relaxed);
    size_t head = atomic_load_explicit(&p->head, memory_order_acquire);
    while (tail != head) {
        const RXCmd *cmd = &p->ring[tail & (RX_CMD_RING - 1)];
        if (cmd->type == RX_CMD_MASTER_VOLUME)
            p->master_gain = (float)cmd->a;
        else if (cmd->deck >= 0 && cmd->deck < RX_DECKS)
            rx_player_deck_apply(&p->rt[cmd->deck], cmd, rate);
        tail++;
    }
    atomic_store_explicit(&p->tail, tail, memory_order_release);

    if (frames > 0) {
        memset(out, 0, sizeof(*out) * 2 * frames);
        for (int i = 0; i < RX_DECKS; i++)
            rx_player_deck_render(&p->rt[i], out, frames, rate);

        float g0 = p->master_prev;
        float dg = (p->master_gain - g0) / frames;
        for (int n = 0; n < frames; n++) {
            float g = dg != 0.0f ? g0 + dg * (n + 1) : g0;
            out[2 * n + 0] = soft_clip(out[2 * n + 0] * g);
            out[2 * n + 1] = soft_clip(out[2 * n + 1] * g);
        }
        p->master_prev = p->master_gain;
    }

    for (int i = 0; i < RX_DECKS; i++)
        publish(&p->pub[i], &p->rt[i], time_us, rate > 0 ? (int64_t)frames * 1000000 / rate : 0);
}

/* ------------------------------------------------------------ owner side */

static void release_audio(RXPlayer *p, RXAudio *audio)
{
    if (p->opt.release) {
        p->opt.release(p->opt.release_opaque, audio);
        return;
    }
    rx_audio_free(audio);
    free(audio);
}

static void drain_releases(RXPlayer *p)
{
    for (int i = 0; i < p->nb_releases;) {
        RXRelease *r = &p->releases[i];
        RXDeckSnap s;
        snapshot(&p->pub[r->deck], &s);
        if (s.serial < r->serial) {
            i++;
            continue;
        }
        release_audio(p, r->audio);
        *r = p->releases[--p->nb_releases];
    }
}

static void defer_release(RXPlayer *p, int deck, RXAudio *audio)
{
    if (!audio)
        return;
    if (p->nb_releases == p->releases_cap) {
        int cap = p->releases_cap ? 2 * p->releases_cap : 8;
        RXRelease *r = realloc(p->releases, sizeof(*r) * cap);
        if (!r) {
            /* Freeing now could pull the buffer from under the real-time
             * thread; leaking it is the safe failure. */
            rx_err("player: out of memory, leaking a released track buffer");
            return;
        }
        p->releases = r;
        p->releases_cap = cap;
    }
    p->releases[p->nb_releases++] = (RXRelease) {
        .audio  = audio,
        .deck   = deck,
        .serial = p->own[deck].sent,
    };
}

static int push(RXPlayer *p, RXCmd cmd)
{
    drain_releases(p);

    size_t head = atomic_load_explicit(&p->head, memory_order_relaxed);
    size_t tail = atomic_load_explicit(&p->tail, memory_order_acquire);
    if (head - tail >= RX_CMD_RING) {
        if (!p->ring_full_warned)
            rx_warn("player: command queue full (is the audio output running?), dropping commands");
        p->ring_full_warned = true;
        return RX_ERROR(EAGAIN);
    }
    p->ring_full_warned = false;

    p->ring[head & (RX_CMD_RING - 1)] = cmd;
    atomic_store_explicit(&p->head, head + 1, memory_order_release);
    if (cmd.deck >= 0)
        p->own[cmd.deck].sent++;
    return 0;
}

/* Most commands are a type, a deck and at most one value. */
static int send(RXPlayer *p, enum RXCmdType type, int deck, double a)
{
    return push(p, (RXCmd) {
        .type = type,
        .deck = deck,
        .a    = a,
    });
}

static RXDeckOwner *loaded_deck(RXPlayer *p, int deck)
{
    if (!p || deck < 0 || deck >= RX_DECKS || !p->own[deck].audio)
        return NULL;
    return &p->own[deck];
}

static double duration_of(const RXDeckOwner *o)
{
    return (double)o->audio->nb_frames / o->audio->rate;
}

/* Seconds to frames, clamped to the track. */
static double to_frames(const RXDeckOwner *o, double seconds)
{
    double t = RX_CLAMP(seconds, 0.0, duration_of(o));
    return t * o->audio->rate;
}

static void owner_reset(RXDeckOwner *o)
{
    o->pos_frames   = 0.0;
    o->playing      = false;
    o->bend         = 0.0;
    o->loop_enabled = false;
    o->loop_in      = -1.0;
    o->loop_out     = -1.0;
    o->scrubbing    = false;
    o->scrub_resume = false;
}

/* ------------------------------------------------------------ the player */

void rx_player_options_default(RXPlayerOptions *o)
{
    *o = (RXPlayerOptions) {
        .backend  = RX_AUDIO_PIPEWIRE,
        .rate     = 48000,
        .period   = 256,
        .app_name = "rxbox",
    };
}

int rx_player_create(RXPlayer **out, const RXPlayerOptions *opt)
{
    int ret = 0;
    RXPlayerOptions def;
    if (!opt) {
        rx_player_options_default(&def);
        opt = &def;
    }

    *out = NULL;
    if (opt->rate < 0 || opt->rate > 768000)
        return RX_ERROR(EINVAL);
    if (opt->period < -65536 || opt->period > 65536 ||
        (opt->backend != RX_AUDIO_NULL && opt->period < 0))
        return RX_ERROR(EINVAL);

    size_t size = RX_ALIGN(sizeof(RXPlayer), 64);
    RXPlayer *p = aligned_alloc(64, size);
    if (!p)
        return RX_ERROR(ENOMEM);
    memset(p, 0, size);

    p->opt = *opt;
    p->sink = rx_strdup(opt->sink);
    p->app_name = rx_strdup(opt->app_name ? opt->app_name : "rxbox");
    if ((opt->sink && !p->sink) || !p->app_name) {
        ret = RX_ERROR(ENOMEM);
        goto fail;
    }
    p->opt.sink = p->sink;
    p->opt.app_name = p->app_name;
    if (!p->opt.rate)
        p->opt.rate = 48000;
    p->manual = opt->backend == RX_AUDIO_NULL && opt->period < 0;
    p->period = opt->period ? abs(opt->period) : 256;

    atomic_init(&p->head, 0);
    atomic_init(&p->tail, 0);
    atomic_init(&p->device_rate, p->opt.rate);
    atomic_init(&p->out_state, RX_OUT_CONNECTING);
    atomic_init(&p->error_set, false);
    p->master_gain = p->master_prev = 1.0f;
    p->master_volume = 1.0f;
    for (int i = 0; i < RX_DECKS; i++) {
        rx_player_deck_init(&p->rt[i]);
        RXDeckOwner *o = &p->own[i];
        owner_reset(o);
        o->tempo  = 1.0;
        o->volume = 1.0f;
        atomic_init(&p->pub[i].seq, 0);
    }

    if (opt->backend == RX_AUDIO_NULL)
        ret = rx_player_null_open(p);
    else
        ret = rx_player_pw_open(p);
    if (ret < 0)
        goto fail;

    *out = p;
    return 0;

fail:
    rx_player_destroy(p);
    return ret;
}

void rx_player_destroy(RXPlayer *p)
{
    if (!p)
        return;

    /* The backend's thread is joined first; after that nothing reads the
     * buffers and every one of them can go. */
    if (p->null)
        rx_player_null_close(p);
    if (p->pw)
        rx_player_pw_close(p);

    for (int i = 0; i < p->nb_releases; i++)
        release_audio(p, p->releases[i].audio);
    free(p->releases);
    for (int i = 0; i < RX_DECKS; i++) {
        if (p->own[i].audio)
            release_audio(p, p->own[i].audio);
        free(p->own[i].grid.sections);
    }
    free(p->sink);
    free(p->app_name);
    free(p);
}

int rx_player_rate(const RXPlayer *p)
{
    return atomic_load_explicit(&p->device_rate, memory_order_relaxed);
}

enum RXAudioBackend rx_player_backend(const RXPlayer *p)
{
    return p->opt.backend;
}

const char *rx_player_sink(const RXPlayer *p)
{
    return p->sink;
}

bool rx_player_running(const RXPlayer *p)
{
    return p && atomic_load_explicit(&p->out_state, memory_order_acquire) == RX_OUT_RUNNING;
}

const char *rx_player_error(const RXPlayer *p)
{
    if (!p)
        return "no player";
    /* The acquire load pairs with the backend's release store of ERROR,
     * after which the message is never written again. */
    switch (atomic_load_explicit(&p->out_state, memory_order_acquire)) {
    case RX_OUT_RUNNING:
        return NULL;
    case RX_OUT_CONNECTING:
        return "connecting to the audio server";
    case RX_OUT_PAUSED:
        return "audio stream paused (not linked to a sink yet)";
    case RX_OUT_UNCONNECTED:
        return "audio stream disconnected";
    case RX_OUT_ERROR:
    default:
        return p->error[0] ? p->error : "audio stream error";
    }
}

void rx_player_set_master_volume(RXPlayer *p, float gain)
{
    if (!p || !isfinite(gain))
        return;
    gain = RX_CLAMP(gain, 0.0f, 1.0f);
    if (send(p, RX_CMD_MASTER_VOLUME, -1, gain) == 0)
        p->master_volume = gain;
}

void rx_player_null_advance(RXPlayer *p, int frames)
{
    if (!p || !p->manual || frames <= 0)
        return;
    drain_releases(p);
    rx_player_null_run(p, frames);
    drain_releases(p);
}

/* ----------------------------------------------------------------- decks */

static void copy_grid(RXGrid *dst, const RXGrid *src)
{
    free(dst->sections);
    *dst = (RXGrid) { .beats_per_bar = 4 };
    if (!src || src->nb_sections <= 0 || !src->sections)
        return;
    dst->sections = malloc(sizeof(*dst->sections) * src->nb_sections);
    if (!dst->sections)
        return;
    memcpy(dst->sections, src->sections, sizeof(*dst->sections) * src->nb_sections);
    dst->nb_sections   = src->nb_sections;
    dst->beats_per_bar = src->beats_per_bar;
}

int rx_player_deck_set_grid(RXPlayer *p, int deck, const RXGrid *grid)
{
    if (!p || deck < 0 || deck >= RX_DECKS)
        return RX_ERROR(EINVAL);
    copy_grid(&p->own[deck].grid, grid);
    if (grid && grid->nb_sections > 0 && !p->own[deck].grid.sections)
        return RX_ERROR(ENOMEM);
    return 0;
}

const RXGrid *rx_player_deck_grid(const RXPlayer *p, int deck)
{
    if (!p || deck < 0 || deck >= RX_DECKS)
        return NULL;
    return &p->own[deck].grid;
}

int rx_deck_load(RXPlayer *p, int deck, RXAudio *audio, const RXGrid *grid)
{
    if (!p || deck < 0 || deck >= RX_DECKS || !audio)
        return RX_ERROR(EINVAL);
    if (audio->channels < 1 || audio->rate <= 0 || audio->nb_frames < 0 ||
        (audio->nb_frames > 0 && !audio->samples))
        return RX_ERROR(EINVAL);

    RXGrid copy = { 0 };
    copy_grid(&copy, grid);
    if (grid && grid->nb_sections > 0 && !copy.sections)
        return RX_ERROR(ENOMEM);

    int ret = push(p, (RXCmd) {
        .type      = RX_CMD_LOAD,
        .deck      = deck,
        .samples   = audio->samples,
        .channels  = audio->channels,
        .rate      = audio->rate,
        .nb_frames = audio->nb_frames,
    });
    if (ret < 0) {
        free(copy.sections);
        return ret;
    }

    RXDeckOwner *o = &p->own[deck];
    defer_release(p, deck, o->audio);
    o->audio = audio;
    free(o->grid.sections);
    o->grid = copy;
    owner_reset(o);
    o->pos_serial = o->play_serial = o->sent;
    return 0;
}

void rx_deck_unload(RXPlayer *p, int deck)
{
    RXDeckOwner *o = loaded_deck(p, deck);
    if (!o || send(p, RX_CMD_UNLOAD, deck, 0.0) < 0)
        return;
    defer_release(p, deck, o->audio);
    o->audio = NULL;
    free(o->grid.sections);
    o->grid = (RXGrid) { .beats_per_bar = 4 };
    owner_reset(o);
    o->pos_serial = o->play_serial = o->sent;
}

bool rx_deck_loaded(const RXPlayer *p, int deck)
{
    return p && deck >= 0 && deck < RX_DECKS && p->own[deck].audio;
}

const RXAudio *rx_deck_audio(const RXPlayer *p, int deck)
{
    if (!p || deck < 0 || deck >= RX_DECKS)
        return NULL;
    return p->own[deck].audio;
}

/* The owner's view of the playing state right now. */
static bool owner_playing(RXPlayer *p, int deck)
{
    RXDeckSnap s;
    snapshot(&p->pub[deck], &s);
    const RXDeckOwner *o = &p->own[deck];
    return s.serial < o->play_serial ? o->playing : s.playing;
}

static void set_playing(RXPlayer *p, int deck, enum RXCmdType type, bool playing)
{
    RXDeckOwner *o = loaded_deck(p, deck);
    if (!o || send(p, type, deck, 0.0) < 0)
        return;
    if (o->scrubbing) {
        o->scrub_resume = playing;
        return;
    }
    o->playing = playing;
    o->play_serial = o->sent;
}

void rx_deck_play(RXPlayer *p, int deck)
{
    set_playing(p, deck, RX_CMD_PLAY, true);
}

void rx_deck_pause(RXPlayer *p, int deck)
{
    set_playing(p, deck, RX_CMD_PAUSE, false);
}

static void seek_cmd(RXPlayer *p, int deck, double seconds, enum RXCmdType type)
{
    RXDeckOwner *o = loaded_deck(p, deck);
    if (!o || !isfinite(seconds))
        return;
    double frames = to_frames(o, seconds);
    if (send(p, type, deck, frames) < 0)
        return;
    o->pos_frames = frames;
    o->pos_serial = o->sent;
    if (type == RX_CMD_PLAY_FROM) {
        if (o->scrubbing) {
            o->scrub_resume = true;
        } else {
            o->playing = frames < (double)o->audio->nb_frames;
            o->play_serial = o->sent;
        }
    } else if (frames >= (double)o->audio->nb_frames && !o->scrubbing) {
        /* A seek to the very end stops the deck there. */
        o->playing = false;
        o->play_serial = o->sent;
    }
}

void rx_deck_seek(RXPlayer *p, int deck, double seconds)
{
    seek_cmd(p, deck, seconds, RX_CMD_SEEK);
}

void rx_deck_play_from(RXPlayer *p, int deck, double seconds)
{
    seek_cmd(p, deck, seconds, RX_CMD_PLAY_FROM);
}

void rx_deck_set_tempo(RXPlayer *p, int deck, double ratio)
{
    if (!p || deck < 0 || deck >= RX_DECKS || !isfinite(ratio))
        return;
    /* Wider than any CDJ range (+-100 %), and never backwards. */
    ratio = RX_CLAMP(ratio, 0.0, 4.0);
    if (send(p, RX_CMD_TEMPO, deck, ratio) == 0)
        p->own[deck].tempo = ratio;
}

bool rx_deck_set_master_tempo(RXPlayer *p, int deck, bool on)
{
    return false;
}

void rx_deck_set_loop(RXPlayer *p, int deck, double in, double out, bool enabled)
{
    RXDeckOwner *o = loaded_deck(p, deck);
    if (!o || !isfinite(in) || !isfinite(out))
        return;
    in  = RX_CLAMP(in, 0.0, duration_of(o));
    out = RX_CLAMP(out, 0.0, duration_of(o));
    /* At least one frame long, or the loop could never be left. */
    if ((out - in) * o->audio->rate < 1.0)
        return;
    RXCmd cmd = {
        .type = RX_CMD_LOOP_SET,
        .deck = deck,
        .a    = in * o->audio->rate,
        .b    = out * o->audio->rate,
        .flag = enabled,
    };
    if (push(p, cmd) < 0)
        return;
    o->loop_in = in;
    o->loop_out = out;
    o->loop_enabled = enabled;
}

void rx_deck_loop_enable(RXPlayer *p, int deck, bool enabled)
{
    RXDeckOwner *o = loaded_deck(p, deck);
    if (!o || o->loop_in < 0.0)
        return;
    RXCmd cmd = {
        .type = RX_CMD_LOOP_ENABLE,
        .deck = deck,
        .flag = enabled,
    };
    if (push(p, cmd) == 0)
        o->loop_enabled = enabled;
}

void rx_deck_clear_loop(RXPlayer *p, int deck)
{
    RXDeckOwner *o = loaded_deck(p, deck);
    if (!o || send(p, RX_CMD_LOOP_CLEAR, deck, 0.0) < 0)
        return;
    o->loop_enabled = false;
    o->loop_in = o->loop_out = -1.0;
}

void rx_deck_set_volume(RXPlayer *p, int deck, float gain)
{
    if (!p || deck < 0 || deck >= RX_DECKS || !isfinite(gain))
        return;
    gain = RX_CLAMP(gain, 0.0f, 1.0f);
    if (send(p, RX_CMD_VOLUME, deck, gain) == 0)
        p->own[deck].volume = gain;
}

void rx_deck_set_bend(RXPlayer *p, int deck, double ratio_offset)
{
    if (!p || deck < 0 || deck >= RX_DECKS || !isfinite(ratio_offset))
        return;
    ratio_offset = RX_CLAMP(ratio_offset, -1.0, 1.0);
    if (send(p, RX_CMD_BEND, deck, ratio_offset) == 0)
        p->own[deck].bend = ratio_offset;
}

void rx_deck_scrub_begin(RXPlayer *p, int deck)
{
    RXDeckOwner *o = loaded_deck(p, deck);
    if (!o || o->scrubbing)
        return;
    bool was_playing = owner_playing(p, deck);
    if (send(p, RX_CMD_SCRUB_BEGIN, deck, 0.0) < 0)
        return;
    o->scrubbing = true;
    o->scrub_resume = was_playing;
    o->playing = false;
    o->play_serial = o->sent;
}

void rx_deck_scrub_to(RXPlayer *p, int deck, double seconds)
{
    seek_cmd(p, deck, seconds, RX_CMD_SCRUB_TO);
}

void rx_deck_scrub_end(RXPlayer *p, int deck)
{
    RXDeckOwner *o = loaded_deck(p, deck);
    if (!o || !o->scrubbing || send(p, RX_CMD_SCRUB_END, deck, 0.0) < 0)
        return;
    o->scrubbing = false;
    o->playing = o->scrub_resume && o->pos_frames < (double)o->audio->nb_frames;
    o->play_serial = o->sent;
}

void rx_deck_state(const RXPlayer *cp, int deck, RXDeckState *out)
{
    *out = (RXDeckState) {
        .tempo    = 1.0,
        .loop_in  = -1.0,
        .loop_out = -1.0,
    };
    if (!cp || deck < 0 || deck >= RX_DECKS)
        return;

    /* Logically const: the only thing changed is the release list, which
     * the owner thread alone touches. */
    RXPlayer *p = (RXPlayer *)cp;
    drain_releases(p);

    const RXDeckOwner *o = &p->own[deck];
    const RXDeckPub *pub = &p->pub[deck];
    RXDeckSnap s;
    snapshot(pub, &s);

    out->tempo   = o->tempo;
    out->serial  = s.serial;
    out->level_l = bitsf(atomic_load_explicit(&pub->level_l, memory_order_relaxed));
    out->level_r = bitsf(atomic_load_explicit(&pub->level_r, memory_order_relaxed));
    if (!o->audio)
        return;

    double frames = s.frames;
    bool playing = s.playing;
    bool pending = false;
    if (s.serial < o->pos_serial) {
        frames = o->pos_frames;
        pending = true;
    }
    if (s.serial < o->play_serial) {
        playing = o->playing;
        pending = true;
    }

    double end = (double)o->audio->nb_frames;
    /* Between callbacks the playhead moves on: extrapolate from the
     * callback's time stamp by at most the length of the block it rendered,
     * which is where the next callback will publish. A late callback then
     * holds the playhead still instead of making it step back, and a
     * stalled stream does not run away. Never past a loop's out point or
     * the end. The manual null backend is exact instead. */
    if (!pending && playing && !o->scrubbing && !p->manual && s.speed > 0.0) {
        double dt = RX_CLAMP((now_us() - s.time_us) * 1e-6, 0.0, s.span_us * 1e-6);
        double limit = end;
        double out_frames = o->loop_out * o->audio->rate;
        if (o->loop_enabled && frames < out_frames)
            limit = out_frames;
        frames = RX_MIN(frames + dt * s.speed, limit);
    }
    frames = RX_CLAMP(frames, 0.0, end);

    out->loaded       = true;
    out->playing      = playing;
    out->scrubbing    = o->scrubbing;
    out->position     = frames / o->audio->rate;
    out->duration     = duration_of(o);
    out->frame        = (int64_t)floor(frames);
    out->loop_enabled = o->loop_enabled;
    out->loop_in      = o->loop_in;
    out->loop_out     = o->loop_out;
}
