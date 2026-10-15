/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* Link end to end on the loopback: rxbox's server (rx_link_start) and the
 * fake player (rx_link_client_start) on real sockets, each with ports of
 * its own so the two fit on one host and nothing competes with a running
 * rxbox or rekordbox for 50000. The server is 127.0.0.1 and "broadcasts"
 * to the player's 127.0.0.2, the player the other way round.
 *
 *  - The player announces itself; the server, which waits for a player,
 *    joins, takes 17 and lists the player.
 *  - The player browses as a CDJ-3000 does (rx_link_client_browse) and
 *    reads every track over NFS, at the size the track info gave: each a
 *    16-bit WAV of the file made as it is read (src/link/wav.c), the
 *    file's samples as rx_decode has them, and nothing made on disk.
 *  - The server sees what the player loaded and that it plays; a cue
 *    the player saves is handed on for the library's owner.
 *  - A client that never announced itself (127.0.0.3) gets no database
 *    port and no NFS answer: the fences of docs/LINK.md, "Security".
 *  - A library change is served again: a new track is browsable.
 *
 * Tracks are made by the ffmpeg CLI (skips, 77, without it, or without
 * libopus). The protocol details are the codecs' tests' (test_packets,
 * test_db, test_nfs); this one is about the parts working together. */

#include <math.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>

#include "rxbox/analysis.h"
#include "rxbox/audio.h"
#include "rxbox/link.h"

#include "src/link/client.h"
#include "src/link/dbserver.h"
#include "src/link/net.h"
#include "tests/formats/test.h"

static char root[512], music[700], libdir[700];

