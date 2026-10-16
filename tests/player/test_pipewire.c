/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* PipeWire smoke test: connects to the sink named by CYANBOX_PW_SINK, plays
 * a second of silence and checks the stream ran and the deck moved, then
 * the teardown paths: a player destroyed while its stream is still
 * connecting, a buffer unloaded while playing (released only once
 * PipeWire's data thread has let go of it), and a player destroyed while
 * its stream is playing.
 *
 * Skipped (exit 77) unless CYANBOX_PW_TEST=1 and CYANBOX_PW_SINK are both set,
 * so `meson test` never opens an audio device by itself. Use a null sink:
 *
 *   pw-cli create-node adapter '{ factory.name=support.null-audio-sink
 *     node.name=cyanbox-null media.class=Audio/Sink object.linger=true
 *     audio.position=[FL FR] }'
 *   CYANBOX_PW_TEST=1 CYANBOX_PW_SINK=cyanbox-null meson test ...
 *
 * The stream sets node.dont-fallback, so a sink that does not exist is an
 * error rather than the default device; PIPEWIRE_NODE (which PipeWire
 * prefers over the stream's own target) is pointed at the same sink. */

#include "cyanbox/player.h"

#include <stdio.h>
#include <time.h>

static void sleep_ms(int ms)
{
    struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000 };
    nanosleep(&ts, NULL);
}

static int released;

static void on_release(void *opaque, CBAudio *audio)
{
    released++;
    free(audio->samples);
    free(audio);
}

static CBAudio *silence(void)
{
    CBAudio *a = calloc(1, sizeof(*a));
    if (!a)
        return NULL;
    a->rate = 44100;
    a->channels = 2;
    a->nb_frames = 3 * 44100;
    a->samples = calloc((size_t)a->nb_frames * 2, sizeof(float));
    if (!a->samples) {
        free(a);
        return NULL;
    }
    return a;
}

int main(void)
{
    const char *enable = getenv("CYANBOX_PW_TEST");
    const char *sink = getenv("CYANBOX_PW_SINK");
    if (!enable || strcmp(enable, "1") || !sink || !*sink) {
        printf("skipped: set CYANBOX_PW_TEST=1 and CYANBOX_PW_SINK=<a null sink>\n");
        return 77;
    }
    setenv("PIPEWIRE_NODE", sink, 1);

    CBPlayerOptions opt;
    cb_player_options_default(&opt);
    opt.backend = CB_AUDIO_PIPEWIRE;
    opt.sink = sink;
    opt.release = on_release;

    /* Torn down before the stream has had time to connect. */
    CBPlayer *p = NULL;
    int ret = cb_player_create(&p, &opt);
    if (ret < 0) {
        fprintf(stderr, "create: %s\n", strerror(-ret));
        return 1;
    }
    cb_player_destroy(p);

    p = NULL;
    ret = cb_player_create(&p, &opt);
    if (ret < 0) {
        fprintf(stderr, "create: %s\n", strerror(-ret));
        return 1;
    }

    for (int i = 0; i < 500 && !cb_player_running(p); i++) {
        const char *err = cb_player_error(p);
        if (err && strstr(err, "PipeWire:"))
            break;
        sleep_ms(10);
    }
    if (!cb_player_running(p)) {
        fprintf(stderr, "not running on %s: %s\n", sink, cb_player_error(p));
        cb_player_destroy(p);
        return 1;
    }
    printf("running on %s at %d Hz\n", sink, cb_player_rate(p));

    CBAudio *a = silence();
    if (!a || cb_deck_load(p, 0, a, NULL) < 0)
        return 1;

    cb_deck_play(p, 0);
    sleep_ms(1000);

    CBDeckState s;
    cb_deck_state(p, 0, &s);
    bool ok = cb_player_running(p) && s.playing && s.position > 0.5 && s.position < 2.0 &&
              s.serial >= 2;
    printf("after 1 s: playing %d, position %.3f s, serial %llu\n", s.playing, s.position,
           (unsigned long long)s.serial);
    if (!ok)
        fprintf(stderr, "the deck did not play\n");

    /* Unloaded while playing: released once the data thread has applied
     * the unload, from cb_deck_state on this thread. */
    cb_deck_unload(p, 0);
    for (int i = 0; i < 200 && !released; i++) {
        sleep_ms(5);
        cb_deck_state(p, 0, &s);
    }
    if (released != 1) {
        fprintf(stderr, "the unloaded buffer was not released: %d\n", released);
        ok = false;
    }

    /* Destroyed while playing: the stream goes first, then the buffer. */
    a = silence();
    if (!a || cb_deck_load(p, 0, a, NULL) < 0)
        return 1;
    cb_deck_play(p, 0);
    sleep_ms(200);
    cb_player_destroy(p);
    if (released != 2) {
        fprintf(stderr, "destroy did not release the loaded buffer: %d\n", released);
        ok = false;
    }
    return ok ? 0 : 1;
}
