/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* What the library removes when it opens: entries it does not know or
 * cannot use. An invalid or unknown entry is removed, not kept and worked
 * around: the interface and the export then never meet one, and a track
 * holding one can be saved again (rx_library_save_track refuses an
 * invalid grid section or cue). Each kind is counted and said once, at
 * open; each entry at debug level.
 *
 *  - A track colour that is not one of the eight (an older tagging's hue
 *    between them, a hue once set by degrees): no colour.
 *  - A grid that is not valid (rx_grid_valid: no section, sections out of
 *    order, a tempo out of range, a start that is not a number): no grid,
 *    the lock off, the beats at version 1 (older than any analyser that
 *    wrote one), so the next start analyses them again.
 *  - A cue at a time that is not a number or below 0, on a hot cue slot
 *    that does not exist, a loop that does not end after it starts, or a
 *    second hot cue on one slot (the first in time stays): gone.
 *  - A key that is no key (rx_key_normalise refuses it): none, at version
 *    0, so the next analysis of the track (a load, Analyse all, `rxbox
 *    analyze`) finds it again.
 *  - Colour probabilities that are not probabilities: none, the legend's
 *    hash with them.
 *  - A cover (artwork) no track has: gone. The triggers remove a cover
 *    with its last track; this takes what they could not have seen (a
 *    library edited by hand, one from before them).
 *  - Settings: config.c's rx_core_config_clean (a value its key does not
 *    take, a key rxbox does not know), and for My Settings the formats
 *    module's rx_settings_clean, called by the CLI and the app after the
 *    open (this module does not link the settings tables).
 *
 * Nothing here fails the open: a library on read-only media opens as it
 * is, and the next open tries again. */

#include "core.h"

#include <math.h>

#include "rxbox/colors.h"
#include "rxbox/notation.h"

/* The eight hues from the RGB the colour table holds (rxbox/colors.h),
 * by the formula of rx_rgb_hue (src/formats/colors.c), which this module
 * does not link: the schema 4 migration (db.c) does the same in SQL. */
