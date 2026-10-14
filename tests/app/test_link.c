/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* PRO DJ LINK in the app (src/app/link.c). First the play counter on
 * statuses made here, on a clock of the test's own: 30 s of a track's
 * beats played forward is a play, once per load; a loop, a seek, a pause,
 * a track of another source and a load of another track are not; without
 * beat numbers the time it played counts, at its pitch. Then the switch
 * on the loopback with the fake player of rxbox/link.h: refused without
 * --pro-link (link_state.enabled), started on a thread, the player seen
 * joining, loading a track of ours and playing it, the play recorded in
 * the library after 30 s, a stop, and a stop asked for while starting.
 *
 * Usage: test_link FILE.wav SCRATCH_DIR */

#include "rxbox/app.h"

#include <errno.h>
#include <ftw.h>
#include <stdio.h>
#include <unistd.h>

#include "src/app/applink.h"
#include "src/link/net.h"
#include "tests/formats/test.h"

/* ---------------------------------------------------------------- plays */

static RXLinkPlayer status(uint32_t id, bool playing, int beat, double bpm, double pitch)
{
    return (RXLinkPlayer) {
        .number = 2, .type = RX_LINK_DEVICE_PLAYER, .has_status = true,
        .track_device = 17, .track_slot = 4, .track_type = 1, .track_id = id,
        .playing = playing, .bpm = bpm, .pitch = pitch, .beat = beat,
    };
}

/* A player's statuses every 200 ms, looked at every 50 ms, from `from` to
 * `to` seconds on the counter's clock: the beat a 120 BPM track is at,
 * through `beat_of`. Returns the time of the play counted, -1 none. */
static double run(RXLinkPlays *p, uint32_t id, double from, double to, bool playing,
                  int (*beat_of)(double t), double bpm, double pitch, int ours)
{
    double at = -1;
    for (double t = from; t < to; t += 0.05) {
        double sent = (int)(t / 0.2) * 0.2;
        RXLinkPlayer s = status(id, playing, beat_of ? beat_of(sent) : -1, bpm, pitch);
        if (rx_link_plays_feed(p, &s, ours, (int64_t)(t * 1e6) + 1000000)) {
            CHECK(at < 0);
            at = t;
        }
    }
    return at;
}

static int beat_120(double t)       { return 1 + (int)(t * 2.0); }
static int beat_loop(double t)      { return 1 + (int)(t * 2.0) % 4; }
static int beat_seek(double t)      { return t < 5.0 ? 1 + (int)(t * 2.0) : 500 + (int)(t * 2.0); }

static void test_plays(void)
{
    RXLinkPlays p;

    /* 60 beats after the first one looked at: 30 s. */
    rx_link_plays_reset(&p);
    double at = run(&p, 5, 0, 40, true, beat_120, 120, 0, 17);
    CHECK(at >= 29.9 && at <= 30.5);
    CHECK(p.recorded);

    /* Once per load: on and on, nothing more. */
    CHECK_EQ(run(&p, 5, 40, 80, true, beat_120, 120, 0, 17) < 0, 1);

    /* A 4-beat loop for a minute counts its 4 beats. */
    rx_link_plays_reset(&p);
    CHECK_EQ(run(&p, 5, 0, 60, true, beat_loop, 120, 0, 17) < 0, 1);
    CHECK(p.seconds <= 2.0);

    /* A seek ahead adds nothing: 5 s, then 20 s from beat 510 on. */
    rx_link_plays_reset(&p);
    CHECK_EQ(run(&p, 5, 0, 25, true, beat_seek, 120, 0, 17) < 0, 1);
    CHECK(p.seconds > 23.0 && p.seconds < 25.0);

    /* Paused, or a track of another source, or before we have a number. */
    rx_link_plays_reset(&p);
    CHECK_EQ(run(&p, 5, 0, 60, false, beat_120, 120, 0, 17) < 0, 1);
    CHECK_EQ(run(&p, 5, 0, 60, true, beat_120, 120, 0, 3) < 0, 1);
    CHECK_EQ(run(&p, 5, 0, 60, true, beat_120, 120, 0, 0) < 0, 1);

    /* Another track loaded starts again: 20 s of 5, then 6 counts from
     * its own start. */
    rx_link_plays_reset(&p);
    CHECK_EQ(run(&p, 5, 0, 20, true, beat_120, 120, 0, 17) < 0, 1);
    at = run(&p, 6, 20, 60, true, beat_120, 120, 0, 17);
    CHECK(at >= 49.9 && at <= 50.5);

    /* No beat numbers: the time played, at its pitch. */
    rx_link_plays_reset(&p);
    at = run(&p, 5, 0, 40, true, NULL, 0, 0, 17);
    CHECK(at >= 29.9 && at <= 30.3);
    rx_link_plays_reset(&p);
    at = run(&p, 5, 0, 70, true, NULL, 0, -50, 17);
    CHECK(at >= 59.9 && at <= 60.4);
}

