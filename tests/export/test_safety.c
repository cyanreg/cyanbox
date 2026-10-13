/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* What the stick writer must never do, and what the sync must never
 * believe, on staging "sticks" under the build directory:
 *
 *  - prune a file the library still needs (a library kept on the stick);
 *  - write or delete through a symbolic link out of the stick;
 *  - start copying when a database cannot be built (a name too long for
 *    export.pdb is found before anything is written);
 *  - leave a sync record describing databases other than those written;
 *  - take a stick another library wrote as this library's (reused ids);
 *  - miss a player's cues because export.pdb is damaged;
 *  - read outside the stick through an analysis path in export.pdb;
 *  - propose a setting value the library cannot store;
 *  - write anything in a dry run (no transcode, no cache), which instead
 *    lists every file it would write;
 *  - and the free-space estimate counts what is really written.
 * Also: progress counts from 1, every track is a "prepare" item, the
 * transcodes run before the plan, and both databases date a track by when
 * it was added to the library.
 *
 * The audio is WAV written here, so no ffmpeg CLI is needed. */

#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "rxbox/export.h"

#include "src/export/export_priv.h"
#include "src/formats/buf.h"
#include "src/formats/formats_priv.h"
#include "tests/formats/test.h"

static char root[512];

typedef struct Stages {
    int copy, check, prune;
} Stages;

static bool on_progress(void *opaque, const char *stage, int done, int total, const char *item)
{
    (void)done;
    (void)total;
    Stages *s = opaque;
    if (s && !strcmp(stage, "copy"))
        s->copy++;
    if (s && !strcmp(stage, "check") && item && !rx_str_starts(item, "warning"))
        s->check++;
    if (s && !strcmp(stage, "prune") && item && item[0] == '/')
        s->prune++;
    return true;
}

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

static char *at(const char *fmt, ...) RX_PRINTF(1, 2);
static char *at(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char *rel = rx_vasprintf(fmt, ap);
    va_end(ap);
    char *p = rel ? rx_asprintf("%s/%s", root, rel) : NULL;
    free(rel);
    return p;
}

/* Two seconds of a stereo tone: 16-bit PCM, or A-law (WAV format 6, which
 * no player decodes, so the export transcodes it). */
static int write_wav_as(const char *path, double freq, bool alaw)
{
    int rate = 44100, frames = 2 * rate, bytes = alaw ? 1 : 2;
    RXBuf b = { 0 };
    rx_buf_put(&b, "RIFF", 4);
    rx_buf_le32(&b, (uint32_t)(36 + frames * 2 * bytes));
    rx_buf_put(&b, "WAVEfmt ", 8);
    rx_buf_le32(&b, 16);
    rx_buf_le16(&b, alaw ? 6 : 1);
    rx_buf_le16(&b, 2);
    rx_buf_le32(&b, (uint32_t)rate);
    rx_buf_le32(&b, (uint32_t)(rate * 2 * bytes));
    rx_buf_le16(&b, (unsigned)(2 * bytes));
    rx_buf_le16(&b, (unsigned)(8 * bytes));
    rx_buf_put(&b, "data", 4);
    rx_buf_le32(&b, (uint32_t)(frames * 2 * bytes));
    for (int i = 0; i < frames; i++) {
        int v = (int)(8000 * sin(2 * 3.141592653589793 * freq * i / rate));
        if (alaw) {
            /* Not real A-law, a byte per sample is all the decoder needs. */
            rx_buf_u8(&b, (unsigned)(0x55 ^ (v >> 8 & 0xFF)));
            rx_buf_u8(&b, (unsigned)(0x55 ^ (v >> 8 & 0xFF)));
        } else {
            rx_buf_le16(&b, (unsigned)(uint16_t)v);
            rx_buf_le16(&b, (unsigned)(uint16_t)v);
        }
    }
    char *dir = rx_strdup(path);
    *strrchr(dir, '/') = '\0';
    rx_mkdir_p(dir);
    free(dir);
    int ret = b.err ? b.err : rx_file_write_atomic(path, b.data, b.len);
    rx_buf_free(&b);
    return ret;
}

static int write_wav(const char *path, double freq)
{
    return write_wav_as(path, freq, false);
}

