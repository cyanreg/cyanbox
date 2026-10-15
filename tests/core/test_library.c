/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* The SQLite library (src/core/library.c, db.c, config.c): a library made
 * in a scratch directory from real WAV files, edited, closed and opened
 * again, and checked field by field, then with SQLite's own integrity
 * and foreign key checks; the file's comment left out on import unless
 * import.comment asks for it; the legend's settings and the model's colour
 * probabilities; a dynamic grid kept; covers stored once, going with their
 * last track; waveforms stored as previews and detail apart; a library of
 * schema 8 converted, an older one refused, and one a newer rxbox converts
 * while this one opens it; free pages given back.
 * Waveforms are stored through the raw blob layer because the analysis
 * module (which packs them) is not linked here; rxbox/analysis.h is used
 * for its constants and inline helpers only. */

#include "rxbox/library.h"

#include <pthread.h>
#include <unistd.h>

#include <sqlite3.h>

#include "rxbox/analysis.h"
#include "rxbox/colors.h"
#include "src/core/core.h"
#include "util.h"

static char *music, *root;

static char *path_in(const char *dir, const char *name)
{
    return rx_asprintf("%s/%s", dir, name);
}

typedef struct AddLog {
    int reported, failed;
} AddLog;

static void add_cb(void *opaque, const char *path, RXTrack *t, int err)
{
    AddLog *log = opaque;
    if (err < 0)
        log->failed++;
    else if (t && !strcmp(t->path, path))
        log->reported++;
}

/* Rows of a table for one track, straight from SQLite. */
static int count_rows(const RXLibrary *lib, const char *table, uint64_t id)
{
    char *sql = rx_asprintf("SELECT count(*) FROM %s WHERE track_id = %llu", table, (unsigned long long)id);
    sqlite3_stmt *st;
    int n = -1;
    if (sqlite3_prepare_v2(lib->db->db, sql, -1, &st, NULL) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW)
            n = sqlite3_column_int(st, 0);
        sqlite3_finalize(st);
    }
    free(sql);
    return n;
}

/* A track's hue column straight from SQLite: NAN for NULL or no row. */
static double hue_column(const RXLibrary *lib, uint64_t id)
{
    char *sql = rx_asprintf("SELECT hue FROM track WHERE id = %llu", (unsigned long long)id);
    sqlite3_stmt *st;
    double v = NAN;
    if (sqlite3_prepare_v2(lib->db->db, sql, -1, &st, NULL) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_type(st, 0) != SQLITE_NULL)
            v = sqlite3_column_double(st, 0);
        sqlite3_finalize(st);
    }
    free(sql);
    return v;
}

static void make_music(void)
{
    char *p;
    test_write_wav(p = path_in(music, "a.wav"), 1.0, 44100, 2, "Alpha", "Artist A", "2024-05-01");
    free(p);
    test_write_wav(p = path_in(music, "b.wav"), 0.5, 48000, 1, NULL, NULL, NULL);
    free(p);
    test_write_wav(p = path_in(music, "sub/c.wav"), 0.25, 44100, 2, "Gamma", NULL, "1999");
    free(p);
    test_write_wav(p = path_in(music, ".hidden/x.wav"), 0.1, 44100, 2, NULL, NULL, NULL);
    free(p);
    test_write_wav(p = path_in(music, "._d.wav"), 0.1, 44100, 2, NULL, NULL, NULL);
    free(p);
    FILE *f = fopen(p = path_in(music, "notes.txt"), "w");
    fputs("not audio\n", f);
    fclose(f);
    free(p);
}

/* A directory goes in as the album runs: by the track tags, not the file
 * names; files without a number after the numbered ones, by name. */
static void test_album_order(void)
{
    char *dir = test_dir("library-album");
    char *files = path_in(dir, "album"), *lroot = path_in(dir, "lib");
    const struct {
        const char *name;
        int track;
    } tracks[] = {
        { "a-side.wav", 3 }, { "b-side.wav", 1 }, { "bonus.wav", 0 }, { "c-side.wav", 2 },
        { "alt.wav", 0 },
    };
    for (size_t i = 0; i < RX_ARRAY_ELEMS(tracks); i++) {
        char *p = path_in(files, tracks[i].name);
        CHECK(test_write_wav_album(p, 0.1, 44100, 1, tracks[i].name, "The Album", tracks[i].track) == 0);
        free(p);
    }

    RXLibrary lib;
    CHECK(rx_library_open(&lib, lroot) == 0);
    CHECK(rx_library_add_path(&lib, files, NULL, NULL) == 5 && lib.nb_tracks == 5);
    if (lib.nb_tracks == 5) {
        const char *want[] = { "/b-side.wav", "/c-side.wav", "/a-side.wav", "/alt.wav", "/bonus.wav" };
        for (int i = 0; i < 5; i++) {
            CHECK(lib.tracks[i]->id == (uint64_t)i + 1);
            CHECK(rx_str_ends(lib.tracks[i]->path, want[i]));
            CHECK_STR(lib.tracks[i]->meta.album, "The Album");
        }
        CHECK(lib.tracks[0]->meta.track_number == 1 && lib.tracks[2]->meta.track_number == 3);
        CHECK(lib.tracks[3]->meta.track_number == 0);
    }

    /* A flat folder of two albums and two loose files goes in an album at
     * a time, Zeta first (its first file by name is the folder's first),
     * then Alpha, each as it runs, then the files without an album by
     * name. Before, by track number over the folder: 02 03 01 04. */
    char *two = path_in(dir, "two");
    const struct {
        const char *name, *album;
        int track;
    } mixed[] = {
        { "01 x.wav", "Zeta", 2 }, { "02 y.wav", "Alpha", 1 }, { "03 z.wav", "Zeta", 1 },
        { "04 w.wav", "Alpha", 2 }, { "05 v.wav", NULL, 1 }, { "00 u.wav", NULL, 0 },
    };
    for (size_t i = 0; i < RX_ARRAY_ELEMS(mixed); i++) {
        char *p = path_in(two, mixed[i].name);
        CHECK(test_write_wav_album(p, 0.1, 44100, 1, mixed[i].name, mixed[i].album,
                                   mixed[i].track) == 0);
        free(p);
    }
    CHECK(rx_library_add_path(&lib, two, NULL, NULL) == 6 && lib.nb_tracks == 11);
    if (lib.nb_tracks == 11) {
        const char *want[] = {
            "/03 z.wav", "/01 x.wav", "/02 y.wav", "/04 w.wav", "/00 u.wav", "/05 v.wav",
        };
        for (int i = 0; i < 6; i++)
            CHECK(rx_str_ends(lib.tracks[5 + i]->path, want[i]));
    }
    rx_library_close(&lib);

    test_rm_rf(dir);
    free(two);
    free(files);
    free(lroot);
    free(dir);
}

static void test_create_and_add(void)
{
    RXLibrary lib;
    CHECK(rx_library_open(&lib, root) == 0);
    char *db = path_in(root, "library.db");
    CHECK(rx_file_exists(db));
    free(db);
    CHECK(lib.nb_tracks == 0 && lib.root_folder && !lib.root_folder->nb_playlists);
    CHECK(lib.cfg.snap_bpm && lib.cfg.auto_cues && !strcmp(lib.cfg.key_notation, "classic"));
    CHECK(lib.cfg.export_pdb && lib.cfg.export_onelibrary && lib.cfg.detail_bars == 12);
    CHECK(!strcmp(lib.cfg.traffic_light, "related3"));
    CHECK(!lib.cfg.nb_player && !lib.cfg.nb_mixer && !lib.cfg.nb_device);
    CHECK(!lib.cfg.import_comment);

    sqlite3_stmt *st;
    sqlite3_prepare_v2(lib.db->db, "PRAGMA user_version", -1, &st, NULL);
    CHECK(sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int(st, 0) == 10);
    sqlite3_finalize(st);
    /* Made with incremental auto_vacuum, which only takes before WAL. */
    sqlite3_prepare_v2(lib.db->db, "PRAGMA auto_vacuum", -1, &st, NULL);
    CHECK(sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int(st, 0) == 2);
    sqlite3_finalize(st);
    sqlite3_prepare_v2(lib.db->db, "PRAGMA journal_mode", -1, &st, NULL);
    CHECK(sqlite3_step(st) == SQLITE_ROW && !strcmp((const char *)sqlite3_column_text(st, 0), "wal"));
    sqlite3_finalize(st);

    AddLog log = { 0 };
    int n = rx_library_add_path(&lib, music, add_cb, &log);
    CHECK(n == 3 && log.reported == 3 && !log.failed);
    CHECK(lib.nb_tracks == 3);
    if (lib.nb_tracks == 3) {
        RXTrack *a = lib.tracks[0], *b = lib.tracks[1], *c = lib.tracks[2];
        /* Ids in name order, within a directory before its subdirectories. */
        CHECK(a->id == 1 && b->id == 2 && c->id == 3);
        CHECK(rx_str_ends(a->path, "/a.wav") && rx_str_ends(b->path, "/b.wav") && rx_str_ends(c->path, "/sub/c.wav"));
        CHECK_STR(a->file_meta.title, "Alpha");
        CHECK_STR(a->meta.title, "Alpha");
        CHECK_STR(a->meta.artist, "Artist A");
        CHECK(a->file_meta.year == 2024 && a->meta.year == 2024);
        CHECK_STR(b->meta.title, "b");          /* dj: the file name without extension */
        CHECK(c->meta.year == 1999);
        CHECK_NEAR(a->duration, 1.0, 1e-3);
        CHECK(a->sample_rate == 44100 && a->channels == 2 && b->sample_rate == 48000 && b->channels == 1);
        CHECK_STR(a->codec, "pcm_s16le");
        CHECK(a->file_size > 0 && a->mtime > 0 && a->added > 0 && !a->missing);
        /* No colour until the user gives one (a zero hue would be red). */
        CHECK(a->meta.hue == RX_HUE_NONE && b->meta.hue == RX_HUE_NONE && a->file_meta.hue == RX_HUE_NONE);
    }

    /* Again: nothing new, every known file reported with its track. */
    log = (AddLog) { 0 };
    CHECK(rx_library_add_path(&lib, music, add_cb, &log) == 0);
    CHECK(lib.nb_tracks == 3 && log.reported == 3 && !log.failed);

    int err;
    char *p = path_in(music, "nope.wav");
    CHECK(!rx_library_add_file(&lib, p, &err) && err == RX_ERROR(ENOENT));
    free(p);
    p = path_in(music, "notes.txt");
    CHECK(!rx_library_add_file(&lib, p, &err) && err < 0);
    free(p);
    p = path_in(music, "a.wav");
    CHECK(rx_library_add_file(&lib, p, &err) == lib.tracks[0] && err == 0);
    free(p);
    CHECK(lib.nb_tracks == 3);
    CHECK(rx_is_audio_path("/x/y.FLAC") && rx_is_audio_path("a.opus") && !rx_is_audio_path("/x/._y.flac"));
    CHECK(!rx_is_audio_path("/x/y.txt") && !rx_is_audio_path("/x/flac"));
    CHECK(rx_library_check(&lib) == 0);
    rx_library_close(&lib);
}

static const uint8_t blob[] = { 'R', 'X', 'W', 'V', 0, 1, 2, 3, 255 };
static const uint8_t jpeg[] = { 0xff, 0xd8, 0xff, 0xe0, 'J', 'P', 'E', 'G', 0xff, 0xd9 };