static int sh(const char *fmt, ...) RX_PRINTF(1, 2);
static int sh(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char *cmd = rx_vasprintf(fmt, ap);
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
static int add_track(RXLibrary *lib, const char *file, double bpm)
{
    char *path = rx_asprintf("%s/%s", music, file);
    int err = 0;
    RXTrack *t = path ? rx_library_add_file(lib, path, &err) : NULL;
    free(path);
    CHECK(t);
    if (!t)
        return -1;
    CHECK_EQ(rx_grid_init(&t->grid, 0.1, bpm), 0);
    CHECK_EQ(rx_library_save_track(lib, t), 0);
    RXAudio a;
    RXWaveforms w;
    CHECK_EQ(rx_decode(t->path, 1, 0, &a), 0);
    CHECK_EQ(rx_waveforms_compute(a.samples, a.nb_frames, a.rate, &w), 0);
    CHECK_EQ(rx_library_waveforms_save(lib, t->id, &w), 0);
    rx_waveforms_free(&w);
    rx_audio_free(&a);
    return 0;
}

static int make_library(void)
{
    for (size_t i = 0; i < RX_ARRAY_ELEMS(tracks); i++) {
        if (make_track(tracks[i].file, tracks[i].codec, tracks[i].bpm))
            return 77;
    }
    RXLibrary lib = { 0 };
    CHECK_EQ(rx_library_open(&lib, libdir), 0);
    for (size_t i = 0; i < RX_ARRAY_ELEMS(tracks); i++)
        add_track(&lib, tracks[i].file, tracks[i].bpm);
    RXPlaylist *p = rx_library_playlist_create(&lib, lib.root_folder, "Loop");
    CHECK(p);
    for (int i = 0; p && i < lib.nb_tracks; i++)
        rx_library_playlist_add(&lib, p, lib.tracks[i], -1);
    rx_library_close(&lib);
    return 0;
}

/* Two free UDP ports on 127.0.0.2 for the player, which the server is
 * told before the player binds them. */
static int free_ports(uint16_t *a, uint16_t *b)
{
    RXLinkIf lo;
    int ret = rx_link_if_find(&lo, "lo", "127.0.0.2", NULL);
    if (ret < 0)
        return ret;
    int fa = rx_link_udp(&lo, lo.addr, 0, a), fb = rx_link_udp(&lo, lo.addr, 0, b);
    if (fa >= 0)
        close(fa);
    if (fb >= 0)
        close(fb);
    return fa < 0 ? fa : fb < 0 ? fb : 0;
}

static void quiet_line(void *opaque, const char *line)
{
    if (getenv("RX_TEST_VERBOSE"))
        fprintf(stderr, "  %s\n", line);
}

/* The bytes of a track's WAV: the 44-byte header of 16-bit stereo, then
 * the samples of its file (by the name in its path) as rx_decode has them,
 * mono doubled, within dither and rounding. */
static bool same_as_decoded(const char *usb_path, const uint8_t *data, size_t size)
{
    const char *base = NULL;
    for (size_t i = 0; i < RX_ARRAY_ELEMS(tracks) && !base; i++) {
        char stem[32];
        snprintf(stem, sizeof(stem), "%.*s", (int)strcspn(tracks[i].file, "."), tracks[i].file);
        if (strstr(usb_path, stem))
            base = tracks[i].file;
    }
    char *path = base ? rx_asprintf("%s/%s", music, base) : NULL;
    RXAudio d = { 0 };
    bool ok = path && rx_decode(path, 0, 0, &d) >= 0 && size >= 44 && !memcmp(data, "RIFF", 4) &&
              !memcmp(data + 8, "WAVEfmt ", 8) && !memcmp(data + 36, "data", 4);
    int64_t frames = ok ? RX_MIN((int64_t)(size - 44) / 4, d.nb_frames) : 0;
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
    rx_audio_free(&d);
    free(path);
    return ok;
}

static int wait_up(RXLink *link, int timeout_ms)
{
    for (int i = 0; i < timeout_ms / 20; i++) {
        int number = 0;
        if (rx_link_state(link, &number) == RX_LINK_UP)
            return number;
        usleep(20000);
    }
    return -1;
}

int main(void)
{
    const char *dir = getenv("RX_TEST_DIR");
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

    RXLinkOptions so;
    rx_link_options_default(&so);
    /* The wait for a player, which rxbox can do as rekordbox does (the
     * service's default is to join at once: test_link). */
    so.start_up = false;
    so.library = libdir;
    so.interface = "lo";
    so.broadcast = "127.0.0.2";
    so.ports = (RXLinkPorts) { 0 };
    so.peer_ports = (RXLinkPorts) { .announce = pa, .status = ps };
    RXLink *link = NULL;
    CHECK_EQ(rx_link_start(&link, &so), 0);
    if (!link)
        return test_result("test_loopback");
    RXLinkPorts sp;
    rx_link_bound_ports(link, &sp);
    /* Waiting, nothing is announced into an empty network. */
    usleep(300000);
    CHECK_EQ(rx_link_state(link, NULL), RX_LINK_WAITING);

    RXLinkClientOptions co;
    rx_link_client_options_default(&co);
    co.interface = "lo";
    co.address = "127.0.0.2";
    co.broadcast = "127.0.0.1";
    co.number = 2;
    co.ports = (RXLinkPorts) { .announce = pa, .status = ps };
    co.server_ports = sp;
    RXLinkClient *player = NULL;
    CHECK_EQ(rx_link_client_start(&player, &co), 0);
    if (!player) {
        rx_link_stop(&link);
        return test_result("test_loopback");
    }

    /* The server joins once it hears the player, as rekordbox does: 17. */
    CHECK_EQ(wait_up(link, 15000), 17);
    uint32_t addr = 0;
    int number = 0;
    CHECK_EQ(rx_link_client_wait_source(player, 15000, &addr, &number), 0);
    CHECK_EQ(addr, 0x7F000001);
    CHECK_EQ(number, 17);
    RXLinkPlayer seen[8];
    int nb = rx_link_players(link, seen, 8);
    bool listed = false;
    for (int i = 0; i < nb && i < 8; i++)
        listed |= seen[i].number == 2 && seen[i].addr == 0x7F000002 &&
                  seen[i].type == RX_LINK_DEVICE_PLAYER;
    CHECK(listed);

    /* Browse and load the first track. */
    CHECK_EQ(rx_link_client_browse(player, 0, quiet_line, NULL), 0);

    /* Every track's file over NFS, as its track info names it. */
    int err = 0;
    RXDbClient *db = rx_link_client_db(player, &err);
    CHECK(db);
    RXDbRow *rows = NULL;
    int nb_rows = 0;
    if (db)
        CHECK_EQ(rx_dbclient_menu(db, RX_DB_TRACKS, 1, (const uint32_t[]) { 0 }, 1, &rows,
                                  &nb_rows), 0);
    CHECK_EQ(nb_rows, (int)RX_ARRAY_ELEMS(tracks));
    for (int i = 0; i < nb_rows; i++) {
        char *path = NULL;
        uint64_t size = 0;
        CHECK_EQ(rx_dbclient_track_info(db, rows[i].id, &path, &size), 0);
        CHECK(path && rx_str_starts(path, "/Contents/"));
        uint8_t *data = NULL;
        size_t got = 0;
        if (path)
            CHECK_EQ(rx_link_client_fetch(player, path, &data, &got), 0);
        CHECK_EQ(got, size);
        if (path && !rx_str_ends(path, ".wav"))
            fprintf(stderr, "test_loopback: served as %s\n", path), failures++;
        CHECK(path && data && same_as_decoded(path, data, got));
        free(data);
        free(path);
    }
    uint32_t loaded = nb_rows ? rows[nb_rows - 1].id : 0, other = nb_rows ? rows[0].id : 0;
    rx_dbrows_free(&rows, nb_rows);

    /* What the player says it plays reaches the server. */
    rx_link_client_set_loaded(player, 17, 4, loaded, true);
    bool playing = false;
    for (int k = 0; k < 100 && !playing; k++) {
        usleep(20000);
        nb = rx_link_players(link, seen, 8);
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
    rx_link_client_set_loaded(player, 17, 4, loaded, false);
    CHECK_EQ(rx_link_load(link, 2, other), 0);
    CHECK_EQ(rx_link_load(link, 5, other), RX_ERROR(ENOENT));
    bool switched = false;
    for (int k = 0; k < 100 && !switched; k++) {
        usleep(20000);
        nb = rx_link_players(link, seen, 8);
        for (int i = 0; i < nb && i < 8; i++)
            switched |= seen[i].number == 2 && seen[i].track_device == 17 &&
                        seen[i].track_id == other && seen[i].status_us > 0;
    }
    CHECK(switched);
    /* Its beats from the track's grid: the status names a beat (the
     * start's), still paused. */
    CHECK(rx_link_client_play(player, other) > 0);
    bool placed = false;
    for (int k = 0; k < 100 && !placed; k++) {
        usleep(20000);
        nb = rx_link_players(link, seen, 8);
        for (int i = 0; i < nb && i < 8; i++)
            placed |= seen[i].number == 2 && seen[i].track_id == other && seen[i].beat >= 0 &&
                      !seen[i].playing && seen[i].bpm > 0;
    }
    CHECK(placed);

    /* A memory cue the player saves on the deck: the answer has it, and
     * the service hands it on once, for the library's owner (it never
     * writes the library itself). */
    if (db) {
        RXPlanCue cue = { .time = 0.75, .loop_end = -1, .hot = -1 };
        RXDbMsg r;
        CHECK_EQ(rx_dbclient_save_cue(db, other, false, &cue, false, &r), 0);
        CHECK(r.kind == RX_DB_CUES_EXT_REPLY && rx_dbmsg_num(&r, 1) == 0);
        RXLinkCueEdit e[4];
        CHECK_EQ(rx_link_cue_edits(link, e, 4), 1);
        CHECK(e[0].track_id == other && e[0].player == 2 && !e[0].remove && e[0].cue.hot < 0 &&
              fabs(e[0].cue.time - 0.75) < 0.001);
        CHECK_EQ(rx_link_cue_edits(link, e, 4), 0);
    }

    /* A host that never joined the link: no database, no files. */
    RXLinkClientOptions qo = co;
    qo.address = "127.0.0.3";
    qo.quiet = true;
    qo.server = "127.0.0.1";
    RXLinkClient *stranger = NULL;
    CHECK_EQ(rx_link_client_start(&stranger, &qo), 0);
    if (stranger) {
        CHECK(!rx_link_client_db(stranger, &err));
        CHECK(err < 0);
        uint8_t *data = NULL;
        size_t got = 0;
        CHECK_EQ(rx_link_client_fetch(stranger, "/Contents", &data, &got), RX_ERROR(ETIMEDOUT));
        free(data);
        rx_link_client_stop(&stranger);
    }

    /* A track added: served again once the server is told. */
    RXLibrary lib = { 0 };
    CHECK_EQ(make_track("delta.flac", "-c:a flac", 122), 0);
    CHECK_EQ(rx_library_open(&lib, libdir), 0);
    add_track(&lib, "delta.flac", 122);
    rx_library_close(&lib);
    rx_link_library_changed(link);
    bool four = false;
    for (int k = 0; k < 100 && !four && db; k++) {
        usleep(50000);
        if (rx_dbclient_menu(db, RX_DB_TRACKS, 1, (const uint32_t[]) { 0 }, 1, &rows,
                             &nb_rows) >= 0) {
            four = nb_rows == 4;
            rx_dbrows_free(&rows, nb_rows);
        }
    }
    CHECK(four);

    /* Nothing was made on disk for it. */
    char *cache = rx_asprintf("%s/cache/transcode", libdir);
    CHECK(cache && !rx_file_exists(cache));
    free(cache);

    rx_link_client_stop(&player);
    rx_link_stop(&link);
    sh("rm -rf '%s'", root);
    return test_result("test_loopback");
}