static RXTrack *add_track(RXLibrary *lib, const char *path, const char *artist, double hot_at)
{
    if (!rx_file_exists(path) && write_wav(path, 440.0) < 0)
        return NULL;
    int err = 0;
    RXTrack *t = rx_library_add_file(lib, path, &err);
    CHECK(t);
    if (!t)
        return NULL;
    rx_grid_init(&t->grid, 0.0, 120.0);
    rx_meta_set(&t->meta.artist, artist);
    rx_meta_set(&t->meta.album, "Album");
    if (hot_at >= 0) {
        RXCue c;
        rx_cue_init(&c, hot_at);
        c.hot = 0;
        rx_track_add_cue(t, &c);
    }
    CHECK_EQ(rx_library_save_track(lib, t), 0);
    return t;
}

static int export_to(RXLibrary *lib, const char *stick, bool prune, Stages *st)
{
    RXExportOptions opt;
    rx_export_options_default(&opt);
    opt.whole_library = true;
    opt.prune = prune;
    return rx_export(lib, stick, &opt, on_progress, st);
}

static bool exists(const char *path)
{
    struct stat s;
    return lstat(path, &s) == 0;
}

static int count_kind(const RXLibrary *lib, const char *stick, enum RXSyncKind kind, int *total)
{
    RXStickState s;
    if (rx_stick_read(lib, stick, &s) < 0)
        return -1;
    RXSyncItem *items;
    int nb, n = 0;
    if (rx_stick_diff(lib, &s, &items, &nb) < 0) {
        rx_stick_state_free(&s);
        return -1;
    }
    for (int i = 0; i < nb; i++)
        n += items[i].kind == kind;
    if (total)
        *total = nb;
    free(items);
    rx_stick_state_free(&s);
    return n;
}

/* A player at a gig: a memory cue at `time` on track `t` (or, with time
 * < 0, every cue cleared), written into the stick's ANLZ files as
 * rekordbox writes them. */
static void player_adds_cue(RXLibrary *lib, RXTrack *t, const char *stick, double time)
{
    RXTrack *tr[1] = { t };
    RXPlan plan;
    CHECK_EQ(rx_plan_build(lib, tr, 1, NULL, 0, NULL, false, &plan), 0);
    if (plan.nb_tracks != 1)
        return;
    RXPlanTrack *pt = &plan.tracks[0];
    RXPlanCue *cues = calloc((size_t)pt->nb_cues + 1, sizeof(*cues));
    memcpy(cues, pt->cues, (size_t)pt->nb_cues * sizeof(*cues));
    cues[pt->nb_cues] = (RXPlanCue) { .time = time, .hot = -1, .loop_end = -1 };
    free(pt->cues);
    pt->cues = cues;
    pt->nb_cues = time < 0 ? 0 : pt->nb_cues + 1;
    RXAnlzFiles f;
    CHECK_EQ(rx_anlz_build(pt, NULL, &f), 0);
    const char *ext[2] = { ".DAT", ".EXT" };
    const uint8_t *data[2] = { f.dat, f.ext };
    size_t size[2] = { f.dat_size, f.ext_size };
    for (int i = 0; i < 2; i++) {
        char *rel = rx_plan_anlz_path(pt, ext[i]);
        char *p = rx_stick_file(stick, rel);
        CHECK_EQ(rx_file_write_atomic(p, data[i], size[i]), 0);
        free(rel);
        free(p);
    }
    rx_anlz_files_free(&f);
    rx_plan_free(&plan);
}

/* ----------------------------------------------------------------- tests */

/* The library keeps its audio on the stick, outside the place the plan
 * gives it: prune must not delete the library's own files. */