/* ------------------------------------------------------------------ app */

static int remove_one(const char *path, const struct stat *st, int flag, struct FTW *ftw)
{
    remove(path);
    return 0;
}

static bool tick_until(RXApp *app, bool (*done)(RXApp *app), int seconds)
{
    int64_t t0 = rx_time_us();
    while (rx_time_us() - t0 < seconds * 1000000LL) {
        rx_app_tick(app);
        if (done(app))
            return true;
        usleep(20000);
    }
    return false;
}

static bool settled(RXApp *app)     { return !app->link_state.busy; }
static bool up(RXApp *app)          { return app->link_state.state == RX_LINK_UP; }

static bool sees_player(RXApp *app)
{
    for (int i = 0; i < app->link_state.nb_devices; i++) {
        const RXLinkPlayer *d = &app->link_state.devices[i];
        if (d->number == 2 && d->type == RX_LINK_DEVICE_PLAYER && d->addr == 0x7F000002)
            return true;
    }
    return false;
}

static uint32_t loaded_id;

static int player_index(RXApp *app)
{
    for (int i = 0; i < app->link_state.nb_devices; i++) {
        if (app->link_state.devices[i].number == 2)
            return i;
    }
    return -1;
}

static bool sees_load(RXApp *app)
{
    int i = player_index(app);
    return i >= 0 && app->link_state.devices[i].track_id == loaded_id &&
           app->link_state.devices[i].playing && app->link_state.played[i] > 0.5;
}

static bool played(RXApp *app)
{
    RXTrack *t = rx_library_find_id(&app->lib, loaded_id);
    return t && t->play_count == 1;
}

/* A free port on the loopback for the server, which the player is told
 * before the server binds it. */
static uint16_t free_udp(const char *addr)
{
    RXLinkIf lo;
    uint16_t port = 0;
    if (rx_link_if_find(&lo, "lo", addr, NULL) < 0)
        return 0;
    int fd = rx_link_udp(&lo, lo.addr, 0, &port);
    if (fd < 0)
        return 0;
    close(fd);
    return port;
}

static uint16_t free_tcp(void)
{
    RXLinkIf lo;
    uint16_t port = 0;
    if (rx_link_if_find(&lo, "lo", "127.0.0.1", NULL) < 0)
        return 0;
    int fd = rx_link_tcp_listen(&lo, 0, &port);
    if (fd < 0)
        return 0;
    close(fd);
    return port;
}