static void test_edit_and_reopen(void)
{
    RXLibrary lib;
    CHECK(rx_library_open(&lib, root) == 0);
    CHECK(lib.nb_tracks == 3);
    RXTrack *a = rx_library_find_id(&lib, 1);
    CHECK(a && rx_library_index_of(&lib, a) == 0);
    CHECK(rx_library_find_path(&lib, a->path) == a);
    if (!a)
        return;

    rx_meta_set(&a->meta.title, "Alpha (Edit)");
    rx_meta_set(&a->meta.comment, "hand edited");
    a->meta.track_number = 7;
    a->meta.rating = 4;
    a->meta.hue = rx_track_color_hue(6);   /* aqua: a colour the library keeps */
    rx_meta_add_tag(&a->meta, "Situation/Peak Time");
    rx_meta_add_tag(&a->meta, "vocal");
    snprintf(a->key, sizeof(a->key), "Am");
    a->key_confidence = 0.8;
    rx_grid_add_section(&a->grid, 0.3125, 128.0, 1);
    rx_grid_add_section(&a->grid, 120.123456789, 130.5, 0);
    a->grid_confidence = 0.97;
    a->grid_flags = 5;
    a->grid_locked = true;
    a->analysed = 1760000000;
    a->beats_version = 3;
    a->key_version = 2;
    a->waves_version = 1;

    RXCue c;
    rx_cue_init(&c, 0.3125);
    rx_cue_attach_to_grid(&c, &a->grid);
    rx_track_add_cue(a, &c);
    rx_cue_init(&c, 12.5);
    c.hot = 0;
    snprintf(c.name, sizeof(c.name), "drop \"quoted\" 日本");
    snprintf(c.color, sizeof(c.color), "#ff0000");
    rx_track_add_cue(a, &c);
    rx_cue_init(&c, rx_grid_bar_time(&a->grid, 17, 1));
    c.loop_end = rx_grid_bar_time(&a->grid, 18, 1);
    c.active = true;
    rx_cue_attach_to_grid(&c, &a->grid);
    rx_track_add_cue(a, &c);
    CHECK(rx_library_save_track(&lib, a) == 0);

    CHECK(rx_library_record_play(&lib, a) == 0);
    CHECK(rx_library_record_play(&lib, a) == 0);
    CHECK(a->play_count == 2 && a->last_played > 0);

    lib.cfg.key_notation[0] = '\0';
    snprintf(lib.cfg.key_notation, sizeof(lib.cfg.key_notation), "camelot");
    lib.cfg.background_color = 8;
    lib.cfg.bpm_min = 70;
    lib.cfg.bpm_max = 180.5;
    lib.cfg.snap_bpm = false;
    lib.cfg.compat_transcode = true;
    lib.cfg.device_name = rx_strdup("MY STICK");
    lib.cfg.audio_sink = rx_strdup("rxbox-null");
    lib.cfg.detail_bars = 16;
    lib.cfg.split_tree = 0.25f;
    snprintf(lib.cfg.traffic_light, sizeof(lib.cfg.traffic_light), "related1");
    lib.cfg.player = calloc(2, sizeof(RXKv));
    lib.cfg.player[0] = (RXKv) { rx_strdup("quantize"), rx_strdup("true") };
    lib.cfg.player[1] = (RXKv) { rx_strdup("tempo_range"), rx_strdup("10") };
    lib.cfg.nb_player = 2;
    lib.cfg.device = calloc(1, sizeof(RXKv));
    lib.cfg.device[0] = (RXKv) { rx_strdup("waveform_color"), rx_strdup("3band") };
    lib.cfg.nb_device = 1;
    CHECK(rx_library_config_save(&lib) == 0);
    CHECK(rx_library_setting_set(&lib, "ui.columns", "title,artist,bpm") == 0);
    char *v;
    CHECK(rx_library_setting_get(&lib, "ui.columns", &v) == 0 && v && !strcmp(v, "title,artist,bpm"));
    free(v);
    CHECK(rx_library_setting_get(&lib, "ui.nothing", &v) == RX_ERROR(ENOENT) && !v);

    CHECK(rx_library_artwork_save(&lib, 1, jpeg, sizeof(jpeg), 240, 240) == 0);
    CHECK(rx_library_artwork_save(&lib, 99, jpeg, sizeof(jpeg), 240, 240) == RX_ERROR(ENOENT));
    CHECK(rx_core_waveform_blob_put(&lib, 1, blob, sizeof(blob), 4) == 0);
    CHECK(rx_core_waveform_blob_put(&lib, 1, blob, sizeof(blob), sizeof(blob)) ==
          RX_ERROR(EINVAL));
    CHECK(rx_library_has_waveforms(&lib, 1) && !rx_library_has_waveforms(&lib, 2));

    unsigned gen = lib.generation;
    RXTrack *b = rx_library_find_id(&lib, 2);
    b->meta.rating = 2;
    /* A dynamic grid: a straight intro, three measured beats, a straight
     * outro, stored and read back bit for bit. */
    rx_grid_add_section(&b->grid, 0.5, 120.0, 1);
    rx_grid_add_beat(&b->grid, 4.5, 118.0, 0);
    rx_grid_add_beat(&b->grid, 5.0084745762711864, 116.5, 0);
    rx_grid_add_beat(&b->grid, 5.5235, 119.0, 3);
    rx_grid_add_section(&b->grid, 6.0277, 121.0, 0);
    CHECK(rx_library_save_track(&lib, b) == 0 && lib.generation != gen);
    rx_library_close(&lib);

    /* Everything back from the disk. */
    CHECK(rx_library_open(&lib, root) == 0);
    a = rx_library_find_id(&lib, 1);
    CHECK(a != NULL);
    if (!a)
        return;
    CHECK_STR(a->meta.title, "Alpha (Edit)");
    CHECK_STR(a->file_meta.title, "Alpha");
    CHECK_STR(a->meta.comment, "hand edited");
    CHECK(!a->file_meta.comment);
    CHECK(a->meta.track_number == 7 && a->file_meta.track_number == 0);
    CHECK(a->meta.rating == 4 && a->meta.hue == rx_track_color_hue(6));
    CHECK(rx_library_find_id(&lib, 2)->meta.hue == RX_HUE_NONE);
    {
        const RXGrid *bg = &rx_library_find_id(&lib, 2)->grid;
        CHECK(bg->nb_sections == 5 && rx_grid_dynamic(bg) && rx_grid_valid(bg));
        if (bg->nb_sections == 5) {
            CHECK(!bg->sections[0].measured && bg->sections[1].measured && bg->sections[3].measured);
            CHECK(!bg->sections[4].measured && bg->sections[4].start == 6.0277);
            CHECK(bg->sections[2].start == 5.0084745762711864 && bg->sections[2].bpm == 116.5);
            CHECK(bg->sections[3].beat == 3 && bg->sections[1].beat == 0);
        }
    }
    CHECK(a->meta.nb_tags == 2 && !strcmp(a->meta.tags[0], "Situation/Peak Time") && !strcmp(a->meta.tags[1], "vocal"));
    CHECK_STR(a->key, "Am");
    CHECK(a->key_confidence == 0.8 && a->grid_confidence == 0.97);
    CHECK(a->grid_flags == 5 && a->grid_locked && a->analysed == 1760000000);
    CHECK(a->beats_version == 3 && a->key_version == 2 && a->waves_version == 1);
    CHECK(a->grid.nb_sections == 2 && a->grid.beats_per_bar == 4);
    if (a->grid.nb_sections == 2) {
        CHECK(a->grid.sections[0].start == 0.3125 && a->grid.sections[0].bpm == 128.0 && a->grid.sections[0].beat == 1);
        CHECK(a->grid.sections[1].start == 120.123456789 && a->grid.sections[1].bpm == 130.5 && a->grid.sections[1].beat == 0);
    }
    CHECK(a->nb_cues == 3);
    if (a->nb_cues == 3) {
        CHECK(a->cues[0].time == 0.3125 && a->cues[0].grid_beat == 0.0 && a->cues[0].hot == -1);
        CHECK(!rx_cue_is_loop(&a->cues[0]) && isnan(a->cues[0].loop_beats));
        CHECK(a->cues[1].time == 12.5 && isnan(a->cues[1].grid_beat) && a->cues[1].hot == 0);
        CHECK_STR(a->cues[1].name, "drop \"quoted\" 日本");
        CHECK_STR(a->cues[1].color, "#ff0000");
        CHECK(a->cues[2].grid_beat == 64.0 && a->cues[2].loop_beats == 4.0 && a->cues[2].active);
        CHECK(a->cues[2].time == rx_grid_bar_time(&a->grid, 17, 1));
        CHECK(a->cues[2].loop_end == rx_grid_bar_time(&a->grid, 18, 1));
    }
    CHECK(a->play_count == 2 && a->last_played > 0);
    CHECK(count_rows(&lib, "play", 1) == 2);
    CHECK(a->has_waveforms && rx_library_has_waveforms(&lib, 1));
    CHECK(rx_library_find_id(&lib, 2)->meta.rating == 2);

    /* The hue column holds degrees, 360 being 0 again, or NULL. */
    CHECK(hue_column(&lib, 1) == rx_track_color_hue(6) && isnan(hue_column(&lib, 2)));
    b = rx_library_find_id(&lib, 2);
    b->meta.hue = 360.0;
    CHECK(rx_library_save_track(&lib, b) == 0 && hue_column(&lib, 2) == 0.0);
    b->meta.hue = -5.0;
    CHECK(rx_library_save_track(&lib, b) == 0 && isnan(hue_column(&lib, 2)));
    b->meta.hue = RX_HUE_NONE;

    const RXConfig *cfg = &lib.cfg;
    CHECK_STR(cfg->key_notation, "camelot");
    CHECK(cfg->background_color == 8);
    CHECK(cfg->bpm_min == 70 && cfg->bpm_max == 180.5 && !cfg->snap_bpm && cfg->auto_cues);
    CHECK(cfg->compat_transcode && cfg->export_pdb && cfg->detail_bars == 16 && cfg->split_tree == 0.25f);
    CHECK_STR(cfg->device_name, "MY STICK");
    CHECK_STR(cfg->audio_sink, "rxbox-null");
    CHECK_STR(cfg->traffic_light, "related1");
    /* export.key_notation names keys on the stick only: the players' key
     * display is a device setting of its own, not set by it. */
    CHECK(cfg->nb_player == 2 && cfg->nb_device == 1 && cfg->nb_mixer == 0);
    if (cfg->nb_player == 2) {
        CHECK_STR(cfg->player[0].key, "quantize");
        CHECK_STR(cfg->player[1].key, "tempo_range");
        CHECK_STR(cfg->player[1].value, "10");
    }
    if (cfg->nb_device == 1) {
        CHECK_STR(cfg->device[0].key, "waveform_color");
        CHECK_STR(cfg->device[0].value, "3band");
    }
    CHECK(rx_library_setting_get(&lib, "ui.columns", &v) == 0 && v && !strcmp(v, "title,artist,bpm"));
    free(v);

    uint8_t *data;
    size_t size;
    int w, h;
    CHECK(rx_library_artwork_load(&lib, 1, &data, &size, &w, &h) == 0);
    CHECK(size == sizeof(jpeg) && !memcmp(data, jpeg, size) && w == 240 && h == 240);
    free(data);
    CHECK(rx_library_artwork_load(&lib, 2, &data, &size, &w, &h) == RX_ERROR(ENOENT));
    CHECK(rx_core_waveform_blob_get(&lib, 1, true, &data, &size) == 0);
    CHECK(size == sizeof(blob) && !memcmp(data, blob, size));
    free(data);
    /* The preview alone: the bytes before the split. */
    CHECK(rx_core_waveform_blob_get(&lib, 1, false, &data, &size) == 0);
    CHECK(size == 4 && !memcmp(data, blob, size));
    free(data);
    CHECK(rx_core_waveform_blob_get(&lib, 2, true, &data, &size) == RX_ERROR(ENOENT) && !data);

    /* A player-only key removed from the config goes from the database. */
    free(lib.cfg.player[1].key);
    free(lib.cfg.player[1].value);
    lib.cfg.nb_player = 1;
    free(lib.cfg.device_name);
    lib.cfg.device_name = NULL;
    CHECK(rx_library_config_save(&lib) == 0);
    rx_library_close(&lib);
    CHECK(rx_library_open(&lib, root) == 0);
    CHECK(lib.cfg.nb_player == 1 && !lib.cfg.device_name);
    rx_library_close(&lib);
}

