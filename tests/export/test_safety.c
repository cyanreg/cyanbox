/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* What the stick writer must never do, and what the sync must never
 * believe, on staging "sticks" under the build directory:
 *
 *  - prune a file the library still needs (a library kept on the stick);
 *  - write or delete through a symbolic link out of the stick, at any
 *    depth, or through a link as the file itself;
 *  - give two tracks one file when exFAT's case rules make their names one
 *    (Șerban, șerban), or keep a file whose name differs from the plan's
 *    in case alone on a case-sensitive staging directory;
 *  - leave what a cancelled run wrote unsynced, or call a run cancelled
 *    during its last sync cancelled;
 *  - take a directory with no database or settings file for a stick with
 *    nothing to sync;
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
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "cyanbox/export.h"

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
    if (s && !strcmp(stage, "check") && item && !cb_str_starts(item, "warning"))
        s->check++;
    if (s && !strcmp(stage, "prune") && item && item[0] == '/')
        s->prune++;
    return true;
}

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

static char *at(const char *fmt, ...) CB_PRINTF(1, 2);
static char *at(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char *rel = cb_vasprintf(fmt, ap);
    va_end(ap);
    char *p = rel ? cb_asprintf("%s/%s", root, rel) : NULL;
    free(rel);
    return p;
}

/* Two seconds of a stereo tone: 16-bit PCM, or A-law (WAV format 6, which
 * no player decodes, so the export transcodes it). */
static int write_wav_as(const char *path, double freq, bool alaw)
{
    int rate = 44100, frames = 2 * rate, bytes = alaw ? 1 : 2;
    CBBuf b = { 0 };
    cb_buf_put(&b, "RIFF", 4);
    cb_buf_le32(&b, (uint32_t)(36 + frames * 2 * bytes));
    cb_buf_put(&b, "WAVEfmt ", 8);
    cb_buf_le32(&b, 16);
    cb_buf_le16(&b, alaw ? 6 : 1);
    cb_buf_le16(&b, 2);
    cb_buf_le32(&b, (uint32_t)rate);
    cb_buf_le32(&b, (uint32_t)(rate * 2 * bytes));
    cb_buf_le16(&b, (unsigned)(2 * bytes));
    cb_buf_le16(&b, (unsigned)(8 * bytes));
    cb_buf_put(&b, "data", 4);
    cb_buf_le32(&b, (uint32_t)(frames * 2 * bytes));
    for (int i = 0; i < frames; i++) {
        int v = (int)(8000 * sin(2 * 3.141592653589793 * freq * i / rate));
        if (alaw) {
            /* Not real A-law, a byte per sample is all the decoder needs. */
            cb_buf_u8(&b, (unsigned)(0x55 ^ (v >> 8 & 0xFF)));
            cb_buf_u8(&b, (unsigned)(0x55 ^ (v >> 8 & 0xFF)));
        } else {
            cb_buf_le16(&b, (unsigned)(uint16_t)v);
            cb_buf_le16(&b, (unsigned)(uint16_t)v);
        }
    }
    char *dir = cb_strdup(path);
    *strrchr(dir, '/') = '\0';
    cb_mkdir_p(dir);
    free(dir);
    int ret = b.err ? b.err : cb_file_write_atomic(path, b.data, b.len);
    cb_buf_free(&b);
    return ret;
}

static int write_wav(const char *path, double freq)
{
    return write_wav_as(path, freq, false);
}

static CBTrack *add_track(CBLibrary *lib, const char *path, const char *artist, double hot_at)
{
    if (!cb_file_exists(path) && write_wav(path, 440.0) < 0)
        return NULL;
    int err = 0;
    CBTrack *t = cb_library_add_file(lib, path, &err);
    CHECK(t);
    if (!t)
        return NULL;
    cb_grid_init(&t->grid, 0.0, 120.0);
    cb_meta_set(&t->meta.artist, artist);
    cb_meta_set(&t->meta.album, "Album");
    if (hot_at >= 0) {
        CBCue c;
        cb_cue_init(&c, hot_at);
        c.hot = 0;
        cb_track_add_cue(t, &c);
    }
    CHECK_EQ(cb_library_save_track(lib, t), 0);
    return t;
}

static int export_to(CBLibrary *lib, const char *stick, bool prune, Stages *st)
{
    CBExportOptions opt;
    cb_export_options_default(&opt);
    opt.whole_library = true;
    opt.prune = prune;
    return cb_export(lib, stick, &opt, on_progress, st);
}

