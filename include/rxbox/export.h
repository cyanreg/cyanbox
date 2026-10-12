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
#include "rxbox/analysis.h"
#include "rxbox/formats.h"

/* Progress: `stage` is one of "prepare", "check", "copy", "analysis",
 * "artwork", "database", "prune", "done"; `item` is the file or track being
 * handled (may be NULL). Returning false cancels. */
typedef bool (*rx_progress_cb)(void *opaque, const char *stage, int done, int total, const char *item);

typedef struct RxExportOptions {
    bool prune;                 /* delete files on the stick that are not in the library */
    bool compat_transcode;      /* AAC and hi-res FLAC -> 16/44.1 FLAC (overrides the config) */
    bool dry_run;               /* plan and check, write nothing */
    bool force_analysis;        /* re-analyse everything first */
    const char *device_name;    /* NULL: the config's, else the stick's label */
    const RxPlaylist *const *playlists; int nb_playlists;   /* NULL: every playlist; else only these (and their tracks) */
    bool only_playlists;        /* with `playlists`: export only tracks in them, not the whole library */
} RxExportOptions;
void rx_export_options_default(RxExportOptions *o);

/* The pre-write checks, each reported through the callback as "check" with
 * the problem in `item`; the export refuses to start when any fails:
 *   - the destination is a mounted FAT32/exFAT volume with enough free
 *     space for the audio not yet on it plus 64 MiB for the databases;
 *   - no track has more text than fits one DeviceSQL page (reported by name);
 *   - every track has a grid (analysed first when it lacks one);
 *   - no two tracks map to the same path on the stick. */
int  rx_export(RxLibrary *lib, const char *dest, const RxExportOptions *opt,
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

enum RxSyncKind {
    RX_SYNC_HOT_CUE_SET,      /* a hot cue slot the player set or changed */
    RX_SYNC_HOT_CUE_CLEARED,  /* a slot the player cleared */
    RX_SYNC_MEMORY_ADDED,     /* a memory cue/loop not in the library (20 ms tolerance) */
    RX_SYNC_MEMORY_REMOVED,   /* a library memory cue the stick no longer has */
    RX_SYNC_PLAY,             /* a history entry: a play of a track */
    RX_SYNC_SETTING,          /* a My Settings value changed on the player */
};

typedef struct RxSyncItem {
    enum RxSyncKind kind;
    uint64_t  track_id;       /* cues and plays; 0 for settings */
    RxCue     cue;            /* the stick's cue (SET/ADDED) or the library's (CLEARED/REMOVED) */
    int       slot;           /* hot cue slot for the hot cue kinds */
    int64_t   played_at;      /* RX_SYNC_PLAY: epoch seconds, 0 when the player gave no time */
    int       play_index;     /* position in the history list */
    char      setting_key[48];    /* RX_SYNC_SETTING: "player.quantize" ... */
    char      setting_value[32];
    char      setting_old[32];
    bool      accepted;       /* the interface's choice; default true */
} RxSyncItem;

typedef struct RxStickState {
    char     *mount;
    int       nb_tracks;              /* tracks on the stick */
    int       nb_tracks_unknown;      /* not matched to the library */
    struct { uint64_t track_id; RxCue *cues; int nb_cues; char *stick_path; } *tracks; int nb_track_states;
    struct { uint64_t track_id; int64_t played_at; char *label; } *plays; int nb_plays;
    RxKv     *settings; int nb_settings;  /* every value in the settings files found */
    bool      has_pdb, has_onelibrary;
} RxStickState;

int  rx_stick_read(const RxLibrary *lib, const char *mount, RxStickState *out);
void rx_stick_state_free(RxStickState *s);
int  rx_stick_diff(const RxLibrary *lib, const RxStickState *s, RxSyncItem **items, int *nb);
/* Applies every item with accepted == true: cues into the tracks (saved),
 * plays into the history (deduplicated against existing plays of the same
 * track within a minute), settings into lib->cfg (saved). Returns the
 * number applied. */
int  rx_stick_apply(RxLibrary *lib, const RxSyncItem *items, int nb, rx_progress_cb cb, void *opaque);
/* read + diff + apply everything. Returns the number applied. */
int  rx_pull(RxLibrary *lib, const char *stick, rx_progress_cb cb, void *opaque);

/* The stick's label (FAT volume label, or the mount point's base name). */
int  rx_stick_label(const char *mount, char *buf, size_t size);
bool rx_stick_is_mounted(const char *mount);

#endif /* RXBOX_EXPORT_H */