static void library_on_stick(void)
{
    RXLibrary lib = { 0 };
    char *libdir = at("lib-on-stick"), *stick = at("stick-own");
    char *own = at("stick-own/Contents/My Music/song.wav");
    /* And one exactly where the plan puts it: skipped, not copied onto
     * itself. */
    char *there = at("stick-own/Contents/Other/Album/there.wav");
    CHECK_EQ(rx_library_open(&lib, libdir), 0);
    RXTrack *t = add_track(&lib, own, "Artist", -1);
    add_track(&lib, there, "Other", -1);
    uint8_t *before = NULL, *after = NULL;
    size_t nb = 0, na = 0;
    rx_file_read(there, &before, &nb);
    Stages st = { 0 };
    CHECK_EQ(export_to(&lib, stick, true, &st), 0);
    char *placed = at("stick-own/Contents/Artist/Album/song.wav");
    CHECK(rx_file_exists(placed));
    CHECK(rx_file_exists(own));
    CHECK(t && !t->missing);
    CHECK_EQ(st.prune, 0);
    CHECK_EQ(st.copy, 1);
    CHECK_EQ(rx_file_read(there, &after, &na), 0);
    CHECK(nb && na == nb && !memcmp(before, after, nb));
    free(before);
    free(after);
    free(there);
    rx_library_close(&lib);
    free(placed);
    free(own);
    free(libdir);
    free(stick);
}

/* /Contents a link to somewhere else: nothing is written or pruned
 * through it. */
static void symlinked_contents(void)
{
    RXLibrary lib = { 0 };
    char *libdir = at("lib-link"), *stick = at("stick-link"), *outside = at("outside");
    char *keep = at("outside/keep.wav"), *song = at("music-link/song.wav");
    char *contents = at("stick-link/Contents");
    rx_mkdir_p(stick);
    CHECK_EQ(write_wav(keep, 220.0), 0);
    CHECK_EQ(symlink(outside, contents), 0);
    CHECK_EQ(rx_library_open(&lib, libdir), 0);
    add_track(&lib, song, "Artist", -1);
    Stages st = { 0 };
    CHECK(export_to(&lib, stick, true, &st) < 0);
    CHECK(st.check > 0);
    CHECK_EQ(st.copy, 0);
    CHECK(rx_file_exists(keep));
    char *wrote = at("outside/Artist");
    CHECK(!exists(wrote));
    free(wrote);
    rx_library_close(&lib);
    free(libdir);
    free(stick);
    free(outside);
    free(keep);
    free(song);
    free(contents);
}

/* An artist name no export.pdb row can hold: refused in the checks, before
 * any audio is copied. */
static void name_too_long(void)
{
    RXLibrary lib = { 0 };
    char *libdir = at("lib-long"), *stick = at("stick-long"), *song = at("music-long/song.wav");
    rx_mkdir_p(stick);
    CHECK_EQ(rx_library_open(&lib, libdir), 0);
    char *name = malloc(5001);
    memset(name, 'n', 5000);
    name[5000] = '\0';
    add_track(&lib, song, name, -1);
    free(name);
    Stages st = { 0 };
    CHECK(export_to(&lib, stick, false, &st) < 0);
    CHECK(st.check > 0);
    CHECK_EQ(st.copy, 0);
    char *contents = at("stick-long/Contents");
    CHECK(!exists(contents));
    free(contents);
    rx_library_close(&lib);
    free(libdir);
    free(stick);
    free(song);
}

/* A stick exported by one library and read by another whose ids overlap:
 * the other library's track 1 is not the stick's track 1. */
static void other_library(void)
{
    RXLibrary a = { 0 }, b = { 0 };
    char *dir_a = at("lib-a"), *dir_b = at("lib-b"), *stick = at("stick-ab");
    char *song_a = at("music-a/first.wav"), *song_b = at("music-b/second.wav");
    rx_mkdir_p(stick);
    CHECK_EQ(rx_library_open(&a, dir_a), 0);
    CHECK_EQ(rx_library_open(&b, dir_b), 0);
    RXTrack *ta = add_track(&a, song_a, "Artist", 1.0);
    RXTrack *tb = add_track(&b, song_b, "Artist", 1.5);
    CHECK(ta && tb && ta->id == tb->id);
    CHECK_EQ(export_to(&a, stick, false, NULL), 0);
    if (ta)
        player_adds_cue(&a, ta, stick, 1.75);
    /* Library A sees the player's cue; library B sees nothing of its own. */
    CHECK_EQ(count_kind(&a, stick, RX_SYNC_MEMORY_ADDED, NULL), 1);
    int total = -1;
    CHECK_EQ(count_kind(&b, stick, RX_SYNC_MEMORY_ADDED, &total), 0);
    CHECK_EQ(total, 0);
    RXStickState s;
    CHECK_EQ(rx_stick_read(&b, stick, &s), 0);
    CHECK_EQ(s.nb_track_states, 0);
    CHECK_EQ(s.nb_tracks_unknown, 1);
    rx_stick_state_free(&s);
    rx_library_close(&a);
    rx_library_close(&b);
    free(dir_a);
    free(dir_b);
    free(stick);
    free(song_a);
    free(song_b);
}