static bool exists(const char *path)
{
    struct stat s;
    return lstat(path, &s) == 0;
}

static int count_kind(const CBLibrary *lib, const char *stick, enum CBSyncKind kind, int *total)
{
    CBStickState s;
    if (cb_stick_read(lib, stick, &s) < 0)
        return -1;
    CBSyncItem *items;
    int nb, n = 0;
    if (cb_stick_diff(lib, &s, &items, &nb) < 0) {
        cb_stick_state_free(&s);
        return -1;
    }
    for (int i = 0; i < nb; i++)
        n += items[i].kind == kind;
    if (total)
        *total = nb;
    free(items);
    cb_stick_state_free(&s);
    return n;
}

/* A player at a gig: a memory cue at `time` on track `t` (or, with time
 * < 0, every cue cleared), written into the stick's ANLZ files as
 * rekordbox writes them. */
static void player_adds_cue(CBLibrary *lib, CBTrack *t, const char *stick, double time)
{
    CBTrack *tr[1] = { t };
    CBPlan plan;
    CHECK_EQ(cb_plan_build(lib, tr, 1, NULL, 0, NULL, false, &plan), 0);
    if (plan.nb_tracks != 1)
        return;
    CBPlanTrack *pt = &plan.tracks[0];
    CBPlanCue *cues = calloc((size_t)pt->nb_cues + 1, sizeof(*cues));
    memcpy(cues, pt->cues, (size_t)pt->nb_cues * sizeof(*cues));
    cues[pt->nb_cues] = (CBPlanCue) { .time = time, .hot = -1, .loop_end = -1 };
    free(pt->cues);
    pt->cues = cues;
    pt->nb_cues = time < 0 ? 0 : pt->nb_cues + 1;
    CBAnlzFiles f;
    CHECK_EQ(cb_anlz_build(pt, NULL, &f), 0);
    const char *ext[2] = { ".DAT", ".EXT" };
    const uint8_t *data[2] = { f.dat, f.ext };
    size_t size[2] = { f.dat_size, f.ext_size };
    for (int i = 0; i < 2; i++) {
        char *rel = cb_plan_anlz_path(pt, ext[i]);
        char *p = cb_stick_file(stick, rel);
        CHECK_EQ(cb_file_write_atomic(p, data[i], size[i]), 0);
        free(rel);
        free(p);
    }
    cb_anlz_files_free(&f);
    cb_plan_free(&plan);
}

/* ----------------------------------------------------------------- tests */

/* The library keeps its audio on the stick, outside the place the plan
 * gives it: prune must not delete the library's own files. */
static void library_on_stick(void)
{
    CBLibrary lib = { 0 };
    char *libdir = at("lib-on-stick"), *stick = at("stick-own");
    char *own = at("stick-own/Contents/My Music/song.wav");
    /* And one exactly where the plan puts it: skipped, not copied onto
     * itself. */
    char *there = at("stick-own/Contents/Other/Album/there.wav");
    CHECK_EQ(cb_library_open(&lib, libdir), 0);
    CBTrack *t = add_track(&lib, own, "Artist", -1);
    add_track(&lib, there, "Other", -1);
    uint8_t *before = NULL, *after = NULL;
    size_t nb = 0, na = 0;
    cb_file_read(there, &before, &nb);
    Stages st = { 0 };
    CHECK_EQ(export_to(&lib, stick, true, &st), 0);
    char *placed = at("stick-own/Contents/Artist/Album/song.wav");
    CHECK(cb_file_exists(placed));
    CHECK(cb_file_exists(own));
    CHECK(t && !t->missing);
    CHECK_EQ(st.prune, 0);
    CHECK_EQ(st.copy, 1);
    CHECK_EQ(cb_file_read(there, &after, &na), 0);
    CHECK(nb && na == nb && !memcmp(before, after, nb));
    free(before);
    free(after);
    free(there);
    cb_library_close(&lib);
    free(placed);
    free(own);
    free(libdir);
    free(stick);
}

/* /Contents a link to somewhere else: nothing is written or pruned
 * through it. */
