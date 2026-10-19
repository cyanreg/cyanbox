/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* PRO DJ LINK in the app (src/app/link.c). First the play counter on
 * statuses made here, on a clock of the test's own: 30 s of a track's
 * beats played forward is a play, once per load; a loop, a seek, a pause,
 * a track of another source and a load of another track are not; without
 * beat numbers the time it played counts, at its pitch. Then Link in the
 * app on the loopback with the fake player of cyanbox/link.h: refused
 * without --pro-link (link_state.enabled), started by the set's thread
 * and on the link at once, before any player; the player seen joining,
 * loading a track of ours and playing it, the play recorded in the
 * library after 30 s, the library read again, a hot cue the player saves
 * on the deck kept in the library (and on disk) and a rating it gives, a
 * track it tags on the app's TAG LIST, a stop, and a stop right
 * after a start, while the set is still starting the service. Last LINK
 * CUE with the player's ring backend (as --pro-link has it): a mixer of
 * the test's own hears nothing while the deck is paused; the deck played,
 * its timing is answered packet for packet with the deck's audio, the
 * deck moving by what was taken, and nothing once it is paused again.
 * Then the deck's output following the mixer (--pro-link), from the
 * player's usual output (the null backend here): not moved by the mixer's
 * keep-alive alone, moved to the ring once its timing is heard and played
 * there at its pace, back between 2.5 and 4 s after it went silent with
 * the deck playing on, to the ring again, and back when Link stops.
 *
 * Usage: test_link FILE.wav SCRATCH_DIR */

#include "cyanbox/app.h"

#include <errno.h>
#include <ftw.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cyanbox/colors.h"

#include "src/app/applink.h"
#include "src/link/client.h"
#include "src/link/dbserver.h"
#include "src/link/net.h"
#include "src/link/packet.h"
#include "tests/formats/test.h"

/* ---------------------------------------------------------------- plays */

static CBLinkPlayer status(uint32_t id, bool playing, int beat, double bpm, double pitch)
{
    return (CBLinkPlayer) {
        .number = 2, .type = CB_LINK_DEVICE_PLAYER, .has_status = true,
        .track_device = 17, .track_slot = 4, .track_type = 1, .track_id = id,
        .playing = playing, .bpm = bpm, .pitch = pitch, .beat = beat,
    };
}

/* A player's statuses every 200 ms, looked at every 50 ms, from `from` to
 * `to` seconds on the counter's clock: the beat a 120 BPM track is at,
 * through `beat_of`. Returns the time of the play counted, -1 none. */
