/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

#ifndef RXBOX_EXPORT_H
#define RXBOX_EXPORT_H

/* Writing, checking and reading sticks. The plan (what goes where, with
 * which ids) is built from the library, then every writer renders its
 * file(s) from the plan: export.pdb and exportExt.pdb (rxbox/formats/pdb.h),
 * exportLibrary.db (rxbox/formats/onelibrary.h), the ANLZ files
 * (rxbox/formats/anlz.h), the artwork, the My Settings files
 * (rxbox/formats/settings.h) and the audio itself. Progress and every
 * decision are reported through the callback; nothing prints. */

#include "rxbox/library.h"
#include <stdio.h>

#include "rxbox/analysis.h"
#include "rxbox/formats.h"

/* Progress: `stage` is one of "prepare", "check", "transcode", "copy",
 * "skip", "analysis files", "artwork", "database", "prune", "flush",
 * "done"; `item` is the file or track being handled, NULL for a step that
 * wrote nothing (an analysis file or artwork the stick held unchanged) and
 * for the ticks a long copy sends while it runs (same step again; a chance
 * to cancel and to redraw the rate). Returning false cancels. */
typedef bool (*rx_progress_cb)(void *opaque, const char *stage, int done, int total, const char *item);

/* The copy's throughput, kept up to date by the export thread for whoever
 * displays it (read with __atomic_load_n, or on the progress callback's
 * thread). The audio is written straight to the stick (O_DIRECT where the
 * file system takes it), so the bytes done are bytes on the stick and the
 * rate is the stick's, not the page cache's. */
typedef struct RXExportStats {
    int64_t bytes_total;        /* audio bytes this export copies */
    int64_t bytes_done;
    int64_t rate;               /* bytes a second over the last few seconds; 0 until known */
    int64_t seconds_left;       /* at that rate; -1 until known */
} RXExportStats;

/* "18.3 MB/s, 2:31 left"; empty while the rate is unknown. */
static inline void rx_export_rate_text(int64_t rate, int64_t seconds_left, char *buf, size_t size)
{
    if (rate <= 0) {
        if (size)
            buf[0] = '\0';
        return;
    }
    double mbs = (double)rate / 1e6;
    if (seconds_left < 0)
        snprintf(buf, size, "%.1f MB/s", mbs);
    else if (seconds_left >= 3600)
        snprintf(buf, size, "%.1f MB/s, %lld:%02lld:%02lld left", mbs, (long long)seconds_left / 3600,
                 (long long)seconds_left / 60 % 60, (long long)seconds_left % 60);
    else
        snprintf(buf, size, "%.1f MB/s, %lld:%02lld left", mbs, (long long)seconds_left / 60,
                 (long long)seconds_left % 60);
}

/* The export model (rekordbox's sync manager): one click exports the
 * settings, every playlist, and the union of the tracks in them; nothing
 * else. Files already on the stick with the same size and mtime are
 * skipped; the ANLZ files and artwork are rebuilt from the library (no
 * analysis runs here) and written only where the stick's differ; the
 * settings files and both databases are rewritten every time. Tracks no
 * longer in any playlist are pruned (the
 * interface lists them and asks once; the CLI needs --prune). A stick that
 * holds player-written cues, history or presets not yet synced into the
 * library blocks the export (reported as a "check" item) unless
 * ignore_unsynced is set, so a gig's hot cues are never overwritten. */
typedef struct RXExportOptions {
    bool whole_library;         /* every track, not only those in playlists */
    const RXPlaylist *const *playlists; int nb_playlists;   /* NULL: every playlist; else only these */
    bool prune;                 /* delete files on the stick that are no longer exported */
    bool ignore_unsynced;       /* export even if the stick has changes not synced back */
    bool compat_transcode;      /* AAC and hi-res FLAC -> 16/44.1 FLAC; the caller folds in the
                                   export.compat_transcode setting (its default) */
    bool dry_run;               /* plan and check, report every copy/prune/rewrite, write nothing */
    bool force_analysis;        /* re-analyse everything first */
    const char *device_name;    /* NULL: the config's, else the stick's label */
    RXExportStats *stats;       /* kept up to date while the export runs; may be NULL */
} RXExportOptions;
void rx_export_options_default(RXExportOptions *o);

/* The pre-write checks, each reported through the callback as "check" with
 * the problem in `item`; the export refuses to start when any fails:
 *   - the destination is a mounted FAT32/exFAT volume with enough free
 *     space for the audio not yet on it plus 64 MiB for the databases;
 *   - no track has more text than fits one DeviceSQL page (reported by name);
 *   - every track has a grid (the caller analyses first; a track without
 *     one is reported and skipped);
 *   - no two tracks map to the same path on the stick;
 *   - no missing (greyed out) track is selected (reported and skipped);
 *   - the stick has nothing unsynced (rx_stick_diff is empty), unless
 *     ignore_unsynced.
 * A dry run reports, as "copy"/"skip"/"prune"/"rewrite" stages, what a real
 * run would do, so the interface can show the prune list and ask; one that
 * only the unsynced changes stop reports all of it too, then returns
 * -EBUSY as a real run would. */
int  rx_export(RXLibrary *lib, const char *dest, const RXExportOptions *opt,
               rx_progress_cb cb, void *opaque);

/* The plan's sources, for whoever serves the library's files as the
 * export would copy them (the export, and Link: docs/LINK.md, section 4). */

/* Codecs every player decodes (dj's PLAYABLE); anything else goes to the
 * stick as FLAC. */
bool rx_plan_codec_playable(const char *codec);
/* Whether a track goes to the stick transcoded (1), and the cache file it
 * goes to (NULL when not). Compatibility mode also converts AAC (nobody
 * knows what players do with its priming) and FLAC above 16 bit/48 kHz.
 * `create` makes the cache directory; a dry run passes false. */
