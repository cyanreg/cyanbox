/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* The PipeWire backend: one pw_stream, F32 stereo, driven by a
 * pw_thread_loop, its process callback running on PipeWire's real-time
 * data thread (PW_STREAM_FLAG_RT_PROCESS).
 *
 * The process callback is the real-time thread of player.h: it dequeues a
 * buffer, runs the shared mix into it and queues it back, and does
 * nothing else. State and format changes arrive on the thread loop's own
 * (non real-time) thread; they publish through atomics only.
 *
 * Choosing the output. With options.sink the stream targets that node and
 * also sets node.dont-fallback, node.dont-reconnect and node.dont-move, so
 * WirePlumber never routes it anywhere else: a missing sink is an error
 * ("defined target not found", see WirePlumber's
 * linking/find-defined-target.lua), not the default device, and a sink that
 * disappears is not replaced by another. PIPEWIRE_NODE is honoured by
 * pw_stream_connect itself and gets the same guards. Without either, the
 * session manager picks the default sink, which is the one case where
 * rxbox lets something else choose. */

#include "player_internal.h"

#include <stdio.h>

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/pod/builder.h>

struct RXPwOut {
    struct pw_thread_loop *loop;
    struct pw_stream      *stream;
    bool                   inited;
};

static void on_process(void *data)
{
    RXPlayer *p = data;
    struct pw_stream *stream = p->pw->stream;
    struct pw_buffer *b = pw_stream_dequeue_buffer(stream);
    if (!b)
        return;

    struct spa_buffer *buf = b->buffer;
    struct spa_data *d = &buf->datas[0];
    if (!d->data) {
        pw_stream_queue_buffer(stream, b);
        return;
    }

    uint32_t stride = 2 * sizeof(float);
    uint32_t frames = d->maxsize / stride;
    if (b->requested)
        frames = SPA_MIN(frames, (uint32_t)b->requested);

    rx_player_mix(p, d->data, (int)frames);

    d->chunk->offset = 0;
    d->chunk->stride = stride;
    d->chunk->size   = frames * stride;
    pw_stream_queue_buffer(stream, b);
}

static void on_state_changed(void *data, enum pw_stream_state old,
                             enum pw_stream_state state, const char *error)
{
    RXPlayer *p = data;
    enum RXOutState s;

    switch (state) {
    case PW_STREAM_STATE_STREAMING:
        s = RX_OUT_RUNNING;
        break;
    case PW_STREAM_STATE_PAUSED:
        s = RX_OUT_PAUSED;
        break;
    case PW_STREAM_STATE_CONNECTING:
        s = RX_OUT_CONNECTING;
        break;
    case PW_STREAM_STATE_UNCONNECTED:
        s = RX_OUT_UNCONNECTED;
        break;
    case PW_STREAM_STATE_ERROR:
    default:
        s = RX_OUT_ERROR;
        /* The message is written once, before the release store below;
         * rx_player_error reads it only after seeing ERROR. */
        if (!atomic_exchange_explicit(&p->error_set, true, memory_order_relaxed))
            snprintf(p->error, sizeof(p->error), "PipeWire: %s",
                     error ? error : "stream error");
        break;
    }

    /* ERROR is final: a later UNCONNECTED (teardown) must not hide it. */
    if (atomic_load_explicit(&p->out_state, memory_order_relaxed) == RX_OUT_ERROR)
        return;
    atomic_store_explicit(&p->out_state, s, memory_order_release);

    if (s == RX_OUT_ERROR)
        rx_err("player: %s", p->error);
    else
        rx_debug("player: PipeWire stream %s", pw_stream_state_as_string(state));
}

/* The negotiated format. The stream asks for exactly one rate and the
 * adapter converts to the graph's, so this normally confirms the request,
 * but the mix reads the rate from here every callback either way. */
