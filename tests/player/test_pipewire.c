/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* PipeWire smoke test: connects to the sink named by RXBOX_PW_SINK, plays
 * a second of silence and checks the stream ran and the deck moved.
 *
 * Skipped (exit 77) unless RXBOX_PW_TEST=1 and RXBOX_PW_SINK are both set,
 * so `meson test` never opens an audio device by itself. Use a null sink:
 *
 *   pw-cli create-node adapter '{ factory.name=support.null-audio-sink
 *     node.name=rxbox-null media.class=Audio/Sink object.linger=true
 *     audio.position=[FL FR] }'
 *   RXBOX_PW_TEST=1 RXBOX_PW_SINK=rxbox-null meson test ...
 *
 * The stream sets node.dont-fallback, so a sink that does not exist is an
 * error rather than the default device; PIPEWIRE_NODE (which PipeWire
 * prefers over the stream's own target) is pointed at the same sink. */

#include "rxbox/player.h"

#include <stdio.h>
#include <time.h>

static void sleep_ms(int ms)
{
    struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000 };
    nanosleep(&ts, NULL);
}

int main(void)
{
    const char *enable = getenv("RXBOX_PW_TEST");
    const char *sink = getenv("RXBOX_PW_SINK");
    if (!enable || strcmp(enable, "1") || !sink || !*sink) {
        printf("skipped: set RXBOX_PW_TEST=1 and RXBOX_PW_SINK=<a null sink>\n");
        return 77;
    }
    setenv("PIPEWIRE_NODE", sink, 1);

    RxPlayerOptions opt;
    rx_player_options_default(&opt);
    opt.backend = RX_AUDIO_PIPEWIRE;
    opt.sink = sink;

    RxPlayer *p = NULL;
    int ret = rx_player_create(&p, &opt);
    if (ret < 0) {
        fprintf(stderr, "create: %s\n", strerror(-ret));
        return 1;
    }

    for (int i = 0; i < 500 && !rx_player_running(p); i++) {
        const char *err = rx_player_error(p);
        if (err && strstr(err, "PipeWire:"))
            break;
        sleep_ms(10);
    }
    if (!rx_player_running(p)) {
        fprintf(stderr, "not running on %s: %s\n", sink, rx_player_error(p));
        rx_player_destroy(p);
        return 1;
    }
    printf("running on %s at %d Hz\n", sink, rx_player_rate(p));

    RxAudio *a = calloc(1, sizeof(*a));
    if (!a)
        return 1;
    a->rate = 44100;
    a->channels = 2;
    a->nb_frames = 3 * 44100;
    a->samples = calloc((size_t)a->nb_frames * 2, sizeof(float));
    if (!a->samples || rx_deck_load(p, 0, a, NULL) < 0)
        return 1;

    rx_deck_play(p, 0);
    sleep_ms(1000);

    RxDeckState s;
    rx_deck_state(p, 0, &s);
    bool ok = rx_player_running(p) && s.playing && s.position > 0.5 && s.position < 2.0 &&
              s.serial >= 2;
    printf("after 1 s: playing %d, position %.3f s, serial %llu\n", s.playing, s.position,
           (unsigned long long)s.serial);

    rx_player_destroy(p);
    if (!ok) {
        fprintf(stderr, "the deck did not play\n");
        return 1;
    }
    return 0;
}