/* The file is retagged: fields the user did not change follow it. */
static void test_refresh(void)
{
    RXLibrary lib;
    CHECK(rx_library_open(&lib, root) == 0);
    RXTrack *a = rx_library_find_id(&lib, 1);
    char *p = path_in(music, "a.wav");
    sleep(1);       /* a new mtime */
    test_write_wav(p, 2.0, 44100, 2, "Alpha 2", "Artist B", "2025");
    free(p);
    CHECK(rx_library_refresh_track(&lib, a) == 0);
    CHECK_STR(a->file_meta.title, "Alpha 2");
    CHECK_STR(a->meta.title, "Alpha (Edit)");
    CHECK_STR(a->meta.artist, "Artist B");
    CHECK(a->meta.year == 2025 && a->meta.track_number == 7);
    CHECK_NEAR(a->duration, 2.0, 1e-3);
    CHECK(a->nb_cues == 3 && a->grid.nb_sections == 2 && a->meta.rating == 4);

    /* Taking an override back makes the field follow the file again. */
    rx_meta_set(&a->meta.title, a->file_meta.title);
    CHECK(rx_library_save_track(&lib, a) == 0);
    rx_library_close(&lib);

    CHECK(rx_library_open(&lib, root) == 0);
    a = rx_library_find_id(&lib, 1);
    CHECK_STR(a->meta.title, "Alpha 2");
    CHECK_STR(a->meta.artist, "Artist B");
    CHECK(count_rows(&lib, "cue", 1) == 3);
    rx_library_close(&lib);
}

/* The record of changes: a saved track and a played one by id, once
 * each; a playlist made, everything; taking it starts it again. And a
 * track read back through a second connection as the first saved it,
 * the second's own copy untouched. */
static void test_changes(void)
{
    RXLibrary lib, other;
    CHECK(rx_library_open(&lib, root) == 0);
    CHECK(rx_library_open(&other, root) == 0);
    uint64_t *ids = NULL;
    int nb = 0;
    CHECK(rx_library_take_changes(&lib, &ids, &nb) == 0 && !ids && nb == 0);

    RXTrack *a = rx_library_find_id(&lib, 1), *b = rx_library_find_id(&lib, 2);
    CHECK(a && b);
    if (!a || !b)
        goto done;
    int rating = a->meta.rating, cues = a->nb_cues;
    a->meta.rating = 2;
    RXCue c;
    rx_cue_init(&c, 3.25);
    CHECK(rx_track_add_cue(a, &c) >= 0);
    CHECK(rx_library_save_track(&lib, a) == 0);
    CHECK(rx_library_save_track(&lib, a) == 0);
    CHECK(rx_library_record_play(&lib, b) == 0);
    CHECK(rx_library_take_changes(&lib, &ids, &nb) == 0 && nb == 2 && ids[0] == a->id &&
          ids[1] == b->id);
    free(ids);
    CHECK(rx_library_take_changes(&lib, &ids, &nb) == 0 && nb == 0);

    int err = 0;
    RXTrack *read = rx_library_read_track(&other, a->id, &err);
    CHECK(read && read->meta.rating == 2 && read->nb_cues == cues + 1 &&
          read->grid.nb_sections == a->grid.nb_sections && !strcmp(read->path, a->path) &&
          read->has_waveforms == a->has_waveforms);
    CHECK(rx_library_find_id(&other, a->id)->meta.rating == rating);
    rx_track_free(read);
    CHECK(!rx_library_read_track(&other, 999999, &err) && err == RX_ERROR(ENOENT));

    RXPlaylist *pl = rx_library_playlist_create(&lib, lib.root_folder, "Changes");
    CHECK(pl);
    CHECK(rx_library_take_changes(&lib, &ids, &nb) == 1 && !ids && nb == 0);
    CHECK(rx_library_take_changes(&lib, &ids, &nb) == 0 && nb == 0);
    if (pl)
        CHECK(rx_library_playlist_delete(&lib, pl) == 0);
    a->meta.rating = rating;
    int at = rx_track_memory_cue_at(a, 3.25, 0.001);
    CHECK(at >= 0 && rx_track_remove_cue(a, at) == 0);
    CHECK(rx_library_save_track(&lib, a) == 0);
done:
    rx_library_close(&other);
    rx_library_close(&lib);
}

/* The History: rows oldest first whatever order they were added in, a
 * track's rows all taken off at once, kept through an open, and gone with
 * the track. */
static void test_history(void)
{
    char *dir = test_dir("library-history");
    char *m = path_in(dir, "music");
    CHECK(rx_mkdir_p(m) == 0);
    char *pa = path_in(m, "a.wav"), *pb = path_in(m, "b.wav");
    test_write_wav(pa, 1.0, 44100, 2, "A", "X", "2024");
    test_write_wav(pb, 1.0, 44100, 2, "B", "X", "2024");
    RXLibrary lib;
    CHECK(rx_library_open(&lib, dir) == 0);
    int err = 0;
    RXTrack *a = rx_library_add_file(&lib, pa, &err), *b = rx_library_add_file(&lib, pb, &err);
    CHECK(a && b);
    RXHistoryEntry *h = NULL;
    int nb = 0;
    if (a && b) {
        CHECK(rx_library_history(&lib, &h, &nb) == 0 && nb == 0);
        free(h);
        CHECK(rx_library_history_add(&lib, a, 2000, 1) == 0);
        CHECK(rx_library_history_add(&lib, b, 1000, 2) == 0);
        CHECK(rx_library_history_add(&lib, a, 3000, 0) == 0);
        CHECK(rx_library_history(&lib, &h, &nb) == 0 && nb == 3);
        CHECK(nb == 3 && h[0].track_id == b->id && h[0].played_at == 1000 && h[0].player == 2 &&
              h[1].track_id == a->id && h[1].player == 1 && h[2].track_id == a->id &&
              h[2].played_at == 3000 && h[2].player == 0);
        free(h);
        uint64_t aid = a->id, bid = b->id;
        rx_library_close(&lib);
        CHECK(rx_library_open(&lib, dir) == 0);
        CHECK(rx_library_history(&lib, &h, &nb) == 0 && nb == 3);
        free(h);
        CHECK(rx_library_history_remove(&lib, aid) == 2);
        CHECK(rx_library_history_remove(&lib, aid) == 0);
        CHECK(rx_library_history(&lib, &h, &nb) == 0 && nb == 1 && h[0].track_id == bid);
        free(h);
        CHECK(rx_library_remove(&lib, rx_library_find_id(&lib, bid)) == 0);
        CHECK(rx_library_history(&lib, &h, &nb) == 0 && nb == 0);
        free(h);
    }
    rx_library_close(&lib);
    free(pa);
    free(pb);
    free(m);
    free(dir);
}

/* A dynamic grid's measured beats are kept through a save and an open. */
static void test_dynamic_grid(void)
{
    char *dir = test_dir("library-dynamic");
    char *a_path = path_in(music, "a.wav");
    RXLibrary lib;
    int err;
    CHECK(rx_library_open(&lib, dir) == 0);
    RXTrack *a = rx_library_add_file(&lib, a_path, &err);
    CHECK(a != NULL);
    if (a) {
        rx_grid_add_section(&a->grid, 0.25, 125.0, 1);
        rx_grid_add_section(&a->grid, 60.25, 126.0, 0);
        CHECK(rx_library_save_track(&lib, a) == 0);
    }
    rx_library_close(&lib);

    CHECK(rx_library_open(&lib, dir) == 0);
    a = rx_library_find_path(&lib, a_path);
    CHECK(a && a->grid.nb_sections == 2 && !rx_grid_dynamic(&a->grid));
    if (a && a->grid.nb_sections == 2) {
        CHECK(a->grid.sections[0].start == 0.25 && a->grid.sections[0].bpm == 125.0);
        CHECK(a->grid.sections[1].start == 60.25 && !a->grid.sections[1].measured);
        /* And a dynamic grid is kept from now on. */
        rx_grid_add_beat(&a->grid, 30.25, 124.5, 0);
        CHECK(rx_library_save_track(&lib, a) == 0);
    }
    rx_library_close(&lib);
    CHECK(rx_library_open(&lib, dir) == 0);
    a = rx_library_find_path(&lib, a_path);
    CHECK(a && a->grid.nb_sections == 3 && rx_grid_dynamic(&a->grid));
    if (a && a->grid.nb_sections == 3)
        CHECK(a->grid.sections[1].measured && a->grid.sections[1].start == 30.25);
    rx_library_close(&lib);

    free(a_path);
    test_rm_rf(dir);
    free(dir);
}

/* The legend: eight descriptions, the defaults until the user writes
 * others, stored as color.<name> rows only when they differ; and the
 * model's colour probabilities and legend hash, kept with the track. */