int  rx_plan_transcode_path(const RXLibrary *lib, const RXTrack *t, bool compat, bool create,
                            char **cache_path, int *max_rate);
/* The transcode exists and is newer than the track's file. */
bool rx_plan_transcode_fresh(const RXTrack *t, const char *cache);
/* Makes sure the transcode exists and is newer than the track's file. */
int  rx_plan_transcode(const RXLibrary *lib, const RXTrack *t, bool compat, char **source);

/* rx_plan_build (rxbox/formats.h) with a "prepare" item per track (1-based) and every
 * warning (unknown colour, unreadable artwork, playlist entry not
 * exported) reported through `cb`; returning false from it cancels. A dry
 * plan transcodes nothing: a track whose transcode is not made yet is
 * described from its original as the transcode would be. */
int  rx_plan_build_cb(const RXLibrary *lib, RXTrack *const *tracks, int nb_tracks,
                      const RXPlaylist *const *playlists, int nb_playlists,
                      const char *device_name, bool compat_transcode, bool dry, RXPlan *out,
                      rx_progress_cb cb, void *opaque);

/* Reads back a stick and validates what dj's `inspect` did plus the rbxport
 * checks: ANLZ embedded path matches the audio, folder matches the path
 * hash, fixed track-row values, both libraries agree on hot-cue auto load,
 * declared cue counts match parsed ones, previews have no zero column,
 * every referenced file exists. Writes a report through `line`. Returns
 * the number of problems found, or a negative errno. */
typedef void (*rx_report_cb)(void *opaque, const char *line);
int  rx_inspect(const char *stick, rx_report_cb line, void *opaque);

/* ------------------------------------------------------------------- sync */

/* What a player leaves on a stick after a gig, read back for review:
 *  - cues and loops it saved into the ANLZ files (matched to library tracks
 *    by the track id rxbox wrote);
 *  - the play history it appended (export.pdb history tables and, on
 *    OneLibrary players, exportLibrary.db), as plays with times where the
 *    player recorded them;
 *  - My Settings files it wrote (PIONEER/{MY,MY2,DJMMY,DEV}SETTING.DAT), as
 *    name/value pairs that differ from the library's settings.
 * rx_stick_read parses all of it; rx_stick_diff compares it with the library
 * into a list of proposed changes the interface shows for review (each can
 * be accepted or rejected); rx_stick_apply writes the accepted ones to the
 * library. rx_pull is the CLI's "accept everything". */

enum RXSyncKind {
    RX_SYNC_HOT_CUE_SET,      /* a hot cue slot the player set or changed */
    RX_SYNC_HOT_CUE_CLEARED,  /* a slot the player cleared */
    RX_SYNC_MEMORY_ADDED,     /* a memory cue/loop not in the library (20 ms tolerance) */
    RX_SYNC_MEMORY_REMOVED,   /* a library memory cue the stick no longer has */
    RX_SYNC_PLAY,             /* a history entry: a play of a track */
    RX_SYNC_SETTING,          /* a My Settings value changed on the player */
};

typedef struct RXSyncItem {
    enum RXSyncKind kind;
    uint64_t  track_id;       /* cues and plays; 0 for settings */
    RXCue     cue;            /* the stick's cue (SET/ADDED) or the library's (CLEARED/REMOVED) */
    int       slot;           /* hot cue slot for the hot cue kinds */
    int64_t   played_at;      /* RX_SYNC_PLAY: epoch seconds, 0 when the player gave no time */
    int       play_index;     /* position in the history list */
    char      setting_key[48];    /* RX_SYNC_SETTING: "player.quantize" ...; RX_SYNC_PLAY: the history marker apply records */
    char      setting_value[32];
    char      setting_old[32];
    bool      accepted;       /* the interface's choice; default true */
} RXSyncItem;

typedef struct RXStickState {
    char     *mount;
    int       nb_tracks;              /* tracks on the stick */
    int       nb_tracks_unknown;      /* not matched to the library */
    struct { uint64_t track_id; RXCue *cues; int nb_cues; char *stick_path; } *tracks; int nb_track_states;
    struct { uint64_t track_id; int64_t played_at; char *label; } *plays; int nb_plays;
    RXKv     *settings; int nb_settings;  /* every value in the settings files found */
    bool      has_pdb, has_onelibrary;
    struct RXStickPriv *priv;         /* the reader's own state for the diff (the export's sync record, history lists) */
} RXStickState;

/* -ENOENT when `mount` holds neither database nor a My Settings file: not
 * a stick (a mistyped mount point), never a stick with nothing to sync. */
int  rx_stick_read(const RXLibrary *lib, const char *mount, RXStickState *out);
void rx_stick_state_free(RXStickState *s);
int  rx_stick_diff(const RXLibrary *lib, const RXStickState *s, RXSyncItem **items, int *nb);
/* Applies every item with accepted == true: cues into the tracks (saved),
 * plays into the history (deduplicated against existing plays of the same
 * track within a minute), settings into lib->cfg (saved). Returns the
 * number applied. */
int  rx_stick_apply(RXLibrary *lib, const RXSyncItem *items, int nb, rx_progress_cb cb, void *opaque);
/* read + diff + apply everything. Returns the number applied. */
int  rx_pull(RXLibrary *lib, const char *stick, rx_progress_cb cb, void *opaque);

/* The stick's label (FAT volume label, or the mount point's base name). */
int  rx_stick_label(const char *mount, char *buf, size_t size);
bool rx_stick_is_mounted(const char *mount);

#endif /* RXBOX_EXPORT_H */