static void symlinked_contents(void)
{
    CBLibrary lib = { 0 };
    char *libdir = at("lib-link"), *stick = at("stick-link"), *outside = at("outside");
    char *keep = at("outside/keep.wav"), *song = at("music-link/song.wav");
    char *contents = at("stick-link/Contents");
    cb_mkdir_p(stick);
    CHECK_EQ(write_wav(keep, 220.0), 0);
    CHECK_EQ(symlink(outside, contents), 0);
    CHECK_EQ(cb_library_open(&lib, libdir), 0);
    add_track(&lib, song, "Artist", -1);
    Stages st = { 0 };
    CHECK(export_to(&lib, stick, true, &st) < 0);
    CHECK(st.check > 0);
    CHECK_EQ(st.copy, 0);
    CHECK(cb_file_exists(keep));
    char *wrote = at("outside/Artist");
    CHECK(!exists(wrote));
    free(wrote);
    cb_library_close(&lib);
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
    CBLibrary lib = { 0 };
    char *libdir = at("lib-long"), *stick = at("stick-long"), *song = at("music-long/song.wav");
    cb_mkdir_p(stick);
    CHECK_EQ(cb_library_open(&lib, libdir), 0);
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
    cb_library_close(&lib);
    free(libdir);
    free(stick);
    free(song);
}

/* A stick exported by one library and read by another whose ids overlap:
 * the other library's track 1 is not the stick's track 1. */