static void test_legend(void)
{
    char *dir = test_dir("library-legend");
    RXLibrary lib;
    CHECK(rx_library_open(&lib, dir) == 0);
    RXConfig *cfg = &lib.cfg;
    static const char *const defaults[RX_TRACK_COLORS] = {
        "dark and intense, hard", "intense, peak time", "energetic and bright, driving",
        "bright, uplifting", "groovy and relaxed, warm", "calm, smooth", "deep, melancholic",
        "calm and dark, ambient",
    };
    for (int id = 1; id <= RX_TRACK_COLORS; id++) {
        CHECK_STR(rx_config_vibe(cfg, id), defaults[id - 1]);
        CHECK_STR(rx_config_vibe_default(id), defaults[id - 1]);
        CHECK(!cfg->vibes[id - 1]);
        char *key = rx_asprintf("color.%s", rx_track_color_names[id - 1]);
        CHECK_STR(rx_config_vibe_key(id), key);
        free(key);
    }
    CHECK(!rx_config_vibe(cfg, 0) && !rx_config_vibe(cfg, 9) && !rx_config_vibe_key(9));
    CHECK(rx_config_set_vibe(cfg, 0, "x") == RX_ERROR(EINVAL));

    /* Trimmed; the default's own words, blank and NULL are the default. */
    CHECK(rx_config_set_vibe(cfg, 2, "  pounding, relentless \n") == 0);
    CHECK_STR(cfg->vibes[1], "pounding, relentless");
    CHECK(rx_config_set_vibe(cfg, 6, " calm, smooth ") == 0 && !cfg->vibes[5]);
    CHECK(rx_config_set_vibe(cfg, 7, "   ") == 0 && !cfg->vibes[6]);
    CHECK(rx_config_set_vibe(cfg, 8, "hypnotic, deep, rolling") == 0);
    const char *legend[RX_TRACK_COLORS];
    rx_config_legend(cfg, legend);
    CHECK_STR(legend[1], "pounding, relentless");
    CHECK_STR(legend[7], "hypnotic, deep, rolling");
    CHECK_STR(legend[0], defaults[0]);
    CHECK(rx_library_config_save(&lib) == 0);
    char *v = NULL;
    CHECK(rx_library_setting_get(&lib, "color.red", &v) == 0 && v && !strcmp(v, "pounding, relentless"));
    free(v);
    CHECK(rx_library_setting_get(&lib, "color.aqua", &v) == RX_ERROR(ENOENT));

    /* The model's colour: eight probabilities and the legend's hash, NAN
     * and 0 (NULL in the column) for a track it never coloured. */
    CHECK(rx_library_add_path(&lib, music, NULL, NULL) == 3);
    RXTrack *a = lib.nb_tracks ? lib.tracks[0] : NULL, *b = lib.nb_tracks > 1 ? lib.tracks[1] : NULL;
    CHECK(a && b);
    if (!a || !b) {
        rx_library_close(&lib);
        return;
    }
    for (int i = 0; i < RX_TRACK_COLORS; i++)
        CHECK(isnan(a->ml_colors[i]));
    CHECK(!a->ml_legend);
    static const float p[RX_TRACK_COLORS] = {
        0.01f, 0.625f, 0.21f, 0.05f, 0.03f, 0.0125f, 0.02f, 0.0425f,
    };
    memcpy(a->ml_colors, p, sizeof(p));
    a->ml_legend = 0xDEADBEEFu;
    a->tags_version = 2;
    CHECK(rx_library_save_track(&lib, a) == 0);
    /* A rewrite of everything keeps them (and binds the blob in time). */
    a->meta.rating = 3;
    CHECK(rx_library_save_track(&lib, a) == 0);
    rx_library_close(&lib);

    CHECK(rx_library_open(&lib, dir) == 0);
    cfg = &lib.cfg;
    CHECK_STR(rx_config_vibe(cfg, 2), "pounding, relentless");
    CHECK_STR(rx_config_vibe(cfg, 8), "hypnotic, deep, rolling");
    CHECK_STR(rx_config_vibe(cfg, 6), defaults[5]);
    a = rx_library_find_id(&lib, 1);
    b = rx_library_find_id(&lib, 2);
    CHECK(a && b);
    if (a && b) {
        CHECK(!memcmp(a->ml_colors, p, sizeof(p)) && a->ml_legend == 0xDEADBEEFu);
        CHECK(a->tags_version == 2 && a->meta.rating == 3);
        CHECK(isnan(b->ml_colors[0]) && !b->ml_legend);
    }
    sqlite3_stmt *st;
    sqlite3_prepare_v2(lib.db->db, "SELECT length(ml_colors), ml_legend FROM track ORDER BY id",
                       -1, &st, NULL);
    CHECK(sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int(st, 0) == 32 &&
          sqlite3_column_int64(st, 1) == 0xDEADBEEF);
    CHECK(sqlite3_step(st) == SQLITE_ROW && sqlite3_column_type(st, 0) == SQLITE_NULL &&
          sqlite3_column_int64(st, 1) == 0);
    sqlite3_finalize(st);

    /* Back to the defaults: the rows go. A row of blanks reads as the
     * default too. */
    CHECK(rx_config_set_vibe(cfg, 2, NULL) == 0 && rx_config_set_vibe(cfg, 8, "") == 0);
    CHECK(rx_library_config_save(&lib) == 0);
    CHECK(rx_library_setting_get(&lib, "color.red", &v) == RX_ERROR(ENOENT));
    CHECK(rx_library_setting_get(&lib, "color.purple", &v) == RX_ERROR(ENOENT));
    CHECK(rx_library_setting_set(&lib, "color.green", "  ") == 0);
    rx_library_close(&lib);
    CHECK(rx_library_open(&lib, dir) == 0);
    for (int id = 1; id <= RX_TRACK_COLORS; id++)
        CHECK(!lib.cfg.vibes[id - 1] && !strcmp(rx_config_vibe(&lib.cfg, id), defaults[id - 1]));
    rx_library_close(&lib);

    test_rm_rf(dir);
    free(dir);
}

/* The library's cleaning (src/core/clean.c): what it holds that is not
 * valid, or not known, goes when it opens, and stays gone; what is valid
 * is left alone. Planted by SQL as an older rxbox, or a hand, could leave
 * it (the schema's checks refuse a tempo of 0 and a slot outside -1..7
 * already). */
static void test_clean(void)
{
    char *dir = test_dir("library-clean");
    char *db = path_in(dir, "library.db");
    RXLibrary lib;
    CHECK(rx_library_open(&lib, dir) == 0);
    rx_library_close(&lib);

    sqlite3 *s;
    CHECK(sqlite3_open(db, &s) == SQLITE_OK);
    const char *sql =
        /* a: everything wrong; b: everything right. */
        "INSERT INTO track (id, path, duration, hue, rating, key, key_version, beats_version, "
        "ml_colors, ml_legend) VALUES (1, '/gone/a.wav', 100, 317, 4, 'Xm#', 1, 5, "
        "X'0000004000000040000000400000004000000040000000400000004000000040', 77);"
        "INSERT INTO track (id, path, duration, hue, rating, key, key_version) "
        "VALUES (2, '/gone/b.wav', 100, 120, 3, 'Am', 1);"
        /* c..j: the eight colours, by the hue colors.c gives each. */
        "INSERT INTO grid_section (track_id, idx, start, bpm) VALUES (1, 0, 0.5, 5000);"
        "INSERT INTO grid_section (track_id, idx, start, bpm) VALUES (2, 0, 0.5, 128);"
        "INSERT INTO cue (track_id, time, hot) VALUES (1, 10, -1);"           /* kept */
        "INSERT INTO cue (track_id, time, hot) VALUES (1, -5, -1);"           /* before 0 */
        "INSERT INTO cue (track_id, time, hot, loop_end) VALUES (1, 10, -1, 5);" /* ends first */
        "INSERT INTO cue (track_id, time, hot) VALUES (1, 20, 0);"            /* kept */
        "INSERT INTO cue (track_id, time, hot) VALUES (1, 30, 0);"            /* slot taken */
        "INSERT INTO cue (track_id, time, hot, loop_end) VALUES (2, 10, -1, 12);"
        "INSERT OR REPLACE INTO setting VALUES ('analysis.snap_bpm', 'maybe');"
        "INSERT OR REPLACE INTO setting VALUES ('bogus.key', '1');"
        "INSERT OR REPLACE INTO setting VALUES ('ui.columns', 'title');"
        "INSERT OR REPLACE INTO setting VALUES ('sync.history.x', '1:0');"
        "INSERT OR REPLACE INTO setting VALUES ('export.pdb', 'false');"
        /* Text in NFD, as tags written on macOS hold it: b's title
         * "ヘ+゙ージュ" and artist "Cafe+́", its file's title (the tag as
         * read, which the track's save does not write), a cue's name, a
         * playlist's. */
        "UPDATE track SET title = 'ヘ\xe3\x82\x99ージュ', artist = 'Cafe\xcc\x81', "
        "file_title = 'ヘ\xe3\x82\x99ージュ' WHERE id = 2;"
        "UPDATE cue SET name = 'シ\xe3\x82\x99ャ' WHERE track_id = 2;"
        "INSERT INTO playlist (parent_id, name, is_folder, position) "
        "VALUES (NULL, 'おやすみヘ\xe3\x82\x99ージュ', 0, 0);";
    CHECK(sqlite3_exec(s, sql, NULL, NULL, NULL) == SQLITE_OK);
    for (int i = 0; i < RX_TRACK_COLORS; i++) {
        char *ins = rx_asprintf("INSERT INTO track (id, path, hue) "
                                "VALUES (%d, '/gone/%d.wav', %.17g)",
                                3 + i, i, rx_track_color_hue(i + 1));
        CHECK(sqlite3_exec(s, ins, NULL, NULL, NULL) == SQLITE_OK);
        free(ins);
    }
    sqlite3_close(s);

    for (int pass = 0; pass < 2; pass++) {
        /* The second open finds it as the first left it. */
        CHECK(rx_library_open(&lib, dir) == 0);
        CHECK(lib.nb_tracks == 2 + RX_TRACK_COLORS);
        for (int i = 0; i < RX_TRACK_COLORS; i++) {
            RXTrack *t = rx_library_find_id(&lib, 3 + i);
            CHECK(t && t->meta.hue == rx_track_color_hue(i + 1));
        }
        RXTrack *a = rx_library_find_id(&lib, 1), *b = rx_library_find_id(&lib, 2);
        CHECK(a && b);
        if (a && b) {
            CHECK(a->meta.hue == RX_HUE_NONE);
            CHECK(a->meta.rating == 4);
            CHECK(!a->key[0] && a->key_version == 0);
            CHECK(a->grid.nb_sections == 0 && a->beats_version == 1);
            CHECK(rx_analysis_stale(a) & RX_ANALYSE_BEATS);
            CHECK(isnan(a->ml_colors[0]) && isnan(a->ml_colors[7]) && !a->ml_legend);
            CHECK(a->nb_cues == 2);
            if (a->nb_cues == 2) {
                CHECK(a->cues[0].time == 10.0 && a->cues[0].hot == -1 && !rx_cue_is_loop(&a->cues[0]));
                CHECK(a->cues[1].time == 20.0 && a->cues[1].hot == 0);
            }
            CHECK(b->meta.hue == 120.0 && b->meta.rating == 3 && !strcmp(b->key, "Am"));
            CHECK(b->grid.nb_sections == 1 && b->grid.sections[0].bpm == 128.0);
            CHECK(b->nb_cues == 1 && rx_cue_is_loop(&b->cues[0]));
            /* NFC, as the second open finds it in the database too. */
            CHECK(b->meta.title && !strcmp(b->meta.title, "ベージュ"));
            CHECK(b->meta.artist && !strcmp(b->meta.artist, "Café"));
            CHECK(b->file_meta.title && !strcmp(b->file_meta.title, "ベージュ"));
            CHECK(b->nb_cues == 1 && !strcmp(b->cues[0].name, "ジャ"));
        }
        bool named = false;
        for (int i = 0; i < lib.root_folder->nb_playlists; i++)
            named |= !strcmp(lib.root_folder->playlists[i]->name, "おやすみベージュ");
        CHECK(named);
        char *v = NULL;
        CHECK(rx_library_setting_get(&lib, "analysis.snap_bpm", &v) == RX_ERROR(ENOENT));
        CHECK(lib.cfg.snap_bpm);
        CHECK(rx_library_setting_get(&lib, "bogus.key", &v) == RX_ERROR(ENOENT));
        CHECK(rx_library_setting_get(&lib, "ui.columns", &v) == 0 && v && !strcmp(v, "title"));
        free(v);
        v = NULL;
        CHECK(rx_library_setting_get(&lib, "sync.history.x", &v) == 0 && v);
        free(v);
        v = NULL;
        CHECK(rx_library_setting_get(&lib, "export.pdb", &v) == 0 && v && !strcmp(v, "false"));
        free(v);
        CHECK(!lib.cfg.export_pdb);
        CHECK(rx_library_check(&lib) == 0);
        rx_library_close(&lib);
    }

    /* The database keeps the playlist's name NFC too, and the file's
     * title: an open does not convert them again and again. */
    CHECK(sqlite3_open(db, &s) == SQLITE_OK);
    sqlite3_stmt *st = NULL;
    CHECK(sqlite3_prepare_v2(s, "SELECT count(*) FROM playlist WHERE name = 'おやすみベージュ'", -1,
                             &st, NULL) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW &&
          sqlite3_column_int(st, 0) == 1);
    sqlite3_finalize(st);
    st = NULL;
    CHECK(sqlite3_prepare_v2(s, "SELECT count(*) FROM track WHERE id = 2 AND file_title = 'ベージュ'",
                             -1, &st, NULL) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW &&
          sqlite3_column_int(st, 0) == 1);
    sqlite3_finalize(st);
    sqlite3_close(s);

    /* What is set from now on is NFC as it is set. */
    RXMeta m = { 0 };
    CHECK(rx_meta_set(&m.title, "Cafe\xcc\x81") == 0 && !strcmp(m.title, "Café"));
    CHECK(rx_meta_add_tag(&m, "e\xcc\x81") == 0 && rx_meta_add_tag(&m, "é") == 0 && m.nb_tags == 1);
    rx_meta_free(&m);

    free(db);
    test_rm_rf(dir);
    free(dir);
}

