/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* End to end, dj's test_export in rxbox's model: a small library of
 * generated tracks in five formats (FLAC, MP3, WAV, AAC and Opus, the last
 * transcoded to FLAC for the stick) sharing a folder cover, two playlists,
 * player and device presets; exported to a "stick" directory, inspected,
 * exported again (nothing copied), then changed the way a player would
 * (a hot cue moved and recoloured, a memory cue added, a history list, a
 * preset), which must block the next export until synced; pulled into the
 * library; and pruned when a track leaves the playlists.
 *
 * Needs the ffmpeg CLI with libmp3lame and libopus; skips (77) without. */

#include <math.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#include "rxbox/export.h"
#include "rxbox/colors.h"

#include "src/export/export_priv.h"
#include "src/formats/buf.h"
#include "src/formats/formats_priv.h"
#include "tests/formats/test.h"

static char root[512], music[600], libdir[600], stick[600];

typedef struct Stages {
    int copy, skip, prune, check, rewrite;
    bool quiet;
} Stages;

static bool on_progress(void *opaque, const char *stage, int done, int total, const char *item)
{
    Stages *s = opaque;
    if (!strcmp(stage, "copy"))
        s->copy++;
    else if (!strcmp(stage, "skip"))
        s->skip++;
    else if (!strcmp(stage, "prune") && item && item[0] == '/')
        s->prune++;
    else if (!strcmp(stage, "rewrite"))
        s->rewrite++;
    else if (!strcmp(stage, "check"))
        s->check++;
    if (item && !s->quiet && strcmp(stage, "copy") && strcmp(stage, "skip") && strcmp(stage, "analysis") &&
        strcmp(stage, "artwork") && strcmp(stage, "rewrite"))
        fprintf(stderr, "  [%s] %s\n", stage, item);
    return true;
}

static void on_line(void *opaque, const char *line)
{
    if (opaque)
        fprintf(stderr, "  %s\n", line);
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

/* A kick on every beat at `bpm` from `downbeat`, with a hat between. */
static int make_track(const char *name, double bpm, double downbeat, const char *codec_args)
{
    double p = 60.0 / bpm;
    return sh("ffmpeg -v error -nostdin -y -f lavfi -i \"aevalsrc='0.7*sin(2*PI*55*t)*exp(-14*mod(t-%.4f+%.4f*64,%.6f))"
              "+0.05*sin(2*PI*7000*t)*exp(-60*mod(t-%.4f+%.4f*64.5,%.6f))':s=44100:d=20\" "
              "-metadata title=%s -metadata artist=Synth -metadata album=Synthetic %s '%s/%s'",
              downbeat, p, p, downbeat, p, p, name, codec_args, music, name);
}

static RXTrack *track_named(RXLibrary *lib, const char *file)
{
    for (int i = 0; i < lib->nb_tracks; i++)
        if (!strcmp(rx_path_base(lib->tracks[i]->path), file))
            return lib->tracks[i];
    return NULL;
}

static int kv_add(RXKv **kv, int *nb, const char *key, const char *value)
{
    RXKv *n = realloc(*kv, (size_t)(*nb + 1) * sizeof(*n));
    if (!n)
        return RX_ERROR(ENOMEM);
    *kv = n;
    n[*nb] = (RXKv) { .key = rx_strdup(key), .value = rx_strdup(value) };
    (*nb)++;
    return 0;
}

static int read_stick(const char *path, uint8_t **b, size_t *n)
{
    char *p = rx_stick_file(stick, path);
    int ret = p ? rx_file_read(p, b, n) : RX_ERROR(ENOMEM);
    free(p);
    return ret;
}

static int count_items(RXLibrary *lib, enum RXSyncKind kind, RXSyncItem **out, int *nb_out)
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
    rx_stick_state_free(&s);
    if (out) {
        *out = items;
        *nb_out = nb;
    } else {
        free(items);
    }
    return n;
}

