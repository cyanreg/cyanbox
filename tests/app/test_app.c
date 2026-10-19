/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* The application without a window, on the null audio backend: a fresh
 * library in a scratch directory, two copies of a 128 BPM WAV added and
 * analysed through the jobs, the analysis applied (grid, waveforms, the
 * automatic cue), what still needs analysing, the rows (search, sort, a
 * playlist), the model's tags applied (a made-up result: the colour it
 * picked from the legend, its probabilities) and the colour sort, loading
 * the deck, rescanning, an export to a directory
 * running through its phases to the end, and reading and inspecting that
 * stick on a worker. Then the edges: an export answers exactly what its
 * dry run asked about although the library changed meanwhile, and asks
 * about a player's unsynced change and the files to delete at once (after
 * one dry run); an export does not wait for analyses of other tracks; a
 * stick read given up on is dropped; a dry run cancelled as it ends
 * exports nothing; an analysis of a file gone since it was added fails
 * with the track named in the status; a load the player cannot take
 * keeps the track the deck has, the 30 s play record (not for a seek loop,
 * not twice), the end of a track: paused there in the collection, played
 * on into the next row of the playlist being viewed with no load (the
 * player moves on by itself), followed there when the view changes as the
 * deck moves on (inside MASTER TEMPO's delay), the next row of any view
 * with PLAY MODE continue, a track removed while loaded and analysed,
 * waveforms of an older format version made again, results of an older
 * analyser made again by "analyse all" and, unasked, by opening the
 * library (a locked grid kept both times), and tearing down with jobs in
 * flight (LeakSanitizer checks the rest).
 *
 * The player is the null backend in manual mode: the ticks advance it.
 *
 * Usage: test_app FILE.wav SCRATCH_DIR LONG.wav (LONG: 40 s) */

#include "cyanbox/app.h"

#include <ftw.h>
#include <math.h>
#include <stdio.h>
#include <unistd.h>

#include <sqlite3.h>

#include "cyanbox/formats.h"

static int failures;

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond);   \
            fprintf(stderr, __VA_ARGS__);                                     \
            fputc('\n', stderr);                                              \
            failures++;                                                       \
        }                                                                     \
    } while (0)

static int remove_one(const char *path, const struct stat *st, int flag, struct FTW *ftw)
{
    remove(path);
    return 0;
}

static void remove_tree(const char *dir)
{
    nftw(dir, remove_one, 16, FTW_DEPTH | FTW_PHYS);
}

static int copy_file(const char *from, const char *to)
{
    uint8_t *data = NULL;
    size_t size = 0;
    int ret = cb_file_read(from, &data, &size);
    if (ret < 0)
        return ret;
    ret = cb_file_write_atomic(to, data, size);
    free(data);
    return ret;
}

/* Ticks (and plays 10 ms) until `done` says so, up to `seconds`. */
static bool tick_until(CBApp *app, bool (*done)(CBApp *app), int seconds)
{
    int64_t t0 = cb_time_us();
    while (cb_time_us() - t0 < seconds * 1000000LL) {
        cb_player_null_advance(app->player, 480);
        cb_app_tick(app);
        if (done(app))
            return true;
        usleep(2000);
    }
    return false;
}

/* Plays `seconds` of the deck in steps of `step`, ticking after each,
 * with `pace_us` of wall time per step; `back_from`/`back_to` (when > 0)
 * seek back whenever the playhead passes `back_from`. */
static void play_for(CBApp *app, double seconds, double step, int pace_us, double back_from,
                     double back_to)
{
    int frames = (int)lround(step * 48000);
    for (double done = 0; done < seconds; done += step) {
        cb_player_null_advance(app->player, frames);
        cb_app_tick(app);
        CBDeckState st;
        cb_deck_state(app->player, 0, &st);
        if (back_from > 0 && st.position >= back_from)
            cb_ctl_seek(&app->decks[0].ctl, back_to);
        if (pace_us)
            usleep(pace_us);
    }
}

/* Makes a track's stored waveforms format version 0 (bytes 4..7 of its
 * preview blob), as a library analysed before a change of the format
 * would hold them. */
static int stale_waveforms(const char *root, uint64_t id)
{
    char *path = cb_path_join(root, "library.db");
    sqlite3 *db = NULL;
    int rc = path ? sqlite3_open(path, &db) : SQLITE_NOMEM;
    free(path);
    sqlite3_stmt *get = NULL, *put = NULL;
    uint8_t *data = NULL;
    int size = 0;
    if (rc == SQLITE_OK)
        rc = sqlite3_prepare_v2(db, "SELECT data FROM waveform_preview WHERE track_id = ?", -1,
                                &get, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_int64(get, 1, (sqlite3_int64)id);
        rc = sqlite3_step(get) == SQLITE_ROW ? SQLITE_OK : SQLITE_ERROR;
    }
    if (rc == SQLITE_OK) {
        size = sqlite3_column_bytes(get, 0);
        data = size > 8 ? malloc(size) : NULL;
        rc = data ? SQLITE_OK : SQLITE_ERROR;
    }
    if (rc == SQLITE_OK) {
        memcpy(data, sqlite3_column_blob(get, 0), size);
        data[4] = data[5] = data[6] = data[7] = 0;
        rc = sqlite3_prepare_v2(db, "UPDATE waveform_preview SET data = ? WHERE track_id = ?", -1,
                                &put, NULL);
    }
    if (rc == SQLITE_OK) {
        sqlite3_bind_blob(put, 1, data, size, SQLITE_TRANSIENT);
        sqlite3_bind_int64(put, 2, (sqlite3_int64)id);
        rc = sqlite3_step(put) == SQLITE_DONE ? SQLITE_OK : SQLITE_ERROR;
    }
    sqlite3_finalize(get);
    sqlite3_finalize(put);
    sqlite3_close(db);
    free(data);
    return rc == SQLITE_OK ? 0 : -1;
}

/* Runs `sql` on the library at `root`, which no app has open. */
static int library_exec(const char *root, const char *sql)
{
    char *path = cb_path_join(root, "library.db");
    sqlite3 *db = NULL;
    int rc = path ? sqlite3_open(path, &db) : SQLITE_NOMEM;
    free(path);
    if (rc == SQLITE_OK)
        rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
    sqlite3_close(db);
    return rc == SQLITE_OK ? 0 : -1;
}

/* b's grid as the test set it by hand: one section at 0.125 s, 127 BPM. */
static bool hand_grid(const CBTrack *t)
{
    return t->grid.nb_sections == 1 && t->grid.sections[0].start == 0.125 &&
           t->grid.sections[0].bpm == 127.0;
}

/* Plays a player that changed a My Settings value on the stick since the
 * export: `key` in PIONEER/MYSETTING.DAT set to `value`. */
static int stick_setting(const char *stick, const char *key, const char *value)
{
    char *path = cb_path_join(stick, "PIONEER/MYSETTING.DAT");
    uint8_t *data = NULL, *out = NULL;
    size_t size = 0, out_size = 0;
    CBKv *kv = NULL;
    int nb = 0;
    int ret = path ? cb_file_read(path, &data, &size) : CB_ERROR(ENOMEM);
    if (ret >= 0)
        ret = cb_settings_decode("MYSETTING.DAT", data, size, &kv, &nb);
    if (ret >= 0) {
        ret = CB_ERROR(ENOENT);
        for (int i = 0; i < nb; i++) {
            if (strcmp(kv[i].key, key))
                continue;
            free(kv[i].value);
            kv[i].value = cb_strdup(value);
            ret = kv[i].value ? 0 : CB_ERROR(ENOMEM);
        }
    }
    if (ret >= 0)
        ret = cb_settings_encode("MYSETTING.DAT", kv, nb, &out, &out_size);
    if (ret >= 0)
        ret = cb_file_write_atomic(path, out, out_size);
    cb_kv_free(kv, nb);
    free(out);
    free(data);
    free(path);
    return ret;
}

static bool deck_idle(CBApp *app)
{
    return !app->decks[0].loading_request;
}

/* The stick copy of an audio file: the file under `dir` named `base`. */
static char *found_path;
static const char *found_base;

static int find_one(const char *path, const struct stat *st, int flag, struct FTW *ftw)
{
    if (flag == FTW_F && !strcmp(path + ftw->base, found_base)) {
        found_path = cb_strdup(path);
        return 1;
    }
    return 0;
}

static char *find_file(const char *dir, const char *base)
{
    found_path = NULL;
    found_base = base;
    nftw(dir, find_one, 16, FTW_PHYS);
    return found_path;
}

static bool analysis_done(CBApp *app)
{
    return !app->analysing_total && !cb_jobs_pending(app->jobs);
}

static bool deck_loaded(CBApp *app)
{
    return app->decks[0].track && !app->decks[0].loading_request;
}

static bool export_done(CBApp *app)
{
    return !app->export_state.running || app->export_state.confirm;
}

static bool stick_done(CBApp *app)
{
    return !app->stick_request;
}

static bool export_cancel_done(CBApp *app)
{
    return !app->export_state.running;
}

/* The track the deck should have moved on to, playing. */
static CBTrack *continue_to;

static bool continued(CBApp *app)
{
    CBDeckState st;
    cb_deck_state(app->player, 0, &st);
    return app->decks[0].track == continue_to && !app->decks[0].loading_request && st.playing;
}

/* That track queued on the deck, its decode in. */
static bool next_queued(CBApp *app)
{
    return continue_to && app->decks[0].next_track_id == continue_to->id &&
           cb_deck_has_next(app->player, 0);
}

/* A My Setting in CBConfig, as the Player settings dialog sets it (the app
 * reads CBConfig; the table is only what the next open reads). */