/* The comment is the user's: the file's comment tag is read and left out
 * of the library unless import.comment is set, adding a file or a folder
 * and re-reading one; a comment the library already holds stays. */
static void test_comment_import(void)
{
    char *dir = test_dir("library-comment");
    char *files = path_in(dir, "music"), *libdir = path_in(dir, "lib");
    char *a_path = path_in(files, "a.wav"), *b_path = path_in(files, "sub/b.wav");
    CHECK(test_write_wav_comment(a_path, 0.2, 44100, 2, "A", "Artist", NULL, "from the file") == 0);
    CHECK(test_write_wav_comment(b_path, 0.2, 44100, 2, "B", "Artist", NULL, "b's comment") == 0);

    RXLibrary lib;
    CHECK(rx_library_open(&lib, libdir) == 0);
    CHECK(!lib.cfg.import_comment);
    int err;
    RXTrack *a = rx_library_add_file(&lib, a_path, &err);
    CHECK(a && !a->file_meta.comment && !a->meta.comment);
    CHECK(a && !strcmp(a->meta.title, "A") && !strcmp(a->meta.artist, "Artist"));
    if (a) {
        /* Re-read: still not taken. */
        CHECK(rx_library_refresh_track(&lib, a) == 0 && !a->file_meta.comment && !a->meta.comment);
        sqlite3_stmt *st;
        sqlite3_prepare_v2(lib.db->db, "SELECT count(*) FROM track WHERE file_comment IS NULL "
                                       "AND comment IS NULL", -1, &st, NULL);
        CHECK(sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int(st, 0) == 1);
        sqlite3_finalize(st);
        /* The field is free for the user. */
        rx_meta_set(&a->meta.comment, "mine");
        CHECK(rx_library_save_track(&lib, a) == 0);
    }

    /* Asked for: a folder's files bring their comments. */
    lib.cfg.import_comment = true;
    CHECK(rx_library_config_save(&lib) == 0);
    rx_library_close(&lib);
    CHECK(rx_library_open(&lib, libdir) == 0);
    CHECK(lib.cfg.import_comment);
    char *sub = path_in(files, "sub");
    CHECK(rx_library_add_path(&lib, sub, NULL, NULL) == 1);
    free(sub);
    RXTrack *b = rx_library_find_path(&lib, b_path);
    CHECK(b && b->file_meta.comment && !strcmp(b->file_meta.comment, "b's comment"));
    CHECK(b && b->meta.comment && !strcmp(b->meta.comment, "b's comment"));
    a = rx_library_find_path(&lib, a_path);
    CHECK(a && a->meta.comment && !strcmp(a->meta.comment, "mine") && !a->file_meta.comment);

    /* Off again: re-reading b keeps the comment it brought, as a library
     * from before keeps the comments it imported. */
    lib.cfg.import_comment = false;
    if (b) {
        CHECK(rx_library_refresh_track(&lib, b) == 0);
        CHECK(b->file_meta.comment && !strcmp(b->file_meta.comment, "b's comment"));
        CHECK(b->meta.comment && !strcmp(b->meta.comment, "b's comment"));
    }
    rx_library_close(&lib);

    free(a_path);
    free(b_path);
    free(files);
    free(libdir);
    test_rm_rf(dir);
    free(dir);
}

static void test_missing_relocate_remove(void)
{
    RXLibrary lib;
    char *b_path = path_in(music, "b.wav");
    CHECK(unlink(b_path) == 0);
    CHECK(rx_library_open(&lib, root) == 0);
    RXTrack *b = rx_library_find_id(&lib, 2);
    CHECK(b && b->missing && b->meta.rating == 2);
    CHECK(rx_library_rescan_missing(&lib) == 0);
    rx_library_close(&lib);

    /* Still flagged after a reopen, back when the file is. */
    CHECK(rx_library_open(&lib, root) == 0);
    b = rx_library_find_id(&lib, 2);
    CHECK(b && b->missing);
    test_write_wav(b_path, 0.5, 48000, 1, NULL, NULL, NULL);
    CHECK(rx_library_rescan_missing(&lib) == 1);
    CHECK(!b->missing);
    /* Adding a missing track's file brings it back too. */
    unlink(b_path);
    CHECK(rx_library_rescan_missing(&lib) == 1 && b->missing);
    test_write_wav(b_path, 0.5, 48000, 1, NULL, NULL, NULL);
    int err;
    CHECK(rx_library_add_file(&lib, b_path, &err) == b && !b->missing);

    /* The user moved a file. */
    RXTrack *c = rx_library_find_id(&lib, 3);
    char *c_old = rx_strdup(c->path), *c_new = path_in(music, "sub/c moved.wav");
    CHECK(rename(c_old, c_new) == 0);
    CHECK(rx_library_rescan_missing(&lib) == 1 && c->missing);
    CHECK(rx_library_relocate(&lib, c, c_new) == 0);
    CHECK(!c->missing && !strcmp(c->path, c_new));
    CHECK(rx_library_relocate(&lib, c, rx_library_find_id(&lib, 1)->path) == RX_ERROR(EEXIST));
    /* The same file under another spelling is still that track's file:
     * two tracks must never share one. */
    char *a_other = path_in(music, "sub/../a.wav");
    CHECK(rx_library_relocate(&lib, c, a_other) == RX_ERROR(EEXIST));
    CHECK(!strcmp(c->path, c_new));
    char *a_link = path_in(music, "sub/link to a.wav");
    CHECK(symlink(rx_library_find_id(&lib, 1)->path, a_link) == 0);
    CHECK(rx_library_relocate(&lib, c, a_link) == RX_ERROR(EEXIST));
    unlink(a_link);
    free(a_link);
    free(a_other);
    rx_library_close(&lib);
    CHECK(rx_library_open(&lib, root) == 0);
    c = rx_library_find_id(&lib, 3);
    CHECK(c && !strcmp(c->path, c_new) && !c->missing);

    /* Removing takes everything that refers to the track. */
    RXTrack *a = rx_library_find_id(&lib, 1);
    RXPlaylist *pl = rx_library_playlist_create(&lib, NULL, "Set");
    CHECK(pl && rx_library_playlist_add(&lib, pl, a, -1) == 0);
    CHECK(rx_library_playlist_add(&lib, pl, c, -1) == 0);
    char *cache = rx_library_cache_path(&lib, "transcode", 1, "flac");
    CHECK(cache && rx_str_ends(cache, "/cache/transcode/1.flac"));
    FILE *f = fopen(cache, "w");
    fclose(f);
    CHECK(rx_library_remove(&lib, a) == 0);
    CHECK(!rx_library_find_id(&lib, 1) && lib.nb_tracks == 2);
    CHECK(!rx_file_exists(cache));
    CHECK(pl->nb == 1 && pl->track_ids[0] == 3);
    static const char *const tables[] = { "cue", "grid_section", "track_tag", "waveform_preview",
                                          "waveform_detail", "track_artwork", "play",
                                          "playlist_entry" };
    for (size_t i = 0; i < RX_ARRAY_ELEMS(tables); i++) {
        if (count_rows(&lib, tables[i], 1) != 0) {
            fprintf(stderr, "%s still has rows of a removed track\n", tables[i]);
            test_failures++;
        }
    }
    RXTrack *stranger = rx_track_new("/not/in/the/library.wav");
    stranger->id = 1;
    CHECK(rx_library_remove(&lib, stranger) == RX_ERROR(ENOENT));
    CHECK(rx_library_save_track(&lib, stranger) == RX_ERROR(ENOENT));
    rx_track_free(stranger);
    CHECK(rx_library_check(&lib) == 0);
    rx_library_close(&lib);

    CHECK(rx_library_open(&lib, root) == 0);
    pl = rx_library_playlist_find(&lib, "Set");
    CHECK(pl && pl->nb == 1 && pl->track_ids[0] == 3);

    /* Ids are never handed out twice: remove the newest, add another. */
    c = rx_library_find_id(&lib, 3);
    CHECK(rx_library_remove(&lib, c) == 0);
    char *d = path_in(music, "d.wav");
    test_write_wav(d, 0.25, 44100, 2, NULL, NULL, NULL);
    RXTrack *t = rx_library_add_file(&lib, d, &err);
    CHECK(t && t->id == 4);
    CHECK(rx_library_check(&lib) == 0);
    rx_library_close(&lib);
    free(d);
    free(cache);
    free(c_old);
    free(c_new);
    free(b_path);
}

/* A database from a newer rxbox is left alone; a foreign one too. */
static void test_refusals(void)
{
    char *dir = test_dir("library-newer");
    char *db = path_in(dir, "library.db");
    sqlite3 *s;
    sqlite3_open(db, &s);
    char sql[96];
    snprintf(sql, sizeof(sql), "PRAGMA application_id = %d; PRAGMA user_version = 99", 0x52584258);
    sqlite3_exec(s, sql, NULL, NULL, NULL);
    sqlite3_close(s);
    RXLibrary lib;
    CHECK(rx_library_open(&lib, dir) == RX_ERROR(EPROTO));
    free(db);
    test_rm_rf(dir);
    free(dir);

    dir = test_dir("library-foreign");
    db = path_in(dir, "library.db");
    sqlite3_open(db, &s);
    sqlite3_exec(s, "PRAGMA application_id = 7; CREATE TABLE x (y)", NULL, NULL, NULL);
    sqlite3_close(s);
    CHECK(rx_library_open(&lib, dir) == RX_ERROR(EINVAL));
    sqlite3_open(db, &s);
    sqlite3_exec(s, "PRAGMA application_id = 0", NULL, NULL, NULL);
    sqlite3_close(s);
    CHECK(rx_library_open(&lib, dir) == RX_ERROR(EINVAL));
    /* Refused before anything is written: still in its own journal mode. */
    sqlite3_open(db, &s);
    sqlite3_stmt *st;
    sqlite3_prepare_v2(s, "PRAGMA journal_mode", -1, &st, NULL);
    CHECK(sqlite3_step(st) == SQLITE_ROW && !strcmp((const char *)sqlite3_column_text(st, 0), "delete"));
    sqlite3_finalize(st);
    sqlite3_close(s);
    free(db);
    test_rm_rf(dir);
    free(dir);

    /* $RXBOX_LIBRARY, else ~/.local/share/rxbox. */
    setenv("RXBOX_LIBRARY", "/some/where", 1);
    char *r = rx_library_default_root();
    CHECK_STR(r, "/some/where");
    free(r);
    unsetenv("RXBOX_LIBRARY");
    setenv("HOME", "/home/someone", 1);
    r = rx_library_default_root();
    CHECK_STR(r, "/home/someone/.local/share/rxbox");
    free(r);
}