static int setup_library(RXLibrary *lib)
{
    static const struct { const char *name; double bpm, downbeat; const char *args; } files[] = {
        { "one.flac", 126.0, 0.25, "-c:a flac" },
        { "two.mp3", 128.0, 0.5, "-c:a libmp3lame -b:a 320k" },
        { "three.wav", 140.0, 0.1, "-c:a pcm_s16le" },
        { "four.m4a", 122.0, 0.3, "-c:a aac -b:a 256k" },
        { "five.opus", 130.0, 0.2, "-c:a libopus" },
    };
    for (size_t i = 0; i < RX_ARRAY_ELEMS(files); i++)
        if (make_track(files[i].name, files[i].bpm, files[i].downbeat, files[i].args))
            return 77;
    /* A folder cover the tracks share: one artwork row (dj hashed it). */
    if (sh("ffmpeg -v error -nostdin -y -f lavfi -i color=c=orange:s=320x320 -frames:v 1 '%s/front.jpg'", music))
        return 77;

    CHECK_EQ(rx_library_open(lib, libdir), 0);
    for (size_t i = 0; i < RX_ARRAY_ELEMS(files); i++) {
        char *path = rx_asprintf("%s/%s", music, files[i].name);
        int err = 0;
        RXTrack *t = rx_library_add_file(lib, path, &err);
        free(path);
        CHECK(t);
        if (!t)
            return 1;
        CHECK_EQ(rx_grid_init(&t->grid, files[i].downbeat, files[i].bpm), 0);
        snprintf(t->key, sizeof(t->key), "%s", i % 2 ? "Am" : "F#");
        RXCue c;
        rx_cue_init(&c, files[i].downbeat);
        rx_track_add_cue(t, &c);
        if (i == 0) {
            /* dj's hand edit: a hot cue with a 4-beat loop on bar 5. */
            double p = 60.0 / files[i].bpm;
            rx_cue_init(&c, files[i].downbeat + 16 * p);
            c.hot = 0;
            c.loop_end = c.time + 4 * p;
            snprintf(c.name, sizeof(c.name), "loop");
            snprintf(c.color, sizeof(c.color), "red");
            rx_track_add_cue(t, &c);
        }
        CHECK_EQ(rx_library_save_track(lib, t), 0);

        RXAudio a;
        CHECK_EQ(rx_decode(t->path, 1, 0, &a), 0);
        RXWaveforms w;
        CHECK_EQ(rx_waveforms_compute(a.samples, a.nb_frames, a.rate, &w), 0);
        CHECK_EQ(rx_library_waveforms_save(lib, t->id, &w), 0);
        rx_waveforms_free(&w);
        rx_audio_free(&a);
    }

    RXPlaylist *set = rx_library_playlist_create(lib, lib->root_folder, "Set");
    RXPlaylistFolder *f = rx_library_folder_create(lib, lib->root_folder, "Peak");
    RXPlaylist *peak = f ? rx_library_playlist_create(lib, f, "Peak time") : NULL;
    CHECK(set && f && peak);
    if (!set || !peak)
        return 1;
    rx_library_playlist_add(lib, set, track_named(lib, "two.mp3"), -1);
    rx_library_playlist_add(lib, set, track_named(lib, "one.flac"), -1);
    rx_library_playlist_add(lib, peak, track_named(lib, "three.wav"), -1);
    rx_library_playlist_add(lib, peak, track_named(lib, "four.m4a"), -1);
    rx_library_playlist_add(lib, peak, track_named(lib, "five.opus"), -1);

    kv_add(&lib->cfg.player, &lib->cfg.nb_player, "quantize", "true");
    kv_add(&lib->cfg.device, &lib->cfg.nb_device, "waveform_color", "3band");
    CHECK_EQ(rx_library_config_save(lib), 0);
    return 0;
}