static void set_my_setting(CBKv **kv, int *nb, const char *key, const char *value)
{
    for (int i = 0; i < *nb; i++) {
        if (strcmp((*kv)[i].key, key))
            continue;
        free((*kv)[i].value);
        (*kv)[i].value = cb_strdup(value);
        return;
    }
    CBKv *n = realloc(*kv, (size_t)(*nb + 1) * sizeof(*n));
    if (!n)
        abort();
    *kv = n;
    n[(*nb)++] = (CBKv) { cb_strdup(key), cb_strdup(value) };
}

static CBTrack *track_named(CBApp *app, const char *base)
{
    for (int i = 0; i < app->lib.nb_tracks; i++) {
        if (!strcmp(cb_path_base(app->lib.tracks[i]->path), base))
            return app->lib.tracks[i];
    }
    return NULL;
}

/* The rows in order, as tracks. */
static bool rows_are(CBApp *app, const CBTrack *first, const CBTrack *second)
{
    cb_app_rebuild_rows(app);
    return app->nb_rows == 2 && app->lib.tracks[app->rows[0]] == first &&
           app->lib.tracks[app->rows[1]] == second;
}

/* What a tagging writes, from a result made up here (the model is not run):
 * the colour it picked as that colour's hue, its eight probabilities and
 * the legend's hash; the genre unless the user typed one; then the table
 * sorted by colour, in the legend's order with no colour last. */