/* SQLite rolls a whole transaction back by itself on some errors (a write
 * interrupted before its statement journal opened, SQLITE_FULL or
 * SQLITE_IOERR without one). A batch that loses its transaction that way
 * must not go on writing in autocommit mode: every file of it is reported
 * failed, so none of them may be in the database afterwards either. The
 * statement after the second track row is interrupted as it starts. */
typedef struct Interrupt {
    int inserts;
    bool armed;
    int fired;
} Interrupt;

static void count_inserts(void *opaque, int op, const char *db, const char *table, sqlite3_int64 rowid)
{
    Interrupt *in = opaque;
    if (op == SQLITE_INSERT && !strcmp(table, "track"))
        in->inserts++;
}

static int arm_at_statement(unsigned type, void *opaque, void *stmt, void *sql)
{
    Interrupt *in = opaque;
    if (in->inserts == 2 && !in->fired)
        in->armed = true;
    return 0;
}

static int interrupt_once(void *opaque)
{
    Interrupt *in = opaque;
    if (!in->armed)
        return 0;
    in->armed = false;
    in->fired++;
    return 1;
}

static int track_rows(const RXLibrary *lib)
{
    sqlite3_stmt *st;
    int n = -1;
    if (sqlite3_prepare_v2(lib->db->db, "SELECT count(*) FROM track", -1, &st, NULL) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW)
            n = sqlite3_column_int(st, 0);
        sqlite3_finalize(st);
    }
    return n;
}

static void test_lost_transaction(void)
{
    char *dir = test_dir("library-lost");
    char *files = path_in(dir, "music"), *lroot = path_in(dir, "lib");
    static const char *const names[] = { "a.wav", "b.wav", "c.wav", "d.wav" };
    for (size_t i = 0; i < RX_ARRAY_ELEMS(names); i++) {
        char *p = path_in(files, names[i]);
        test_write_wav(p, 0.1, 44100, 1, NULL, NULL, NULL);
        free(p);
    }

    RXLibrary lib;
    CHECK(rx_library_open(&lib, lroot) == 0);
    Interrupt in = { 0 };
    sqlite3_update_hook(lib.db->db, count_inserts, &in);
    sqlite3_trace_v2(lib.db->db, SQLITE_TRACE_STMT, arm_at_statement, &in);
    sqlite3_progress_handler(lib.db->db, 1, interrupt_once, &in);
    AddLog log = { 0 };
    int n = rx_library_add_path(&lib, files, add_cb, &log);
    sqlite3_progress_handler(lib.db->db, 0, NULL, NULL);
    sqlite3_trace_v2(lib.db->db, 0, NULL, NULL);
    sqlite3_update_hook(lib.db->db, NULL, NULL);
    CHECK(in.fired == 1);
    CHECK(n < 0 && lib.nb_tracks == 0 && log.failed == 4 && !log.reported);
    CHECK(track_rows(&lib) == 0);
    CHECK(lib.db->depth == 0);

    /* And the library works on: the same files go in normally. */
    log = (AddLog) { 0 };
    CHECK(rx_library_add_path(&lib, files, add_cb, &log) == 4 && lib.nb_tracks == 4);
    CHECK(track_rows(&lib) == 4 && log.reported == 4);
    rx_library_close(&lib);
    CHECK(rx_library_open(&lib, lroot) == 0 && lib.nb_tracks == 4);
    rx_library_close(&lib);

    test_rm_rf(dir);
    free(files);
    free(lroot);
    free(dir);
}

/* A Linux path is bytes, not necessarily UTF-8: such a file goes in, comes
 * back from the database byte for byte, and is found by its path. */
static void test_byte_paths(void)
{
    char *dir = test_dir("library-bytes");
    char *files = path_in(dir, "m\xe4sic"), *lroot = path_in(dir, "lib");
    char *p = path_in(files, "\xff\xfe caf\xe9 \x80.wav");
    test_write_wav(p, 0.1, 44100, 1, NULL, NULL, NULL);

    RXLibrary lib;
    CHECK(rx_library_open(&lib, lroot) == 0);
    int err;
    RXTrack *t = rx_library_add_file(&lib, p, &err);
    CHECK(t && !strcmp(t->path, p) && !strcmp(t->meta.title, "\xff\xfe caf\xe9 \x80"));
    rx_library_close(&lib);
    CHECK(rx_library_open(&lib, lroot) == 0 && lib.nb_tracks == 1);
    t = rx_library_find_path(&lib, p);
    CHECK(t && !t->missing && !strcmp(t->meta.title, "\xff\xfe caf\xe9 \x80"));
    CHECK(rx_library_add_file(&lib, p, &err) == t && lib.nb_tracks == 1);
    CHECK(rx_library_check(&lib) == 0);
    rx_library_close(&lib);

    test_rm_rf(dir);
    free(p);
    free(files);
    free(lroot);
    free(dir);
}

/* The export job opens the library on a connection of its own while the
 * interface's goes on committing edits. What the job loads must be one
 * moment of the database: here the interface adds a track to a playlist
 * just as the job starts reading the playlists, after it read the tracks.
 * (An auto extension hooks the job's connection as it is opened.) */
typedef struct Meddle {
    RXLibrary *ui;
    const char *path;
    bool done;
} Meddle;

static Meddle meddle;

static int meddle_at_playlists(unsigned type, void *opaque, void *stmt, void *sql)
{
    const char *text = sqlite3_sql(stmt);
    if (meddle.done || !text || !rx_str_starts(text, "SELECT id, parent_id"))
        return 0;
    meddle.done = true;
    int err;
    RXTrack *t = rx_library_add_file(meddle.ui, meddle.path, &err);
    RXPlaylist *p = rx_library_playlist_find(meddle.ui, "Set");
    if (t && p)
        rx_library_playlist_add(meddle.ui, p, t, -1);
    return 0;
}

static int hook_connection(sqlite3 *db, const char **err, const struct sqlite3_api_routines *api)
{
    sqlite3_trace_v2(db, SQLITE_TRACE_STMT, meddle_at_playlists, NULL);
    return SQLITE_OK;
}

static void test_open_snapshot(void)
{
    char *dir = test_dir("library-snapshot");
    char *files = path_in(dir, "music"), *lroot = path_in(dir, "lib");
    char *a = path_in(files, "a.wav"), *b = path_in(files, "b.wav");
    test_write_wav(a, 0.1, 44100, 1, NULL, NULL, NULL);
    test_write_wav(b, 0.1, 44100, 1, NULL, NULL, NULL);

    RXLibrary ui, job;
    CHECK(rx_library_open(&ui, lroot) == 0);
    int err;
    RXTrack *t = rx_library_add_file(&ui, a, &err);
    RXPlaylist *p = rx_library_playlist_create(&ui, NULL, "Set");
    CHECK(t && p && rx_library_playlist_add(&ui, p, t, -1) == 0);

    meddle = (Meddle) { .ui = &ui, .path = b };
    sqlite3_auto_extension((void (*)(void))hook_connection);
    CHECK(rx_library_open(&job, lroot) == 0);
    sqlite3_cancel_auto_extension((void (*)(void))hook_connection);
    CHECK(meddle.done && ui.nb_tracks == 2 && p->nb == 2);

    RXPlaylist *jp = rx_library_playlist_find(&job, "Set");
    CHECK(jp != NULL);
    for (int i = 0; jp && i < jp->nb; i++) {
        if (!rx_library_find_id(&job, jp->track_ids[i])) {
            fprintf(stderr, "the job's playlist holds track %llu, its track list does not\n",
                    (unsigned long long)jp->track_ids[i]);
            test_failures++;
        }
    }
    rx_library_close(&job);
    rx_library_close(&ui);

    test_rm_rf(dir);
    free(a);
    free(b);
    free(files);
    free(lroot);
    free(dir);
}

/* Two processes starting on a new library at once (the CLI and the
 * interface): whichever creates the schema, the other opens it. */
typedef struct Opener {
    const char *root;
    pthread_barrier_t *go;
    int ret;
} Opener;

static void *open_thread(void *arg)
{
    Opener *o = arg;
    RXLibrary lib;
    pthread_barrier_wait(o->go);
    o->ret = rx_library_open(&lib, o->root);
    if (o->ret >= 0)
        rx_library_close(&lib);
    return NULL;
}

/* A batch whose transaction cannot start (another writer holds the
 * library) reports every file failed and keeps none of them: before,
 * rx_library_add_file leaked the probed track and a folder's files were
 * not reported. A folder whose files are all known takes no write lock:
 * it adds nothing while another writer holds the library. */
static void test_busy_add(void)
{
    char *dir = test_dir("library-busy");
    char *files = path_in(dir, "music"), *lroot = path_in(dir, "lib");
    char *a = path_in(files, "a.wav"), *b = path_in(files, "b.wav");
    CHECK(test_write_wav(a, 0.1, 44100, 1, "A", NULL, NULL) == 0);
    CHECK(test_write_wav(b, 0.1, 44100, 1, "B", NULL, NULL) == 0);
    RXLibrary lib;
    CHECK(rx_library_open(&lib, lroot) == 0);
    sqlite3_busy_timeout(lib.db->db, 50);
    char *db = path_in(lroot, "library.db");
    sqlite3 *s;
    CHECK(sqlite3_open(db, &s) == SQLITE_OK);
    CHECK(sqlite3_exec(s, "BEGIN IMMEDIATE", NULL, NULL, NULL) == SQLITE_OK);

    int err = 0;
    CHECK(!rx_library_add_file(&lib, a, &err) && err == RX_ERROR(EBUSY));
    AddLog log = { 0 };
    CHECK(rx_library_add_path(&lib, files, add_cb, &log) == RX_ERROR(EBUSY));
    CHECK(log.failed == 2 && !log.reported && lib.nb_tracks == 0);

    CHECK(sqlite3_exec(s, "COMMIT", NULL, NULL, NULL) == SQLITE_OK);
    log = (AddLog) { 0 };
    CHECK(rx_library_add_path(&lib, files, add_cb, &log) == 2 && log.reported == 2 && lib.nb_tracks == 2);
    CHECK(sqlite3_exec(s, "BEGIN IMMEDIATE", NULL, NULL, NULL) == SQLITE_OK);
    log = (AddLog) { 0 };
    CHECK(rx_library_add_path(&lib, files, add_cb, &log) == 0 && log.reported == 2 && !log.failed);
    CHECK(sqlite3_exec(s, "COMMIT", NULL, NULL, NULL) == SQLITE_OK);
    sqlite3_close(s);
    rx_library_close(&lib);

    test_rm_rf(dir);
    free(db);
    free(a);
    free(b);
    free(files);
    free(lroot);
    free(dir);
}

/* rx_config_set checks a value against its key and an empty one is the
 * default; a row stored before it checked (an older `rxbox config` took
 * anything) still opens, with the default and a warning, and an empty
 * row is the default without one. A colour's description is at most
 * RX_CONFIG_VIBE_MAX bytes. */
