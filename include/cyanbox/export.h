/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

#ifndef CYANBOX_EXPORT_H
#define CYANBOX_EXPORT_H

/* Writing, checking and reading sticks. The plan (what goes where, with
 * which ids) is built from the library, then every writer renders its
 * file(s) from the plan: export.pdb and exportExt.pdb (cyanbox/formats/pdb.h),
 * exportLibrary.db (cyanbox/formats/onelibrary.h), the ANLZ files
 * (cyanbox/formats/anlz.h), the artwork, the My Settings files
 * (cyanbox/formats/settings.h) and the audio itself. Progress and every
 * decision are reported through the callback; nothing prints. */

#include "cyanbox/library.h"
#include <stdio.h>

#include "cyanbox/analysis.h"
#include "cyanbox/formats.h"

/* Progress: `stage` is one of "prepare", "check", "transcode", "copy",
 * "skip", "analysis files", "artwork", "database", "prune", "flush",
 * "done"; `item` is the file or track being handled, NULL for a step that
 * wrote nothing (an analysis file or artwork the stick held unchanged) and
 * for the ticks a long copy sends while it runs (same step again; a chance
 * to cancel and to redraw the rate). Returning false cancels. */
typedef bool (*cb_progress_cb)(void *opaque, const char *stage, int done, int total, const char *item);

/* The copy's throughput, kept up to date by the export thread for whoever
 * displays it (read with __atomic_load_n, or on the progress callback's
 * thread). The audio is written straight to the stick (O_DIRECT where the
 * file system takes it), so the bytes done are bytes on the stick and the
 * rate is the stick's, not the page cache's. */
typedef struct CBExportStats {
    int64_t bytes_total;        /* audio bytes this export copies */
    int64_t bytes_done;
    int64_t rate;               /* bytes a second over the last few seconds; 0 until known */
    int64_t seconds_left;       /* at that rate; -1 until known */
} CBExportStats;

/* "18.3 MB/s, 2:31 left"; empty while the rate is unknown. */
static inline void cb_export_rate_text(int64_t rate, int64_t seconds_left, char *buf, size_t size)
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
/* What the stick's audio is (export.format): every track, whatever it
 * is, encoded as it is written (cb_encode) from its decoded samples at
 * 44.1 kHz and 16 bits, so it is the audio the analysis was made of; of
 * its one or two channels, stereo for more. FLAC (the default, level 8,
 * for the CDJ-2000NXS2 and later), WAV (every player reads it, the
 * CDJ-900 and the others before the CDJ-2000NXS2 included), or MP3 at
 * 320 kbit/s CBR. */
enum CBStickFormat {
    CB_STICK_FLAC = 0,
    CB_STICK_WAV,
    CB_STICK_MP3,
};
/* "flac", "wav", "mp3"; the parse returns -1 for anything else. */
const char *cb_stick_format_name(int format);
int  cb_stick_format_parse(const char *name);

typedef struct CBExportOptions {
    bool whole_library;         /* every track, not only those in playlists */
    int  format;                /* enum CBStickFormat; the caller folds in export.format */
    const CBPlaylist *const *playlists; int nb_playlists;   /* NULL: every playlist; else only these */
    bool prune;                 /* delete files on the stick that are no longer exported */
    bool ignore_unsynced;       /* export even if the stick has changes not synced back */
    bool dry_run;               /* plan and check, report every copy/prune/rewrite, write nothing */
    bool force_analysis;        /* re-analyse everything first */
    const char *device_name;    /* NULL: the config's, else the stick's label */
    CBExportStats *stats;       /* kept up to date while the export runs; may be NULL */
} CBExportOptions;
void cb_export_options_default(CBExportOptions *o);

