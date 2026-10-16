/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* What the library removes when it opens: entries it does not know or
 * cannot use. An invalid or unknown entry is removed, not kept and worked
 * around: the interface and the export then never meet one, and a track
 * holding one can be saved again (cb_library_save_track refuses an
 * invalid grid section or cue). Each kind is counted and said once, at
 * open; each entry at debug level.
 *
 *  - A track colour that is not one of the eight (a hue between them): no
 *    colour.
 *  - A grid that is not valid (cb_grid_valid: no section, sections out of
 *    order, a tempo out of range, a start that is not a number): no grid,
 *    the lock off, the beats at version 1 (older than any analyser that
 *    wrote one), so the next start analyses them again.
 *  - A cue at a time that is not a number or below 0, on a hot cue slot
 *    that does not exist, a loop that does not end after it starts, or a
 *    second hot cue on one slot (the first in time stays): gone.
 *  - A key that is no key (cb_key_normalise refuses it): none, at version
 *    0, so the next analysis of the track (a load, Analyse all, `cyanbox
 *    analyze`) finds it again.
 *  - Colour probabilities that are not probabilities: none, the legend's
 *    hash with them.
 *  - A cover (artwork) no track has: gone. The triggers remove a cover
 *    with its last track; this takes what they could not have seen (a
 *    library edited by hand).
 *  - Text that is not NFC (tags written on macOS, or a library older than
 *    the rule; src/common/utf8.c): made NFC, the same text, in the track's
 *    texts and tags, its cue names, and the playlists' and folders' names.
 *    Kept NFD, a kana and its dakuten reach a player as two characters.
 *  - Settings: config.c's cb_core_config_clean (a value its key does not
 *    take, a key cyanbox does not know), and for My Settings the formats
 *    module's cb_settings_clean, called by the CLI and the app after the
 *    open (this module does not link the settings tables).
 *
 * Nothing here fails the open: a library on read-only media opens as it
 * is, and the next open tries again. */

#include "core.h"

#include <math.h>
#include <stdio.h>

#include "cyanbox/colors.h"
#include "cyanbox/notation.h"

/* The eight hues from the RGB the colour table holds (cyanbox/colors.h),
 * by the formula of cb_rgb_hue (src/formats/colors.c), which this module
 * does not link. */
static double rgb_hue(uint32_t rgb)
{
    int r = (int)(rgb >> 16 & 0xFF), g = (int)(rgb >> 8 & 0xFF), b = (int)(rgb & 0xFF);
    int max = CB_MAX(r, CB_MAX(g, b)), min = CB_MIN(r, CB_MIN(g, b));
    if (max == min)
        return -1.0;
    double d = max - min, h;
    if (max == r)
        h = (g - b) / d;
    else if (max == g)
        h = (b - r) / d + 2.0;
    else
        h = (r - g) / d + 4.0;
    h *= 60.0;
    return h < 0 ? h + 360.0 : h;
}

/* A hue that is none, or one of the eight to a hundredth of a degree
 * (cb_track_color_of_hue's tolerance). */
static bool hue_known(double hue)
{
    static const uint32_t rgb[CB_TRACK_COLORS] = {
        CB_TRACK_RGB_PINK, CB_TRACK_RGB_RED, CB_TRACK_RGB_ORANGE, CB_TRACK_RGB_YELLOW,
        CB_TRACK_RGB_GREEN, CB_TRACK_RGB_AQUA, CB_TRACK_RGB_BLUE, CB_TRACK_RGB_PURPLE,
    };
    if (!cb_hue_valid(hue))
        return hue == CB_HUE_NONE;
    for (int i = 0; i < CB_TRACK_COLORS; i++) {
        double d = fabs(hue - rgb_hue(rgb[i]));
        if (CB_MIN(d, 360.0 - d) < 0.01)
            return true;
    }
    return false;
}

static bool cue_known(const CBTrack *t, const CBCue *c)
{
    /* Not past the end: before its analysis a track's length is the
     * container's estimate, which can be short (a VBR MP3 without its
     * header), and a cue near the real end would go. */
    if (!isfinite(c->time) || c->time < 0.0)
        return false;
    if (c->hot < -1 || c->hot >= CB_HOT_CUE_SLOTS)
        return false;
    if (cb_cue_is_loop(c) && (!isfinite(c->loop_end) || c->loop_end <= c->time))
        return false;
    return true;
}

typedef struct Counts {
    int colours, grids, cues, keys, probabilities, tracks;  /* tracks: with any of these */
    int texts, nfc_tracks;                                   /* made NFC, and of how many */
} Counts;