static void test_tags(CBApp *app, CBTrack *a, CBTrack *b)
{
    const char *legend[CB_TRACK_COLORS];
    cb_config_legend(&app->lib.cfg, legend);
    uint32_t hash = cb_ml_legend_hash(legend);
    CHECK(hash && hash == cb_ml_legend_hash(legend), "the legend's hash %08x", hash);
    const char *other[CB_TRACK_COLORS];
    memcpy(other, legend, sizeof(other));
    other[3] = "bright, uplifting, euphoric";
    CHECK(cb_ml_legend_hash(other) != hash, "an edited legend hashes the same");
    other[3] = legend[3];
    other[2] = "energetic and bright";
    other[3] = "driving, bright, uplifting";
    CHECK(cb_ml_legend_hash(other) != hash, "words moved between colours hash the same");

    CBMLTags tags = {
        .genre     = (char *)"Tech House",
        .comment   = (char *)"Instrumental. Dark, driving.",
        .color     = 2,
        .colors    = { 0.04f, 0.61f, 0.21f, 0.05f, 0.03f, 0.01f, 0.02f, 0.03f },
        .legend    = hash,
        .club      = 0.9f,
        .intensity = 0.8f,
        .valence   = 0.3f,
        .vocals    = 0.0f,
    };
    int ret = cb_app_apply_tags(app, a, &tags);
    CHECK(ret == 0, "apply_tags: %s", strerror(-ret));
    CHECK(a->meta.hue == cb_track_color_hue(2) && cb_track_color_of_hue(a->meta.hue) == 2,
          "the model's red is the hue %f", a->meta.hue);
    CHECK(!memcmp(a->ml_colors, tags.colors, sizeof(tags.colors)) && a->ml_legend == hash,
          "the probabilities and the legend");
    CHECK(a->tags_version == CB_ML_TAGS_VERSION && a->ml_club == 0.9f && a->ml_intensity == 0.8f &&
          a->ml_valence == 0.3f && a->ml_vocals == 0.0f, "the measures");
    CHECK(a->meta.genre && !strcmp(a->meta.genre, "Tech House"), "genre %s", a->meta.genre);
    CHECK(a->meta.comment && !strcmp(a->meta.comment, tags.comment), "comment %s", a->meta.comment);

    /* A genre the user typed stays; the colour follows the model's pick. */
    cb_meta_set(&a->meta.genre, "Garage");
    tags.color = 8;
    ret = cb_app_apply_tags(app, a, &tags);
    CHECK(ret == 0 && a->meta.genre && !strcmp(a->meta.genre, "Garage"), "genre %s",
          a->meta.genre);
    CHECK(cb_track_color_of_hue(a->meta.hue) == 8, "purple: %f", a->meta.hue);

    /* The sort: no colour last in both directions; pink (hue 300) before
     * red (hue 0), the legend's order rather than the hue's; blue before
     * purple (the library keeps no hue between the eight). */
    app->sort_column = CB_COL_COLOR;
    app->sort_descending = false;
    b->meta.hue = CB_HUE_NONE;
    CHECK(rows_are(app, a, b), "no colour sorts last");
    app->sort_descending = true;
    CHECK(rows_are(app, a, b), "no colour sorts last, descending");
    app->sort_descending = false;
    a->meta.hue = cb_track_color_hue(2);
    b->meta.hue = cb_track_color_hue(1);
    CHECK(rows_are(app, b, a), "pink before red");
    app->sort_descending = true;
    CHECK(rows_are(app, a, b), "red before pink, descending");
    app->sort_descending = false;
    a->meta.hue = cb_track_color_hue(8);
    b->meta.hue = cb_track_color_hue(7);
    CHECK(rows_are(app, b, a), "blue before purple");

    /* As they were for the rest. */
    a->meta.hue = b->meta.hue = CB_HUE_NONE;
    cb_meta_set(&a->meta.genre, a->file_meta.genre);
    cb_meta_set(&a->meta.comment, a->file_meta.comment);
    a->tags_version = 0;
    a->ml_legend = 0;
    a->ml_club = a->ml_intensity = a->ml_valence = a->ml_vocals = NAN;
    for (int i = 0; i < CB_TRACK_COLORS; i++)
        a->ml_colors[i] = NAN;
    CHECK(cb_library_save_track(&app->lib, a) == 0 && cb_library_save_track(&app->lib, b) == 0,
          "saving a and b");
    app->sort_column = -1;
    cb_app_rebuild_rows(app);
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: test_app FILE.wav SCRATCH_DIR LONG.wav\n");
        return 2;
    }
    const char *wav = argv[1];
    const char *dir = argv[2];
    const char *long_wav = argv[3];

    remove_tree(dir);
    char *music = cb_path_join(dir, "music");
    char *more = cb_path_join(dir, "more");
    char *root = cb_path_join(dir, "lib");
    char *stick = cb_path_join(dir, "stick");
    char *a_wav = cb_path_join(music, "a.wav");
    char *b_wav = cb_path_join(music, "b.wav");
    char *c_wav = cb_path_join(more, "long.wav");
    if (!music || !more || !root || !stick || !a_wav || !b_wav || !c_wav ||
        cb_mkdir_p(music) < 0 || cb_mkdir_p(more) < 0 || cb_mkdir_p(stick) < 0 ||
        copy_file(wav, a_wav) < 0 || copy_file(wav, b_wav) < 0 || copy_file(long_wav, c_wav) < 0) {
        fprintf(stderr, "cannot set up %s\n", dir);
        return 2;
    }

    /* Manual: nothing plays unless the test advances it. */
    CBPlayerOptions po;
    cb_player_options_default(&po);
    po.backend = CB_AUDIO_NULL;
    po.period = -480;

    char *a_on_stick = NULL;
    CBApp app;
    int ret = cb_app_create(&app, root, &po, NULL);
    CHECK(ret == 0, "cb_app_create: %s", strerror(-ret));
    if (ret < 0)
        return 1;
    CHECK(!app.window && app.player && app.jobs, "window %p player %p", (void *)app.window,
          (void *)app.player);
    CHECK(app.lib.nb_tracks == 0 && app.nb_rows == 0, "%d tracks in a new library",
          app.lib.nb_tracks);

    /* Adding queues the analysis; the ticks apply it. */
    ret = cb_app_add_path(&app, music);
    CHECK(ret == 2, "added %d", ret);
    CHECK(app.analysing_total == 2, "analysing %d", app.analysing_total);
    CHECK(tick_until(&app, analysis_done, 300), "the analysis did not finish");
    printf("status: %s\n", app.status);

    CBTrack *a = track_named(&app, "a.wav");
    CBTrack *b = track_named(&app, "b.wav");
    CHECK(a && b, "tracks a %p b %p", (void *)a, (void *)b);

    /* The analysis stored the waveforms as previews and detail apart: the
     * previews alone are the whole's, without its detail. */
    if (a) {
        CBWaveforms whole = { 0 }, previews = { 0 };
        CHECK(cb_library_waveforms_load(&app.lib, a->id, &whole) == 0 && whole.nb_detail > 0,
              "a's waveforms");
        CHECK(cb_library_waveforms_load_previews(&app.lib, a->id, &previews) == 0 &&
              !previews.nb_detail && !previews.pwv7 &&
              !memcmp(previews.pwv6, whole.pwv6, sizeof(whole.pwv6)) &&
              !memcmp(previews.pwav, whole.pwav, sizeof(whole.pwav)) &&
              !memcmp(previews.pwv4, whole.pwv4, sizeof(whole.pwv4)), "a's previews");
        cb_waveforms_free(&whole);
        cb_waveforms_free(&previews);
    }
    if (!a || !b)
        goto end;
    for (int i = 0; i < 2; i++) {
        CBTrack *t = i ? b : a;
        CHECK(t->has_waveforms, "%s has no waveforms", t->path);
        CHECK(t->analysed > 0, "%s not marked analysed", t->path);
        CHECK(fabs(t->duration - 12.0) < 1e-6, "%s lasts %f s", t->path, t->duration);
        CHECK(t->total_samples == 12 * 48000, "%s has %lld samples", t->path,
              (long long)t->total_samples);
        if (cb_grid_valid(&t->grid)) {
            printf("%s: %.3f BPM, key \"%s\", %d cue%s\n", cb_path_base(t->path),
                   cb_grid_bpm(&t->grid), t->key, t->nb_cues, t->nb_cues == 1 ? "" : "s");
            CHECK(!app.lib.cfg.auto_cues || t->nb_cues == 1, "%d cues after the analysis",
                  t->nb_cues);
        } else {
            fprintf(stderr, "warning: %s got no grid (the analyser?)\n", t->path);
        }
    }

    /* Nothing left to do unless forced; waveforms always count. */
    CHECK(cb_app_analysis_needed(&app, a, CB_ANALYSE_ALL, false) ==
          (cb_track_analysed(a) ? 0u : (unsigned)CB_ANALYSE_BEATS) +
          (a->key[0] ? 0u : (unsigned)CB_ANALYSE_KEY),
          "needed %#x", cb_app_analysis_needed(&app, a, CB_ANALYSE_ALL, false));
    CHECK(cb_app_analysis_needed(&app, a, CB_ANALYSE_ALL, true) == CB_ANALYSE_ALL, "forced");
    bool locked = a->grid_locked;
    a->grid_locked = true;
    CHECK(!(cb_app_analysis_needed(&app, a, CB_ANALYSE_BEATS, false) & CB_ANALYSE_BEATS),
          "a locked grid needs beats");
    a->grid_locked = locked;
    a->has_waveforms = false;
    CHECK(cb_app_analysis_needed(&app, a, CB_ANALYSE_KEY, false) & CB_ANALYSE_WAVES,
          "no waveforms and none needed");
    a->has_waveforms = true;
    a->missing = true;
    CHECK(cb_app_analysis_needed(&app, a, CB_ANALYSE_ALL, true) == 0, "a missing file needs work");
    CHECK(cb_app_analyse_track(&app, a, CB_ANALYSE_ALL, true) == CB_ERROR(ENOENT),
          "a missing file analysed");
    CHECK(cb_app_load_deck(&app, 0, a) == CB_ERROR(ENOENT), "a missing file loaded");
    a->missing = false;

    /* Rows: everything, a search on the file name, a sort. `#` is each
     * track's place in the view's own order, whatever the sort and the
     * search. */
    cb_app_tick(&app);
    CHECK(app.nb_rows == 2, "%d rows", app.nb_rows);
    /* Neither has an album: the collection is None, by title (the file's
     * name, the files having no tags). */
    uint32_t num_a = 1, num_b = 2;
    CHECK(app.row_numbers[0] == 1 && app.row_numbers[1] == 2, "numbered %u, %u",
          app.row_numbers[0], app.row_numbers[1]);
    app.sort_descending = true;
    cb_app_rebuild_rows(&app);
    CHECK(app.nb_rows == 2 && app.row_numbers[0] == 2 && app.row_numbers[1] == 1,
          "descending numbered %u, %u", app.row_numbers[0], app.row_numbers[1]);
    app.sort_descending = false;
    snprintf(app.search, sizeof(app.search), "B.WAV");
    cb_app_rebuild_rows(&app);
    CHECK(app.nb_rows == 1 && app.lib.tracks[app.rows[0]] == b, "search: %d rows", app.nb_rows);
    CHECK(app.nb_rows == 1 && app.row_numbers[0] == num_b, "the search numbered b %u, not %u",
          app.row_numbers[0], num_b);
    snprintf(app.search, sizeof(app.search), "a.wav nothing");
    cb_app_rebuild_rows(&app);
    CHECK(app.nb_rows == 0, "two words must both match: %d rows", app.nb_rows);
    app.search[0] = '\0';
    app.sort_column = CB_COL_PATH;
    app.sort_descending = true;
    cb_app_rebuild_rows(&app);
    CHECK(app.nb_rows == 2 && app.lib.tracks[app.rows[0]] == b, "sort by path descending");
    CHECK(app.nb_rows == 2 && app.row_numbers[0] == num_b && app.row_numbers[1] == num_a,
          "sorted by path numbered %u, %u", app.row_numbers[0], app.row_numbers[1]);
    app.sort_descending = false;
    cb_app_rebuild_rows(&app);
    CHECK(app.nb_rows == 2 && app.lib.tracks[app.rows[0]] == a, "sort by path");
    test_tags(&app, a, b);

    /* A playlist shows its own tracks in its order. */
    CBPlaylist *p = cb_library_playlist_create(&app.lib, app.lib.root_folder, "Test");
    CHECK(p, "playlist");
    if (p) {
        CHECK(cb_library_playlist_add(&app.lib, p, b, -1) >= 0, "playlist add");
        app.current_playlist = p;
        app.sort_column = -1;
        cb_app_tick(&app);
        CHECK(app.nb_rows == 1 && app.lib.tracks[app.rows[0]] == b, "playlist rows %d",
              app.nb_rows);
        CHECK(app.nb_rows == 1 && app.row_numbers[0] == 1, "b numbered %u in the playlist",
              app.row_numbers[0]);
        app.current_playlist = NULL;
        app.rows_dirty = true;
        cb_app_tick(&app);
    }

    /* An album is a view of the tracks tagged with it, in track order,
     * listed alphabetically among the albums of two tracks or more, after
     * None (the tracks of no album, or of one no other track has); leaving
     * it goes back to the collection. Two more tracks, x and y, only for
     * this (no analysis), removed at the end. */
    {
        char *x_wav = cb_path_join(dir, "x.wav"), *y_wav = cb_path_join(dir, "y.wav");
        int err = 0;
        CBTrack *x = NULL, *y = NULL;
        if (x_wav && y_wav && copy_file(wav, x_wav) == 0 && copy_file(wav, y_wav) == 0) {
            x = cb_library_add_file(&app.lib, x_wav, &err);
            y = cb_library_add_file(&app.lib, y_wav, &err);
        }
        CHECK(x && y, "adding x and y: %s", strerror(-err));
        free(x_wav);
        free(y_wav);
        if (x && y) {
            free(a->meta.album);
            free(b->meta.album);
            a->meta.album = cb_strdup("Zeta");
            b->meta.album = cb_strdup("Zeta");
            a->meta.track_number = 2;
            b->meta.track_number = 1;
            CHECK(cb_library_save_track(&app.lib, a) == 0 &&
                  cb_library_save_track(&app.lib, b) == 0,
                  "saving the album tags");
            /* x has no album, y one no other track has: None, first. */
            free(y->meta.album);
            y->meta.album = cb_strdup("Solo");
            CHECK(cb_library_save_track(&app.lib, y) == 0, "saving y's album");
            const CBAlbum *albums = NULL;
            int n = cb_app_albums(&app, &albums);
            CHECK(n == 2 && albums && !strcmp(albums[0].name, "") && albums[0].nb == 2 &&
                  !strcmp(albums[1].name, "Zeta") && albums[1].nb == 2, "albums: %d, %s first", n,
                  n ? albums[0].name : "none");
            cb_app_show_album(&app, "");
            CHECK(app.current_album && !app.current_playlist && app.nb_rows == 2 &&
                  app.lib.tracks[app.rows[0]] == x && app.lib.tracks[app.rows[1]] == y,
                  "None rows %d, in the library's order", app.nb_rows);
            CHECK(app.nb_rows == 2 && app.row_numbers[0] == 1 && app.row_numbers[1] == 2,
                  "None numbered %u, %u", app.row_numbers[0], app.row_numbers[1]);
            cb_app_show_album(&app, "Zeta");
            CHECK(app.current_album && !app.current_playlist && app.nb_rows == 2 &&
                  app.lib.tracks[app.rows[0]] == b && app.lib.tracks[app.rows[1]] == a,
                  "album rows %d, in track order", app.nb_rows);
            /* Numbered in the album's order, kept through a sort. */
            CHECK(app.nb_rows == 2 && app.row_numbers[0] == 1 && app.row_numbers[1] == 2,
                  "the album numbered %u, %u", app.row_numbers[0], app.row_numbers[1]);
            app.sort_column = CB_COL_PATH;
            cb_app_rebuild_rows(&app);
            CHECK(app.nb_rows == 2 && app.lib.tracks[app.rows[0]] == a && app.row_numbers[0] == 2 &&
                  app.row_numbers[1] == 1, "the album by path numbered %u, %u", app.row_numbers[0],
                  app.row_numbers[1]);
            app.sort_column = -1;
            cb_app_rebuild_rows(&app);
            /* y joins Zeta: None is x alone. */
            free(y->meta.album);
            y->meta.album = cb_strdup("zeta");
            CHECK(cb_library_save_track(&app.lib, y) == 0, "saving y's album");
            n = cb_app_albums(&app, &albums);
            CHECK(n == 2 && albums[0].nb == 1 && albums[1].nb == 3, "None %d, Zeta %d",
                  n ? albums[0].nb : 0, n > 1 ? albums[1].nb : 0);
            /* Every track in an album of two: no None. */
            free(x->meta.album);
            x->meta.album = cb_strdup("Alpha");
            free(y->meta.album);
            y->meta.album = cb_strdup("Alpha");
            CHECK(cb_library_save_track(&app.lib, x) == 0 &&
                  cb_library_save_track(&app.lib, y) == 0,
                  "saving x's and y's album");
            n = cb_app_albums(&app, &albums);
            CHECK(n == 2 && !strcmp(albums[0].name, "Alpha") && !strcmp(albums[1].name, "Zeta"),
                  "albums alphabetically, no None: %d, %s first", n, n ? albums[0].name : "none");
            /* In natural order: a number in a name counts as a number. */
            free(a->meta.album);
            a->meta.album = cb_strdup("AD:HOUSE 10");
            free(b->meta.album);
            b->meta.album = cb_strdup("AD:HOUSE 10");
            free(x->meta.album);
            x->meta.album = cb_strdup("AD:HOUSE 3");
            free(y->meta.album);
            y->meta.album = cb_strdup("AD:HOUSE 3");
            CHECK(cb_library_save_track(&app.lib, a) == 0 &&
                  cb_library_save_track(&app.lib, b) == 0 &&
                  cb_library_save_track(&app.lib, x) == 0 &&
                  cb_library_save_track(&app.lib, y) == 0,
                  "saving the numbered albums");
            n = cb_app_albums(&app, &albums);
            CHECK(n == 2 && !strcmp(albums[0].name, "AD:HOUSE 3") &&
                  !strcmp(albums[1].name, "AD:HOUSE 10"), "3 before 10: %s first",
                  n ? albums[0].name : "none");
            /* By artist: x's album artist names AD:HOUSE 3's, the artist
             * a and b share AD:HOUSE 10's, which comes first. */
            char *artist_a = a->meta.artist, *artist_b = b->meta.artist;
            a->meta.artist = cb_strdup("Diverse System");
            b->meta.artist = cb_strdup("diverse system");
            free(x->meta.album_artist);
            x->meta.album_artist = cb_strdup("Zed");
            CHECK(cb_library_save_track(&app.lib, a) == 0 &&
                  cb_library_save_track(&app.lib, b) == 0 &&
                  cb_library_save_track(&app.lib, x) == 0, "saving the artists");
            n = cb_app_albums(&app, &albums);
            CHECK(n == 2 && !strcmp(albums[0].name, "AD:HOUSE 10") && albums[0].artist &&
                  !strcmp(albums[0].artist, "Diverse System") && albums[1].artist &&
                  !strcmp(albums[1].artist, "Zed"), "by artist: %s (%s) first",
                  n ? albums[0].name : "none", n && albums[0].artist ? albums[0].artist : "none");
            /* a and b share no artist: their album has none, and goes last. */
            free(b->meta.artist);
            b->meta.artist = cb_strdup("Other");
            CHECK(cb_library_save_track(&app.lib, b) == 0, "saving b's artist");
            n = cb_app_albums(&app, &albums);
            CHECK(n == 2 && !strcmp(albums[0].name, "AD:HOUSE 3") &&
                  !strcmp(albums[1].name, "AD:HOUSE 10") && !albums[1].artist,
                  "no artist last: %s first", n ? albums[0].name : "none");
            free(a->meta.artist);
            free(b->meta.artist);
            a->meta.artist = artist_a;
            b->meta.artist = artist_b;
            free(x->meta.album_artist);
            x->meta.album_artist = NULL;
            CHECK(cb_library_save_track(&app.lib, a) == 0 &&
                  cb_library_save_track(&app.lib, b) == 0 &&
                  cb_library_save_track(&app.lib, x) == 0, "saving the artists back");
            /* The album view follows the tags: a leaves AD:HOUSE 10 for an
             * album of its own, and b is alone in it. */
            cb_app_show_album(&app, "AD:HOUSE 10");
            free(a->meta.album);
            a->meta.album = cb_strdup("Zeta");
            CHECK(cb_library_save_track(&app.lib, a) == 0, "saving a's album");
            cb_app_tick(&app);
            CHECK(app.nb_rows == 1 && app.lib.tracks[app.rows[0]] == b,
                  "the album view follows the tags");
            /* One album whatever the case of its tag. */
            free(b->meta.album);
            b->meta.album = cb_strdup("zeta");
            CHECK(cb_library_save_track(&app.lib, b) == 0, "saving b's album");
            n = cb_app_albums(&app, &albums);
            CHECK(n == 2 && !strcmp(albums[1].name, "Zeta") && albums[1].nb == 2,
                  "Zeta and zeta are one album: %d", n);
            cb_app_show_album(&app, "Zeta");
            CHECK(app.nb_rows == 2, "both in the album view: %d", app.nb_rows);
            cb_app_show_album(&app, NULL);
            CHECK(!app.current_album && app.nb_rows == 4, "back to the collection: %d rows",
                  app.nb_rows);
            /* The collection is the albums one after the other: None's
             * tracks by title, then each album in the list's order (by
             * artist), each in track order. */
            free(a->meta.album_artist);
            a->meta.album_artist = cb_strdup("Beta Artist");
            free(y->meta.album);
            y->meta.album = cb_strdup("Omega");
            free(x->meta.album);
            x->meta.album = cb_strdup("Omega");
            free(x->meta.album_artist);
            x->meta.album_artist = cb_strdup("Alpha Artist");
            y->meta.track_number = 1;
            x->meta.track_number = 2;
            CHECK(cb_library_save_track(&app.lib, a) == 0 &&
                  cb_library_save_track(&app.lib, x) == 0 &&
                  cb_library_save_track(&app.lib, y) == 0, "saving the two albums");
            cb_app_tick(&app);
            CHECK(app.nb_rows == 4 && app.lib.tracks[app.rows[0]] == y &&
                  app.lib.tracks[app.rows[1]] == x && app.lib.tracks[app.rows[2]] == b &&
                  app.lib.tracks[app.rows[3]] == a && app.row_numbers[0] == 1 &&
                  app.row_numbers[3] == 4, "the collection is not Omega, then Zeta");
            free(x->meta.album);
            x->meta.album = NULL;
            free(x->meta.title);
            x->meta.title = cb_strdup("Zulu");
            free(y->meta.album);
            y->meta.album = NULL;
            free(y->meta.title);
            y->meta.title = cb_strdup("Alpha");
            CHECK(cb_library_save_track(&app.lib, x) == 0 &&
                  cb_library_save_track(&app.lib, y) == 0, "saving x and y in None");
            cb_app_tick(&app);
            CHECK(app.nb_rows == 4 && app.lib.tracks[app.rows[0]] == y &&
                  app.lib.tracks[app.rows[1]] == x && app.lib.tracks[app.rows[2]] == b &&
                  app.lib.tracks[app.rows[3]] == a, "the collection is not None, then Zeta");
            cb_app_show_album(&app, "");
            CHECK(app.nb_rows == 2 && app.lib.tracks[app.rows[0]] == y &&
                  app.lib.tracks[app.rows[1]] == x, "None is not by title");

            /* Album Artists: None first, None's tracks; then the artists
             * of the albums, each a view of its albums' tracks. */
            const CBAlbumArtist *artists = NULL;
            n = cb_app_album_artists(&app, &artists);
            CHECK(n == 2 && !strcmp(artists[0].name, "") && artists[0].nb == 2 &&
                  !strcmp(artists[1].name, "Beta Artist") && artists[1].nb == 2,
                  "album artists: %d, %s first", n, n ? artists[0].name : "none");
            cb_app_show_album_artist(&app, "beta artist");
            CHECK(app.current_album_artist && !app.current_album && app.nb_rows == 2 &&
                  app.lib.tracks[app.rows[0]] == b && app.lib.tracks[app.rows[1]] == a,
                  "the album artist's rows %d, in track order", app.nb_rows);
            /* Its albums one after the other as Albums lists them: Omega
             * (y's album artist, the same but for case) before Zeta. */
            free(x->meta.album);
            x->meta.album = cb_strdup("Omega");
            free(y->meta.album);
            y->meta.album = cb_strdup("Omega");
            free(x->meta.album_artist);
            x->meta.album_artist = NULL;
            free(y->meta.album_artist);
            y->meta.album_artist = cb_strdup("BETA ARTIST");
            CHECK(cb_library_save_track(&app.lib, x) == 0 &&
                  cb_library_save_track(&app.lib, y) == 0, "saving Omega's artist");
            cb_app_tick(&app);
            n = cb_app_album_artists(&app, &artists);
            CHECK(n == 1 && !strcmp(artists[0].name, "BETA ARTIST") && artists[0].nb == 4,
                  "one album artist whatever the case: %d, %s", n, n ? artists[0].name : "none");
            CHECK(app.nb_rows == 4 && app.lib.tracks[app.rows[0]] == y &&
                  app.lib.tracks[app.rows[1]] == x && app.lib.tracks[app.rows[2]] == b &&
                  app.lib.tracks[app.rows[3]] == a && app.row_numbers[0] == 1 &&
                  app.row_numbers[3] == 4, "the album artist is not Omega, then Zeta");
            /* The albums of no artist are None's, after None's own tracks:
             * x leaves Omega (y alone in it: None's), and Zeta's tracks
             * share no artist and have no album artist. */
            artist_a = a->meta.artist;
            artist_b = b->meta.artist;
            a->meta.artist = cb_strdup("One");
            b->meta.artist = cb_strdup("Two");
            free(a->meta.album_artist);
            a->meta.album_artist = NULL;
            free(x->meta.album);
            x->meta.album = NULL;
            CHECK(cb_library_save_track(&app.lib, a) == 0 &&
                  cb_library_save_track(&app.lib, b) == 0 &&
                  cb_library_save_track(&app.lib, x) == 0, "saving Zeta with no artist");
            cb_app_show_album_artist(&app, "");
            n = cb_app_album_artists(&app, &artists);
            CHECK(n == 1 && !artists[0].name[0] && artists[0].nb == 4, "None alone: %d", n);
            CHECK(app.nb_rows == 4 && app.lib.tracks[app.rows[0]] == y &&
                  app.lib.tracks[app.rows[1]] == x && app.lib.tracks[app.rows[2]] == b &&
                  app.lib.tracks[app.rows[3]] == a, "None is not None's tracks, then Zeta");
            /* One view at a time. */
            cb_app_show_album(&app, "Zeta");
            CHECK(app.current_album && !app.current_album_artist && app.nb_rows == 2,
                  "the album view kept the album artist");
            cb_app_show_album_artist(&app, "");
            CHECK(!app.current_album && app.current_album_artist && app.nb_rows == 4,
                  "the album artist view kept the album");
            cb_app_show_album_artist(&app, "Nobody");
            CHECK(app.nb_rows == 0, "an album artist of no album: %d rows", app.nb_rows);
            cb_app_show_album(&app, NULL);
            CHECK(!app.current_album && !app.current_album_artist && app.nb_rows == 4,
                  "back to the collection from an album artist: %d rows", app.nb_rows);
            free(a->meta.artist);
            free(b->meta.artist);
            a->meta.artist = artist_a;
            b->meta.artist = artist_b;
            free(y->meta.album);
            y->meta.album = NULL;
            free(y->meta.album_artist);
            y->meta.album_artist = NULL;
            CHECK(cb_library_save_track(&app.lib, a) == 0 &&
                  cb_library_save_track(&app.lib, b) == 0 &&
                  cb_library_save_track(&app.lib, y) == 0, "saving the artists back");

            cb_app_show_album(&app, NULL);
            free(a->meta.album_artist);
            a->meta.album_artist = NULL;
            CHECK(cb_library_save_track(&app.lib, a) == 0, "saving a's album artist back");
            CHECK(cb_library_remove(&app.lib, x) == 0 && cb_library_remove(&app.lib, y) == 0,
                  "removing x and y");
            cb_app_tick(&app);
            CHECK(app.nb_rows == 2, "x and y removed: %d rows", app.nb_rows);
        }
    }

    /* The TAG LIST: none without --pro-link; with it, the tracks added
     * show in its view in the order they were added, and the view follows
     * the list. */
    CHECK(!cb_app_tag_list(&app), "a tag list without --pro-link");
    cb_app_show_tag_list(&app);
    CHECK(!app.current_tag_list, "the tag list shown without --pro-link");
    app.link_state.enabled = true;
    CBLinkTagList *tags = cb_app_tag_list(&app);
    CHECK(tags && cb_app_tag_list(&app) == tags, "no tag list, or another each time");
    if (tags) {
        CHECK(cb_link_tag_list_add(tags, (uint32_t)b->id) == 1 &&
              cb_link_tag_list_add(tags, (uint32_t)a->id) == 1 &&
              cb_link_tag_list_add(tags, (uint32_t)b->id) == 0, "adding to the tag list");
        cb_app_show_tag_list(&app);
        CHECK(app.current_tag_list && !app.current_playlist && !app.current_album &&
              app.nb_rows == 2 && app.lib.tracks[app.rows[0]] == b &&
              app.lib.tracks[app.rows[1]] == a, "the tag list's rows: %d", app.nb_rows);
        cb_link_tag_list_remove(tags, (uint32_t)b->id);
        cb_app_tick(&app);
        cb_app_tick(&app);
        CHECK(app.nb_rows == 1 && app.lib.tracks[app.rows[0]] == a,
              "the tag list view after a removal: %d rows", app.nb_rows);
        cb_app_show_album(&app, NULL);
        CHECK(!app.current_tag_list && app.nb_rows == app.lib.nb_tracks,
              "back to the collection from the tag list: %d rows", app.nb_rows);
        cb_link_tag_list_clear(tags);
    }
    app.link_state.enabled = false;

    /* The History: none without --pro-link; with it every play, a track as
     * often as it was played, the latest first and numbered down (the
     * table's own order backwards until another is picked); each row's
     * play and its set, plays more than 15 minutes apart in sets of their
     * own, a set ending as its last track did; one play taken off, then a
     * track's at once; leaving it puts the order back. */
    CHECK(cb_app_history_count(&app) == 0, "a history count without --pro-link");
    cb_app_show_history(&app);
    CHECK(!app.current_history, "the history shown without --pro-link");
    app.link_state.enabled = true;
    CHECK(cb_library_history_add(&app.lib, a, 1000, 1) == 0 &&
          cb_library_history_add(&app.lib, b, 1600, 2) == 0 &&
          cb_library_history_add(&app.lib, a, 3000, 0) == 0, "adding to the history");
    CHECK(cb_app_history_count(&app) == 3, "history count %d", cb_app_history_count(&app));
    cb_app_show_history(&app);
    CHECK(app.current_history && !app.current_tag_list && !app.current_playlist &&
          app.sort_column < 0 && app.sort_descending, "the history's view and order");
    CHECK(app.nb_rows == 3 && app.lib.tracks[app.rows[0]] == a && app.row_numbers[0] == 3 &&
          app.lib.tracks[app.rows[1]] == b && app.row_numbers[1] == 2 &&
          app.lib.tracks[app.rows[2]] == a && app.row_numbers[2] == 1,
          "the history's rows: %d, latest first, a twice", app.nb_rows);
    int64_t plays[3] = { 0 };
    int sets[3] = { -1, -1, -1 };
    for (int r = 0; r < 3; r++)
        CHECK(cb_app_history_row(&app, r, &plays[r], &sets[r]), "row %d's play", r);
    CHECK(plays[0] && plays[1] && plays[2] && plays[0] != plays[2] && sets[0] == 1 &&
          sets[1] == 0 && sets[2] == 0, "the plays' sets: %d %d %d", sets[0], sets[1], sets[2]);
    int64_t s0, e0, s1, e1;
    CHECK(cb_app_history_set_times(&app, 0, &s0, &e0) && cb_app_history_set_times(&app, 1, &s1, &e1) &&
          !cb_app_history_set_times(&app, 2, &s1, &e1), "the sets' times");
    CHECK(s0 == 1000 && e0 == 1600 + (int64_t)ceil(b->duration) && s1 == 3000 && e1 == 3000,
          "the sets: %lld-%lld, %lld-%lld", (long long)s0, (long long)e0, (long long)s1,
          (long long)e1);
    CHECK(!cb_app_history_row(&app, 3, NULL, NULL), "a row past the end");
    CHECK(cb_library_history_remove_entry(&app.lib, plays[0]) == 1, "a's latest play");
    cb_app_tick(&app);
    CHECK(app.nb_rows == 2 && app.lib.tracks[app.rows[0]] == b &&
          app.lib.tracks[app.rows[1]] == a && cb_app_history_count(&app) == 2,
          "the history after a play's removal: %d rows", app.nb_rows);
    CHECK(cb_library_history_remove(&app.lib, a->id) == 1, "a's history rows");
    cb_app_tick(&app);
    CHECK(app.nb_rows == 1 && app.lib.tracks[app.rows[0]] == b && cb_app_history_count(&app) == 1,
          "the history after a removal: %d rows", app.nb_rows);
    cb_app_show_album(&app, NULL);
    CHECK(!app.current_history && !app.sort_descending, "leaving the history");
    CHECK(cb_library_history_remove(&app.lib, b->id) == 1, "b's history row");
    app.link_state.enabled = false;

    /* The deck: decoded on the io queue, waveforms from the library. */
    ret = cb_app_load_deck(&app, 0, b);
    CHECK(ret == 0, "load: %s", strerror(-ret));
    CHECK(tick_until(&app, deck_loaded, 60), "the deck did not load");
    CHECK(app.decks[0].track == b, "deck holds %p", (void *)app.decks[0].track);
    CHECK(cb_deck_loaded(app.player, 0), "the player has no track");
    CHECK(app.decks[0].waves.nb_detail > 0, "the deck has no waveforms");
    cb_app_unload_deck(&app, 0);
    CHECK(!app.decks[0].track && !cb_deck_loaded(app.player, 0), "unload");

    CHECK(cb_app_rescan(&app) == 0, "rescan changed something");

    /* The export runs its phases to the end or to a question. */
    snprintf(app.export_state.dest, sizeof(app.export_state.dest), "%s", stick);
    ret = cb_app_start_export(&app);
    CHECK(ret == 0, "start export: %s", strerror(-ret));
    CHECK(tick_until(&app, export_done, 300), "the export did not finish");
    if (app.export_state.confirm) {
        printf("export asks: %d to prune, %d unsynced\n", app.export_state.nb_prune,
               app.export_state.unsynced);
        app.export_state.prune = false;
        app.export_state.ignore_unsynced = true;
        cb_app_start_export(&app);
        CHECK(tick_until(&app, export_done, 300), "the export did not finish");
    }
    printf("export: %s, result %d (%s)\n%s", app.export_state.stage, app.export_state.result,
           strerror(-app.export_state.result), app.export_state.log);
    CHECK(app.export_state.result >= 0, "the export failed");
    /* The "Test" playlist holds b alone. */
    CHECK(app.export_state.copied == 1 && app.export_state.skipped == 0 &&
          app.export_state.rewritten > 0, "counted %d copied, %d skipped, %d rewritten",
          app.export_state.copied, app.export_state.skipped, app.export_state.rewritten);

    /* The whole library: a joins b on the stick, and with a player setting
     * in the library, My Settings files too. */
    ret = cb_library_setting_set(&app.lib, "player.quantize", "true");
    CHECK(ret >= 0, "set player.quantize: %s", strerror(-ret));
    if (app.export_state.result >= 0) {
        app.export_state.whole_library = true;
        CHECK(cb_app_start_export(&app) == 0, "whole library export");
        CHECK(tick_until(&app, export_done, 300), "the whole library export did not finish");
        CHECK(!app.export_state.confirm && app.export_state.result >= 0 &&
              app.export_state.copied == 1 && app.export_state.skipped == 1,
              "whole library: %s, %d copied, %d skipped", strerror(-app.export_state.result),
              app.export_state.copied, app.export_state.skipped);
        app.export_state.whole_library = false;
    }
    a_on_stick = find_file(stick, "a.wav");
    CHECK(a_on_stick, "a.wav is not on the stick");

    /* Exporting only another playlist: the dry run finds b's files on the
     * stick, asks, and pruning on the answer deletes them, and only them:
     * a taken out of the playlist while the question is open is still
     * exported as the dry run had it (the snapshot), not pruned unasked.
     * A player changed a setting on the stick meanwhile: the one question
     * covers that change and the files to delete, and one answer to both
     * runs the export. */
    CBPlaylist *other = cb_library_playlist_create(&app.lib, app.lib.root_folder, "Other");
    if (app.export_state.result >= 0 && other && cb_library_playlist_add(&app.lib, other, a, -1) >= 0) {
        char *pdb = cb_path_join(stick, "PIONEER/rekordbox/export.pdb");
        CHECK(pdb && cb_file_exists(pdb), "no export.pdb on the stick");
        free(pdb);

        ret = stick_setting(stick, "quantize", "false");
        CHECK(ret >= 0, "change the stick's quantize setting: %s", strerror(-ret));

        CBPlaylist *only[1] = { other };
        app.export_state.playlists = only;
        app.export_state.nb_playlists = 1;
        app.export_state.prune = false;
        app.export_state.ignore_unsynced = false;
        CHECK(cb_app_start_export(&app) == 0, "second export");
        CHECK(tick_until(&app, export_done, 300), "the second export did not finish");
        CHECK(app.export_state.confirm && app.export_state.nb_prune > 0,
              "no question about pruning (confirm %d, %d to prune)", app.export_state.confirm,
              app.export_state.nb_prune);
        CHECK(app.export_state.confirm && app.export_state.unsynced == (ret >= 0),
              "the question counts %d unsynced changes", app.export_state.unsynced);
        printf("export asks: %d to prune, %d unsynced\n%s", app.export_state.nb_prune,
               app.export_state.unsynced, app.export_state.log);
        bool b_listed = false;
        for (int i = 0; i < app.export_state.nb_prune; i++)
            b_listed |= cb_str_ends(app.export_state.prune_list[i], "/b.wav");
        CHECK(b_listed, "b.wav is not in the prune list");
        if (app.export_state.confirm) {
            CHECK(cb_library_playlist_remove_at(&app.lib, other, 0) >= 0, "playlist remove");
            app.export_state.prune = true;
            app.export_state.ignore_unsynced = true;
            CHECK(cb_app_start_export(&app) == 0, "confirm");
            CHECK(tick_until(&app, export_done, 300), "the pruning export did not finish");
            CHECK(!app.export_state.confirm && app.export_state.result >= 0,
                  "pruning export (confirm %d, asked again?): %s", app.export_state.confirm,
                  strerror(-app.export_state.result));
            CHECK(app.export_state.pruned == app.export_state.nb_prune, "%d pruned of %d",
                  app.export_state.pruned, app.export_state.nb_prune);
            for (int i = 0; i < app.export_state.nb_prune; i++) {
                char *f = cb_path_join(stick, app.export_state.prune_list[i] + 1);
                CHECK(f && !cb_file_exists(f), "%s was not pruned", f);
                free(f);
            }
            CHECK(a_on_stick && cb_file_exists(a_on_stick),
                  "%s was deleted although the confirmation did not list it", a_on_stick);
        }
        app.export_state.playlists = NULL;
        app.export_state.nb_playlists = 0;
        app.export_state.ignore_unsynced = false;
    }

    /* The stick read on a worker, one at a time: nothing changed on the
     * stick since the export. */
    ret = cb_app_read_stick(&app, stick);
    CHECK(ret == 0, "read the stick: %s", strerror(-ret));
    CHECK(cb_app_read_stick(&app, stick) == CB_ERROR(EBUSY), "a second read while one runs");
    CHECK(tick_until(&app, stick_done, 60), "the stick read did not finish");
    CBJobResult *r = app.stick_result;
    app.stick_result = NULL;
    CHECK(r && r->kind == CB_JOB_STICK_READ && !r->err && r->stick, "stick read: %s",
          r ? strerror(-r->err) : "no result");
    if (r && r->stick)
        CHECK(r->stick->nb_tracks > 0 && r->nb_sync_items == 0, "%d tracks, %d changes",
              r->stick->nb_tracks, r->nb_sync_items);
    cb_job_result_free(r);

    ret = cb_app_inspect_stick(&app, stick);
    CHECK(ret == 0, "inspect the stick: %s", strerror(-ret));
    CHECK(tick_until(&app, stick_done, 60), "the inspection did not finish");
    r = app.stick_result;
    app.stick_result = NULL;
    CHECK(r && r->kind == CB_JOB_INSPECT && !r->err && r->report && !r->problems,
          "inspect: %s, %d problems", r ? strerror(-r->err) : "no result", r ? r->problems : -1);
    if (r && r->report)
        printf("inspect:\n%s", r->report);
    cb_job_result_free(r);

    /* A read given up on (its dialog closed): another may start at once,
     * and the result that comes is the new one's; one given up on after
     * it finished is freed. */
    ret = cb_app_read_stick(&app, stick);
    uint64_t gave_up = app.stick_request;
    CHECK(ret == 0 && gave_up, "read the stick: %s", strerror(-ret));
    cb_app_cancel_stick(&app, gave_up);
    CHECK(!app.stick_request, "the read is still in flight after giving it up");
    ret = cb_app_read_stick(&app, stick);
    uint64_t wanted = app.stick_request;
    CHECK(ret == 0 && wanted != gave_up, "read again: %s", strerror(-ret));
    CHECK(tick_until(&app, stick_done, 60), "the second read did not finish");
    for (int i = 0; i < 50; i++) {
        cb_app_tick(&app);
        usleep(2000);
    }
    CHECK(app.stick_result && app.stick_result->request == wanted,
          "the result is request %llu, not %llu",
          app.stick_result ? (unsigned long long)app.stick_result->request : 0ULL,
          (unsigned long long)wanted);
    cb_app_cancel_stick(&app, wanted);
    CHECK(!app.stick_result, "a finished read given up on kept its result");

    /* An export waits for its own tracks' analyses, not for the rest of
     * the queue: with a's (not in it) in flight, the dry run starts at the
     * next tick. Cancelled there. */
    uint64_t ida = a->id;
    CHECK(cb_app_analyse_tracks(&app, &ida, 1, CB_ANALYSE_ALL, true) == 1, "queue a's analysis");
    ret = cb_app_start_export(&app);
    CHECK(ret == 0, "start export: %s", strerror(-ret));
    cb_app_tick(&app);
    CHECK(strcmp(app.export_state.stage, "analysis") && app.analysing_total > 0,
          "the export waited for an analysis not its own (stage %s, %d analysing)",
          app.export_state.stage, app.analysing_total);
    app.export_state.cancel = true;
    CHECK(tick_until(&app, export_cancel_done, 60), "the cancelled export did not stop");
    CHECK(tick_until(&app, analysis_done, 300), "a's analysis did not finish");

    /* A dry run cancelled after it got to its end: nothing was exported
     * and the real run never starts. The dry run is given time to finish
     * before the cancel is seen. */
    ret = cb_app_start_export(&app);
    CHECK(ret == 0, "start export: %s", strerror(-ret));
    cb_app_tick(&app);
    cb_app_tick(&app);
    usleep(3000000);
    app.export_state.cancel = true;
    CHECK(tick_until(&app, export_cancel_done, 60), "the cancelled export did not stop");
    CHECK(app.export_state.result == CB_ERROR(ECANCELED) &&
          !strcmp(app.export_state.stage, "cancelled"),
          "a cancelled dry run ended as \"%s\" (%s)", app.export_state.stage,
          strerror(-app.export_state.result));

    /* A load the player cannot take (its command queue full, as when the
     * output stalls) keeps the track the deck has. */
    ret = cb_app_load_deck(&app, 0, b);
    CHECK(ret == 0 && tick_until(&app, deck_loaded, 60) && app.decks[0].track == b, "load b");
    ret = cb_app_load_deck(&app, 0, a);
    CHECK(ret == 0, "load a: %s", strerror(-ret));
    for (int i = 0; i < 3000; i++)
        cb_deck_seek(app.player, 0, 1.0);
    for (int64_t t0 = cb_time_us(); !deck_idle(&app) && cb_time_us() - t0 < 60000000;) {
        cb_app_tick(&app);
        usleep(2000);
    }
    CHECK(app.decks[0].track == b && app.decks[0].ctl.track == b && cb_deck_loaded(app.player, 0),
          "a failed load emptied the deck (track %p, b %p)", (void *)app.decks[0].track, (void *)b);
    printf("status: %s\n", app.status);
    for (int i = 0; i < 100; i++)
        cb_player_null_advance(app.player, 480);
    cb_app_unload_deck(&app, 0);

    /* The play record. A 40 s track, played in a loop between 10 and 15 s
     * for over 30 s of wall time, is not a play; 31 s straight through
     * then is, once. */
    ret = cb_app_add_path(&app, more);
    CHECK(ret == 1, "added %d", ret);
    CHECK(tick_until(&app, analysis_done, 300), "the analysis of the long track did not finish");
    CBTrack *c = track_named(&app, "long.wav");
    CHECK(c && c->duration > 39, "long.wav: %f s", c ? c->duration : 0.0);
    if (c) {
        ret = cb_app_load_deck(&app, 0, c);
        CHECK(ret == 0 && tick_until(&app, deck_loaded, 60) && app.decks[0].track == c,
              "load the long track");
        cb_ctl_seek(&app.decks[0].ctl, 10.0);
        cb_ctl_play_pause(&app.decks[0].ctl);
        play_for(&app, 31.0, 0.01, 10000, 15.0, 10.0);
        CHECK(c->play_count == 0, "a 5 s loop played for 31 s counted as a play");
        cb_ctl_seek(&app.decks[0].ctl, 0.0);
        play_for(&app, 31.0, 0.1, 0, 0, 0);
        CHECK(c->play_count == 1, "31 s played straight through: %d plays", c->play_count);
        cb_ctl_seek(&app.decks[0].ctl, 0.0);
        play_for(&app, 35.0, 0.1, 0, 0, 0);
        CHECK(c->play_count == 1, "one load, played on: %d plays", c->play_count);

        /* The end of a track. In the collection, with PLAY MODE SINGLE
         * (the default), the deck stays paused on the end, nothing
         * queued. */
        cb_ctl_seek(&app.decks[0].ctl, c->duration - 0.5);
        play_for(&app, 1.0, 0.01, 0, 0, 0);
        CBDeckState st;
        cb_deck_state(app.player, 0, &st);
        CHECK(app.decks[0].track == c && !st.playing && st.position > c->duration - 0.06 &&
              !app.decks[0].loading_request && !app.decks[0].next_track_id,
              "single: track %p, playing %d at %f, next %llu", (void *)app.decks[0].track,
              st.playing, st.position, (unsigned long long)app.decks[0].next_track_id);

        /* The playlist being viewed holds c then a: a is queued to follow
         * as c plays its last seconds, and the deck plays on into it by
         * itself (no load: the player moves on, the view follows), from
         * its start; at a's end, the playlist's last, it stops. */
        CBTrack *a_track = track_named(&app, "a.wav");
        CBPlaylist *run = cb_library_playlist_create(&app.lib, app.lib.root_folder, "Run");
        CHECK(run && a_track && cb_library_playlist_add(&app.lib, run, c, -1) >= 0 &&
              cb_library_playlist_add(&app.lib, run, a_track, -1) >= 0, "the Run playlist");
        app.current_playlist = run;
        app.sort_column = -1;
        app.rows_dirty = true;
        cb_app_tick(&app);
        CHECK(app.nb_rows == 2 && app.lib.tracks[app.rows[0]] == c, "Run rows %d", app.nb_rows);
        /* The deck stopped at its end with nothing to follow: showing a
         * playlist that has a track after it does not start it. */
        play_for(&app, 1.0, 0.01, 0, 0, 0);
        cb_deck_state(app.player, 0, &st);
        CHECK(app.decks[0].track == c && !st.playing,
              "a deck stopped at its end started when the view changed: track %p, playing %d",
              (void *)app.decks[0].track, st.playing);
        unsigned adv = st.advanced;
        cb_ctl_seek(&app.decks[0].ctl, c->duration - 0.5);
        cb_ctl_play_pause(&app.decks[0].ctl);
        cb_app_tick(&app);
        CHECK(a_track && app.decks[0].next_track_id == a_track->id, "a is queued to follow: %llu",
              (unsigned long long)app.decks[0].next_track_id);
        continue_to = a_track;
        CHECK(tick_until(&app, continued, 60), "the playlist's next: track %p, want %p",
              (void *)app.decks[0].track, (void *)continue_to);
        cb_deck_state(app.player, 0, &st);
        CHECK(st.advanced == adv + 1 && st.playing && st.position < 1.5,
              "moved on by itself, from the start: advanced %u (was %u), playing %d at %f",
              st.advanced, adv, st.playing, st.position);
        CHECK(app.decks[0].ctl.track == a_track && !app.decks[0].next_track_id,
              "the controls follow, nothing queued yet");
        if (a_track) {
            cb_ctl_seek(&app.decks[0].ctl, a_track->duration - 0.5);
            play_for(&app, 1.0, 0.01, 0, 0, 0);
            cb_deck_state(app.player, 0, &st);
            CHECK(app.decks[0].track == a_track && !st.playing &&
                  st.position > a_track->duration - 0.06 && !app.decks[0].next_track_id,
                  "the playlist's last track stops at its end: playing %d at %f", st.playing,
                  st.position);
        }

        /* The candidate changing as the deck moves on: with MASTER TEMPO
         * on the deck reads a, queued after c, the shifter's delay (120
         * ms) before a is heard. Leaving the playlist 60 ms before c's
         * end cancels a when the deck plays it already: the view follows
         * the deck into a all the same, and nothing is queued after it
         * (the collection, PLAY MODE SINGLE). */
        ret = cb_app_load_deck(&app, 0, c);
        CHECK(ret == 0 && tick_until(&app, deck_loaded, 60) && app.decks[0].track == c,
              "load c again");
        CHECK(cb_deck_set_master_tempo(app.player, 0, true), "master tempo");
        cb_deck_state(app.player, 0, &st);
        adv = st.advanced;
        cb_ctl_seek(&app.decks[0].ctl, c->duration - 0.5);
        cb_ctl_play_pause(&app.decks[0].ctl);
        continue_to = a_track;
        CHECK(tick_until(&app, next_queued, 10), "a is not queued after c");
        for (int i = 0; i < 100; i++) {
            cb_player_null_advance(app.player, 480);
            cb_app_tick(&app);
            cb_deck_state(app.player, 0, &st);
            if (st.position >= c->duration - 0.06)
                break;
        }
        CHECK(app.decks[0].track == c && st.advanced == adv && st.playing && st.has_next &&
              st.position >= c->duration - 0.06, "not at c's end: advanced %u (was %u), at %f",
              st.advanced, adv, st.position);
        app.current_playlist = NULL;
        app.rows_dirty = true;
        cb_app_tick(&app);
        CHECK(tick_until(&app, continued, 5), "the view stayed on %p while the deck plays a",
              (void *)app.decks[0].track);
        cb_deck_state(app.player, 0, &st);
        CHECK(st.advanced == adv + 1 && app.decks[0].ctl.track == a_track &&
              !app.decks[0].next_track_id && !st.has_next,
              "followed a: advanced %u (was %u), next %llu, has_next %d", st.advanced, adv,
              (unsigned long long)app.decks[0].next_track_id, st.has_next);
        if (a_track) {
            cb_ctl_seek(&app.decks[0].ctl, a_track->duration - 0.5);
            play_for(&app, 1.0, 0.01, 0, 0, 0);
        }
        CHECK(cb_deck_set_master_tempo(app.player, 0, false), "master tempo off");

        /* PLAY MODE CONTINUE: in any view, the next row, the first after
         * the last, the same way. */
        app.current_playlist = NULL;
        app.rows_dirty = true;
        set_my_setting(&app.lib.cfg.player, &app.lib.cfg.nb_player, "play_mode", "continue");
        cb_app_rebuild_rows(&app);
        int at = -1;
        for (int i = 0; i < app.nb_rows; i++) {
            if (app.lib.tracks[app.rows[i]] == a_track)
                at = i;
        }
        continue_to = at >= 0 ? app.lib.tracks[app.rows[(at + 1) % app.nb_rows]] : NULL;
        CHECK(continue_to && continue_to != a_track, "no row after a.wav");
        if (a_track) {
            cb_ctl_seek(&app.decks[0].ctl, a_track->duration - 0.5);
            cb_ctl_play_pause(&app.decks[0].ctl);
        }
        CHECK(tick_until(&app, continued, 60), "continue: track %p, want %p",
              (void *)app.decks[0].track, (void *)continue_to);
        CHECK(!app.decks[0].loading_request, "no load was needed");
        set_my_setting(&app.lib.cfg.player, &app.lib.cfg.nb_player, "play_mode", "single");
        cb_app_unload_deck(&app, 0);
    }

    /* A file gone since it was added: its analysis fails, the status names
     * the track and why, and no worker shows it as analysed any more. */
    char *gone_dir = cb_path_join(dir, "gone");
    char *gone_wav = gone_dir ? cb_path_join(gone_dir, "gone.wav") : NULL;
    if (gone_wav && cb_mkdir_p(gone_dir) >= 0 && copy_file(wav, gone_wav) >= 0 &&
        cb_app_add_path(&app, gone_dir) == 1 && tick_until(&app, analysis_done, 300)) {
        CBTrack *g = track_named(&app, "gone.wav");
        unsigned errors = app.last_error_count;
        CHECK(g && remove(gone_wav) == 0, "remove gone.wav");
        CHECK(g && cb_app_analyse_track(&app, g, CB_ANALYSE_ALL, true) == 1, "analyse gone.wav");
        CHECK(tick_until(&app, analysis_done, 300), "the analysis of a gone file never ended");
        printf("status: %s\n", app.status);
        CHECK(strstr(app.status, "Analysis failed for") && strstr(app.status, "gone") &&
              strstr(app.status, "the file is missing"), "status \"%s\"", app.status);
        CHECK(app.last_error_count == errors + 1, "%u errors counted",
              app.last_error_count - errors);
        uint64_t ids[8];
        CHECK(cb_jobs_active_tracks(app.jobs, ids, 8) == 0, "a worker still shows a track");
    } else {
        CHECK(0, "cannot set up %s", gone_wav ? gone_wav : "gone.wav");
    }
    free(gone_wav);
    free(gone_dir);

    /* A track removed from the library while it is loaded and analysed:
     * the deck lets go of it and the result is dropped. */
    ret = cb_app_load_deck(&app, 0, a);
    CHECK(ret == 0 && tick_until(&app, deck_loaded, 60) && app.decks[0].track == a, "load a");
    CHECK(cb_app_analyse_track(&app, a, CB_ANALYSE_ALL, true) == 1, "forced analysis of a");
    uint64_t a_id = a->id;
    CHECK(cb_library_remove(&app.lib, a) >= 0, "remove a");
    a = NULL;
    CHECK(tick_until(&app, analysis_done, 300), "the analysis of a removed track never ended");
    CHECK(!app.decks[0].track && !cb_deck_loaded(app.player, 0) &&
          !cb_library_find_id(&app.lib, a_id), "the deck still holds a removed track");

    /* A library analysed before a change of the waveform format: b's
     * stored waveforms are version 0. They are as good as none, so opening
     * the library queues b for them, and the deck that loads it meanwhile
     * gets the new ones. */
    uint64_t b_id = b->id;
    cb_app_destroy(&app);
    CHECK(stale_waveforms(root, b_id) == 0, "cannot make b's waveforms stale");
    ret = cb_app_create(&app, root, &po, NULL);
    CHECK(ret == 0, "cb_app_create again: %s", strerror(-ret));
    if (ret < 0)
        goto end;
    b = cb_library_find_id(&app.lib, b_id);
    CHECK(b && b->has_waveforms, "b lost its waveforms");
    if (!b)
        goto end;
    CHECK(cb_app_analysis_needed(&app, b, CB_ANALYSE_ALL, false) & CB_ANALYSE_WAVES,
          "stale waveforms need no analysis");
    CHECK(cb_app_analysis_stale(&app, b) == CB_ANALYSE_WAVES && app.analysing_total == 1,
          "opening did not queue waveforms of an older format (stale %#x, %d queued)",
          cb_app_analysis_stale(&app, b), app.analysing_total);
    ret = cb_app_load_deck(&app, 0, b);
    CHECK(ret == 0 && tick_until(&app, deck_loaded, 60), "load b with stale waveforms");
    CHECK(tick_until(&app, analysis_done, 300), "the stale waveforms were not made again");
    cb_app_tick(&app);
    CHECK(app.decks[0].track == b && app.decks[0].waves.nb_detail > 0,
          "the deck has no waveforms after the analysis");
    CHECK(!(cb_app_analysis_needed(&app, b, CB_ANALYSE_ALL, false) & CB_ANALYSE_WAVES),
          "the new waveforms still count as stale");
    cb_app_unload_deck(&app, 0);

    /* Results an older analyser made (version 0, as a library from before
     * the versions holds them) are as good as none: "Analyse all
     * unanalysed", the "what is missing" path, analyses long.wav again for
     * them, and they come back at the current versions. b's grid, set by
     * hand and locked, is as old, and stays as it is at its version. */
    c = track_named(&app, "long.wav");
    CHECK(c && c->has_waveforms, "long.wav lost its waveforms");
    if (!c)
        goto end;
    uint64_t c_id = c->id;
    c->beats_version = c->key_version = c->waves_version = 0;
    /* Its cues as an older analyser left them: the automatic cue on the
     * grid's first downbeat, beside a hot cue and a named memory cue of the
     * DJ's. The new analysis takes the automatic cue off and places it
     * again (where the drums are in: long.wav's kick is there from its
     * first beat), the DJ's beside it. */
    while (c->nb_cues)
        cb_track_remove_cue(c, 0);
    CBCue old_auto, hot_a, mine;
    cb_cue_init(&old_auto, cb_grid_beat_to_time(&c->grid, 0.0));
    cb_cue_attach_to_grid(&old_auto, &c->grid);
    cb_cue_init(&hot_a, 10.0);
    hot_a.hot = 0;
    cb_cue_init(&mine, 20.0);
    snprintf(mine.name, sizeof(mine.name), "mine");
    CHECK(cb_track_add_cue(c, &old_auto) >= 0 && cb_track_add_cue(c, &hot_a) >= 0 &&
          cb_track_add_cue(c, &mine) >= 0, "long.wav's cues");
    const double first_down = old_auto.time;
    cb_app_save_track(&app, c);
    cb_grid_free(&b->grid);
    CHECK(cb_grid_init(&b->grid, 0.125, 127.0) >= 0, "b's grid by hand");
    cb_track_resolve_cues(b);
    b->grid_locked = true;
    b->beats_version = 0;
    cb_app_save_track(&app, b);
    unsigned stale = cb_analysis_stale(c);
    unsigned expect = CB_ANALYSE_WAVES | (cb_track_analysed(c) ? CB_ANALYSE_BEATS : 0u) |
                      (c->key[0] ? CB_ANALYSE_KEY : 0u);
    CHECK(stale == expect, "long.wav's stale stages are %#x, not %#x", stale, expect);
    CHECK((cb_app_analysis_needed(&app, c, CB_ANALYSE_ALL, false) & stale) == stale,
          "stale stages need no analysis: %#x", cb_app_analysis_needed(&app, c, CB_ANALYSE_ALL,
                                                                       false));
    CHECK(cb_analysis_stale(b) & CB_ANALYSE_BEATS, "b's old grid is not stale");
    CHECK(!(cb_app_analysis_needed(&app, b, CB_ANALYSE_ALL, false) & CB_ANALYSE_BEATS),
          "a locked grid needs beats");
    CHECK(cb_app_analyse_all(&app, false) >= 1, "nothing queued for the stale results");
    CHECK(tick_until(&app, analysis_done, 300), "the stale results were not analysed again");
    CHECK(c->beats_version == CB_BEATS_VERSION && c->key_version == CB_KEY_VERSION &&
          c->waves_version == CB_WAVES_VERSION, "long.wav is at versions %d, %d, %d",
          c->beats_version, c->key_version, c->waves_version);
    CHECK(!cb_analysis_stale(c), "long.wav is still stale");
    int plain = 0, named = 0, hot = 0;
    double auto_at = NAN;
    for (int i = 0; i < c->nb_cues; i++) {
        const CBCue *q = &c->cues[i];
        if (cb_cue_is_hot(q))
            hot++;
        else if (!strcmp(q->name, "mine"))
            named++;
        else if (!q->name[0] && cb_cue_is_musical(q))
            plain++, auto_at = q->time;
    }
    CHECK(c->nb_cues == 3 && plain == 1 && named == 1 && hot == 1 &&
          fabs(auto_at - first_down) < 0.05, "long.wav's cues after the analysis: %d (%d automatic "
          "at %.3f, first downbeat %.3f; %d named, %d hot)", c->nb_cues, plain, auto_at, first_down,
          named, hot);
    CHECK(b->grid_locked && b->beats_version == 0 && hand_grid(b),
          "the locked grid was analysed again (version %d, %d sections)", b->beats_version,
          b->grid.nb_sections);
    /* A forced re-analysis of a selection of two keeps the locked grid
     * too: only a track named on its own (the deck's RE-ANALYSE) loses it. */
    uint64_t both[2] = { b->id, c->id };
    CHECK(cb_app_analyse_tracks(&app, both, 2, CB_ANALYSE_ALL, true) == 2, "forced selection");
    CHECK(tick_until(&app, analysis_done, 300), "the forced selection was not analysed");
    CHECK(b->grid_locked && b->beats_version == 0 && hand_grid(b),
          "a forced selection replaced the locked grid (version %d, %d sections)",
          b->beats_version, b->grid.nb_sections);

    /* The same unasked after an upgrade: opening a library with long.wav's
     * results at version 0 queues it, and only it (b's one stale result
     * is its locked grid), with the status saying so. */
    cb_app_destroy(&app);
    char sql[160];
    snprintf(sql, sizeof(sql), "UPDATE track SET beats_version = 0, key_version = 0, "
             "waves_version = 0 WHERE id = %llu", (unsigned long long)c_id);
    CHECK(library_exec(root, sql) == 0, "cannot make long.wav's results old");
    ret = cb_app_create(&app, root, &po, NULL);
    CHECK(ret == 0, "cb_app_create after the upgrade: %s", strerror(-ret));
    if (ret < 0)
        goto end;
    b = cb_library_find_id(&app.lib, b_id);
    c = cb_library_find_id(&app.lib, c_id);
    CHECK(b && c, "b %p, long.wav %p", (void *)b, (void *)c);
    if (!b || !c)
        goto end;
    printf("status: %s\n", app.status);
    CHECK(app.analysing_total == 1 && strstr(app.status, "1 made by an older analyser"),
          "opening queued %d tracks, status \"%s\"", app.analysing_total, app.status);
    CHECK(tick_until(&app, analysis_done, 300), "the stale results were not analysed again");
    CHECK(c->beats_version == CB_BEATS_VERSION && c->key_version == CB_KEY_VERSION &&
          c->waves_version == CB_WAVES_VERSION, "long.wav is at versions %d, %d, %d",
          c->beats_version, c->key_version, c->waves_version);
    CHECK(b->grid_locked && b->beats_version == 0 && hand_grid(b),
          "the locked grid was analysed on opening (version %d)", b->beats_version);

    /* The export analyses what is stale before it writes: long.wav's
     * waveforms, now of an older analyser, are made again first, to a
     * fresh destination (nothing to prune, nothing unsynced). */
    c->waves_version = 0;
    cb_app_save_track(&app, c);
    char *stick2 = cb_path_join(dir, "stick2");
    if (!stick2 || cb_mkdir_p(stick2) < 0) {
        CHECK(0, "cannot make the second stick's directory");
    } else {
        snprintf(app.export_state.dest, sizeof(app.export_state.dest), "%s", stick2);
        app.export_state.whole_library = true;
        app.export_state.prune = false;
        app.export_state.ignore_unsynced = false;
        CHECK(cb_app_start_export(&app) == 0, "export with a stale track");
        CHECK(strstr(app.export_state.log, "Analysing 1 track first"),
              "the export did not analyse the stale track first:\n%s", app.export_state.log);
        CHECK(tick_until(&app, export_done, 300), "the export with a stale track did not finish");
        CHECK(!app.export_state.confirm && app.export_state.result >= 0,
              "export with a stale track: %s (confirm %d)\n%s",
              strerror(-app.export_state.result), app.export_state.confirm,
              app.export_state.log);
        CHECK(c->waves_version == CB_WAVES_VERSION && !cb_app_analysis_stale(&app, c),
              "the export left long.wav's waveforms at version %d", c->waves_version);
        app.export_state.whole_library = false;
    }
    free(stick2);

    /* Torn down with an analysis and a decode in flight, and a decode the
     * deck moved on from. */
    CHECK(cb_app_analyse_track(&app, b, CB_ANALYSE_ALL, true) == 1, "forced analysis of b");
    CHECK(cb_app_load_deck(&app, 0, b) == 0, "load b");
    cb_app_unload_deck(&app, 0);
    CHECK(cb_app_load_deck(&app, 0, b) == 0, "load b again");

end:
    cb_app_destroy(&app);
    CHECK(!app.priv, "destroy left state");
    remove_tree(dir);
    free(a_on_stick);
    free(music);
    free(more);
    free(root);
    free(stick);
    free(a_wav);
    free(b_wav);
    free(c_wav);

    if (failures) {
        fprintf(stderr, "%d failure%s\n", failures, failures == 1 ? "" : "s");
        return 1;
    }
    printf("app: OK\n");
    return 0;
}
