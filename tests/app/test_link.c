/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* PRO DJ LINK in the app (src/app/link.c). First the play counter on
 * statuses made here, on a clock of the test's own: 30 s of a track's
 * beats played forward is a play, once per load; a loop, a seek, a pause,
 * a track of another source and a load of another track are not; without
 * beat numbers the time it played counts, at its pitch. Then Link in the
 * app on the loopback with the fake player of rxbox/link.h: refused
 * without --pro-link (link_state.enabled), started by the set's thread
 * and on the link at once, before any player; the player seen joining,
 * loading a track of ours and playing it, the play recorded in the
 * library after 30 s, the library read again, a hot cue the player saves
 * on the deck kept in the library (and on disk) and a rating it gives, a
 * track it tags on the app's TAG LIST, a stop, and a stop right
 * after a start, while the set is still starting the service.
 *
 * Usage: test_link FILE.wav SCRATCH_DIR */

#include "rxbox/app.h"

#include <errno.h>
#include <ftw.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "rxbox/colors.h"

#include "src/app/applink.h"
#include "src/link/client.h"
#include "src/link/dbserver.h"
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

static bool up(RXApp *app)
{
    const typeof(app->link_state) *ls = &app->link_state;
    return ls->nb_interfaces == 1 && ls->interfaces[0].running &&
           ls->interfaces[0].state == RX_LINK_UP;
}

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

/* Hot cue C at 1.5 s on the loaded track, blue. */
static bool hot_c_at(const RXTrack *t)
{
    int i = t ? rx_track_hot_cue(t, 2) : -1;
    return i >= 0 && fabs(t->cues[i].time - 1.5) < 0.001 && !strcmp(t->cues[i].color, "blue");
}

static bool has_hot_c(RXApp *app)
{
    return hot_c_at(rx_library_find_id(&app->lib, loaded_id));
}

static bool rated_4(RXApp *app)
{
    RXTrack *t = rx_library_find_id(&app->lib, loaded_id);
    return t && t->meta.rating == 4;
}

/* ------------------------------------------------------------- history */

/* As run(), for the History's timer: the time the load went on it, -1
 * none. The clock starts at epoch 1000 s. */
static double straight(RXLinkRun *r, uint32_t id, double from, double to, bool playing,
                       int (*beat_of)(double t), double bpm, int ours)
{
    double at = -1;
    for (double t = from; t < to; t += 0.05) {
        double sent = (int)(t / 0.2) * 0.2;
        RXLinkPlayer s = status(id, playing, beat_of ? beat_of(sent) : -1, bpm, 0);
        if (rx_link_run_feed(r, &s, ours, (int64_t)(t * 1e6) + 1000000, 1000 + (int64_t)t)) {
            CHECK(at < 0);
            at = t;
        }
    }
    return at;
}

/* A 4-beat loop at 120 BPM from 30 s on. */
static int beat_loop_30(double t)   { return t < 30 ? 1 + (int)(t * 2.0) : 61 + (int)(t * 2.0) % 4; }