static void other_library(void)
{
    CBLibrary a = { 0 }, b = { 0 };
    char *dir_a = at("lib-a"), *dir_b = at("lib-b"), *stick = at("stick-ab");
    char *song_a = at("music-a/first.wav"), *song_b = at("music-b/second.wav");
    cb_mkdir_p(stick);
    CHECK_EQ(cb_library_open(&a, dir_a), 0);
    CHECK_EQ(cb_library_open(&b, dir_b), 0);
    CBTrack *ta = add_track(&a, song_a, "Artist", 1.0);
    CBTrack *tb = add_track(&b, song_b, "Artist", 1.5);
    CHECK(ta && tb && ta->id == tb->id);
    CHECK_EQ(export_to(&a, stick, false, NULL), 0);
    if (ta)
        player_adds_cue(&a, ta, stick, 1.75);
    /* Library A sees the player's cue; library B sees nothing of its own. */
    CHECK_EQ(count_kind(&a, stick, CB_SYNC_MEMORY_ADDED, NULL), 1);
    int total = -1;
    CHECK_EQ(count_kind(&b, stick, CB_SYNC_MEMORY_ADDED, &total), 0);
    CHECK_EQ(total, 0);
    CBStickState s;
    CHECK_EQ(cb_stick_read(&b, stick, &s), 0);
    CHECK_EQ(s.nb_track_states, 0);
    CHECK_EQ(s.nb_tracks_unknown, 1);
    cb_stick_state_free(&s);
    cb_library_close(&a);
    cb_library_close(&b);
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
    CBLibrary lib = { 0 };
    char *libdir = at("lib-dmg"), *stick = at("stick-dmg"), *song = at("music-dmg/song.wav");
    cb_mkdir_p(stick);
    CHECK_EQ(cb_library_open(&lib, libdir), 0);
    CBTrack *t = add_track(&lib, song, "Artist", 0.5);
    CHECK_EQ(export_to(&lib, stick, false, NULL), 0);
    if (!t)
        goto out;

    /* The player clears hot cue A: proposed against our record, never
     * without one (a stick rekordbox wrote, or a lost record). */
    player_adds_cue(&lib, t, stick, -1);
    CHECK_EQ(count_kind(&lib, stick, CB_SYNC_HOT_CUE_CLEARED, NULL), 1);
    char *rec = cb_stick_file(stick, CB_SYNC_PATH), *rec_moved = at("record-moved");
    CHECK_EQ(rename(rec, rec_moved), 0);
    int total = -1;
    CHECK_EQ(count_kind(&lib, stick, CB_SYNC_HOT_CUE_CLEARED, &total), 0);
    CHECK_EQ(total, 0);
    CHECK_EQ(rename(rec_moved, rec), 0);
    free(rec);
    free(rec_moved);
    player_adds_cue(&lib, t, stick, 1.25);
    CHECK_EQ(count_kind(&lib, stick, CB_SYNC_MEMORY_ADDED, NULL), 1);

    char *pdb = cb_stick_file(stick, "/PIONEER/rekordbox/export.pdb");
    uint8_t *b;
    size_t n;
    CHECK_EQ(cb_file_read(pdb, &b, &n), 0);
    CHECK_EQ(cb_file_write_atomic(pdb, b, CB_PDB_PAGE), 0);
    CHECK_EQ(count_kind(&lib, stick, CB_SYNC_MEMORY_ADDED, NULL), 1);
    CHECK_EQ(export_to(&lib, stick, false, NULL), CB_ERROR(EBUSY));
    CHECK_EQ(cb_file_write_atomic(pdb, b, n), 0);
    free(b);

    /* export.pdb naming an analysis file outside the stick, which holds a
     * cue the stick does not; no exportLibrary.db. */
    CBTrack *tr[1] = { t };
    CBPlan plan;
    CHECK_EQ(cb_plan_build(&lib, tr, 1, NULL, 0, NULL, false, &plan), 0);
    if (plan.nb_tracks == 1) {
        CBPlanTrack *pt = &plan.tracks[0];
        free(pt->anlz_dir);
        pt->anlz_dir = cb_strdup("/PIONEER/USBANLZ/../../../elsewhere");
        CHECK_EQ(cb_pdb_build(&plan, &b, &n), 0);
        CHECK_EQ(cb_file_write_atomic(pdb, b, n), 0);
        free(b);
        CBPlanCue *c = realloc(pt->cues, (size_t)(pt->nb_cues + 1) * sizeof(*c));
        pt->cues = c;
        c[pt->nb_cues++] = (CBPlanCue) { .time = 1.6, .hot = -1, .loop_end = -1 };
        CBAnlzFiles f;
        CHECK_EQ(cb_anlz_build(pt, NULL, &f), 0);
        char *dat = at("elsewhere/ANLZ0000.DAT");
        char *dir = at("elsewhere");
        cb_mkdir_p(dir);
        free(dir);
        CHECK_EQ(cb_file_write_atomic(dat, f.dat, f.dat_size), 0);
        free(dat);
        cb_anlz_files_free(&f);
    }
    cb_plan_free(&plan);
    char *db = cb_stick_file(stick, "/PIONEER/rekordbox/exportLibrary.db");
    unlink(db);
    free(db);
    CBStickState s;
    CHECK_EQ(cb_stick_read(&lib, stick, &s), 0);
    bool outside = false;
    for (int i = 0; i < s.nb_track_states; i++)
        for (int k = 0; k < s.tracks[i].nb_cues; k++)
            outside |= fabs(s.tracks[i].cues[k].time - 1.6) < 0.002;
    CHECK(!outside);
    CHECK_EQ(s.nb_track_states, 1);
    cb_stick_state_free(&s);

    /* MYSETTING.DAT with quantize stored as a byte no rekordbox writes,
     * and a real change of the tempo range (from the default ±6 %). */
    CBKv kv[1] = { { "tempo_range", "16" } };
    CHECK_EQ(cb_settings_encode("MYSETTING.DAT", kv, 1, &b, &n), 0);
    b[0x68 + 10] = 0x99;
    char *ms = cb_stick_file(stick, "/PIONEER/MYSETTING.DAT");
    CHECK_EQ(cb_file_write_atomic(ms, b, n), 0);
    free(ms);
    free(b);
    CHECK_EQ(cb_stick_read(&lib, stick, &s), 0);
    CBSyncItem *items;
    int nb;
    CHECK_EQ(cb_stick_diff(&lib, &s, &items, &nb), 0);
    int tempo = 0, quantize = 0;
    for (int i = 0; i < nb; i++) {
        if (items[i].kind != CB_SYNC_SETTING)
            continue;
        tempo += !strcmp(items[i].setting_key, "player.tempo_range");
        quantize += !strcmp(items[i].setting_key, "player.quantize");
    }
    CHECK_EQ(tempo, 1);
    CHECK_EQ(quantize, 0);
    free(items);
    cb_stick_state_free(&s);
    free(pdb);
out:
    cb_library_close(&lib);
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
    CBLibrary lib = { 0 };
    char *libdir = at("lib-ro"), *stick = at("stick-ro"), *song = at("music-ro/song.wav");
    cb_mkdir_p(stick);
    CHECK_EQ(cb_library_open(&lib, libdir), 0);
    CBTrack *t = add_track(&lib, song, "Artist", -1);
    CHECK_EQ(export_to(&lib, stick, false, NULL), 0);
    /* exportLibrary.db: encrypted with fresh IVs, so any rewrite shows. */
    char *pdb = cb_stick_file(stick, "/PIONEER/rekordbox/exportLibrary.db");
    uint8_t *before = NULL, *after = NULL;
    size_t nb = 0, na = 0;
    CHECK_EQ(cb_file_read(pdb, &before, &nb), 0);
    if (t) {
        CBCue c;
        cb_cue_init(&c, 1.0);
        cb_track_add_cue(t, &c);
        cb_library_save_track(&lib, t);
    }
    char *rxdir = cb_stick_file(stick, "/PIONEER/cyanbox");
    chmod(rxdir, 0555);
    CHECK(export_to(&lib, stick, false, NULL) < 0);
    chmod(rxdir, 0755);
    CHECK_EQ(cb_file_read(pdb, &after, &na), 0);
    CHECK(na == nb && !memcmp(before, after, nb));
    free(before);
    free(after);
    free(rxdir);
    free(pdb);
    cb_library_close(&lib);
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
    cb_mkdir_p(stick);
    CHECK_EQ(write_wav(src, 330.0), 0);
    int64_t size = cb_file_size(src);
    CBTrack tr = { .path = src };
    CBPlanTrack pt = {
        .id = 1, .track = &tr, .source = src, .usb_path = "/Contents/A/B/a.wav",
        .anlz_dir = "/PIONEER/USBANLZ/P000/00000000", .file_size = size, .duration = 0,
    };
    CBPlan plan = { .tracks = &pt, .nb_tracks = 1 };
    int64_t empty = cb_export_space_needed(&plan, stick, 1);
    CHECK(empty >= size);
    /* On the stick already, larger (a different file): the new one
     * replaces it in place, so the audio needs nothing more. */
    char *dst = cb_stick_file(stick, pt.usb_path);
    char *d = cb_strdup(dst);
    *strrchr(d, '/') = '\0';
    cb_mkdir_p(d);
    free(d);
    uint8_t *big = calloc(1, (size_t)size * 3);
    CHECK_EQ(cb_file_write_atomic(dst, big, (size_t)size * 3), 0);
    free(big);
    CHECK_EQ(cb_export_space_needed(&plan, stick, 1), empty - size);
    unlink(dst);
    /* Ten minutes of analysis: the waveform details alone are 6 bytes a
     * column, 150 columns a second. */
    pt.duration = 600;
    CHECK(cb_export_space_needed(&plan, stick, 1) - empty >= 6 * 150 * 600);
    /* Clusters: three small files on 32 KiB clusters. */
    pt.duration = 0;
    CHECK(cb_export_space_needed(&plan, stick, 32768) - cb_export_space_needed(&plan, stick, 1) >= 32768);
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
        s->art += item && cb_str_starts(item, "/PIONEER/Artwork/");
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
    CBLibrary lib = { 0 };
    char *libdir = at("lib-dry"), *stick = at("stick-dry");
    char *pcm = at("music-dry/one.wav"), *alaw = at("music-dry/two.wav"), *cover = at("music-dry/front.jpg");
    cb_mkdir_p(stick);
    CHECK_EQ(cb_library_open(&lib, libdir), 0);
    uint8_t rgba[16 * 16 * 4];
    memset(rgba, 0x80, sizeof(rgba));
    uint8_t *jpeg;
    size_t jn;
    CHECK_EQ(write_wav(pcm, 440.0), 0);
    CHECK_EQ(cb_image_jpeg_square(rgba, 16, 16, 16, 90, &jpeg, &jn), 0);
    CHECK_EQ(cb_file_write_atomic(cover, jpeg, jn), 0);
    free(jpeg);
    CHECK_EQ(write_wav_as(alaw, 330.0, true), 0);
    CBTrack *t1 = add_track(&lib, pcm, "Artist", -1);
    CBTrack *t2 = add_track(&lib, alaw, "Artist", -1);
    CHECK(t2 && !cb_plan_codec_playable(t2->codec));
    /* 2020-01-02 12:00 local time. */
    struct tm tm = { .tm_year = 120, .tm_mon = 0, .tm_mday = 2, .tm_hour = 12, .tm_isdst = -1 };
    if (t1 && t2) {
        t1->added = t2->added = (int64_t)mktime(&tm);
        cb_library_save_track(&lib, t1);
        cb_library_save_track(&lib, t2);
    }

    CBExportOptions opt;
    cb_export_options_default(&opt);
    opt.whole_library = true;
    opt.dry_run = true;
    Seen seen = { 0 };
    CHECK_EQ(cb_export(&lib, stick, &opt, on_seen, &seen), 0);
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

    /* The real run encodes the A-law file as it writes it (a "copy"
     * like the other), and keeps nothing of it in the cache. */
    opt.dry_run = false;
    seen = (Seen) { 0 };
    CHECK_EQ(cb_export(&lib, stick, &opt, on_seen, &seen), 0);
    CHECK_EQ(seen.transcode, 0);
    CHECK_EQ(seen.prepare, 2);
    CHECK_EQ(seen.zero, 0);
    char *flac = at("stick-dry/Contents/Artist/Album/two.flac");
    CHECK(cb_file_exists(flac));
    free(flac);
    char *made = at("lib-dry/cache/transcode");
    CHECK_EQ(entries(made), 0);
    free(made);

    char today[16];
    time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);
    strftime(today, sizeof(today), "%Y-%m-%d", &lt);
    char *pdb = cb_stick_file(stick, "/PIONEER/rekordbox/export.pdb");
    uint8_t *b;
    size_t n;
    CHECK_EQ(cb_file_read(pdb, &b, &n), 0);
    CBPdbRawTrack *raw;
    int nr;
    CHECK_EQ(cb_pdb_raw_tracks(b, n, &raw, &nr), 0);
    CHECK_EQ(nr, 2);
    for (int i = 0; i < nr; i++) {
        CHECK_STR(raw[i].strings[CB_PDB_STR_DATE_ADDED], "2020-01-02");
        CHECK_STR(raw[i].strings[CB_PDB_STR_ANALYZE_DATE], today);
    }
    cb_pdb_raw_tracks_free(raw, nr);
    free(b);
    free(pdb);
    char *olib = cb_stick_file(stick, "/PIONEER/rekordbox/exportLibrary.db");
    uint8_t *plain;
    size_t pn;
    CHECK_EQ(cb_file_read(olib, &b, &n), 0);
    CHECK_EQ(cb_sqlcipher_decrypt(b, n, cb_onelibrary_passphrase, NULL, 0, &plain, &pn), 0);
    sqlite3 *db;
    CHECK_EQ(cb_onelibrary_open_plain(plain, pn, &db), 0);
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

    cb_library_close(&lib);
    free(cache);
    free(libdir);
    free(stick);
    free(pcm);
    free(alaw);
    free(cover);
}