/* What the sync reads: a damaged export.pdb still leaves exportLibrary.db
 * to name the tracks, so a player's cue blocks the export; an analysis
 * path in export.pdb that leaves the stick is not followed; a setting
 * value the library cannot store is not proposed. */
static void damaged_stick(void)
{
    RXLibrary lib = { 0 };
    char *libdir = at("lib-dmg"), *stick = at("stick-dmg"), *song = at("music-dmg/song.wav");
    rx_mkdir_p(stick);
    CHECK_EQ(rx_library_open(&lib, libdir), 0);
    RXTrack *t = add_track(&lib, song, "Artist", 0.5);
    CHECK_EQ(export_to(&lib, stick, false, NULL), 0);
    if (!t)
        goto out;

    /* The player clears hot cue A: proposed against our record, never
     * without one (a stick rekordbox wrote, or a lost record). */
    player_adds_cue(&lib, t, stick, -1);
    CHECK_EQ(count_kind(&lib, stick, RX_SYNC_HOT_CUE_CLEARED, NULL), 1);
    char *rec = rx_stick_file(stick, RX_SYNC_PATH), *rec_moved = at("record-moved");
    CHECK_EQ(rename(rec, rec_moved), 0);
    int total = -1;
    CHECK_EQ(count_kind(&lib, stick, RX_SYNC_HOT_CUE_CLEARED, &total), 0);
    CHECK_EQ(total, 0);
    CHECK_EQ(rename(rec_moved, rec), 0);
    free(rec);
    free(rec_moved);
    player_adds_cue(&lib, t, stick, 1.25);
    CHECK_EQ(count_kind(&lib, stick, RX_SYNC_MEMORY_ADDED, NULL), 1);

    char *pdb = rx_stick_file(stick, "/PIONEER/rekordbox/export.pdb");
    uint8_t *b;
    size_t n;
    CHECK_EQ(rx_file_read(pdb, &b, &n), 0);
    CHECK_EQ(rx_file_write_atomic(pdb, b, RX_PDB_PAGE), 0);
    CHECK_EQ(count_kind(&lib, stick, RX_SYNC_MEMORY_ADDED, NULL), 1);
    CHECK_EQ(export_to(&lib, stick, false, NULL), RX_ERROR(EBUSY));
    CHECK_EQ(rx_file_write_atomic(pdb, b, n), 0);
    free(b);

    /* export.pdb naming an analysis file outside the stick, which holds a
     * cue the stick does not; no exportLibrary.db. */
    RXTrack *tr[1] = { t };
    RXPlan plan;
    CHECK_EQ(rx_plan_build(&lib, tr, 1, NULL, 0, NULL, false, &plan), 0);
    if (plan.nb_tracks == 1) {
        RXPlanTrack *pt = &plan.tracks[0];
        free(pt->anlz_dir);
        pt->anlz_dir = rx_strdup("/PIONEER/USBANLZ/../../../elsewhere");
        CHECK_EQ(rx_pdb_build(&plan, &b, &n), 0);
        CHECK_EQ(rx_file_write_atomic(pdb, b, n), 0);
        free(b);
        RXPlanCue *c = realloc(pt->cues, (size_t)(pt->nb_cues + 1) * sizeof(*c));
        pt->cues = c;
        c[pt->nb_cues++] = (RXPlanCue) { .time = 1.6, .hot = -1, .loop_end = -1 };
        RXAnlzFiles f;
        CHECK_EQ(rx_anlz_build(pt, NULL, &f), 0);
        char *dat = at("elsewhere/ANLZ0000.DAT");
        char *dir = at("elsewhere");
        rx_mkdir_p(dir);
        free(dir);
        CHECK_EQ(rx_file_write_atomic(dat, f.dat, f.dat_size), 0);
        free(dat);
        rx_anlz_files_free(&f);
    }
    rx_plan_free(&plan);
    char *db = rx_stick_file(stick, "/PIONEER/rekordbox/exportLibrary.db");
    unlink(db);
    free(db);
    RXStickState s;
    CHECK_EQ(rx_stick_read(&lib, stick, &s), 0);
    bool outside = false;
    for (int i = 0; i < s.nb_track_states; i++)
        for (int k = 0; k < s.tracks[i].nb_cues; k++)
            outside |= fabs(s.tracks[i].cues[k].time - 1.6) < 0.002;
    CHECK(!outside);
    CHECK_EQ(s.nb_track_states, 1);
    rx_stick_state_free(&s);

    /* MYSETTING.DAT with quantize stored as a byte no rekordbox writes,
     * and a real change of the tempo range (from the default ±6 %). */
    RXKv kv[1] = { { "tempo_range", "16" } };
    CHECK_EQ(rx_settings_encode("MYSETTING.DAT", kv, 1, &b, &n), 0);
    b[0x68 + 10] = 0x99;
    char *ms = rx_stick_file(stick, "/PIONEER/MYSETTING.DAT");
    CHECK_EQ(rx_file_write_atomic(ms, b, n), 0);
    free(ms);
    free(b);
    CHECK_EQ(rx_stick_read(&lib, stick, &s), 0);
    RXSyncItem *items;
    int nb;
    CHECK_EQ(rx_stick_diff(&lib, &s, &items, &nb), 0);
    int tempo = 0, quantize = 0;
    for (int i = 0; i < nb; i++) {
        if (items[i].kind != RX_SYNC_SETTING)
            continue;
        tempo += !strcmp(items[i].setting_key, "player.tempo_range");
        quantize += !strcmp(items[i].setting_key, "player.quantize");
    }
    CHECK_EQ(tempo, 1);
    CHECK_EQ(quantize, 0);
    free(items);
    rx_stick_state_free(&s);
    free(pdb);
out:
    rx_library_close(&lib);
    free(libdir);
    free(stick);
    free(song);
}