/* Removes what `t` holds that is not valid and makes its texts NFC; true
 * when anything changed, *file_tags when its file's tags did (which the
 * track's save does not write). */
static bool clean_track(CBTrack *t, Counts *n, bool *file_tags)
{
    bool changed = false;
    *file_tags = false;
    unsigned long long id = (unsigned long long)t->id;

    if (!hue_known(t->meta.hue)) {
        cb_debug("library: track %llu: colour %g is none of the eight; removed", id, t->meta.hue);
        t->meta.hue = CB_HUE_NONE;
        n->colours++;
        changed = true;
    }

    if (t->grid.nb_sections > 0 && !cb_grid_valid(&t->grid)) {
        cb_debug("library: track %llu: its grid is not valid; removed, analysed again", id);
        cb_grid_free(&t->grid);
        t->grid = (CBGrid) { .beats_per_bar = 4 };
        t->grid_locked = false;
        t->grid_confidence = 0.0f;
        t->grid_flags = 0;
        t->beats_version = 1;
        n->grids++;
        changed = true;
    }

    int kept = 0, hot_seen = 0;
    for (int i = 0; i < t->nb_cues; i++) {
        CBCue *c = &t->cues[i];
        bool ok = cue_known(t, c);
        if (ok && c->hot >= 0) {
            ok = !(hot_seen & (1 << c->hot));
            hot_seen |= 1 << c->hot;
        }
        if (!ok) {
            cb_debug("library: track %llu: cue at %g (slot %d, loop end %g) is not valid; removed",
                     id, c->time, c->hot, c->loop_end);
            n->cues++;
            changed = true;
            continue;
        }
        t->cues[kept++] = *c;
    }
    t->nb_cues = kept;

    if (t->key[0]) {
        char k[sizeof(t->key)];
        if (cb_key_normalise(t->key, k, sizeof(k)) < 0) {
            cb_debug("library: track %llu: key \"%s\" is no key; removed, analysed again", id,
                     t->key);
            t->key[0] = '\0';
            t->key_confidence = 0.0f;
            t->key_version = 0;
            n->keys++;
            changed = true;
        }
    }

    bool probabilities = true, any = false;
    for (int i = 0; i < CB_TRACK_COLORS; i++) {
        float p = t->ml_colors[i];
        if (isnan(p))
            continue;
        any = true;
        probabilities &= isfinite(p) && p >= 0.0f && p <= 1.0f;
    }
    if (any && !probabilities) {
        cb_debug("library: track %llu: its colour probabilities are not probabilities; removed",
                 id);
        for (int i = 0; i < CB_TRACK_COLORS; i++)
            t->ml_colors[i] = NAN;
        t->ml_legend = 0;
        n->probabilities++;
        changed = true;
    }

    if (changed)
        n->tracks++;

    int texts = cb_meta_normalise(&t->meta), file_texts = cb_meta_normalise(&t->file_meta);
    for (int i = 0; i < t->nb_cues; i++) {
        char *name = t->cues[i].name[0] && cb_utf8_valid(t->cues[i].name) &&
                     !cb_utf8_is_nfc(t->cues[i].name)
                     ? cb_utf8_nfc(t->cues[i].name) : NULL;
        if (name) {
            snprintf(t->cues[i].name, sizeof(t->cues[i].name), "%s", name);
            free(name);
            texts++;
        }
    }
    if (texts > 0 || file_texts > 0) {
        cb_debug("library: track %llu: %d text%s made NFC", id, CB_MAX(texts, 0) + CB_MAX(file_texts, 0),
                 CB_MAX(texts, 0) + CB_MAX(file_texts, 0) == 1 ? "" : "s");
        n->texts += CB_MAX(texts, 0) + CB_MAX(file_texts, 0);
        n->nfc_tracks++;
        *file_tags = file_texts > 0;
        changed = true;
    }
    return changed;
}

/* A track's file tags, the texts, as the database keeps them: what
 * cb_library_save_track does not write (they change only when the file
 * is read again). In the caller's transaction. */