/* A link deeper in a staging stick (Contents/Artist, as a staging
 * directory may have): the checks refuse it before anything is written,
 * nothing is written or deleted outside, and the stick's file functions
 * refuse every path through a link, a link as the file itself included.
 * Moved off the plan's paths, the link is left alone: the export runs and
 * prune does not walk through it. */
static void links_inside(void)
{
    CBLibrary lib = { 0 };
    char *libdir = at("lib-deep"), *stick = at("stick-deep"), *outside = at("outside-deep");
    char *song = at("music-deep/song.wav"), *old = at("outside-deep/old.wav");
    char *contents = at("stick-deep/Contents"), *artist = at("stick-deep/Contents/Artist");
    char *aside = at("stick-deep/Contents/Elsewhere"), *file = at("stick-deep/Contents/file.wav");
    char *pdb = at("stick-deep/PIONEER/rekordbox/export.pdb");
    cb_mkdir_p(contents);
    CHECK_EQ(write_wav(old, 220.0), 0);
    int64_t old_size = cb_file_size(old);
    CHECK_EQ(symlink(outside, artist), 0);
    CHECK_EQ(cb_library_open(&lib, libdir), 0);
    add_track(&lib, song, "Artist", -1);

    Stages st = { 0 };
    CHECK(export_to(&lib, stick, true, &st) < 0);
    CHECK(st.check > 0);
    CHECK_EQ(st.copy, 0);
    CHECK_EQ(entries(outside), 1);
    CHECK(!exists(pdb));

    int fd = cb_stick_root(stick);
    CHECK(fd >= 0);
    CHECK_EQ(cb_stick_write(fd, "/Contents/Artist/Album/x.wav", "x", 1), CB_ERROR(ELOOP));
    CHECK_EQ(cb_stick_mkdirs(fd, "/Contents/Artist/Album"), CB_ERROR(ELOOP));
    CHECK_EQ(cb_stick_unlink(fd, "/Contents/Artist/old.wav", 0), CB_ERROR(ELOOP));
    uint8_t *b = NULL;
    size_t n = 0;
    CHECK_EQ(cb_stick_read_file(fd, "/Contents/Artist/old.wav", &b, &n), CB_ERROR(ELOOP));
    free(b);
    CHECK_EQ(symlink(old, file), 0);
    CHECK_EQ(cb_stick_write(fd, "/Contents/file.wav", "x", 1), CB_ERROR(ELOOP));
    CHECK_EQ(cb_stick_open(fd, "/Contents/file.wav", O_WRONLY | O_TRUNC, 0), CB_ERROR(ELOOP));
    close(fd);
    CHECK_EQ(entries(outside), 1);
    CHECK_EQ(cb_file_size(old), old_size);

    CHECK_EQ(rename(artist, aside), 0);
    st = (Stages) { 0 };
    CHECK_EQ(export_to(&lib, stick, true, &st), 0);
    CHECK_EQ(st.copy, 1);
    CHECK_EQ(st.prune, 0);
    CHECK_EQ(entries(outside), 1);
    CHECK_EQ(cb_file_size(old), old_size);
    cb_library_close(&lib);
    free(libdir);
    free(stick);
    free(outside);
    free(song);
    free(old);
    free(contents);
    free(artist);
    free(aside);
    free(file);
    free(pdb);
}