/* The sync record cannot be replaced (its directory is read-only): the
 * databases are not written either, so the record on the stick never
 * describes other databases than the stick's. */
static void record_not_writable(void)
{
    if (!geteuid())
        return;
    RXLibrary lib = { 0 };
    char *libdir = at("lib-ro"), *stick = at("stick-ro"), *song = at("music-ro/song.wav");
    rx_mkdir_p(stick);
    CHECK_EQ(rx_library_open(&lib, libdir), 0);
    RXTrack *t = add_track(&lib, song, "Artist", -1);
    CHECK_EQ(export_to(&lib, stick, false, NULL), 0);
    /* exportLibrary.db: encrypted with fresh IVs, so any rewrite shows. */
    char *pdb = rx_stick_file(stick, "/PIONEER/rekordbox/exportLibrary.db");
    uint8_t *before = NULL, *after = NULL;
    size_t nb = 0, na = 0;
    CHECK_EQ(rx_file_read(pdb, &before, &nb), 0);
    if (t) {
        RXCue c;
        rx_cue_init(&c, 1.0);
        rx_track_add_cue(t, &c);
        rx_library_save_track(&lib, t);
    }
    char *rxdir = rx_stick_file(stick, "/PIONEER/rxbox");
    chmod(rxdir, 0555);
    CHECK(export_to(&lib, stick, false, NULL) < 0);
    chmod(rxdir, 0755);
    CHECK_EQ(rx_file_read(pdb, &after, &na), 0);
    CHECK(na == nb && !memcmp(before, after, nb));
    free(before);
    free(after);
    free(rxdir);
    free(pdb);
    rx_library_close(&lib);
    free(libdir);
    free(stick);
    free(song);
}

/* Free space: replacing a file needs its new size while the old one
 * still exists (written beside it, then renamed), a smaller file frees
 * nothing in advance, and the analysis files count. */