static void check_stick(RXLibrary *lib)
{
    uint8_t *b;
    size_t n;
    CHECK_EQ(read_stick("/PIONEER/rekordbox/export.pdb", &b, &n), 0);
    RXPdb pdb;
    CHECK_EQ(rx_pdb_read(b, n, &pdb), 0);
    CHECK_EQ(pdb.nb_tracks, 5);
    RXPdbRawTrack *raw;
    int nr;
    CHECK_EQ(rx_pdb_raw_tracks(b, n, &raw, &nr), 0);
    static const struct { const char *file; int type; int tempo; const char *ext; } want[] = {
        { "one.flac", 5, 12600, ".flac" }, { "two.mp3", 1, 12800, ".mp3" }, { "three.wav", 11, 14000, ".wav" },
        { "four.m4a", 4, 12200, ".m4a" }, { "five.opus", 5, 13000, ".flac" },
    };
    for (size_t i = 0; i < RX_ARRAY_ELEMS(want); i++) {
        RXTrack *t = track_named(lib, want[i].file);
        bool found = false;
        for (int k = 0; t && k < nr; k++) {
            if (raw[k].id != t->id)
                continue;
            found = true;
            CHECK_EQ(raw[k].file_type, want[i].type);
            CHECK_EQ(raw[k].tempo, want[i].tempo);
            CHECK(rx_str_ends(raw[k].strings[RX_PDB_STR_FILE_PATH], want[i].ext));
            CHECK_EQ(raw[k].artwork_id, 1);          /* the shared folder cover */
        }
        CHECK(found);
    }
    /* Playlist "Set": two, one, with the library's ids. */
    bool set = false;
    for (int i = 0; i < pdb.nb_playlists; i++) {
        if (strcmp(pdb.playlists[i].name, "Set"))
            continue;
        set = true;
        CHECK(pdb.playlists[i].nb == 2 && pdb.playlists[i].track_ids[0] == track_named(lib, "two.mp3")->id &&
              pdb.playlists[i].track_ids[1] == track_named(lib, "one.flac")->id);
    }
    CHECK(set);

    /* one.flac: the grid from 0.25 s, the hot cue loop in the .DAT. */
    RXTrack *one = track_named(lib, "one.flac");
    for (int k = 0; one && k < pdb.nb_tracks; k++) {
        if (pdb.tracks[k].id != one->id)
            continue;
        uint8_t *dat;
        size_t dn;
        CHECK_EQ(read_stick(pdb.tracks[k].anlz_path, &dat, &dn), 0);
        RXBeat *beats;
        int nb;
        CHECK_EQ(rx_anlz_read_beats(dat, dn, &beats, &nb), 0);
        CHECK(nb > 10 && beats[0].number == 1 && fabs(beats[0].time - 0.25) < 0.0015);
        free(beats);
        RXPlanCue *cues;
        int nc;
        CHECK_EQ(rx_anlz_read_cues(dat, dn, &cues, &nc), 0);
        bool hot = false;
        for (int c = 0; c < nc; c++)
            hot |= cues[c].hot == 0 && cues[c].loop_end > cues[c].time;
        CHECK(hot);
        free(cues);
        free(dat);
    }
    rx_pdb_raw_tracks_free(raw, nr);
    rx_pdb_free(&pdb);
    free(b);

    CHECK_EQ(read_stick("/PIONEER/rekordbox/exportLibrary.db", &b, &n), 0);
    uint8_t *plain;
    size_t pn;
    CHECK_EQ(rx_sqlcipher_decrypt(b, n, rx_onelibrary_passphrase, NULL, 0, &plain, &pn), 0);
    RXOneLibTrack *ol;
    int no;
    CHECK_EQ(rx_onelibrary_read(plain, pn, &ol, &no), 0);
    CHECK_EQ(no, 5);
    for (int i = 0; i < no; i++) {
        free(ol[i].path);
        free(ol[i].title);
    }
    free(ol);
    free(plain);
    free(b);

    char *p;
    CHECK((p = rx_stick_file(stick, "/PIONEER/DEVSETTING.DAT")) && rx_file_exists(p));
    free(p);
    CHECK((p = rx_stick_file(stick, "/PIONEER/MYSETTING.DAT")) && rx_file_exists(p));
    free(p);
    CHECK((p = rx_stick_file(stick, "/PIONEER/DJMMYSETTING.DAT")) && !rx_file_exists(p));
    free(p);
    CHECK((p = rx_stick_file(stick, "/PIONEER/Artwork/00001/a1_m.jpg")) && rx_file_exists(p));
    free(p);
    CHECK((p = rx_stick_file(stick, "/PIONEER/Artwork/00001/b1.jpg")) && rx_file_exists(p));
    free(p);
    CHECK((p = rx_stick_file(stick, RX_SYNC_PATH)) && rx_file_exists(p));
    free(p);
}

