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

/* Copies cues the player saved on the stick back into the sidecars:
 * matched by track id (masterDbId / the export.pdb id), reading the ANLZ
 * cue tags. Hot cues replace the slot; memory cues within 20 ms of an
 * existing one are kept as they are. Returns the number of tracks
 * updated. */
int  rx_pull(RxLibrary *lib, const char *stick, rx_progress_cb cb, void *opaque);

/* The stick's label (FAT volume label, or the mount point's base name). */
int  rx_stick_label(const char *mount, char *buf, size_t size);
bool rx_stick_is_mounted(const char *mount);

#endif /* RXBOX_EXPORT_H */