static void space(void)
{
    char *stick = at("stick-space"), *src = at("music-space/a.wav");
    rx_mkdir_p(stick);
    CHECK_EQ(write_wav(src, 330.0), 0);
    int64_t size = rx_file_size(src);
    RXTrack tr = { .path = src };
    RXPlanTrack pt = {
        .id = 1, .track = &tr, .source = src, .usb_path = "/Contents/A/B/a.wav",
        .anlz_dir = "/PIONEER/USBANLZ/P000/00000000", .file_size = size, .duration = 0,
    };
    RXPlan plan = { .tracks = &pt, .nb_tracks = 1 };
    int64_t empty = rx_export_space_needed(&plan, stick, 1);
    CHECK(empty >= size);
    /* On the stick already, larger (a different file): the new one
     * replaces it in place, so the audio needs nothing more. */
    char *dst = rx_stick_file(stick, pt.usb_path);
    char *d = rx_strdup(dst);
    *strrchr(d, '/') = '\0';
    rx_mkdir_p(d);
    free(d);
    uint8_t *big = calloc(1, (size_t)size * 3);
    CHECK_EQ(rx_file_write_atomic(dst, big, (size_t)size * 3), 0);
    free(big);
    CHECK_EQ(rx_export_space_needed(&plan, stick, 1), empty - size);
    unlink(dst);
    /* Ten minutes of analysis: the waveform details alone are 6 bytes a
     * column, 150 columns a second. */
    pt.duration = 600;
    CHECK(rx_export_space_needed(&plan, stick, 1) - empty >= 6 * 150 * 600);
    /* Clusters: three small files on 32 KiB clusters. */
    pt.duration = 0;
    CHECK(rx_export_space_needed(&plan, stick, 32768) - rx_export_space_needed(&plan, stick, 1) >= 32768);
    free(dst);
    free(stick);
    free(src);
}

/* What a dry run reports, and what a real run's progress says. */
typedef struct Seen {
    int transcode, prepare, rewrite, art, zero;
    int prepare_total;
} Seen;

static bool on_seen(void *opaque, const char *stage, int done, int total, const char *item)
{
    Seen *s = opaque;
    if (item && total > 0 && done < 1)
        s->zero++;
    if (!strcmp(stage, "transcode") && item && total > 0)
        s->transcode++;
    if (!strcmp(stage, "prepare") && item && total > 0) {
        s->prepare++;
        s->prepare_total = total;
    }
    if (!strcmp(stage, "rewrite")) {
        s->rewrite++;
        s->art += item && rx_str_starts(item, "/PIONEER/Artwork/");
    }
    return true;
}

static int entries(const char *dir)
{
    int n = 0;
    DIR *d = opendir(dir);
    struct dirent *e;
    while (d && (e = readdir(d)))
        n += strcmp(e->d_name, ".") && strcmp(e->d_name, "..");
    if (d)
        closedir(d);
    return n;
}

/* A dry run writes nothing: no transcode into the cache, nothing on the
 * stick; it lists the transcode it would make and every file it would
 * write, four artwork files per cover. The real run then transcodes (on
 * the pool), counts from 1 and dates the tracks by when they were added. */