/* What a player does at a gig: moves hot cue A by a second and makes it
 * blue, adds a memory cue at 10 s (written to the ANLZ files as rekordbox
 * would); appends a history list to exportLibrary.db; saves a preset. */
static void play_a_gig(RXLibrary *lib)
{
    RXTrack *one = track_named(lib, "one.flac");
    RXTrack *tr[1] = { one };
    RXPlan plan;
    CHECK_EQ(rx_plan_build(lib, tr, 1, NULL, 0, NULL, false, &plan), 0);
    RXPlanTrack *pt = &plan.tracks[0];
    RXPlanCue *cues = calloc((size_t)pt->nb_cues + 1, sizeof(*cues));
    memcpy(cues, pt->cues, (size_t)pt->nb_cues * sizeof(*cues));
    for (int i = 0; i < pt->nb_cues; i++) {
        if (cues[i].hot == 0) {
            cues[i].time += 1.0;
            cues[i].loop_end += 1.0;
            rx_hot_cue_color("blue", &cues[i].color_code, &cues[i].rgb);
        }
    }
    cues[pt->nb_cues] = (RXPlanCue) { .time = 10.0, .hot = -1, .loop_end = -1 };
    free(pt->cues);
    pt->cues = cues;
    pt->nb_cues++;
    RXAnlzFiles f;
    CHECK_EQ(rx_anlz_build(pt, NULL, &f), 0);
    char *dat = rx_plan_anlz_path(pt, ".DAT"), *ext = rx_plan_anlz_path(pt, ".EXT");
    char *pd = rx_stick_file(stick, dat), *pe = rx_stick_file(stick, ext);
    CHECK_EQ(rx_file_write_atomic(pd, f.dat, f.dat_size), 0);
    CHECK_EQ(rx_file_write_atomic(pe, f.ext, f.ext_size), 0);
    free(dat);
    free(ext);
    free(pd);
    free(pe);
    rx_anlz_files_free(&f);
    rx_plan_free(&plan);

    /* History: a OneLibrary player plays two then one. */
    uint8_t *b, *plain;
    size_t n, pn;
    CHECK_EQ(read_stick("/PIONEER/rekordbox/exportLibrary.db", &b, &n), 0);
    CHECK_EQ(rx_sqlcipher_decrypt(b, n, rx_onelibrary_passphrase, NULL, 0, &plain, &pn), 0);
    free(b);
    sqlite3 *db;
    CHECK_EQ(rx_onelibrary_open_plain(plain, pn, &db), 0);
    free(plain);
    char *sql = rx_asprintf("INSERT INTO history VALUES (1, 0, 'HISTORY 001', 0, 0);"
                            "INSERT INTO history_content VALUES (1, %llu, 1), (1, %llu, 2);",
                            (unsigned long long)track_named(lib, "two.mp3")->id,
                            (unsigned long long)one->id);
    CHECK_EQ(sqlite3_exec(db, sql, NULL, NULL, NULL), SQLITE_OK);
    free(sql);
    sqlite3_int64 sz;
    unsigned char *img = sqlite3_serialize(db, "main", &sz, 0);
    sqlite3_close(db);
    CHECK(img != NULL);
    uint8_t *enc;
    size_t en;
    CHECK_EQ(rx_sqlcipher_encrypt(img, (size_t)sz, rx_onelibrary_passphrase, NULL, &enc, &en), 0);
    sqlite3_free(img);
    char *p = rx_stick_file(stick, "/PIONEER/rekordbox/exportLibrary.db");
    CHECK_EQ(rx_file_write_atomic(p, enc, en), 0);
    free(p);
    free(enc);

    /* The player saves My Settings with a ±6 % tempo range. */
    RXKv kv[2] = { { "quantize", "true" }, { "tempo_range", "6" } };
    CHECK_EQ(rx_settings_encode("MYSETTING.DAT", kv, 2, &b, &n), 0);
    p = rx_stick_file(stick, "/PIONEER/MYSETTING.DAT");
    CHECK_EQ(rx_file_write_atomic(p, b, n), 0);
    free(p);
    free(b);
}