/* The pre-write checks, each reported through the callback as "check" with
 * the problem in `item`; the export refuses to start when any fails:
 *   - the destination is a mounted FAT32/exFAT volume with enough free
 *     space for the audio not yet on it plus 64 MiB for the databases;
 *   - no track has more text than fits one DeviceSQL page (reported by name);
 *   - every track has a grid (the caller analyses first; a track without
 *     one is reported and skipped);
 *   - no two tracks map to the same path on the stick;
 *   - no missing (greyed out) track is selected (reported and skipped);
 *   - the stick has nothing unsynced (cb_stick_diff is empty), unless
 *     ignore_unsynced.
 * A dry run reports, as "copy"/"skip"/"prune"/"rewrite" stages, what a real
 * run would do, so the interface can show the prune list and ask; one that
 * only the unsynced changes stop reports all of it too, then returns
 * -EBUSY as a real run would. */
int  cb_export(CBLibrary *lib, const char *dest, const CBExportOptions *opt,
               cb_progress_cb cb, void *opaque);

/* What a plan's tracks become, for whoever serves the library's files
 * (the export, and Link: docs/LINK.md, section 4). Nothing is made ahead:
 *
 * LINK: every track as a 16-bit WAV at 44.1 kHz (CB_LINK_RATE) of its one
 * or two channels, which every PRO DJ LINK player reads (the players
 * before the CDJ-2000NXS2, the CDJ-900 among them, read no FLAC), made as
 * it is read (CBPlanTrack.pcm, src/link/wav.c).
 *
 * STICK_*: an export (CBStickFormat), every track encoded as it is
 * written (CBPlanTrack.encode). */
#define CB_LINK_RATE 44100

enum CBTranscode {
    CB_TRANSCODE_LINK = 0,
    CB_TRANSCODE_STICK_FLAC,
    CB_TRANSCODE_STICK_WAV,
    CB_TRANSCODE_STICK_MP3,
};

/* How a plan learns about the files. */
enum CBPlanMode {
    /* The export's: every file probed (its frame tables too), every cover
     * read from its file. */
    CB_PLAN_EXPORT = 0,
    /* The export's dry run: the same. */
    CB_PLAN_DRY,
    /* As dry, but reading no audio: each file described from what the
     * library stored when it probed it (a stat says it is still that
     * file; one changed since is probed), each cover from the library's
     * thumbnail (a cover without one from its file), no frame tables.
     * Link's, made again at every library change (docs/LINK.md): a
     * library of a thousand tracks in a fraction of a second instead of a
     * read of every file. */
    CB_PLAN_STORED,
};

/* cb_plan_build (cyanbox/formats.h) with a "prepare" item per track (1-based) and every
 * warning (unknown colour, unreadable artwork, playlist entry not
 * exported) reported through `cb`; returning false from it cancels. */
int  cb_plan_build_cb(const CBLibrary *lib, CBTrack *const *tracks, int nb_tracks,
                      const CBPlaylist *const *playlists, int nb_playlists,
                      const char *device_name, int kind, enum CBPlanMode mode,
                      CBPlan *out, cb_progress_cb cb, void *opaque);
/* A plan track's cues made again from `t`'s, as the plan makes them:
 * Link's plan follows a cue a player saved on the deck. */
int  cb_plan_track_cues(CBPlanTrack *pt, const CBTrack *t);

/* A cue as the stick stores it, and as a player gives one back, on a
 * stick it wrote to or over Link (a cue saved on the deck). */
typedef struct CBStickCue {
    int      hot;                /* -1 memory, 0..7 */
    uint32_t in, out;            /* ms; out 0xFFFFFFFF when not a loop */
    int      code;               /* hot: palette code; memory: colour id */
    uint32_t rgb;                /* hot cues: the pad RGB */
    bool     active;
    char     name[CB_CUE_NAME_MAX];
} CBStickCue;

/* Back to a library cue on `grid` (musical when it sits on a beat). */
void cb_stick_cue_to_track(const CBStickCue *s, const CBGrid *grid, CBCue *c);

/* Reads back a stick and validates what dj's `inspect` did plus the rbxport
 * checks: ANLZ embedded path matches the audio, folder matches the path
 * hash, fixed track-row values, both libraries agree on hot-cue auto load,
 * declared cue counts match parsed ones, previews have no zero column,
 * every referenced file exists. Writes a report through `line`. Returns
 * the number of problems found, or a negative errno. */