static void test_runs(void)
{
    RXLinkRun r;

    /* 90 s straight on: on the History, once, from when it started. */
    rx_link_run_reset(&r);
    double at = straight(&r, 5, 0, 100, true, beat_120, 120, 17);
    CHECK(at >= 89.9 && at <= 90.3);
    CHECK(r.recorded && r.start_at == 1000);
    CHECK_EQ(straight(&r, 5, 100, 200, true, beat_120, 120, 17) < 0, 1);

    /* Not added up: 60 s, a pause, 60 s more. Then 90 s straight on from
     * the next start. */
    rx_link_run_reset(&r);
    CHECK_EQ(straight(&r, 5, 0, 60, true, beat_120, 120, 17) < 0, 1);
    CHECK_EQ(straight(&r, 5, 60, 65, false, beat_120, 120, 17) < 0, 1);
    CHECK_EQ(straight(&r, 5, 65, 125, true, beat_120, 120, 17) < 0, 1);
    at = straight(&r, 5, 125, 200, true, beat_120, 120, 17);
    CHECK(at >= 154.9 && at <= 155.3 && r.start_at == 1065);

    /* A loop goes back each time round: no stretch, however long. A jump
     * ahead starts one too. */
    rx_link_run_reset(&r);
    CHECK_EQ(straight(&r, 5, 0, 200, true, beat_loop_30, 120, 17) < 0, 1);
    rx_link_run_reset(&r);
    at = straight(&r, 5, 0, 120, true, beat_seek, 120, 17);
    CHECK(at >= 94.9 && at <= 95.3 && r.start_at == 1005);

    /* No beat numbers (a CDJ-900): straight on while it plays. */
    rx_link_run_reset(&r);
    at = straight(&r, 5, 0, 100, true, NULL, 0, 17);
    CHECK(at >= 89.9 && at <= 90.3);
    rx_link_run_reset(&r);
    CHECK_EQ(straight(&r, 5, 0, 80, true, NULL, 0, 17) < 0, 1);
    CHECK_EQ(straight(&r, 5, 80, 81, false, NULL, 0, 17) < 0, 1);
    CHECK_EQ(straight(&r, 5, 81, 160, true, NULL, 0, 17) < 0, 1);

    /* Another load starts again; a track of another source never counts. */
    rx_link_run_reset(&r);
    CHECK_EQ(straight(&r, 5, 0, 80, true, beat_120, 120, 17) < 0, 1);
    at = straight(&r, 6, 80, 200, true, beat_120, 120, 17);
    CHECK(at >= 169.9 && at <= 170.3);
    rx_link_run_reset(&r);
    CHECK_EQ(straight(&r, 5, 0, 200, true, beat_120, 120, 3) < 0, 1);
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
    CHECK(!app.link && !app.link_state.on);

    /* On the link at once, as device 17, with no player yet. */
    app.link_state.enabled = true;
    CHECK_EQ(rx_app_link_start(&app, &so), 0);
    CHECK(app.link_state.on);
    CHECK_EQ(rx_app_link_start(&app, &so), RX_ERROR(EALREADY));
    CHECK(tick_until(&app, up, 15));
    CHECK_STR(app.link_state.interfaces[0].interface, "lo");
    CHECK_EQ(app.link_state.interfaces[0].number, 17);
    CHECK_EQ(app.link_state.nb_devices, 0);

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

    /* The player is seen, its load of our track shows, playing, and 30 s
     * of it is a play. */
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

    /* A hot cue the player saves on the deck, as a CDJ-3000X does: the
     * answer has it, the app keeps it in the library, and it is on disk
     * (a library opened again has it). */
    RXDbClient *db = player ? rx_link_client_db(player, &err) : NULL;
    CHECK(db);
    if (db) {
        int code;
        uint32_t rgb;
        CHECK_EQ(rx_hot_cue_color("blue", &code, &rgb), 0);
        RXPlanCue cue = { .time = 1.5, .loop_end = -1, .hot = 2, .color_code = code, .rgb = rgb };
        RXDbMsg r;
        CHECK_EQ(rx_dbclient_save_cue(db, loaded_id, false, &cue, false, &r), 0);
        CHECK(r.kind == RX_DB_CUES_EXT_REPLY && rx_dbmsg_num(&r, 1) == 0 &&
              rx_dbmsg_num(&r, 4) == 1);
        CHECK(tick_until(&app, has_hot_c, 5));
        CHECK(strstr(app.status, "Player 2 saved hot cue C on") != NULL);
        RXLibrary again;
        CHECK_EQ(rx_library_open(&again, root), 0);
        CHECK(hot_c_at(rx_library_find_id(&again, loaded_id)));
        rx_library_close(&again);
        /* A rating given on the deck: kept too. */
        CHECK_EQ(rx_dbclient_request(db, RX_DB_SAVE_RATING, 8, (const uint32_t[]) { loaded_id, 4 },
                                     2, &r), 0);
        CHECK(r.kind == RX_DB_AVAILABLE && rx_dbmsg_num(&r, 1) == 0);
        CHECK(tick_until(&app, rated_4, 5));
        CHECK(strstr(app.status, "Player 2 rated") != NULL);
        /* TAG TRACK on the deck: on the app's TAG LIST, which the services
         * share. */
        CHECK_EQ(rx_dbclient_request(db, RX_DB_TAG_LIST_ADD, 8,
                                     (const uint32_t[]) { loaded_id, 1 }, 2, &r), 0);
        CHECK(r.kind == RX_DB_AVAILABLE && rx_dbmsg_num(&r, 1) == 0);
        RXLinkTagList *tags = rx_app_tag_list(&app);
        CHECK(tags && rx_link_tag_list_has(tags, loaded_id));
    }

    /* Off. */
    rx_app_link_stop(&app);
    CHECK(!app.link_state.on && !app.link_state.nb_devices && !app.link_state.nb_interfaces);
    rx_link_client_stop(&player);

    /* Off right after on: the set's thread may be starting the service. */
    CHECK_EQ(rx_app_link_start(&app, &so), 0);
    usleep(1000);
    rx_app_link_stop(&app);
    CHECK(!app.link_state.on);

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
    test_runs();
    test_app(argv[1], argv[2]);
    return test_result("test_link");
}