static void on_param_changed(void *data, uint32_t id, const struct spa_pod *param)
{
    RXPlayer *p = data;
    if (!param || id != SPA_PARAM_Format)
        return;

    struct spa_audio_info_raw info = { 0 };
    if (spa_format_audio_raw_parse(param, &info) < 0 || !info.rate)
        return;
    atomic_store_explicit(&p->device_rate, (int)info.rate, memory_order_relaxed);
    rx_debug("player: PipeWire format %u Hz, %u channels", info.rate, info.channels);
}

static const struct pw_stream_events stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .state_changed = on_state_changed,
    .param_changed = on_param_changed,
    .process       = on_process,
};

int rx_player_pw_open(RXPlayer *p)
{
    RXPwOut *pw = calloc(1, sizeof(*pw));
    if (!pw)
        return RX_ERROR(ENOMEM);
    p->pw = pw;

    pw_init(NULL, NULL);
    pw->inited = true;

    pw->loop = pw_thread_loop_new("rxbox-audio", NULL);
    if (!pw->loop)
        return RX_ERROR(errno ? errno : ENOMEM);

    const char *name = p->app_name;
    struct pw_properties *props = pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio",
                                                    PW_KEY_MEDIA_CATEGORY, "Playback",
                                                    PW_KEY_MEDIA_ROLE, "Music",
                                                    PW_KEY_APP_NAME, name,
                                                    PW_KEY_NODE_NAME, name,
                                                    NULL);
    if (!props)
        return RX_ERROR(ENOMEM);
    int rate = p->opt.rate;
    pw_properties_setf(props, PW_KEY_NODE_LATENCY, "%d/%d", p->period, rate);
    if (p->sink)
        pw_properties_set(props, PW_KEY_TARGET_OBJECT, p->sink);
    if (p->sink || getenv("PIPEWIRE_NODE")) {
        pw_properties_set(props, "node.dont-fallback", "true");
        pw_properties_set(props, PW_KEY_NODE_DONT_RECONNECT, "true");
        pw_properties_set(props, "node.dont-move", "true");
    }

    uint8_t pod[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(pod, sizeof(pod));
    const struct spa_pod *params[1];
    struct spa_audio_info_raw info = {
        .format   = SPA_AUDIO_FORMAT_F32,
        .rate     = (uint32_t)rate,
        .channels = 2,
        .position = { SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR },
    };
    params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info);

    if (pw_thread_loop_start(pw->loop) < 0) {
        pw_properties_free(props);
        return RX_ERROR(EIO);
    }

    int ret = 0;
    pw_thread_loop_lock(pw->loop);
    /* Takes ownership of props, also on failure. */
    pw->stream = pw_stream_new_simple(pw_thread_loop_get_loop(pw->loop), name, props,
                                      &stream_events, p);
    if (!pw->stream)
        ret = RX_ERROR(errno ? errno : ENOMEM);
    else
        ret = pw_stream_connect(pw->stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
                                PW_STREAM_FLAG_AUTOCONNECT |
                                PW_STREAM_FLAG_MAP_BUFFERS |
                                PW_STREAM_FLAG_RT_PROCESS,
                                params, 1);
    pw_thread_loop_unlock(pw->loop);

    if (ret < 0) {
        rx_err("player: cannot connect a PipeWire stream: %s", strerror(-ret));
        return ret;
    }
    rx_debug("player: PipeWire stream to %s, %d Hz, latency %d/%d",
             p->sink ? p->sink : "the default sink", rate, p->period, rate);
    return 0;
}

void rx_player_pw_close(RXPlayer *p)
{
    RXPwOut *pw = p->pw;
    if (!pw)
        return;

    /* Destroying the stream removes it from the data loop synchronously,
     * so no process callback runs after this. */
    if (pw->loop) {
        pw_thread_loop_lock(pw->loop);
        if (pw->stream)
            pw_stream_destroy(pw->stream);
        pw_thread_loop_unlock(pw->loop);
        pw_thread_loop_stop(pw->loop);
        pw_thread_loop_destroy(pw->loop);
    }
    if (pw->inited)
        pw_deinit();
    free(pw);
    p->pw = NULL;
}