static void test_config_set(void)
{
    RXConfig c;
    rx_config_defaults(&c);
    CHECK(rx_config_set(&c, "analysis.snap_bpm", "maybe") == RX_ERROR(EINVAL) && c.snap_bpm);
    CHECK(rx_config_set(&c, "analysis.snap_bpm", "off") == 0 && !c.snap_bpm);
    CHECK(rx_config_set(&c, "analysis.snap_bpm", "") == 0 && c.snap_bpm);
    CHECK(rx_config_set(&c, "export.pdb", "no") == 0 && !c.export_pdb);
    CHECK(rx_config_set(&c, "export.pdb", NULL) == 0 && c.export_pdb);
    /* The background colour: default or one of the eight, any case. */
    CHECK(rx_config_set(&c, "export.background_color", "Blue") == 0 && c.background_color == 7);
    CHECK(rx_config_set(&c, "export.background_color", "teal") == RX_ERROR(EINVAL) &&
          c.background_color == 7);
    CHECK(rx_config_set(&c, "export.background_color", "default") == 0 && !c.background_color);
    CHECK(rx_config_set(&c, "export.background_color", "pink") == 0 && c.background_color == 1);
    CHECK(rx_config_set(&c, "export.background_color", "") == 0 && !c.background_color);
    CHECK(rx_config_set(&c, "analysis.bpm_min", "fast") == RX_ERROR(EINVAL) && c.bpm_min == 0.0);
    CHECK(rx_config_set(&c, "analysis.bpm_min", "-1") == RX_ERROR(EINVAL));
    CHECK(rx_config_set(&c, "analysis.bpm_min", "85.5") == 0 && c.bpm_min == 85.5);
    CHECK(rx_config_set(&c, "analysis.bpm_min", "") == 0 && c.bpm_min == 0.0);
    CHECK(rx_config_set(&c, "analysis.what", "beats,tempo") == RX_ERROR(EINVAL));
    CHECK(rx_config_set(&c, "analysis.what", "key") == 0 && c.analyse_what == RX_ANALYSE_KEY);
    CHECK(rx_config_set(&c, "analysis.what", "") == 0 && c.analyse_what == RX_ANALYSE_ALL);
    CHECK(rx_config_set(&c, "export.key_notation", "roman") == RX_ERROR(EINVAL));
    CHECK(rx_config_set(&c, "export.key_notation", "open") == 0 && !strcmp(c.key_notation, "open"));
    CHECK(rx_config_set(&c, "ui.detail_bars", "0") == RX_ERROR(EINVAL) && c.detail_bars == 12);
    CHECK(rx_config_set(&c, "ui.detail_bars", "16") == 0 && c.detail_bars == 16);
    CHECK(rx_config_set(&c, "ui.traffic_light", "loud") == RX_ERROR(EINVAL));
    CHECK(rx_config_set(&c, "audio.sink", "sink") == 0 && !strcmp(c.audio_sink, "sink"));
    CHECK(rx_config_set(&c, "audio.sink", "") == 0 && !c.audio_sink);
    CHECK(rx_config_set(&c, "ui.layout", "x") == RX_ERROR(ENOENT));
    CHECK(rx_config_set(&c, "player.quantize", "on") == RX_ERROR(ENOENT));
    CHECK(rx_config_set(&c, "analysis.nothing", "1") == RX_ERROR(ENOENT));
    CHECK(rx_config_takes("analysis.snap_bpm") && !rx_config_takes("ui.layout"));

    char long_vibe[RX_CONFIG_VIBE_MAX + 2];
    memset(long_vibe, 'x', sizeof(long_vibe) - 1);
    long_vibe[sizeof(long_vibe) - 1] = '\0';
    CHECK(rx_config_set(&c, "color.red", long_vibe) == RX_ERROR(E2BIG) && !c.vibes[1]);
    long_vibe[RX_CONFIG_VIBE_MAX] = '\0';
    CHECK(rx_config_set(&c, "color.red", long_vibe) == 0 && strlen(c.vibes[1]) == RX_CONFIG_VIBE_MAX);
    CHECK(rx_config_set(&c, "color.red", "") == 0 && !c.vibes[1]);
    rx_config_free(&c);

    char *dir = test_dir("library-config");
    RXLibrary lib;
    CHECK(rx_library_open(&lib, dir) == 0);
    CHECK(rx_library_setting_set(&lib, "analysis.snap_bpm", "") == 0);
    CHECK(rx_library_setting_set(&lib, "analysis.auto_cues", "maybe") == 0);
    CHECK(rx_library_setting_set(&lib, "ui.detail_bars", "8") == 0);
    rx_library_close(&lib);
    CHECK(rx_library_open(&lib, dir) == 0);
    CHECK(lib.cfg.snap_bpm && lib.cfg.auto_cues && lib.cfg.detail_bars == 8);
    rx_library_close(&lib);
    test_rm_rf(dir);
    free(dir);
}

/* rx_library_find_path keeps a hash of the paths (re-adding a folder of
 * 20000 known files took 5 s scanning the list per file): it finds every
 * track by its path through the index growing, tracks removed and moved,
 * and the library opened again, and nothing by a path no track has. */
static void test_find_path(void)
{
    char *dir = test_dir("library-paths");
    char *files = path_in(dir, "music"), *lroot = path_in(dir, "lib");
    enum { N = 700 };
    for (int i = 0; i < N; i++) {
        char *p = rx_asprintf("%s/%04d.wav", files, i);
        CHECK(test_write_wav(p, 0.01, 8000, 1, NULL, NULL, NULL) == 0);
        free(p);
    }
    RXLibrary lib;
    CHECK(rx_library_open(&lib, lroot) == 0);
    CHECK(rx_library_add_path(&lib, files, NULL, NULL) == N);
    for (int round = 0; round < 2; round++) {
        /* Every third goes, every fifth of the rest moves. */
        for (int i = lib.nb_tracks - 1; round == 0 && i >= 0; i--) {
            RXTrack *t = lib.tracks[i];
            if (i % 3 == 0) {
                CHECK(rx_library_remove(&lib, t) == 0);
            } else if (i % 5 == 0) {
                char *to = rx_asprintf("%s/moved/%d.wav", dir, i);
                CHECK(rx_library_relocate(&lib, t, to) == 0);
                free(to);
            }
        }
        int found = 0, gone = 0;
        for (int i = 0; i < lib.nb_tracks; i++)
            found += rx_library_find_path(&lib, lib.tracks[i]->path) == lib.tracks[i];
        for (int i = 0; i < N; i++) {
            char *p = rx_asprintf("%s/%04d.wav", files, i);
            RXTrack *t = rx_library_find_path(&lib, p);
            gone += !t;
            CHECK(!t || !strcmp(t->path, p));
            free(p);
        }
        CHECK(found == lib.nb_tracks && lib.nb_tracks == N - (N + 2) / 3);
        CHECK(gone == N - lib.nb_tracks + 93);
        CHECK(!rx_library_find_path(&lib, "/nowhere.wav") && !rx_library_find_path(&lib, NULL));
        rx_library_close(&lib);
        CHECK(rx_library_open(&lib, lroot) == 0);
    }
    rx_library_close(&lib);
    test_rm_rf(dir);
    free(files);
    free(lroot);
    free(dir);
}

/* The number of rows of a table. */
static int table_rows(const RXLibrary *lib, const char *table)
{
    char *sql = rx_asprintf("SELECT count(*) FROM %s", table);
    sqlite3_stmt *st;
    int n = -1;
    if (sqlite3_prepare_v2(lib->db->db, sql, -1, &st, NULL) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW)
            n = sqlite3_column_int(st, 0);
        sqlite3_finalize(st);
    }
    free(sql);
    return n;
}

static bool artwork_is(const RXLibrary *lib, uint64_t id, const uint8_t *want, size_t want_size)
{
    uint8_t *data = NULL;
    size_t size = 0;
    int w = 0, h = 0;
    int ret = rx_library_artwork_load(lib, id, &data, &size, &w, &h);
    bool ok = ret == 0 && size == want_size && !memcmp(data, want, size) && w == 240 && h == 240;
    free(data);
    return ok;
}

/* A cover is stored once: the same thumbnail for another track is linked
 * to the stored one, and a cover goes with the last track that has it,
 * whether the track is removed or given another cover. */
static void test_artwork_shared(void)
{
    char *dir = test_dir("library-artwork");
    char *db = path_in(dir, "library.db");
    RXLibrary lib;
    CHECK(rx_library_open(&lib, dir) == 0);
    rx_library_close(&lib);
    sqlite3 *s;
    CHECK(sqlite3_open(db, &s) == SQLITE_OK);
    CHECK(sqlite3_exec(s, "INSERT INTO track (id, path) VALUES (1, '/gone/a.wav'), "
                          "(2, '/gone/b.wav'), (3, '/gone/c.wav')", NULL, NULL, NULL) == SQLITE_OK);
    sqlite3_close(s);

    static const uint8_t album[] = { 0xFF, 0xD8, 0xFF, 0xE0, 1, 2, 3, 0xFF, 0xD9 };
    static const uint8_t single[] = { 0xFF, 0xD8, 0xFF, 0xE0, 4, 5, 6, 0xFF, 0xD9 };
    CHECK(rx_library_open(&lib, dir) == 0);
    CHECK(rx_library_artwork_id(&lib, 1) == 0);
    CHECK(rx_library_artwork_save(&lib, 1, album, sizeof(album), 240, 240) == 0);
    CHECK(rx_library_artwork_save(&lib, 2, album, sizeof(album), 240, 240) == 0);
    CHECK(rx_library_artwork_save(&lib, 3, single, sizeof(single), 240, 240) == 0);
    CHECK(table_rows(&lib, "artwork") == 2 && table_rows(&lib, "track_artwork") == 3);
    int64_t a1 = rx_library_artwork_id(&lib, 1), a3 = rx_library_artwork_id(&lib, 3);
    CHECK(a1 > 0 && a3 > 0 && a1 != a3 && rx_library_artwork_id(&lib, 2) == a1);
    CHECK(artwork_is(&lib, 2, album, sizeof(album)) && artwork_is(&lib, 3, single, sizeof(single)));
    /* Saved again: the same row. */
    CHECK(rx_library_artwork_save(&lib, 1, album, sizeof(album), 240, 240) == 0);
    CHECK(table_rows(&lib, "artwork") == 2 && rx_library_artwork_id(&lib, 1) == a1);

    /* 1 takes 3's cover: the album's stays for 2. Then 2 does: it goes. */
    CHECK(rx_library_artwork_save(&lib, 1, single, sizeof(single), 240, 240) == 0);
    CHECK(table_rows(&lib, "artwork") == 2 && rx_library_artwork_id(&lib, 1) == a3);
    CHECK(rx_library_artwork_save(&lib, 2, single, sizeof(single), 240, 240) == 0);
    CHECK(table_rows(&lib, "artwork") == 1 && rx_library_artwork_id(&lib, 2) == a3);

    /* Removed tracks: the cover goes with the last. */
    CHECK(rx_library_remove(&lib, rx_library_find_id(&lib, 3)) == 0);
    CHECK(rx_library_remove(&lib, rx_library_find_id(&lib, 1)) == 0);
    CHECK(table_rows(&lib, "artwork") == 1 && artwork_is(&lib, 2, single, sizeof(single)));
    CHECK(rx_library_remove(&lib, rx_library_find_id(&lib, 2)) == 0);
    CHECK(table_rows(&lib, "artwork") == 0 && table_rows(&lib, "track_artwork") == 0);
    CHECK(rx_library_check(&lib) == 0);
    rx_library_close(&lib);

    /* A cover no track has, put there by hand: gone at the next open. */
    CHECK(sqlite3_open(db, &s) == SQLITE_OK);
    CHECK(sqlite3_exec(s, "INSERT INTO artwork (size, jpeg, width, height) "
                          "VALUES (3, X'FFD8D9', 240, 240)", NULL, NULL, NULL) == SQLITE_OK);
    sqlite3_close(s);
    CHECK(rx_library_open(&lib, dir) == 0);
    CHECK(table_rows(&lib, "artwork") == 0);
    rx_library_close(&lib);

    free(db);
    test_rm_rf(dir);
    free(dir);
}