static double rgb_hue(uint32_t rgb)
{
    int r = (int)(rgb >> 16 & 0xFF), g = (int)(rgb >> 8 & 0xFF), b = (int)(rgb & 0xFF);
    int max = RX_MAX(r, RX_MAX(g, b)), min = RX_MIN(r, RX_MIN(g, b));
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
 * (rx_track_color_of_hue's tolerance). */
static bool hue_known(double hue)
{
    static const uint32_t rgb[RX_TRACK_COLORS] = {
        RX_TRACK_RGB_PINK, RX_TRACK_RGB_RED, RX_TRACK_RGB_ORANGE, RX_TRACK_RGB_YELLOW,
        RX_TRACK_RGB_GREEN, RX_TRACK_RGB_AQUA, RX_TRACK_RGB_BLUE, RX_TRACK_RGB_PURPLE,
    };
    if (!rx_hue_valid(hue))
        return hue == RX_HUE_NONE;
    for (int i = 0; i < RX_TRACK_COLORS; i++) {
        double d = fabs(hue - rgb_hue(rgb[i]));
        if (RX_MIN(d, 360.0 - d) < 0.01)
            return true;
    }
    return false;
}

static bool cue_known(const RXTrack *t, const RXCue *c)
{
    /* Not past the end: before its analysis a track's length is the
     * container's estimate, which can be short (a VBR MP3 without its
     * header), and a cue near the real end would go. */
    if (!isfinite(c->time) || c->time < 0.0)
        return false;
    if (c->hot < -1 || c->hot >= RX_HOT_CUE_SLOTS)
        return false;
    if (rx_cue_is_loop(c) && (!isfinite(c->loop_end) || c->loop_end <= c->time))
        return false;
    return true;
}

typedef struct Counts {
    int colours, grids, cues, keys, probabilities, tracks;
} Counts;

/* Removes what `t` holds that is not valid; true when anything went. */
static bool clean_track(RXTrack *t, Counts *n)
{
    bool changed = false;
    unsigned long long id = (unsigned long long)t->id;

    if (!hue_known(t->meta.hue)) {
        rx_debug("library: track %llu: colour %g is none of the eight; removed", id, t->meta.hue);
        t->meta.hue = RX_HUE_NONE;
        n->colours++;
        changed = true;
    }

    if (t->grid.nb_sections > 0 && !rx_grid_valid(&t->grid)) {
        rx_debug("library: track %llu: its grid is not valid; removed, analysed again", id);
        rx_grid_free(&t->grid);
        t->grid = (RXGrid) { .beats_per_bar = 4 };
        t->grid_locked = false;
        t->grid_confidence = 0.0f;
        t->grid_flags = 0;
        t->beats_version = 1;
        n->grids++;
        changed = true;
    }

    int kept = 0, hot_seen = 0;
    for (int i = 0; i < t->nb_cues; i++) {
        RXCue *c = &t->cues[i];
        bool ok = cue_known(t, c);
        if (ok && c->hot >= 0) {
            ok = !(hot_seen & (1 << c->hot));
            hot_seen |= 1 << c->hot;
        }
        if (!ok) {
            rx_debug("library: track %llu: cue at %g (slot %d, loop end %g) is not valid; removed",
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
        if (rx_key_normalise(t->key, k, sizeof(k)) < 0) {
            rx_debug("library: track %llu: key \"%s\" is no key; removed, analysed again", id,
                     t->key);
            t->key[0] = '\0';
            t->key_confidence = 0.0f;
            t->key_version = 0;
            n->keys++;
            changed = true;
        }
    }

    bool probabilities = true, any = false;
    for (int i = 0; i < RX_TRACK_COLORS; i++) {
        float p = t->ml_colors[i];
        if (isnan(p))
            continue;
        any = true;
        probabilities &= isfinite(p) && p >= 0.0f && p <= 1.0f;
    }
    if (any && !probabilities) {
        rx_debug("library: track %llu: its colour probabilities are not probabilities; removed",
                 id);
        for (int i = 0; i < RX_TRACK_COLORS; i++)
            t->ml_colors[i] = NAN;
        t->ml_legend = 0;
        n->probabilities++;
        changed = true;
    }

    if (changed)
        n->tracks++;
    return changed;
}

int rx_core_clean(RXLibrary *lib)
{
    Counts n = { 0 };
    int failed = 0;
    bool open = false;
    for (int i = 0; i < lib->nb_tracks; i++) {
        RXTrack *t = lib->tracks[i];
        if (!clean_track(t, &n))
            continue;
        /* One transaction for every track the open cleans. */
        if (!open && rx_db_begin(lib->db) >= 0)
            open = true;
        if (rx_library_save_track(lib, t) < 0)
            failed++;
    }
    if (open && rx_db_commit(lib->db) < 0) {
        rx_db_rollback(lib->db);
        failed = n.tracks;
    }

    int covers = 0;
    if (rx_db_exec(lib->db, "DELETE FROM artwork WHERE NOT EXISTS "
                            "(SELECT 1 FROM track_artwork WHERE artwork_id = artwork.id)") >= 0)
        covers = sqlite3_changes(lib->db->db);
    else
        rx_warn("library: the covers no track has could not be removed");

    int settings = 0;
    if (rx_core_config_clean(lib, &settings) < 0)
        rx_warn("library: the settings that are not valid could not be removed");

    if (n.tracks || covers || settings) {
        rx_info("library: removed what was not valid: %d colour%s none of the eight, %d grid%s, "
                "%d cue%s, %d key%s, %d set%s of colour probabilities (%d track%s), %d cover%s "
                "no track has, %d setting%s",
                n.colours, n.colours == 1 ? "" : "s", n.grids, n.grids == 1 ? "" : "s", n.cues,
                n.cues == 1 ? "" : "s", n.keys, n.keys == 1 ? "" : "s", n.probabilities,
                n.probabilities == 1 ? "" : "s", n.tracks, n.tracks == 1 ? "" : "s", covers,
                covers == 1 ? "" : "s", settings, settings == 1 ? "" : "s");
    }
    if (failed)
        rx_warn("library: %d track%s could not be saved cleaned (read-only?); they are clean "
                "in memory, and the next open tries again", failed, failed == 1 ? "" : "s");
    return 0;
}