static int save_file_tags(CBLibrary *lib, sqlite3_stmt **st, const CBTrack *t)
{
    if (!*st && sqlite3_prepare_v2(lib->db->db,
                                   "UPDATE track SET file_title = ?2, file_artist = ?3, "
                                   "file_album = ?4, file_album_artist = ?5, file_genre = ?6, "
                                   "file_label = ?7, file_remixer = ?8, file_composer = ?9, "
                                   "file_original_artist = ?10, file_comment = ?11 WHERE id = ?1",
                                   -1, st, NULL) != SQLITE_OK)
        return CB_ERROR(EIO);
    const CBMeta *m = &t->file_meta;
    const char *v[10] = { m->title, m->artist, m->album, m->album_artist, m->genre, m->label,
                          m->remixer, m->composer, m->original_artist, m->comment };
    sqlite3_bind_int64(*st, 1, (sqlite3_int64)t->id);
    for (int i = 0; i < 10; i++)
        sqlite3_bind_text(*st, 2 + i, v[i], -1, SQLITE_TRANSIENT);
    int ret = sqlite3_step(*st) == SQLITE_DONE ? 0 : CB_ERROR(EIO);
    sqlite3_reset(*st);
    return ret;
}

/* The playlists' and folders' names as the database keeps them, NFC (the
 * tree in memory is so already: playlist.c). */
static int clean_playlist_names(CBLibrary *lib)
{
    sqlite3_stmt *sel = NULL, *upd = NULL;
    int n = 0;
    if (sqlite3_prepare_v2(lib->db->db, "SELECT id, name FROM playlist", -1, &sel, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(lib->db->db, "UPDATE playlist SET name = ?2 WHERE id = ?1", -1, &upd,
                           NULL) != SQLITE_OK) {
        sqlite3_finalize(sel);
        return 0;
    }
    while (sqlite3_step(sel) == SQLITE_ROW) {
        const char *name = (const char *)sqlite3_column_text(sel, 1);
        if (!name || !cb_utf8_valid(name) || cb_utf8_is_nfc(name))
            continue;
        char *nfc = cb_utf8_nfc(name);
        if (!nfc)
            break;
        sqlite3_bind_int64(upd, 1, sqlite3_column_int64(sel, 0));
        sqlite3_bind_text(upd, 2, nfc, -1, SQLITE_TRANSIENT);
        n += sqlite3_step(upd) == SQLITE_DONE;
        sqlite3_reset(upd);
        free(nfc);
    }
    sqlite3_finalize(sel);
    sqlite3_finalize(upd);
    return n;
}

int cb_core_clean(CBLibrary *lib)
{
    Counts n = { 0 };
    int failed = 0;
    bool open = false;
    sqlite3_stmt *file_st = NULL;
    for (int i = 0; i < lib->nb_tracks; i++) {
        CBTrack *t = lib->tracks[i];
        bool file_tags;
        if (!clean_track(t, &n, &file_tags))
            continue;
        /* One transaction for every track the open cleans. */
        if (!open && cb_db_begin(lib->db) >= 0)
            open = true;
        if (cb_library_save_track(lib, t) < 0 || (file_tags && save_file_tags(lib, &file_st, t) < 0))
            failed++;
    }
    sqlite3_finalize(file_st);
    if (open && cb_db_commit(lib->db) < 0) {
        cb_db_rollback(lib->db);
        failed = n.tracks;
    }

    int covers = 0;
    if (cb_db_exec(lib->db, "DELETE FROM artwork WHERE NOT EXISTS "
                            "(SELECT 1 FROM track_artwork WHERE artwork_id = artwork.id)") >= 0)
        covers = sqlite3_changes(lib->db->db);
    else
        cb_warn("library: the covers no track has could not be removed");

    int settings = 0;
    if (cb_core_config_clean(lib, &settings) < 0)
        cb_warn("library: the settings that are not valid could not be removed");
    int names = clean_playlist_names(lib);
    if (n.texts || names)
        cb_info("library: %d text%s of %d track%s and %d playlist name%s made NFC", n.texts,
                n.texts == 1 ? "" : "s", n.nfc_tracks, n.nfc_tracks == 1 ? "" : "s", names,
                names == 1 ? "" : "s");

    if (n.tracks || covers || settings) {
        cb_info("library: removed what was not valid, from %d track%s: %d colour%s none of the "
                "eight, %d grid%s, %d cue%s, %d key%s, %d set%s of colour probabilities; and %d "
                "cover%s no track has, %d setting%s",
                n.tracks, n.tracks == 1 ? "" : "s", n.colours, n.colours == 1 ? "" : "s", n.grids,
                n.grids == 1 ? "" : "s", n.cues, n.cues == 1 ? "" : "s", n.keys,
                n.keys == 1 ? "" : "s", n.probabilities, n.probabilities == 1 ? "" : "s", covers,
                covers == 1 ? "" : "s", settings, settings == 1 ? "" : "s");
    }
    if (failed)
        cb_warn("library: %d track%s could not be saved cleaned (read-only?); they are clean "
                "in memory, and the next open tries again", failed, failed == 1 ? "" : "s");
    return 0;
}
