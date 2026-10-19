/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* Link end to end on the loopback: cyanbox's server (cb_link_start) and the
 * fake player (cb_link_client_start) on real sockets, each with ports of
 * its own so the two fit on one host and nothing competes with a running
 * cyanbox or rekordbox for 50000. The server is 127.0.0.1 and "broadcasts"
 * to the player's 127.0.0.2, the player the other way round.
 *
 *  - The player announces itself; the server, which waits for a player,
 *    joins, takes 17 and lists the player.
 *  - The player browses as a CDJ-3000 does (cb_link_client_browse) and
 *    reads every track over NFS, at the size the track info gave: each a
 *    16-bit WAV of the file made as it is read (src/link/wav.c), the
 *    file's samples as cb_decode has them, and nothing made on disk.
 *  - The server sees what the player loaded and that it plays; a cue
 *    the player saves is handed on for the library's owner. A LOAD of
 *    My Settings gets the library's player settings, and a mixer's its
 *    mixer settings, cyanbox's defaults where it sets none; settings
 *    pushed to the player reach it.
 *  - A client that never announced itself (127.0.0.3) gets no database
 *    port and no NFS answer: the fences of docs/LINK.md, "Security".
 *  - A library change is served again: a new track is browsable. It is
 *    read on a thread of its own while the player is answered, and a cue
 *    saved meanwhile is served after it. A track whose own state changed
 *    (a rating, a cue) is read again alone and served at once; one whose
 *    title changed has the whole library read again.
 *
 * Tracks are made by the ffmpeg CLI (skips, 77, without it, or without
 * libopus). The protocol details are the codecs' tests' (test_packets,
 * test_db, test_nfs); this one is about the parts working together. */

#include <math.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>

#include "cyanbox/analysis.h"
#include "cyanbox/audio.h"
#include "cyanbox/link.h"

#include "src/link/client.h"
#include "src/link/dbserver.h"
#include "src/link/net.h"
#include "tests/formats/test.h"

static char root[512], music[700], libdir[700];

static int sh(const char *fmt, ...) CB_PRINTF(1, 2);
static int sh(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char *cmd = cb_vasprintf(fmt, ap);
    va_end(ap);
    int r = cmd ? system(cmd) : -1;
    free(cmd);
    return r;
}

static const struct { const char *file, *codec; double bpm; } tracks[] = {
    { "alpha.flac",  "-c:a flac",                 126 },
    { "bravo.mp3",   "-c:a libmp3lame -b:a 320k", 128 },
    { "charlie.opus", "-c:a libopus",             140 },
};

static int make_track(const char *file, const char *codec, double bpm)
{
    double p = 60.0 / bpm;
    return sh("ffmpeg -v error -nostdin -y -f lavfi -i \"aevalsrc='0.7*sin(2*PI*55*t)*exp(-14*mod(t,%.6f))"
              "+0.05*sin(2*PI*7000*t)*exp(-60*mod(t+%.6f,%.6f))':s=44100:d=8\" %s '%s/%s'",
              p, p / 2, p, codec, music, file);
}

/* A track as the analysis would leave it: a grid and waveforms. */
static int add_track(CBLibrary *lib, const char *file, double bpm)
{
    char *path = cb_asprintf("%s/%s", music, file);
    int err = 0;
    CBTrack *t = path ? cb_library_add_file(lib, path, &err) : NULL;
    free(path);
    CHECK(t);
    if (!t)
        return -1;
    CHECK_EQ(cb_grid_init(&t->grid, 0.1, bpm), 0);
    CHECK_EQ(cb_library_save_track(lib, t), 0);
    CBAudio a;
    CBWaveforms w;
    CHECK_EQ(cb_decode(t->path, 1, 0, &a), 0);
    CHECK_EQ(cb_waveforms_compute(a.samples, a.nb_frames, a.rate, &w), 0);
    CHECK_EQ(cb_library_waveforms_save(lib, t->id, &w), 0);
    cb_waveforms_free(&w);
    cb_audio_free(&a);
    return 0;
}