static double run(CBLinkPlays *p, uint32_t id, double from, double to, bool playing,
                  int (*beat_of)(double t), double bpm, double pitch, int ours)
{
    double at = -1;
    for (double t = from; t < to; t += 0.05) {
        double sent = (int)(t / 0.2) * 0.2;
        CBLinkPlayer s = status(id, playing, beat_of ? beat_of(sent) : -1, bpm, pitch);
        if (cb_link_plays_feed(p, &s, ours, (int64_t)(t * 1e6) + 1000000)) {
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
    CBLinkPlays p;

    /* 60 beats after the first one looked at: 30 s. */
    cb_link_plays_reset(&p);
    double at = run(&p, 5, 0, 40, true, beat_120, 120, 0, 17);
    CHECK(at >= 29.9 && at <= 30.5);
    CHECK(p.recorded);

    /* Once per load: on and on, nothing more. */
    CHECK_EQ(run(&p, 5, 40, 80, true, beat_120, 120, 0, 17) < 0, 1);

    /* A 4-beat loop for a minute counts its 4 beats. */
    cb_link_plays_reset(&p);
    CHECK_EQ(run(&p, 5, 0, 60, true, beat_loop, 120, 0, 17) < 0, 1);
    CHECK(p.seconds <= 2.0);

    /* A seek ahead adds nothing: 5 s, then 20 s from beat 510 on. */
    cb_link_plays_reset(&p);
    CHECK_EQ(run(&p, 5, 0, 25, true, beat_seek, 120, 0, 17) < 0, 1);
    CHECK(p.seconds > 23.0 && p.seconds < 25.0);

    /* Paused, or a track of another source, or before we have a number. */
    cb_link_plays_reset(&p);
    CHECK_EQ(run(&p, 5, 0, 60, false, beat_120, 120, 0, 17) < 0, 1);
    CHECK_EQ(run(&p, 5, 0, 60, true, beat_120, 120, 0, 3) < 0, 1);
    CHECK_EQ(run(&p, 5, 0, 60, true, beat_120, 120, 0, 0) < 0, 1);

    /* Another track loaded starts again: 20 s of 5, then 6 counts from
     * its own start. */
    cb_link_plays_reset(&p);
    CHECK_EQ(run(&p, 5, 0, 20, true, beat_120, 120, 0, 17) < 0, 1);
    at = run(&p, 6, 20, 60, true, beat_120, 120, 0, 17);
    CHECK(at >= 49.9 && at <= 50.5);

    /* No beat numbers: the time played, at its pitch. */
    cb_link_plays_reset(&p);
    at = run(&p, 5, 0, 40, true, NULL, 0, 0, 17);
    CHECK(at >= 29.9 && at <= 30.3);
    cb_link_plays_reset(&p);
    at = run(&p, 5, 0, 70, true, NULL, 0, -50, 17);
    CHECK(at >= 59.9 && at <= 60.4);
}

/* ------------------------------------------------------------------ app */

static int remove_one(const char *path, const struct stat *st, int flag, struct FTW *ftw)
{
    remove(path);
    return 0;
}

static bool tick_until(CBApp *app, bool (*done)(CBApp *app), int seconds)
{
    int64_t t0 = cb_time_us();
    while (cb_time_us() - t0 < seconds * 1000000LL) {
        cb_app_tick(app);
        if (done(app))
            return true;
        usleep(20000);
    }
    return false;
}

static bool up(CBApp *app)
{
    const typeof(app->link_state) *ls = &app->link_state;
    return ls->nb_interfaces == 1 && ls->interfaces[0].running &&
           ls->interfaces[0].state == CB_LINK_UP;
}

static bool sees_player(CBApp *app)
{
    for (int i = 0; i < app->link_state.nb_devices; i++) {
        const CBLinkPlayer *d = &app->link_state.devices[i];
        if (d->number == 2 && d->type == CB_LINK_DEVICE_PLAYER && d->addr == 0x7F000002)
            return true;
    }
    return false;
}

static uint32_t loaded_id;

static int player_index(CBApp *app)
{
    for (int i = 0; i < app->link_state.nb_devices; i++) {
        if (app->link_state.devices[i].number == 2)
            return i;
    }
    return -1;
}

static bool sees_load(CBApp *app)
{
    int i = player_index(app);
    return i >= 0 && app->link_state.devices[i].track_id == loaded_id &&
           app->link_state.devices[i].playing && app->link_state.played[i] > 0.5;
}

static bool played(CBApp *app)
{
    CBTrack *t = cb_library_find_id(&app->lib, loaded_id);
    return t && t->play_count == 1;
}

/* Hot cue C at 1.5 s on the loaded track, blue. */
static bool hot_c_at(const CBTrack *t)
{
    int i = t ? cb_track_hot_cue(t, 2) : -1;
    return i >= 0 && fabs(t->cues[i].time - 1.5) < 0.001 && !strcmp(t->cues[i].color, "blue");
}

static bool has_hot_c(CBApp *app)
{
    return hot_c_at(cb_library_find_id(&app->lib, loaded_id));
}

static bool rated_4(CBApp *app)
{
    CBTrack *t = cb_library_find_id(&app->lib, loaded_id);
    return t && t->meta.rating == 4;
}

/* ------------------------------------------------------------- history */

/* As run(), for the History's timer: the time the load went on it, -1
 * none. The clock starts at epoch 1000 s. */
static double straight(CBLinkRun *r, uint32_t id, double from, double to, bool playing,
                       int (*beat_of)(double t), double bpm, int ours)
{
    double at = -1;
    for (double t = from; t < to; t += 0.05) {
        double sent = (int)(t / 0.2) * 0.2;
        CBLinkPlayer s = status(id, playing, beat_of ? beat_of(sent) : -1, bpm, 0);
        if (cb_link_run_feed(r, &s, ours, (int64_t)(t * 1e6) + 1000000, 1000 + (int64_t)t)) {
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
    CBLinkRun r;

    /* 90 s straight on: on the History, once, from when it started. */
    cb_link_run_reset(&r);
    double at = straight(&r, 5, 0, 100, true, beat_120, 120, 17);
    CHECK(at >= 89.9 && at <= 90.3);
    CHECK(r.recorded && r.start_at == 1000);
    CHECK_EQ(straight(&r, 5, 100, 200, true, beat_120, 120, 17) < 0, 1);

    /* Not added up: 60 s, a pause, 60 s more. Then 90 s straight on from
     * the next start. */
    cb_link_run_reset(&r);
    CHECK_EQ(straight(&r, 5, 0, 60, true, beat_120, 120, 17) < 0, 1);
    CHECK_EQ(straight(&r, 5, 60, 65, false, beat_120, 120, 17) < 0, 1);
    CHECK_EQ(straight(&r, 5, 65, 125, true, beat_120, 120, 17) < 0, 1);
    at = straight(&r, 5, 125, 200, true, beat_120, 120, 17);
    CHECK(at >= 154.9 && at <= 155.3 && r.start_at == 1065);

    /* A loop goes back each time round: no stretch, however long. A jump
     * ahead starts one too. */
    cb_link_run_reset(&r);
    CHECK_EQ(straight(&r, 5, 0, 200, true, beat_loop_30, 120, 17) < 0, 1);
    cb_link_run_reset(&r);
    at = straight(&r, 5, 0, 120, true, beat_seek, 120, 17);
    CHECK(at >= 94.9 && at <= 95.3 && r.start_at == 1005);

    /* No beat numbers (a CDJ-900): straight on while it plays. */
    cb_link_run_reset(&r);
    at = straight(&r, 5, 0, 100, true, NULL, 0, 17);
    CHECK(at >= 89.9 && at <= 90.3);
    cb_link_run_reset(&r);
    CHECK_EQ(straight(&r, 5, 0, 80, true, NULL, 0, 17) < 0, 1);
    CHECK_EQ(straight(&r, 5, 80, 81, false, NULL, 0, 17) < 0, 1);
    CHECK_EQ(straight(&r, 5, 81, 160, true, NULL, 0, 17) < 0, 1);

    /* Another load starts again; a track of another source never counts. */
    cb_link_run_reset(&r);
    CHECK_EQ(straight(&r, 5, 0, 80, true, beat_120, 120, 17) < 0, 1);
    at = straight(&r, 6, 80, 200, true, beat_120, 120, 17);
    CHECK(at >= 169.9 && at <= 170.3);
    cb_link_run_reset(&r);
    CHECK_EQ(straight(&r, 5, 0, 200, true, beat_120, 120, 3) < 0, 1);
}

/* A free port on the loopback for the server, which the player is told
 * before the server binds it. */
static uint16_t free_udp(const char *addr)
{
    CBLinkIf lo;
    uint16_t port = 0;
    if (cb_link_if_find(&lo, "lo", addr, NULL) < 0)
        return 0;
    int fd = cb_link_udp(&lo, lo.addr, 0, &port);
    if (fd < 0)
        return 0;
    close(fd);
    return port;
}

static uint16_t free_tcp(void)
{
    CBLinkIf lo;
    uint16_t port = 0;
    if (cb_link_if_find(&lo, "lo", "127.0.0.1", NULL) < 0)
        return 0;
    int fd = cb_link_tcp_listen(&lo, 0, &port);
    if (fd < 0)
        return 0;
    close(fd);
    return port;
}

static void test_app(const char *wav, const char *dir)
{
    nftw(dir, remove_one, 16, FTW_DEPTH | FTW_PHYS);
    char *music = cb_path_join(dir, "music");
    char *root = cb_path_join(dir, "lib");
    char *a_wav = music ? cb_path_join(music, "a.wav") : NULL;
    uint8_t *data = NULL;
    size_t size = 0;
    CHECK(music && root && a_wav && cb_mkdir_p(music) >= 0 &&
          cb_file_read(wav, &data, &size) >= 0 && cb_file_write_atomic(a_wav, data, size) >= 0);
    free(data);

    CBPlayerOptions po;
    cb_player_options_default(&po);
    po.backend = CB_AUDIO_NULL;
    po.period = -480;
    CBApp app;
    CHECK_EQ(cb_app_create(&app, root, &po, NULL), 0);
    int err = 0;
    CBTrack *t = cb_library_add_file(&app.lib, a_wav, &err);
    CHECK(t);
    loaded_id = t ? (uint32_t)t->id : 0;

    CBLinkPorts sp = {
        .announce = free_udp("127.0.0.1"), .status = free_udp("127.0.0.1"),
        .query = free_tcp(), .portmap = free_udp("127.0.0.1"),
    };
    CBLinkPorts pp = { .announce = free_udp("127.0.0.2"), .status = free_udp("127.0.0.2") };
    CHECK(sp.announce && sp.status && sp.query && sp.portmap && pp.announce && pp.status);
    CBLinkOptions so;
    cb_link_options_default(&so);
    so.interface = "lo";
    so.broadcast = "127.0.0.2";
    so.ports = sp;
    so.peer_ports = pp;

    /* Without --pro-link nothing starts. */
    CHECK_EQ(cb_app_link_start(&app, &so), CB_ERROR(EPERM));
    CHECK(!app.link && !app.link_state.on);

    /* On the link at once, as device 17, with no player yet. */
    app.link_state.enabled = true;
    CHECK_EQ(cb_app_link_start(&app, &so), 0);
    CHECK(app.link_state.on);
    CHECK_EQ(cb_app_link_start(&app, &so), CB_ERROR(EALREADY));
    CHECK(tick_until(&app, up, 15));
    CHECK_STR(app.link_state.interfaces[0].interface, "lo");
    CHECK_EQ(app.link_state.interfaces[0].number, 17);
    CHECK_EQ(app.link_state.nb_devices, 0);

    CBLinkClientOptions co;
    cb_link_client_options_default(&co);
    co.interface = "lo";
    co.address = "127.0.0.2";
    co.broadcast = "127.0.0.1";
    co.number = 2;
    co.ports = pp;
    co.server_ports = sp;
    CBLinkClient *player = NULL;
    CHECK_EQ(cb_link_client_start(&player, &co), 0);

    /* The player is seen, its load of our track shows, playing, and 30 s
     * of it is a play. */
    CHECK(tick_until(&app, sees_player, 15));
    if (player)
        cb_link_client_set_loaded(player, 17, 4, loaded_id, true);
    CHECK(tick_until(&app, sees_load, 15));
    fprintf(stderr, "test_link: waiting 30 s for the play to count\n");
    CHECK(tick_until(&app, played, 45));
    int i = player_index(&app);
    CHECK(i >= 0 && app.link_state.counted[i]);
    /* The play changed the library: the service reads it again once it
     * has settled (2 s), the player still on the link. */
    int64_t t0 = cb_time_us();
    while (cb_time_us() - t0 < 3000000) {
        cb_app_tick(&app);
        usleep(20000);
    }
    CHECK(sees_player(&app) && app.link_state.on);

    /* A hot cue the player saves on the deck, as a CDJ-3000X does: the
     * answer has it, the app keeps it in the library, and it is on disk
     * (a library opened again has it). */
    CBDbClient *db = player ? cb_link_client_db(player, &err) : NULL;
    CHECK(db);
    if (db) {
        int code;
        uint32_t rgb;
        CHECK_EQ(cb_hot_cue_color("blue", &code, &rgb), 0);
        CBPlanCue cue = { .time = 1.5, .loop_end = -1, .hot = 2, .color_code = code, .rgb = rgb };
        CBDbMsg r;
        CHECK_EQ(cb_dbclient_save_cue(db, loaded_id, false, &cue, false, &r), 0);
        CHECK(r.kind == CB_DB_CUES_EXT_REPLY && cb_dbmsg_num(&r, 1) == 0 &&
              cb_dbmsg_num(&r, 4) == 1);
        CHECK(tick_until(&app, has_hot_c, 5));
        CHECK(strstr(app.status, "Player 2 saved hot cue C on") != NULL);
        CBLibrary again;
        CHECK_EQ(cb_library_open(&again, root), 0);
        CHECK(hot_c_at(cb_library_find_id(&again, loaded_id)));
        cb_library_close(&again);
        /* A rating given on the deck: kept too. */
        CHECK_EQ(cb_dbclient_request(db, CB_DB_SAVE_RATING, 8, (const uint32_t[]) { loaded_id, 4 },
                                     2, &r), 0);
        CHECK(r.kind == CB_DB_AVAILABLE && cb_dbmsg_num(&r, 1) == 0);
        CHECK(tick_until(&app, rated_4, 5));
        CHECK(strstr(app.status, "Player 2 rated") != NULL);
        /* TAG TRACK on the deck: on the app's TAG LIST, which the services
         * share. */
        CHECK_EQ(cb_dbclient_request(db, CB_DB_TAG_LIST_ADD, 8,
                                     (const uint32_t[]) { loaded_id, 1 }, 2, &r), 0);
        CHECK(r.kind == CB_DB_AVAILABLE && cb_dbmsg_num(&r, 1) == 0);
        CBLinkTagList *tags = cb_app_tag_list(&app);
        CHECK(tags && cb_link_tag_list_has(tags, loaded_id));
    }

    /* Link > Push settings: the library's player settings, to the one
     * player; the lock at its default, Lock. */
    int np = 0, nm = 0;
    CHECK_EQ(cb_app_link_push_settings(&app, &np, &nm), 0);
    CHECK(np == 1 && nm == 0);
    uint8_t ms[40] = { 0 }, ms2[40] = { 0 };
    CHECK(player && cb_link_client_wait_pushed_settings(player, 5000, ms, ms2) == 0);
    CHECK(ms[0] == 0x78 && ms[21] == 0x81);

    /* Off. */
    cb_app_link_stop(&app);
    CHECK(!app.link_state.on && !app.link_state.nb_devices && !app.link_state.nb_interfaces);
    CHECK_EQ(cb_app_link_push_settings(&app, &np, &nm), CB_ERROR(ENOENT));
    cb_link_client_stop(&player);

    /* Off right after on: the set's thread may be starting the service. */
    CHECK_EQ(cb_app_link_start(&app, &so), 0);
    usleep(1000);
    cb_app_link_stop(&app);
    CHECK(!app.link_state.on);

    /* On again and torn down with the app. */
    CHECK_EQ(cb_app_link_start(&app, &so), 0);
    cb_app_destroy(&app);
    CHECK(!app.link);

    free(music);
    free(root);
    free(a_wav);
}

/* ------------------------------------------------------------- LINK CUE */

static bool deck_loaded(CBApp *app)
{
    CBDeckState s;
    cb_deck_state(app->player, 0, &s);
    return app->decks[0].track && s.loaded;
}

static bool sees_mixer(CBApp *app)
{
    for (int i = 0; i < app->link_state.nb_devices; i++) {
        const CBLinkPlayer *d = &app->link_state.devices[i];
        if (d->number == 33 && d->type == CB_LINK_DEVICE_MIXER)
            return true;
    }
    return false;
}

/* The mixer, a DJM-A9 (device 33) on 127.0.0.2, as far as LINK CUE goes:
 * a timing every 7 ms for `ms` to the app's service at `port`, the app
 * ticking every 21 ms, and what comes back: how many timings were
 * answered, the loudest sample and how many took over. It hands over at
 * the first that does, as a mixer does. */
typedef struct Mixer {
    CBLinkIf ifc;
    int fd;
    uint16_t port;              /* the service's */
    uint32_t counter;
    int elected;
    int sent, answered, takes, peak, bad;
} Mixer;

static void mixer_run(CBApp *app, Mixer *m, int ms)
{
    m->sent = m->answered = m->takes = m->peak = 0;
    static uint8_t buf[2048];
    static CBPktCueAudio a;
    int64_t next = cb_time_us(), end = next + ms * INT64_C(1000), tick = next;
    while (cb_time_us() < end) {
        CBPktCueTiming t = { .name = "DJM-A9", .mixer = 33, .counter = m->counter++, .on = true,
                             .source = m->elected };
        CBWr w = cb_wr(buf, sizeof(buf));
        cb_pkt_cue_timing(&w, &t);
        cb_link_send(m->fd, 0x7F000001, m->port, w.p, w.len);
        m->sent++;
        next += 7000;
        while (cb_time_us() < next) {
            if (cb_time_us() >= tick) {
                cb_app_tick(app);
                tick += 21000;
            }
            uint32_t from;
            uint16_t port;
            ssize_t n = cb_link_recv(&m->ifc, m->fd, buf, sizeof(buf), &from, &port);
            if (n <= 0) {
                usleep(500);
                continue;
            }
            if (cb_pkt_cue_audio_parse(&a, buf, (size_t)n) < 0 || a.number != 17 ||
                a.counter + 1 != m->counter) {
                m->bad++;
                continue;
            }
            m->answered++;
            for (int i = 0; i < CB_CUE_FRAMES * 2; i++)
                m->peak = CB_MAX(m->peak, abs(a.pcm[i]));
            if (a.mode == CB_CUE_TAKE) {
                m->takes++;
                CBWr hw = cb_wr(buf, sizeof(buf));
                cb_pkt_cue_handover(&hw, "DJM-A9", 33);
                cb_link_send(m->fd, 0x7F000001, m->port, hw.p, hw.len);
                m->elected = 17;
            }
        }
    }
}

/* An app on `backend` with a track loaded on its deck, Link on the
 * loopback with a cue port of its own, and the mixer's sockets. */
typedef struct Rig {
    CBApp app;
    bool created;
    Mixer m;
    int fa, fs;
    CBLinkPorts sp;
    char *music, *root, *a_wav;
} Rig;

static void rig_open(Rig *r, const char *wav, const char *dir, enum CBAudioBackend backend)
{
    *r = (Rig) { .m = { .counter = 5000, .fd = -1 }, .fa = -1, .fs = -1 };
    nftw(dir, remove_one, 16, FTW_DEPTH | FTW_PHYS);
    r->music = cb_path_join(dir, "music");
    r->root = cb_path_join(dir, "lib");
    r->a_wav = r->music ? cb_path_join(r->music, "a.wav") : NULL;
    uint8_t *data = NULL;
    size_t size = 0;
    CHECK(r->music && r->root && r->a_wav && cb_mkdir_p(r->music) >= 0 &&
          cb_file_read(wav, &data, &size) >= 0 && cb_file_write_atomic(r->a_wav, data, size) >= 0);
    free(data);

    CBPlayerOptions po;
    cb_player_options_default(&po);
    po.backend = backend;
    CBApp *app = &r->app;
    CHECK_EQ(cb_app_create(app, r->root, &po, NULL), 0);
    r->created = true;
    int err = 0;
    CBTrack *t = cb_library_add_file(&app->lib, r->a_wav, &err);
    CHECK(t && cb_app_load_deck(app, 0, t) == 0);
    CHECK(tick_until(app, deck_loaded, 30));

    Mixer *m = &r->m;
    uint16_t ma = 0, ms = 0;
    CHECK_EQ(cb_link_if_find(&m->ifc, "lo", "127.0.0.2", NULL), 0);
    r->fa = cb_link_udp(&m->ifc, m->ifc.addr, 0, &ma);
    r->fs = cb_link_udp(&m->ifc, m->ifc.addr, 0, &ms);
    uint16_t mc = 0;
    m->fd = cb_link_udp(&m->ifc, m->ifc.addr, 0, &mc);
    r->sp = (CBLinkPorts) {
        .announce = free_udp("127.0.0.1"), .status = free_udp("127.0.0.1"),
        .query = free_tcp(), .portmap = free_udp("127.0.0.1"), .cue = free_udp("127.0.0.1"),
    };
    m->port = r->sp.cue;
    CHECK(r->fa >= 0 && r->fs >= 0 && m->fd >= 0 && r->sp.announce && r->sp.status &&
          r->sp.query && r->sp.portmap && r->sp.cue);
    CBLinkOptions so;
    cb_link_options_default(&so);
    so.interface = "lo";
    so.broadcast = "127.0.0.2";
    so.ports = r->sp;
    so.peer_ports = (CBLinkPorts) { .announce = ma, .status = ms, .cue = mc };
    app->link_state.enabled = true;
    CHECK_EQ(cb_app_link_start(app, &so), 0);
    CHECK(app->link_state.cue_enabled);
    CHECK(tick_until(app, up, 15));
}

/* The mixer on the link: its keep-alive, as a DJM-V5's. */
static void rig_keep_alive(Rig *r)
{
    CBPktKeepAlive k = { .name = "DJM-A9", .proto = 2, .number = 33, .first = 2,
                         .mac = { 0xc8, 0x3d, 0xfc, 0x1d, 0xfe, 0xa2 }, .addr = r->m.ifc.addr,
                         .peers = 1, .type = 3, .tail = 0x31 };
    uint8_t kb[64];
    CBWr kw = cb_wr(kb, sizeof(kb));
    cb_pkt_keep_alive(&kw, &k);
    if (r->fa >= 0)
        cb_link_send(r->fa, 0x7F000001, r->sp.announce, kw.p, kw.len);
}

static void rig_close(Rig *r)
{
    if (r->created) {
        cb_app_destroy(&r->app);
        CHECK(!r->app.link);
    }
    int fds[] = { r->fa, r->fs, r->m.fd };
    for (size_t i = 0; i < CB_ARRAY_ELEMS(fds); i++) {
        if (fds[i] >= 0)
            close(fds[i]);
    }
    free(r->music);
    free(r->root);
    free(r->a_wav);
}

static void test_cue(const char *wav, const char *dir)
{
    Rig r;
    rig_open(&r, wav, dir, CB_AUDIO_RING);
    CBApp *app = &r.app;
    Mixer *m = &r.m;
    rig_keep_alive(&r);
    CHECK(tick_until(app, sees_mixer, 5));

    /* Paused: nothing, the mixer seen. */
    mixer_run(app, m, 150);
    CHECK(m->sent > 15 && !m->answered && !m->bad);
    cb_app_tick(app);
    const CBLinkCueState *cs = &app->link_state.cue;
    CHECK(cs->mixer && !strcmp(cs->name, "DJM-A9") && cs->number == 33 && cs->on && !cs->ours &&
          !cs->sending);

    /* Played: a second of the deck's audio, the deck moving by what was
     * taken (the ring holds 21 ms more). */
    CBDeckState st;
    cb_deck_state(app->player, 0, &st);
    double from = st.position;
    cb_deck_play(app->player, 0);
    cb_app_tick(app);
    mixer_run(app, m, 1000);
    cb_deck_state(app->player, 0, &st);
    double moved = st.position - from, taken = m->answered * (double)CB_CUE_FRAMES / 48000;
    fprintf(stderr, "test_link: LINK CUE: %d timings, %d answered, peak %d, moved %.3f s for "
            "%.3f s taken\n", m->sent, m->answered, m->peak, moved, taken);
    CHECK(m->answered >= m->sent - 3 && m->takes == 1 && !m->bad);
    CHECK(m->peak > 10000);
    CHECK(fabs(moved - taken) < 0.06);
    cb_app_tick(app);
    CHECK(cs->mixer && cs->on && cs->source == 17 && cs->ours && cs->sending);

    /* Paused again: nothing. */
    cb_deck_pause(app->player, 0);
    cb_app_tick(app);
    mixer_run(app, m, 150);
    CHECK(m->sent > 15 && !m->answered && !m->bad);
    rig_close(&r);
}

static bool on_ring(CBApp *app)
{
    return cb_player_backend(app->player) == CB_AUDIO_RING;
}

static bool on_null(CBApp *app)
{
    return cb_player_backend(app->player) == CB_AUDIO_NULL;
}

static double deck_position(CBApp *app)
{
    CBDeckState st;
    cb_deck_state(app->player, 0, &st);
    return st.position;
}

/* The deck's output following the mixer, the player's own (here the null
 * backend, standing for PipeWire) while there is none. */
static void test_output(const char *wav, const char *dir)
{
    Rig r;
    rig_open(&r, wav, dir, CB_AUDIO_NULL);
    CBApp *app = &r.app;
    Mixer *m = &r.m;
    CHECK(on_null(app));

    /* The mixer on the link, its keep-alive alone: the deck stays. */
    rig_keep_alive(&r);
    CHECK(tick_until(app, sees_mixer, 5));
    for (int i = 0; i < 15; i++) {
        cb_app_tick(app);
        usleep(20000);
    }
    CHECK(on_null(app) && !app->link_state.cue.mixer);

    /* Its timing heard: the deck in its LINK CUE. */
    mixer_run(app, m, 150);
    CHECK(on_ring(app) && app->link_state.cue.mixer && strstr(app->status, "LINK CUE"));

    /* Played: the deck's audio, at the mixer's pace. */
    double from = deck_position(app);
    cb_deck_play(app->player, 0);
    cb_app_tick(app);
    mixer_run(app, m, 600);
    double moved = deck_position(app) - from, taken = m->answered * (double)CB_CUE_FRAMES / 48000;
    CHECK(m->answered >= m->sent - 3 && m->peak > 10000 && !m->bad);
    CHECK(fabs(moved - taken) < 0.06);

    /* The mixer silent: the deck back where it was after the cue's second
     * and the two held, playing on all the while. */
    int64_t t0 = cb_time_us();
    from = deck_position(app);
    CHECK(tick_until(app, on_null, 8));
    double after = (cb_time_us() - t0) / 1e6;
    moved = deck_position(app) - from;
    fprintf(stderr, "test_link: output back after %.2f s, the deck moved %.2f s\n", after, moved);
    CHECK(after > 2.5 && after < 4.0);
    /* On the clock but for the 150 ms the ring waits for the mixer's next
     * take after its last (RING_IDLE_US). */
    CHECK(moved > after - 0.3 && moved < after + 0.05);
    CHECK(strstr(app->status, "No mixer"));

    /* Heard again, and Link stopped: back. */
    mixer_run(app, m, 150);
    CHECK(on_ring(app));
    cb_app_link_stop(app);
    CHECK(on_null(app));
    rig_close(&r);
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
    char *cue_dir = cb_path_join(argv[2], "cue");
    if (cue_dir)
        test_cue(argv[1], cue_dir);
    free(cue_dir);
    char *output_dir = cb_path_join(argv[2], "output");
    if (output_dir)
        test_output(argv[1], output_dir);
    free(output_dir);
    return test_result("test_link");
}