/* Takes a library of this schema back to schema 8, which kept each
 * track's waveforms as one blob, with auto_vacuum off as schema 8 made it. */
static int back_to_schema8(sqlite3 *db)
{
    return sqlite3_exec(db, "DROP TABLE deck_history;"
                            "DROP TABLE waveform_preview; DROP TABLE waveform_detail;"
                            "CREATE TABLE waveform ("
                            "  track_id INTEGER PRIMARY KEY REFERENCES track(id) ON DELETE CASCADE,"
                            "  data     BLOB NOT NULL"
                            ") STRICT;"
                            "PRAGMA auto_vacuum = NONE; VACUUM; PRAGMA user_version = 8",
                        NULL, NULL, NULL);
}

static int pragma(const RXLibrary *lib, const char *sql)
{
    sqlite3_stmt *st;
    int v = -1;
    if (sqlite3_prepare_v2(lib->db->db, sql, -1, &st, NULL) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW)
            v = sqlite3_column_int(st, 0);
        sqlite3_finalize(st);
    }
    return v;
}

/* Schema 8 kept each track's packed waveforms as one blob: opening it
 * cuts each in two where the previews end, every track keeping its
 * waveforms whole; a blob too short to have detail goes (made again by
 * the analysis). The library is vacuumed once into incremental
 * auto_vacuum. An older schema is refused, untouched. */
static void test_convert_v8(void)
{
    char *dir = test_dir("library-v8");
    char *db = path_in(dir, "library.db");
    RXLibrary lib;
    CHECK(rx_library_open(&lib, dir) == 0);
    rx_library_close(&lib);

    size_t n = RX_WAVE_PACKED_PREVIEW + 12;
    uint8_t *whole = malloc(n);
    for (size_t i = 0; i < n; i++)
        whole[i] = (uint8_t)(i * 7 + 3);
    sqlite3 *s;
    CHECK(sqlite3_open(db, &s) == SQLITE_OK);
    CHECK(back_to_schema8(s) == SQLITE_OK);
    CHECK(sqlite3_exec(s, "INSERT INTO track (id, path) "
                          "VALUES (1, '/gone/a.wav'), (2, '/gone/b.wav')",
                       NULL, NULL, NULL) == SQLITE_OK);
    sqlite3_stmt *st;
    sqlite3_prepare_v2(s, "INSERT INTO waveform (track_id, data) VALUES (?1, ?2)", -1, &st, NULL);
    sqlite3_bind_int(st, 1, 1);
    sqlite3_bind_blob(st, 2, whole, (int)n, SQLITE_STATIC);
    CHECK(sqlite3_step(st) == SQLITE_DONE);
    sqlite3_reset(st);
    sqlite3_bind_int(st, 1, 2);
    sqlite3_bind_blob(st, 2, whole, RX_WAVE_PACKED_PREVIEW, SQLITE_STATIC);
    CHECK(sqlite3_step(st) == SQLITE_DONE);
    sqlite3_finalize(st);
    sqlite3_close(s);

    CHECK(rx_library_open(&lib, dir) == 0);
    CHECK(pragma(&lib, "PRAGMA user_version") == 10);
    CHECK(pragma(&lib, "PRAGMA auto_vacuum") == 2);
    CHECK(pragma(&lib, "SELECT count(*) FROM deck_history") == 0);
    CHECK(pragma(&lib, "SELECT count(*) FROM sqlite_schema WHERE name = 'waveform'") == 0);
    RXTrack *a = rx_library_find_id(&lib, 1), *b = rx_library_find_id(&lib, 2);
    CHECK(a && a->has_waveforms && b && !b->has_waveforms);
    uint8_t *data = NULL;
    size_t size = 0;
    CHECK(rx_core_waveform_blob_get(&lib, 1, true, &data, &size) == 0);
    CHECK(size == n && !memcmp(data, whole, n));
    free(data);
    CHECK(rx_core_waveform_blob_get(&lib, 1, false, &data, &size) == 0);
    CHECK(size == RX_WAVE_PACKED_PREVIEW && !memcmp(data, whole, size));
    free(data);
    CHECK(rx_core_waveform_blob_get(&lib, 2, false, &data, &size) == RX_ERROR(ENOENT));
    CHECK(rx_library_check(&lib) == 0);
    rx_library_close(&lib);
    free(whole);

    /* Schema 9: the History made, empty, the rest as it was. */
    CHECK(sqlite3_open(db, &s) == SQLITE_OK);
    CHECK(sqlite3_exec(s, "DROP TABLE deck_history; PRAGMA user_version = 9", NULL, NULL,
                       NULL) == SQLITE_OK);
    sqlite3_close(s);
    CHECK(rx_library_open(&lib, dir) == 0);
    CHECK(pragma(&lib, "PRAGMA user_version") == 10);
    CHECK(pragma(&lib, "SELECT count(*) FROM deck_history") == 0);
    CHECK(rx_library_find_id(&lib, 1) && rx_library_find_id(&lib, 1)->has_waveforms);
    CHECK(rx_library_check(&lib) == 0);
    rx_library_close(&lib);

    /* Schema 7 and older: refused, as they are. */
    CHECK(sqlite3_open(db, &s) == SQLITE_OK);
    CHECK(sqlite3_exec(s, "PRAGMA user_version = 7", NULL, NULL, NULL) == SQLITE_OK);
    sqlite3_close(s);
    CHECK(rx_library_open(&lib, dir) == RX_ERROR(EPROTO));
    CHECK(sqlite3_open(db, &s) == SQLITE_OK);
    sqlite3_prepare_v2(s, "PRAGMA user_version", -1, &st, NULL);
    CHECK(sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int(st, 0) == 7);
    sqlite3_finalize(st);
    sqlite3_close(s);

    free(db);
    test_rm_rf(dir);
    free(dir);
}

/* Free pages go back to the file system at the next open once there are
 * many: tracks with waveforms removed shrink the library. */
static void test_trim(void)
{
    char *dir = test_dir("library-trim");
    char *db = path_in(dir, "library.db");
    RXLibrary lib;
    CHECK(rx_library_open(&lib, dir) == 0);
    rx_library_close(&lib);
    sqlite3 *s;
    CHECK(sqlite3_open(db, &s) == SQLITE_OK);
    CHECK(sqlite3_exec(s, "WITH RECURSIVE n(i) AS "
                          "  (SELECT 1 UNION ALL SELECT i + 1 FROM n WHERE i < 200) "
                          "INSERT INTO track (id, path) SELECT i, '/gone/' || i || '.wav' FROM n",
                       NULL, NULL, NULL) == SQLITE_OK);
    sqlite3_close(s);

    size_t n = RX_WAVE_PACKED_PREVIEW + 20000;
    uint8_t *w = calloc(1, n);
    CHECK(rx_library_open(&lib, dir) == 0);
    for (uint64_t id = 1; id <= 200; id++)
        CHECK(rx_core_waveform_blob_put(&lib, id, w, n, RX_WAVE_PACKED_PREVIEW) == 0);
    int full = pragma(&lib, "PRAGMA page_count");
    while (lib.nb_tracks > 0)
        CHECK(rx_library_remove(&lib, lib.tracks[0]) == 0);
    CHECK(pragma(&lib, "PRAGMA freelist_count") > 256);
    rx_library_close(&lib);
    free(w);

    CHECK(rx_library_open(&lib, dir) == 0);
    CHECK(pragma(&lib, "PRAGMA freelist_count") == 0);
    CHECK(pragma(&lib, "PRAGMA page_count") < full / 10);
    rx_library_close(&lib);

    free(db);
    test_rm_rf(dir);
    free(dir);
}

/* A newer rxbox converting the library while this one opens it (it holds
 * the write lock at schema 99 as this one reads 8 and waits for the
 * lock): this one sees 99 under the lock and refuses, as it does a
 * library newer from the start. */
static void test_newer_meanwhile(void)
{
    char *dir = test_dir("library-newer-meanwhile");
    char *db = path_in(dir, "library.db");
    RXLibrary lib;
    CHECK(rx_library_open(&lib, dir) == 0);
    rx_library_close(&lib);

    sqlite3 *s;
    CHECK(sqlite3_open(db, &s) == SQLITE_OK);
    CHECK(back_to_schema8(s) == SQLITE_OK);
    CHECK(sqlite3_exec(s, "BEGIN IMMEDIATE; PRAGMA user_version = 99", NULL, NULL, NULL) ==
          SQLITE_OK);
    pthread_barrier_t go;
    pthread_barrier_init(&go, NULL, 2);
    Opener o = { .root = dir, .go = &go };
    pthread_t th;
    pthread_create(&th, NULL, open_thread, &o);
    pthread_barrier_wait(&go);
    usleep(500000);
    CHECK(sqlite3_exec(s, "COMMIT", NULL, NULL, NULL) == SQLITE_OK);
    pthread_join(th, NULL);
    pthread_barrier_destroy(&go);
    CHECK(o.ret == RX_ERROR(EPROTO));
    sqlite3_close(s);

    free(db);
    test_rm_rf(dir);
    free(dir);
}

static void test_concurrent_create(void)
{
    char *dir = test_dir("library-race");
    for (int round = 0; round < 8; round++) {
        char *lroot = rx_asprintf("%s/lib%d", dir, round);
        enum { N = 6 };
        pthread_barrier_t go;
        pthread_barrier_init(&go, NULL, N);
        pthread_t th[N];
        Opener o[N];
        for (int i = 0; i < N; i++) {
            o[i] = (Opener) { .root = lroot, .go = &go };
            pthread_create(&th[i], NULL, open_thread, &o[i]);
        }
        int failed = 0;
        for (int i = 0; i < N; i++) {
            pthread_join(th[i], NULL);
            failed += o[i].ret < 0;
        }
        pthread_barrier_destroy(&go);
        CHECK(!failed);
        free(lroot);
    }
    test_rm_rf(dir);
    free(dir);
}

int main(void)
{
    char *dir = test_dir("library");
    if (!dir)
        return 1;
    music = path_in(dir, "music");
    root = path_in(dir, "lib");
    make_music();

    test_create_and_add();
    test_album_order();
    test_edit_and_reopen();
    test_refresh();
    test_changes();
    test_history();
    test_dynamic_grid();
    test_legend();
    test_clean();
    test_comment_import();
    test_missing_relocate_remove();
    test_refusals();
    test_lost_transaction();
    test_byte_paths();
    test_open_snapshot();
    test_concurrent_create();
    test_artwork_shared();
    test_convert_v8();
    test_trim();
    test_newer_meanwhile();
    test_busy_add();
    test_find_path();
    test_config_set();

    if (!test_failures)
        test_rm_rf(dir);
    free(music);
    free(root);
    free(dir);
    return test_done("core_library");
}