static int make_library(void)
{
    for (size_t i = 0; i < CB_ARRAY_ELEMS(tracks); i++) {
        if (make_track(tracks[i].file, tracks[i].codec, tracks[i].bpm))
            return 77;
    }
    CBLibrary lib = { 0 };
    CHECK_EQ(cb_library_open(&lib, libdir), 0);
    for (size_t i = 0; i < CB_ARRAY_ELEMS(tracks); i++)
        add_track(&lib, tracks[i].file, tracks[i].bpm);
    CBPlaylist *p = cb_library_playlist_create(&lib, lib.root_folder, "Loop");
    CHECK(p);
    for (int i = 0; p && i < lib.nb_tracks; i++)
        cb_library_playlist_add(&lib, p, lib.tracks[i], -1);
    cb_library_close(&lib);
    return 0;
}

/* Two free UDP ports on 127.0.0.2 for the player, which the server is
 * told before the player binds them. */
static int free_ports(uint16_t *a, uint16_t *b)
{
    CBLinkIf lo;
    int ret = cb_link_if_find(&lo, "lo", "127.0.0.2", NULL);
    if (ret < 0)
        return ret;
    int fa = cb_link_udp(&lo, lo.addr, 0, a), fb = cb_link_udp(&lo, lo.addr, 0, b);
    if (fa >= 0)
        close(fa);
    if (fb >= 0)
        close(fb);
    return fa < 0 ? fa : fb < 0 ? fb : 0;
}

static void quiet_line(void *opaque, const char *line)
{
    if (getenv("CB_TEST_VERBOSE"))
        fprintf(stderr, "  %s\n", line);
}

/* The bytes of a track's WAV: the 44-byte header of 16-bit stereo, then
 * the samples of its file (by the name in its path) as cb_decode has them,
 * mono doubled, within dither and rounding. */
static bool same_as_decoded(const char *usb_path, const uint8_t *data, size_t size)
{
    const char *base = NULL;
    for (size_t i = 0; i < CB_ARRAY_ELEMS(tracks) && !base; i++) {
        char stem[32];
        snprintf(stem, sizeof(stem), "%.*s", (int)strcspn(tracks[i].file, "."), tracks[i].file);
        if (strstr(usb_path, stem))
            base = tracks[i].file;
    }
    char *path = base ? cb_asprintf("%s/%s", music, base) : NULL;
    CBAudio d = { 0 };
    bool ok = path && cb_decode(path, 0, 0, &d) >= 0 && size >= 44 && !memcmp(data, "RIFF", 4) &&
              !memcmp(data + 8, "WAVEfmt ", 8) && !memcmp(data + 36, "data", 4);
    int64_t frames = ok ? CB_MIN((int64_t)(size - 44) / 4, d.nb_frames) : 0;
    int64_t off = 0;
    for (int64_t f = 0; f < frames; f++) {
        for (int c = 0; c < 2; c++) {
            const uint8_t *b = data + 44 + f * 4 + c * 2;
            float x = d.samples[f * d.channels + (d.channels > 1 ? c : 0)];
            off += fabs((int16_t)(b[0] | b[1] << 8) - (double)x * 32768.0) > 1.5;
        }
    }
    ok = ok && frames > 0 && !off && llabs((int64_t)(size - 44) / 4 - d.nb_frames) < 2048;
    if (!ok)
        fprintf(stderr, "%s: %zu bytes over NFS, not a WAV of %s (%lld samples off)\n", usb_path,
                size, path ? path : "?", (long long)off);
    cb_audio_free(&d);
    free(path);
    return ok;
}

static int wait_up(CBLink *link, int timeout_ms)
{
    for (int i = 0; i < timeout_ms / 20; i++) {
        int number = 0;
        if (cb_link_state(link, &number) == CB_LINK_UP)
            return number;
        usleep(20000);
    }
    return -1;
}