/* Șerban and șerban are one name to exFAT (its up-case table): their
 * tracks get two files on the stick, not one copied over the other. A
 * file whose name differs from the plan's in case alone is, on a
 * case-sensitive staging directory, another file: prune lists and deletes
 * it, looking both names up as it does, and keeps the plan's. */
static void case_names(void)
{
    CBLibrary lib = { 0 };
    char *libdir = at("lib-case"), *stick = at("stick-case");
    char *a = at("music-case/a/song.wav"), *b = at("music-case/b/song.wav");
    char *first = at("stick-case/Contents/Șerban/Album/song.wav");
    char *second = at("stick-case/Contents/șerban/Album/song-1.wav");
    char *stale = at("stick-case/Contents/ȘERBAN/Album/song.wav");
    cb_mkdir_p(stick);
    CHECK_EQ(write_wav(a, 440.0), 0);
    CHECK_EQ(write_wav(b, 550.0), 0);
    CHECK_EQ(cb_library_open(&lib, libdir), 0);
    add_track(&lib, a, "Șerban", -1);
    add_track(&lib, b, "șerban", -1);
    Stages st = { 0 };
    CHECK_EQ(export_to(&lib, stick, false, &st), 0);
    CHECK_EQ(st.copy, 2);
    CHECK(cb_file_exists(first));
    CHECK(cb_file_exists(second));
    CHECK(cb_file_size(first) > 0 && cb_file_size(first) == cb_file_size(a));

    CHECK_EQ(write_wav(stale, 330.0), 0);
    CBExportOptions opt;
    cb_export_options_default(&opt);
    opt.whole_library = true;
    opt.dry_run = true;
    st = (Stages) { 0 };
    CHECK_EQ(cb_export(&lib, stick, &opt, on_progress, &st), 0);
    CHECK_EQ(st.prune, 1);
    st = (Stages) { 0 };
    CHECK_EQ(export_to(&lib, stick, true, &st), 0);
    CHECK_EQ(st.prune, 1);
    CHECK(!exists(stale));
    CHECK(cb_file_exists(first));
    CHECK(cb_file_exists(second));
    cb_library_close(&lib);
    free(libdir);
    free(stick);
    free(a);
    free(b);
    free(first);
    free(second);
    free(stale);
}