static void test_app(const char *wav, const char *dir)
{
    nftw(dir, remove_one, 16, FTW_DEPTH | FTW_PHYS);
    char *music = rx_path_join(dir, "music");
    char *root = rx_path_join(dir, "lib");
    char *a_wav = music ? rx_path_join(music, "a.wav") : NULL;
    uint8_t *data = NULL;
    size_t size = 0;
    CHECK(music && root && a_wav && rx_mkdir_p(music) >= 0 &&
          rx_file_read(wav, &data, &size) >= 0 && rx_file_write_atomic(a_wav, data, size) >= 0);
    free(data);

    RXPlayerOptions po;
    rx_player_options_default(&po);
    po.backend = RX_AUDIO_NULL;
    po.period = -480;
    RXApp app;
    CHECK_EQ(rx_app_create(&app, root, &po, NULL), 0);
    int err = 0;
    RXTrack *t = rx_library_add_file(&app.lib, a_wav, &err);
    CHECK(t);
    loaded_id = t ? (uint32_t)t->id : 0;

    RXLinkPorts sp = {
        .announce = free_udp("127.0.0.1"), .status = free_udp("127.0.0.1"),
        .query = free_tcp(), .portmap = free_udp("127.0.0.1"),
    };
    RXLinkPorts pp = { .announce = free_udp("127.0.0.2"), .status = free_udp("127.0.0.2") };
    CHECK(sp.announce && sp.status && sp.query && sp.portmap && pp.announce && pp.status);
    RXLinkOptions so;
    rx_link_options_default(&so);
    so.interface = "lo";
    so.broadcast = "127.0.0.2";
    so.ports = sp;
    so.peer_ports = pp;

    /* Without --pro-link nothing starts. */
    CHECK_EQ(rx_app_link_start(&app, &so), RX_ERROR(EPERM));
    CHECK(!app.link && !app.link_state.busy);

    app.link_state.enabled = true;
    CHECK_EQ(rx_app_link_start(&app, &so), 0);
    CHECK(app.link_state.busy);
    CHECK_EQ(rx_app_link_start(&app, &so), RX_ERROR(EBUSY));
    CHECK(tick_until(&app, settled, 30));
    CHECK(app.link_state.on && !app.link_state.err);
    CHECK_EQ(rx_app_link_start(&app, &so), RX_ERROR(EALREADY));
    CHECK_STR(app.link_state.interface, "lo");

    RXLinkClientOptions co;
    rx_link_client_options_default(&co);
    co.interface = "lo";
    co.address = "127.0.0.2";
    co.broadcast = "127.0.0.1";
    co.number = 2;
    co.ports = pp;
    co.server_ports = sp;
    RXLinkClient *player = NULL;
    CHECK_EQ(rx_link_client_start(&player, &co), 0);

    /* The player is seen, we join as 17, its load of our track shows,
     * playing, and 30 s of it is a play. */
    CHECK(tick_until(&app, up, 15));
    CHECK_EQ(app.link_state.number, 17);
    CHECK(tick_until(&app, sees_player, 15));
    if (player)
        rx_link_client_set_loaded(player, 17, 4, loaded_id, true);
    CHECK(tick_until(&app, sees_load, 15));
    fprintf(stderr, "test_link: waiting 30 s for the play to count\n");
    CHECK(tick_until(&app, played, 45));
    int i = player_index(&app);
    CHECK(i >= 0 && app.link_state.counted[i]);
    /* The play changed the library: the service reads it again once it
     * has settled (2 s), the player still on the link. */
    int64_t t0 = rx_time_us();
    while (rx_time_us() - t0 < 3000000) {
        rx_app_tick(&app);
        usleep(20000);
    }
    CHECK(sees_player(&app) && app.link_state.on);

    /* Off. */
    rx_app_link_stop(&app);
    CHECK(app.link_state.busy);
    CHECK(tick_until(&app, settled, 30));
    CHECK(!app.link_state.on && !app.link_state.nb_devices);
    rx_link_client_stop(&player);

    /* Off while starting: on, then off again by itself. */
    CHECK_EQ(rx_app_link_start(&app, &so), 0);
    rx_app_link_stop(&app);
    CHECK(tick_until(&app, settled, 30));
    CHECK(!app.link_state.on && !app.link_state.busy);

    /* On again and torn down with the app. */
    CHECK_EQ(rx_app_link_start(&app, &so), 0);
    rx_app_destroy(&app);
    CHECK(!app.link);

    free(music);
    free(root);
    free(a_wav);
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: test_link FILE.wav SCRATCH_DIR\n");
        return 2;
    }
    test_plays();
    test_app(argv[1], argv[2]);
    return test_result("test_link");
}