typedef void (*cb_report_cb)(void *opaque, const char *line);
int  cb_inspect(const char *stick, cb_report_cb line, void *opaque);

/* ------------------------------------------------------------------- sync */

/* What a player leaves on a stick after a gig, read back for review:
 *  - cues and loops it saved into the ANLZ files (matched to library tracks
 *    by the track id cyanbox wrote);
 *  - the play history it appended (export.pdb history tables and, on
 *    OneLibrary players, exportLibrary.db), as plays with times where the
 *    player recorded them;
 *  - My Settings files it wrote (PIONEER/{MY,MY2,DJMMY,DEV}SETTING.DAT), as
 *    name/value pairs that differ from the library's settings.
 * cb_stick_read parses all of it; cb_stick_diff compares it with the library
 * into a list of proposed changes the interface shows for review (each can
 * be accepted or rejected); cb_stick_apply writes the accepted ones to the
 * library. cb_pull is the CLI's "accept everything". */

enum CBSyncKind {
    CB_SYNC_HOT_CUE_SET,      /* a hot cue slot the player set or changed */
    CB_SYNC_HOT_CUE_CLEARED,  /* a slot the player cleared */
    CB_SYNC_MEMORY_ADDED,     /* a memory cue/loop not in the library (20 ms tolerance) */
    CB_SYNC_MEMORY_REMOVED,   /* a library memory cue the stick no longer has */
    CB_SYNC_PLAY,             /* a history entry: a play of a track */
    CB_SYNC_SETTING,          /* a My Settings value changed on the player */
};

typedef struct CBSyncItem {
    enum CBSyncKind kind;
    uint64_t  track_id;       /* cues and plays; 0 for settings */
    CBCue     cue;            /* the stick's cue (SET/ADDED) or the library's (CLEARED/REMOVED) */
    int       slot;           /* hot cue slot for the hot cue kinds */
    int64_t   played_at;      /* CB_SYNC_PLAY: epoch seconds, 0 when the player gave no time */
    int       play_index;     /* position in the history list */
    char      setting_key[48];    /* CB_SYNC_SETTING: "player.quantize" ...; CB_SYNC_PLAY: the history marker apply records */
    char      setting_value[32];
    char      setting_old[32];
    bool      accepted;       /* the interface's choice; default true */
} CBSyncItem;

typedef struct CBStickState {
    char     *mount;
    int       nb_tracks;              /* tracks on the stick */
    int       nb_tracks_unknown;      /* not matched to the library */
    struct { uint64_t track_id; CBCue *cues; int nb_cues; char *stick_path; } *tracks; int nb_track_states;
    struct { uint64_t track_id; int64_t played_at; char *label; } *plays; int nb_plays;
    CBKv     *settings; int nb_settings;  /* every value in the settings files found */
    bool      has_pdb, has_onelibrary;
    struct CBStickPriv *priv;         /* the reader's own state for the diff (the export's sync record, history lists) */
} CBStickState;

/* -ENOENT when `mount` holds neither database nor a My Settings file: not
 * a stick (a mistyped mount point), never a stick with nothing to sync. */
int  cb_stick_read(const CBLibrary *lib, const char *mount, CBStickState *out);
void cb_stick_state_free(CBStickState *s);
int  cb_stick_diff(const CBLibrary *lib, const CBStickState *s, CBSyncItem **items, int *nb);
/* Applies every item with accepted == true: cues into the tracks (saved),
 * plays into the history (deduplicated against existing plays of the same
 * track within a minute), settings into lib->cfg (saved). Returns the
 * number applied. */
int  cb_stick_apply(CBLibrary *lib, const CBSyncItem *items, int nb, cb_progress_cb cb, void *opaque);
/* read + diff + apply everything. Returns the number applied. */
int  cb_pull(CBLibrary *lib, const char *stick, cb_progress_cb cb, void *opaque);

/* The stick's label (FAT volume label, or the mount point's base name). */
int  cb_stick_label(const char *mount, char *buf, size_t size);
bool cb_stick_is_mounted(const char *mount);

#endif /* CYANBOX_EXPORT_H */