/* What a run said, and when to cancel it. */
typedef struct Flow {
    const char *cancel_at;       /* the stage whose first message cancels */
    int flush, done, after_cancel;
    bool cancelled;
} Flow;

static bool on_flow(void *opaque, const char *stage, int done, int total, const char *item)
{
    Flow *f = opaque;
    f->flush += !strcmp(stage, "flush");
    f->done += !strcmp(stage, "done");
    f->after_cancel += f->cancelled;
    if (f->cancel_at && !strcmp(stage, f->cancel_at))
        f->cancelled = true;
    return !f->cancelled;
}

/* A real run cancelled half way still writes out what it wrote (the
 * stick may be pulled next) and says so; one cancelled during that last
 * sync has exported everything and succeeds. A dry run writes nothing and
 * syncs nothing. */
static void cancel_syncs(void)
{
    CBLibrary lib = { 0 };
    char *libdir = at("lib-cancel"), *stick = at("stick-cancel");
    char *a = at("music-cancel/a.wav"), *b = at("music-cancel/b.wav");
    cb_mkdir_p(stick);
    CHECK_EQ(cb_library_open(&lib, libdir), 0);
    add_track(&lib, a, "Artist", -1);
    add_track(&lib, b, "Other", -1);
    CBExportOptions opt;
    cb_export_options_default(&opt);
    opt.whole_library = true;

    Flow f = { .cancel_at = "analysis files" };
    CHECK_EQ(cb_export(&lib, stick, &opt, on_flow, &f), CB_ERROR(ECANCELED));
    CHECK(f.flush >= 1);
    CHECK_EQ(f.done, 0);
    char *pdb = at("stick-cancel/PIONEER/rekordbox/export.pdb");
    CHECK(!exists(pdb));

    f = (Flow) { .cancel_at = "flush" };
    CHECK_EQ(cb_export(&lib, stick, &opt, on_flow, &f), 0);
    CHECK(f.flush >= 1);
    CHECK_EQ(f.done, 1);
    CHECK(exists(pdb));

    opt.dry_run = true;
    f = (Flow) { 0 };
    CHECK_EQ(cb_export(&lib, stick, &opt, on_flow, &f), 0);
    CHECK_EQ(f.flush, 0);
    free(pdb);
    cb_library_close(&lib);
    free(libdir);
    free(stick);
    free(a);
    free(b);
}