int main(void)
{
    if (system("ffmpeg -hide_banner -encoders 2>/dev/null | grep -q libopus") ||
        system("ffmpeg -hide_banner -encoders 2>/dev/null | grep -q libmp3lame")) {
        fprintf(stderr, "export: ffmpeg with libmp3lame and libopus needed, skipping\n");
        return 77;
    }
    const char *base = getenv("RX_TEST_DIR");
    snprintf(root, sizeof(root), "%s", base ? base : "build-formats/test-stick");
    snprintf(music, sizeof(music), "%s/music", root);
    snprintf(libdir, sizeof(libdir), "%s/lib", root);
    snprintf(stick, sizeof(stick), "%s/stick", root);
    sh("rm -rf '%s'", root);
    rx_mkdir_p(music);
    rx_mkdir_p(stick);

    RXLibrary lib = { 0 };
    int r = setup_library(&lib);
    if (r)
        return r == 77 ? 77 : test_result("export");

    RXExportOptions opt;
    rx_export_options_default(&opt);
    Stages st = { 0 };
    CHECK_EQ(rx_export(&lib, stick, &opt, on_progress, &st), 0);
    CHECK_EQ(st.copy, 5);
    int problems = rx_inspect(stick, on_line, NULL);
    if (problems)
        rx_inspect(stick, on_line, (void *)1);
    CHECK_EQ(problems, 0);
    check_stick(&lib);

    /* A second export copies nothing and the stick stays valid. */
    st = (Stages) { .quiet = true };
    CHECK_EQ(rx_export(&lib, stick, &opt, on_progress, &st), 0);
    CHECK_EQ(st.copy, 0);
    CHECK_EQ(st.skip, 5);
    CHECK_EQ(rx_inspect(stick, on_line, NULL), 0);
    CHECK_EQ(count_items(&lib, RX_SYNC_HOT_CUE_SET, NULL, NULL), 0);

    /* Library edits since the export are not unsynced changes. */
    RXTrack *three = track_named(&lib, "three.wav");
    RXCue extra;
    rx_cue_init(&extra, 5.0);
    extra.hot = 2;
    rx_track_add_cue(three, &extra);
    rx_library_save_track(&lib, three);
    RXSyncItem *items;
    int nb;
    CHECK_EQ(count_items(&lib, RX_SYNC_HOT_CUE_CLEARED, &items, &nb), 0);
    CHECK_EQ(nb, 0);
    free(items);

    play_a_gig(&lib);
    CHECK_EQ(count_items(&lib, RX_SYNC_HOT_CUE_SET, &items, &nb), 1);
    int plays = 0, added = 0, settings = 0;
    for (int i = 0; i < nb; i++) {
        plays += items[i].kind == RX_SYNC_PLAY;
        added += items[i].kind == RX_SYNC_MEMORY_ADDED;
        if (items[i].kind == RX_SYNC_SETTING) {
            settings++;
            CHECK_STR(items[i].setting_key, "player.tempo_range");
            CHECK_STR(items[i].setting_value, "6");
            CHECK_STR(items[i].setting_old, "10");
        }
        if (items[i].kind == RX_SYNC_HOT_CUE_SET)
            CHECK_STR(items[i].cue.color, "blue");
    }
    CHECK_EQ(plays, 2);
    CHECK_EQ(added, 1);
    CHECK_EQ(settings, 1);
    CHECK_EQ(nb, 5);
    free(items);

    /* Unsynced changes block the export. */
    st = (Stages) { .quiet = true };
    CHECK_EQ(rx_export(&lib, stick, &opt, on_progress, &st), RX_ERROR(EBUSY));

    RXTrack *one = track_named(&lib, "one.flac");
    int idx = rx_track_hot_cue(one, 0);
    double before = idx >= 0 ? one->cues[idx].time : 0;
    int two_plays = track_named(&lib, "two.mp3")->play_count;
    CHECK_EQ(rx_pull(&lib, stick, NULL, NULL), 5);
    idx = rx_track_hot_cue(one, 0);
    CHECK(idx >= 0 && fabs(one->cues[idx].time - before - 1.0) < 0.002);
    CHECK(idx >= 0 && !strcmp(one->cues[idx].color, "blue"));
    CHECK(rx_track_memory_cue_at(one, 10.0, 0.02) >= 0);
    CHECK_EQ(track_named(&lib, "two.mp3")->play_count, two_plays + 1);
    bool tempo = false;
    for (int i = 0; i < lib.cfg.nb_player; i++)
        tempo |= !strcmp(lib.cfg.player[i].key, "tempo_range") && !strcmp(lib.cfg.player[i].value, "6");
    CHECK(tempo);

    /* Synced: nothing left, plays not proposed twice, the export runs. */
    CHECK_EQ(count_items(&lib, RX_SYNC_PLAY, &items, &nb), 0);
    CHECK_EQ(nb, 0);
    free(items);
    CHECK_EQ(rx_pull(&lib, stick, NULL, NULL), 0);
    st = (Stages) { .quiet = true };
    CHECK_EQ(rx_export(&lib, stick, &opt, on_progress, &st), 0);
    CHECK_EQ(rx_inspect(stick, on_line, NULL), 0);

    /* four leaves the playlists: a dry run lists its files for pruning,
     * the export with prune removes them. */
    RXPlaylist *pl[8];
    int npl = rx_library_playlists_flat(&lib, pl, 8);
    for (int i = 0; i < npl; i++)
        for (int k = 0; k < pl[i]->nb; k++)
            if (pl[i]->track_ids[k] == track_named(&lib, "four.m4a")->id)
                rx_library_playlist_remove_at(&lib, pl[i], k);
    opt.dry_run = true;
    st = (Stages) { .quiet = true };
    CHECK_EQ(rx_export(&lib, stick, &opt, on_progress, &st), 0);
    CHECK_EQ(st.prune, 4);                   /* the audio and three ANLZ files */
    CHECK(st.rewrite > 0 && st.skip == 4);
    opt.dry_run = false;
    opt.prune = true;
    st = (Stages) { .quiet = true };
    CHECK_EQ(rx_export(&lib, stick, &opt, on_progress, &st), 0);
    CHECK_EQ(st.prune, 4);
    char *four = rx_asprintf("%s/Contents/Synth/Synthetic/four.m4a", stick);
    CHECK(four && !rx_file_exists(four));
    free(four);
    CHECK_EQ(rx_inspect(stick, on_line, NULL), 0);

    rx_library_close(&lib);
    if (!failures)
        sh("rm -rf '%s'", root);
    return test_result("export");
}