int main(void)
{
    const char *dir = getenv("CB_TEST_DIR");
    snprintf(root, sizeof(root), "%s", dir ? dir : "build/test-link-loopback");
    snprintf(music, sizeof(music), "%s/music", root);
    snprintf(libdir, sizeof(libdir), "%s/lib", root);
    if (sh("rm -rf '%s' && mkdir -p '%s'", root, music))
        return 1;
    int ret = make_library();
    if (ret == 77) {
        fprintf(stderr, "test_loopback: no ffmpeg CLI with libmp3lame and libopus; skipped\n");
        return 77;
    }

    uint16_t pa, ps;
    CHECK_EQ(free_ports(&pa, &ps), 0);

    CBLinkOptions so;
    cb_link_options_default(&so);
    /* The wait for a player, which cyanbox can do as rekordbox does (the
     * service's default is to join at once: test_link). */
    so.start_up = false;
    so.library = libdir;
    so.interface = "lo";
    so.broadcast = "127.0.0.2";
    so.ports = (CBLinkPorts) { 0 };
    so.peer_ports = (CBLinkPorts) { .announce = pa, .status = ps };
    /* Every reading of the library after the first 1.5 s slower: what
     * players ask meanwhile is answered all the same. */
    so.test_build_ms = 1500;
    CBLink *link = NULL;
    CHECK_EQ(cb_link_start(&link, &so), 0);
    if (!link)
        return test_result("test_loopback");
    CBLinkPorts sp;
    cb_link_bound_ports(link, &sp);
    /* Waiting, nothing is announced into an empty network. */
    usleep(300000);
    CHECK_EQ(cb_link_state(link, NULL), CB_LINK_WAITING);

    CBLinkClientOptions co;
    cb_link_client_options_default(&co);
    co.interface = "lo";
    co.address = "127.0.0.2";
    co.broadcast = "127.0.0.1";
    co.number = 2;
    co.ports = (CBLinkPorts) { .announce = pa, .status = ps };
    co.server_ports = sp;
    CBLinkClient *player = NULL;
    CHECK_EQ(cb_link_client_start(&player, &co), 0);
    if (!player) {
        cb_link_stop(&link);
        return test_result("test_loopback");
    }

    /* The server joins once it hears the player, as rekordbox does: 17. */
    CHECK_EQ(wait_up(link, 15000), 17);
    uint32_t addr = 0;
    int number = 0;
    CHECK_EQ(cb_link_client_wait_source(player, 15000, &addr, &number), 0);
    CHECK_EQ(addr, 0x7F000001);
    CHECK_EQ(number, 17);
    CBLinkPlayer seen[8];
    int nb = cb_link_players(link, seen, 8);
    bool listed = false;
    for (int i = 0; i < nb && i < 8; i++)
        listed |= seen[i].number == 2 && seen[i].addr == 0x7F000002 &&
                  seen[i].type == CB_LINK_DEVICE_PLAYER;
    CHECK(listed);

    /* A LOAD of My Settings from the server: the library sets none, so
     * cyanbox's defaults, Eject/Load Lock on (byte 21), single play
     * (23), +-6 % (36), and MYSETTING2's, Touch (byte 0)
     * (docs/settings.md sections 3 and 4), with the version word that has
     * a CDJ-3000X read MYSETTING2 too. */
    uint8_t ms[40] = { 0 }, ms2[40] = { 0 };
    CHECK_EQ(cb_link_client_load_settings(player, 5000, ms, ms2), 0);
    CHECK(ms[0] == 0x78 && ms[3] == 0x12 && ms[4] == 3 && ms[21] == 0x81 && ms[23] == 0x81 &&
          ms[36] == 0x80 && ms2[0] == 0x81);
    /* And as a DJM-A9 LOADs its own: rekordbox's mixer defaults, talk over
     * at -18 dB (byte 19; section 5). */
    uint8_t djm[52] = { 0 };
    CHECK_EQ(cb_link_client_load_djm_settings(player, 5000, djm), 0);
    CHECK(djm[0] == 0x78 && djm[8] == 0x20 && djm[12] == 0x81 && djm[19] == 0x81);
    /* Pushed unasked, the settings given (as the interface's are when
     * pushed), not the library's the service read: Continue, Release. */
    CBKv push_player[2] = { { "play_mode", "continue" }, { "vinyl_speed_adjust", "release" } };
    CBConfig push_cfg = { .player = push_player, .nb_player = 2 };
    int pushed_players = 0, pushed_mixers = 0;
    CHECK_EQ(cb_link_push_settings(link, &push_cfg, &pushed_players, &pushed_mixers), 0);
    CHECK(pushed_players == 1 && pushed_mixers == 0);
    memset(ms, 0, sizeof(ms));
    memset(ms2, 0, sizeof(ms2));
    CHECK_EQ(cb_link_client_wait_pushed_settings(player, 5000, ms, ms2), 0);
    CHECK(ms[0] == 0x78 && ms[4] == 3 && ms[21] == 0x81 && ms[23] == 0x80 && ms2[0] == 0x82);
    CBKv wrong[1] = { { "play_mode", "sometimes" } };
    push_cfg.player = wrong;
    push_cfg.nb_player = 1;
    CHECK_EQ(cb_link_push_settings(link, &push_cfg, &pushed_players, &pushed_mixers),
             CB_ERROR(EINVAL));

    /* Browse and load the first track. */
    CHECK_EQ(cb_link_client_browse(player, 0, quiet_line, NULL), 0);

    /* Every track's file over NFS, as its track info names it. */
    int err = 0;
    CBDbClient *db = cb_link_client_db(player, &err);
    CHECK(db);
    CBDbRow *rows = NULL;
    int nb_rows = 0;
    if (db)
        CHECK_EQ(cb_dbclient_menu(db, CB_DB_TRACKS, 1, (const uint32_t[]) { 0 }, 1, &rows,
                                  &nb_rows), 0);
    CHECK_EQ(nb_rows, (int)CB_ARRAY_ELEMS(tracks));
    for (int i = 0; i < nb_rows; i++) {
        char *path = NULL;
        uint64_t size = 0;
        CHECK_EQ(cb_dbclient_track_info(db, rows[i].id, &path, &size), 0);
        CHECK(path && cb_str_starts(path, "/Contents/"));
        uint8_t *data = NULL;
        size_t got = 0;
        if (path)
            CHECK_EQ(cb_link_client_fetch(player, path, &data, &got), 0);
        CHECK_EQ(got, size);
        if (path && !cb_str_ends(path, ".wav"))
            fprintf(stderr, "test_loopback: served as %s\n", path), failures++;
        CHECK(path && data && same_as_decoded(path, data, got));
        free(data);
        free(path);
    }
    uint32_t loaded = nb_rows ? rows[nb_rows - 1].id : 0, other = nb_rows ? rows[0].id : 0;
    cb_dbrows_free(&rows, nb_rows);

    /* What the player says it plays reaches the server. */
    cb_link_client_set_loaded(player, 17, 4, loaded, true);
    bool playing = false;
    for (int k = 0; k < 100 && !playing; k++) {
        usleep(20000);
        nb = cb_link_players(link, seen, 8);
        for (int i = 0; i < nb && i < 8; i++)
            playing |= seen[i].number == 2 && seen[i].has_status && seen[i].playing &&
                       seen[i].track_device == 17 && seen[i].track_id == loaded;
    }
    CHECK(playing);

    /* Told to load another track, the player does (a load command, as
     * rekordbox sends for a track dragged onto a deck), and its status
     * says so; a player not on the link cannot be told. Paused, it stays
     * paused: a load never starts a player. */
    CHECK(other && other != loaded);
    cb_link_client_set_loaded(player, 17, 4, loaded, false);
    CHECK_EQ(cb_link_load(link, 2, other), 0);
    CHECK_EQ(cb_link_load(link, 5, other), CB_ERROR(ENOENT));
    bool switched = false;
    for (int k = 0; k < 100 && !switched; k++) {
        usleep(20000);
        nb = cb_link_players(link, seen, 8);
        for (int i = 0; i < nb && i < 8; i++)
            switched |= seen[i].number == 2 && seen[i].track_device == 17 &&
                        seen[i].track_id == other && seen[i].status_us > 0;
    }
    CHECK(switched);
    /* Its beats from the track's grid: the status names a beat (the
     * start's), still paused. */
    CHECK(cb_link_client_play(player, other) > 0);
    bool placed = false;
    for (int k = 0; k < 100 && !placed; k++) {
        usleep(20000);
        nb = cb_link_players(link, seen, 8);
        for (int i = 0; i < nb && i < 8; i++)
            placed |= seen[i].number == 2 && seen[i].track_id == other && seen[i].beat >= 0 &&
                      !seen[i].playing && seen[i].bpm > 0;
    }
    CHECK(placed);

    /* A memory cue the player saves on the deck: the answer has it, and
     * the service hands it on once, for the library's owner (it never
     * writes the library itself). */
    if (db) {
        CBPlanCue cue = { .time = 0.75, .loop_end = -1, .hot = -1 };
        CBDbMsg r;
        CHECK_EQ(cb_dbclient_save_cue(db, other, false, &cue, false, &r), 0);
        CHECK(r.kind == CB_DB_CUES_EXT_REPLY && cb_dbmsg_num(&r, 1) == 0);
        CBLinkEdit e[4];
        CHECK_EQ(cb_link_edits(link, e, 4), 1);
        CHECK(e[0].track_id == other && e[0].player == 2 && !e[0].remove && e[0].cue.hot < 0 &&
              fabs(e[0].cue.time - 0.75) < 0.001);
        CHECK_EQ(cb_link_edits(link, e, 4), 0);
    }

    /* A host that never joined the link: no database, no files. */
    CBLinkClientOptions qo = co;
    qo.address = "127.0.0.3";
    qo.quiet = true;
    qo.server = "127.0.0.1";
    CBLinkClient *stranger = NULL;
    CHECK_EQ(cb_link_client_start(&stranger, &qo), 0);
    if (stranger) {
        CHECK(!cb_link_client_db(stranger, &err));
        CHECK(err < 0);
        uint8_t *data = NULL;
        size_t got = 0;
        CHECK_EQ(cb_link_client_fetch(stranger, "/Contents", &data, &got), CB_ERROR(ETIMEDOUT));
        free(data);
        cb_link_client_stop(&stranger);
    }

    /* A track added: served again once the server is told. */
    CBLibrary lib = { 0 };
    CHECK_EQ(make_track("delta.flac", "-c:a flac", 122), 0);
    CHECK_EQ(cb_library_open(&lib, libdir), 0);
    add_track(&lib, "delta.flac", 122);
    CHECK(cb_library_setting_set(&lib, "player.play_mode", "continue") >= 0);
    CHECK(cb_library_setting_set(&lib, "player.vinyl_speed_adjust", "release") >= 0);
    CHECK(cb_library_setting_set(&lib, "mixer.talk_over_level", "-12") >= 0);
    cb_library_close(&lib);
    cb_link_library_changed(link);
    /* It is read on a thread of its own: the player is answered at once
     * meanwhile, from the library as it was, and a cue it saves then is
     * served after the swap too, though the owner never saved it. */
    usleep(200000);
    if (db) {
        int64_t t0 = cb_time_us();
        CHECK_EQ(cb_dbclient_menu(db, CB_DB_TRACKS, 1, (const uint32_t[]) { 0 }, 1, &rows,
                                  &nb_rows), 0);
        int64_t took = cb_time_us() - t0;
        if (took > 300000)
            fprintf(stderr, "test_loopback: answered in %lld ms while reading the library\n",
                    (long long)(took / 1000)), failures++;
        CHECK_EQ(nb_rows, 3);
        cb_dbrows_free(&rows, nb_rows);
        CBPlanCue cue = { .time = 0.5, .loop_end = -1, .hot = 1 };
        CBDbMsg r;
        CHECK_EQ(cb_dbclient_save_cue(db, loaded, false, &cue, false, &r), 0);
        CHECK(r.kind == CB_DB_CUES_EXT_REPLY && cb_dbmsg_num(&r, 1) == 0);
    }
    bool four = false;
    for (int k = 0; k < 100 && !four && db; k++) {
        usleep(50000);
        if (cb_dbclient_menu(db, CB_DB_TRACKS, 1, (const uint32_t[]) { 0 }, 1, &rows,
                             &nb_rows) >= 0) {
            four = nb_rows == 4;
            cb_dbrows_free(&rows, nb_rows);
        }
    }
    CHECK(four);
    /* The library's settings with the rest: continue, Release, the lock
     * still on. */
    CHECK_EQ(cb_link_client_load_settings(player, 5000, ms, ms2), 0);
    CHECK(ms[21] == 0x81 && ms[23] == 0x80 && ms[36] == 0x80 && ms2[0] == 0x82);
    CHECK_EQ(cb_link_client_load_djm_settings(player, 5000, djm), 0);
    CHECK(djm[12] == 0x81 && djm[19] == 0x82);
    if (db) {
        uint8_t *blob = NULL;
        size_t bn = 0;
        CBPlanCue *cues = NULL;
        int nc = 0;
        CHECK_EQ(cb_dbclient_blob(db, CB_DB_CUES_EXT, loaded, &blob, &bn), 0);
        CHECK_EQ(cb_db_cues_ext_parse(blob, bn, &cues, &nc), 0);
        bool kept = false;
        for (int i = 0; i < nc; i++)
            kept |= cues[i].hot == 1 && fabs(cues[i].time - 0.5) < 0.001;
        CHECK(kept);
        free(cues);
        free(blob);
    }

    /* Tracks changed through another connection to the library, as the
     * interface changes them: a rating and a memory cue are read again and
     * served at once, well before a whole reading (1.5 s slower here)
     * could be; a new title changes what is planned, so it is the whole
     * library read again, served only once that is done. */
    CBLibrary own = { 0 };
    CHECK_EQ(cb_library_open(&own, libdir), 0);
    CBTrack *t = cb_library_find_id(&own, other);
    CHECK(t);
    if (t && db) {
        t->meta.rating = 4;
        CBCue mc;
        cb_cue_init(&mc, 2.0);
        CHECK(cb_track_add_cue(t, &mc) >= 0);
        CHECK_EQ(cb_library_save_track(&own, t), 0);
        uint64_t *ids = NULL;
        int nids = 0;
        CHECK(cb_library_take_changes(&own, &ids, &nids) == 0 && nids == 1 && ids[0] == other);
        free(ids);
        uint64_t id = other;
        int64_t t0 = cb_time_us();
        cb_link_tracks_changed(link, &id, 1);
        bool rated = false, cued = false;
        while (!(rated && cued) && cb_time_us() - t0 < 400000) {
            usleep(20000);
            if (cb_dbclient_menu(db, CB_DB_RATING_TRACKS, 2, (const uint32_t[]) { 0, 4 }, 2,
                                 &rows, &nb_rows) >= 0) {
                for (int i = 0; i < nb_rows; i++)
                    rated |= rows[i].id == other;
                cb_dbrows_free(&rows, nb_rows);
            }
            uint8_t *blob = NULL;
            size_t bn = 0;
            CBPlanCue *cues = NULL;
            int nc = 0;
            if (cb_dbclient_blob(db, CB_DB_CUES_EXT, other, &blob, &bn) == 0 &&
                cb_db_cues_ext_parse(blob, bn, &cues, &nc) == 0) {
                for (int i = 0; i < nc; i++)
                    cued |= cues[i].hot < 0 && fabs(cues[i].time - 2.0) < 0.001;
            }
            free(cues);
            free(blob);
        }
        CHECK(rated && cued);

        cb_meta_set(&t->meta.title, "Renamed");
        CHECK_EQ(cb_library_save_track(&own, t), 0);
        CHECK(cb_library_take_changes(&own, &ids, &nids) == 0 && nids == 1);
        free(ids);
        cb_link_tracks_changed(link, &id, 1);
        usleep(300000);
        CBDbRow *meta = NULL;
        int nm = 0;
        CHECK_EQ(cb_dbclient_metadata(db, other, &meta, &nm), 0);
        CHECK(nm > 0 && meta[0].text && strcmp(meta[0].text, "Renamed"));
        cb_dbrows_free(&meta, nm);
        bool renamed = false;
        for (int k = 0; k < 100 && !renamed; k++) {
            usleep(50000);
            if (cb_dbclient_metadata(db, other, &meta, &nm) == 0) {
                renamed = nm > 0 && meta[0].text && !strcmp(meta[0].text, "Renamed");
                cb_dbrows_free(&meta, nm);
            }
        }
        CHECK(renamed);
    }
    cb_library_close(&own);

    /* Nothing was made on disk for it. */
    char *cache = cb_asprintf("%s/cache/transcode", libdir);
    CHECK(cache && !cb_file_exists(cache));
    free(cache);

    cb_link_client_stop(&player);
    cb_link_stop(&link);
    sh("rm -rf '%s'", root);
    return test_result("test_loopback");
}