/* A directory with neither database nor settings file is no stick: the
 * sync says so instead of finding nothing to sync (a mistyped mount
 * point). One with only My Settings (saved on a player onto a blank
 * stick) is read; the export writes to an empty one as to a new stick. */
static void not_a_stick(void)
{
    CBLibrary lib = { 0 };
    char *libdir = at("lib-none"), *dir = at("not-a-stick"), *song = at("music-none/song.wav");
    char *ms = at("not-a-stick/PIONEER/MYSETTING.DAT");
    cb_mkdir_p(dir);
    CHECK_EQ(cb_library_open(&lib, libdir), 0);
    add_track(&lib, song, "Artist", -1);
    CBStickState s;
    CHECK_EQ(cb_stick_read(&lib, dir, &s), CB_ERROR(ENOENT));
    CHECK_EQ(cb_pull(&lib, dir, NULL, NULL), CB_ERROR(ENOENT));

    CBKv kv[1] = { { "tempo_range", "16" } };
    uint8_t *b;
    size_t n;
    CHECK_EQ(cb_settings_encode("MYSETTING.DAT", kv, 1, &b, &n), 0);
    char *pio = at("not-a-stick/PIONEER");
    cb_mkdir_p(pio);
    free(pio);
    CHECK_EQ(cb_file_write_atomic(ms, b, n), 0);
    free(b);
    CHECK_EQ(count_kind(&lib, dir, CB_SYNC_SETTING, NULL), 1);
    CHECK_EQ(unlink(ms), 0);

    Stages st = { 0 };
    CHECK_EQ(export_to(&lib, dir, false, &st), 0);
    CHECK_EQ(st.copy, 1);
    CHECK_EQ(cb_stick_read(&lib, dir, &s), 0);
    cb_stick_state_free(&s);
    cb_library_close(&lib);
    free(libdir);
    free(dir);
    free(song);
    free(ms);
}

int main(void)
{
    const char *base = getenv("CB_TEST_DIR");
    snprintf(root, sizeof(root), "%s", base ? base : "build-formats/test-safety");
    sh("chmod -R u+w '%s' 2>/dev/null; rm -rf '%s'", root, root);
    cb_mkdir_p(root);

    library_on_stick();
    symlinked_contents();
    name_too_long();
    other_library();
    damaged_stick();
    record_not_writable();
    space();
    dry_run_and_dates();
    links_inside();
    case_names();
    cancel_syncs();
    not_a_stick();

    if (!failures)
        sh("rm -rf '%s'", root);
    return test_result("safety");
}