static void dry_run_and_dates(void)
{
    RXLibrary lib = { 0 };
    char *libdir = at("lib-dry"), *stick = at("stick-dry");
    char *pcm = at("music-dry/one.wav"), *alaw = at("music-dry/two.wav"), *cover = at("music-dry/front.jpg");
    rx_mkdir_p(stick);
    CHECK_EQ(rx_library_open(&lib, libdir), 0);
    uint8_t rgba[16 * 16 * 4];
    memset(rgba, 0x80, sizeof(rgba));
    uint8_t *jpeg;
    size_t jn;
    CHECK_EQ(write_wav(pcm, 440.0), 0);
    CHECK_EQ(rx_image_jpeg_square(rgba, 16, 16, 16, 90, &jpeg, &jn), 0);
    CHECK_EQ(rx_file_write_atomic(cover, jpeg, jn), 0);
    free(jpeg);
    CHECK_EQ(write_wav_as(alaw, 330.0, true), 0);
    RXTrack *t1 = add_track(&lib, pcm, "Artist", -1);
    RXTrack *t2 = add_track(&lib, alaw, "Artist", -1);
    CHECK(t2 && !rx_plan_codec_playable(t2->codec));
    /* 2020-01-02 12:00 local time. */
    struct tm tm = { .tm_year = 120, .tm_mon = 0, .tm_mday = 2, .tm_hour = 12, .tm_isdst = -1 };
    if (t1 && t2) {
        t1->added = t2->added = (int64_t)mktime(&tm);
        rx_library_save_track(&lib, t1);
        rx_library_save_track(&lib, t2);
    }

    RXExportOptions opt;
    rx_export_options_default(&opt);
    opt.whole_library = true;
    opt.dry_run = true;
    Seen seen = { 0 };
    CHECK_EQ(rx_export(&lib, stick, &opt, on_seen, &seen), 0);
    char *cache = at("lib-dry/cache");
    CHECK_EQ(entries(cache), 0);
    CHECK_EQ(entries(stick), 0);
    CHECK_EQ(seen.transcode, 1);
    CHECK_EQ(seen.prepare, 2);
    CHECK_EQ(seen.prepare_total, 2);
    CHECK_EQ(seen.art, 4);
    /* 3 ANLZ files a track, 4 artwork files, the two databases, the sync
     * record (no My Settings, no My Tags). */
    CHECK_EQ(seen.rewrite, 3 * 2 + 4 + 2 + 1);
    CHECK_EQ(seen.zero, 0);

    opt.dry_run = false;
    seen = (Seen) { 0 };
    CHECK_EQ(rx_export(&lib, stick, &opt, on_seen, &seen), 0);
    CHECK_EQ(seen.transcode, 1);
    CHECK_EQ(seen.prepare, 2);
    CHECK_EQ(seen.zero, 0);
    char *flac = at("stick-dry/Contents/Artist/Album/two.flac");
    CHECK(rx_file_exists(flac));
    free(flac);
    char *made = at("lib-dry/cache/transcode");
    CHECK_EQ(entries(made), 1);
    free(made);

    char today[16];
    time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);
    strftime(today, sizeof(today), "%Y-%m-%d", &lt);
    char *pdb = rx_stick_file(stick, "/PIONEER/rekordbox/export.pdb");
    uint8_t *b;
    size_t n;
    CHECK_EQ(rx_file_read(pdb, &b, &n), 0);
    RXPdbRawTrack *raw;
    int nr;
    CHECK_EQ(rx_pdb_raw_tracks(b, n, &raw, &nr), 0);
    CHECK_EQ(nr, 2);
    for (int i = 0; i < nr; i++) {
        CHECK_STR(raw[i].strings[RX_PDB_STR_DATE_ADDED], "2020-01-02");
        CHECK_STR(raw[i].strings[RX_PDB_STR_ANALYZE_DATE], today);
    }
    rx_pdb_raw_tracks_free(raw, nr);
    free(b);
    free(pdb);
    char *olib = rx_stick_file(stick, "/PIONEER/rekordbox/exportLibrary.db");
    uint8_t *plain;
    size_t pn;
    CHECK_EQ(rx_file_read(olib, &b, &n), 0);
    CHECK_EQ(rx_sqlcipher_decrypt(b, n, rx_onelibrary_passphrase, NULL, 0, &plain, &pn), 0);
    sqlite3 *db;
    CHECK_EQ(rx_onelibrary_open_plain(plain, pn, &db), 0);
    sqlite3_stmt *st;
    int rows = 0;
    if (sqlite3_prepare_v2(db, "SELECT dateAdded, dateCreated FROM content", -1, &st, NULL) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            CHECK_STR((const char *)sqlite3_column_text(st, 0), "2020-01-02");
            CHECK_STR((const char *)sqlite3_column_text(st, 1), "2020-01-02");
            rows++;
        }
        sqlite3_finalize(st);
    }
    CHECK_EQ(rows, 2);
    if (sqlite3_prepare_v2(db, "SELECT createdDate FROM property", -1, &st, NULL) == SQLITE_OK) {
        CHECK(sqlite3_step(st) == SQLITE_ROW && !strcmp((const char *)sqlite3_column_text(st, 0), today));
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
    free(plain);
    free(b);
    free(olib);

    rx_library_close(&lib);
    free(cache);
    free(libdir);
    free(stick);
    free(pcm);
    free(alaw);
    free(cover);
}

int main(void)
{
    const char *base = getenv("RX_TEST_DIR");
    snprintf(root, sizeof(root), "%s", base ? base : "build-formats/test-safety");
    sh("chmod -R u+w '%s' 2>/dev/null; rm -rf '%s'", root, root);
    rx_mkdir_p(root);

    library_on_stick();
    symlinked_contents();
    name_too_long();
    other_library();
    damaged_stick();
    record_not_writable();
    space();
    dry_run_and_dates();

    if (!failures)
        sh("rm -rf '%s'", root);
    return test_result("safety");
}
