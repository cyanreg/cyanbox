/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* What the export module's files share: the plan's source and path rules
 * (dj's export.py: _needs_transcode, usb_path, _fat, _clean), cues in the
 * form the stick stores them (ms, colour code), and the sync record the
 * export leaves on the stick.
 *
 * The sync record, /PIONEER/rxbox/sync.txt, is what makes a gig's changes
 * tellable from the library's: it lists, per exported track, the cues as
 * written to the ANLZ files, and the settings files as they were on the
 * stick after the export. Whatever differs from it later was written by a
 * player. Plain text, one item per line:
 *
 *   rxbox-sync 1
 *   uuid 0123456789abcdef            new on every export: keys the history markers
 *   library 0123...cdef              the exporting library (setting "sync.library")
 *   date 2026-10-13
 *   track 42
 *   hot <slot> <in ms> <out ms|-1> <code> <rrggbb> <name, %-escaped>
 *   mem <in ms> <out ms|-1> <colour id> <active 0|1> <name>
 *   setting <FILE.DAT> <hex of the whole file>
 *
 * Players ignore unknown files under PIONEER/ (they keep their own CDJ/,
 * LOG/ ... there). A record is only believed by the library that wrote it:
 * another library's track ids are not this one's even when the numbers
 * are the same, and a record without the line (an older rxbox) is treated
 * as no record. */

#ifndef RXBOX_EXPORT_PRIV_H
#define RXBOX_EXPORT_PRIV_H

#include "rxbox/export.h"

#define RX_SYNC_PATH "/PIONEER/rxbox/sync.txt"

/* ------------------------------------------------------------------ plan */

/* Codecs every player decodes (dj's PLAYABLE); anything else goes to the
 * stick as FLAC. */
bool rx_plan_codec_playable(const char *codec);
/* Whether a track goes to the stick transcoded (1), and the cache file it
 * goes to (NULL when not). Compatibility mode also converts AAC (nobody
 * knows what players do with its priming) and FLAC above 16 bit/48 kHz.
 * `create` makes the cache directory; a dry run passes false. */
int  rx_plan_transcode_path(const RxLibrary *lib, const RxTrack *t, bool compat, bool create,
                            char **cache_path, int *max_rate);
/* The transcode exists and is newer than the track's file. */
bool rx_plan_transcode_fresh(const RxTrack *t, const char *cache);
/* Makes sure the transcode exists and is newer than the track's file. */
int  rx_plan_transcode(const RxLibrary *lib, const RxTrack *t, bool compat, char **source);

/* rx_plan_build with a "prepare" item per track (1-based) and every
 * warning (unknown colour, unreadable artwork, playlist entry not
 * exported) reported through `cb`; returning false from it cancels. A dry
 * plan transcodes nothing: a track whose transcode is not made yet is
 * described from its original as the transcode would be. */
int  rx_plan_build_cb(const RxLibrary *lib, RxTrack *const *tracks, int nb_tracks,
                      const RxPlaylist *const *playlists, int nb_playlists,
                      const char *device_name, bool compat_transcode, bool dry, RxPlan *out,
                      rx_progress_cb cb, void *opaque);

/* dj's _fat(_clean(s), limit): NFC, FAT-illegal characters to '_', trimmed,
 * cut to `limit` code points, trailing dots and spaces dropped, "_" when
 * nothing is left. */
char *rx_plan_fat_name(const char *s, size_t limit);
/* The file name stem a track gets on the stick before collision suffixes. */
char *rx_plan_usb_stem(const RxTrack *t);
/* Whether a stick path's file name is what the plan gives this track (the
 * stem, or its first 40 characters with a "-N" collision suffix): a guard
 * when a stick's track ids may not be ours. */
bool rx_plan_usb_name_matches(const RxTrack *t, const char *stick_path);
/* Lower-cases ASCII and the common Latin, Greek and Cyrillic capitals, for
 * FAT's case-insensitive names (dj compared with str.lower()). */
char *rx_plan_fold_case(const char *s);
/* The background colour setting ([device] background_color): 0 default,
 * 1..8 pink..purple. */
int  rx_plan_background_color(const RxConfig *cfg);

/* ----------------------------------------------------------- stick cues */

/* A cue as the stick stores it. */
typedef struct RxStickCue {
    int      hot;                /* -1 memory, 0..7 */
    uint32_t in, out;            /* ms; out 0xFFFFFFFF when not a loop */
    int      code;               /* hot: palette code; memory: colour id */
    uint32_t rgb;                /* hot cues: the pad RGB */
    bool     active;
    char     name[RX_CUE_NAME_MAX];
} RxStickCue;

void rx_stick_cue_from_plan(const RxPlanCue *c, RxStickCue *s);
/* A library cue as the export would write it. */
void rx_stick_cue_from_track(const RxCue *c, RxStickCue *s);
/* Back to a library cue on `grid` (musical when it sits on a beat). */
void rx_stick_cue_to_track(const RxStickCue *s, const RxGrid *grid, RxCue *c);
/* Same position, loop, name and colour (1 ms tolerance for rounding). */
bool rx_stick_cue_equal(const RxStickCue *a, const RxStickCue *b);

/* ----------------------------------------------------------- sync record */

typedef struct RxSyncTrack {
    uint64_t    id;
    RxStickCue *cues;
    int         nb_cues;
} RxSyncTrack;

typedef struct RxSyncRecord {
    char         uuid[17];
    char         library[33];    /* "" when the record has none */
    char         date[16];
    RxSyncTrack *tracks;         /* sorted by id */
    int          nb_tracks;
    struct { char name[24]; uint8_t *data; size_t size; } settings[4];
    int          nb_settings;
} RxSyncRecord;

int  rx_sync_record_read(const char *mount, RxSyncRecord *r);   /* -ENOENT when absent */
int  rx_sync_record_write(const char *mount, const RxSyncRecord *r);
void rx_sync_record_free(RxSyncRecord *r);
const RxSyncTrack *rx_sync_record_track(const RxSyncRecord *r, uint64_t id);
/* A fresh record for a plan: new uuid, every track's cues as written. */
int  rx_sync_record_from_plan(const RxPlan *plan, RxSyncRecord *r);
/* Adds the settings files found on the stick now. */
int  rx_sync_record_add_settings(RxSyncRecord *r, const char *mount);
/* The library's identity for the record: 32 hex digits made once and kept
 * in the library (setting "sync.library"). */
int  rx_sync_library_key(RxLibrary *lib, char key[33]);

/* ----------------------------------------------------------------- files */

/* The file system type of a mount point ("vfat", "exfat" ...), -ENOENT
 * when `mount` is not one. */
int  rx_stick_fs_type(const char *mount, char *buf, size_t size);
/* <mount><stick path>, allocating. */
char *rx_stick_file(const char *mount, const char *stick_path);
/* A path read from a stick (a database row, an analysis path) that names a
 * file on the stick: absolute, and no empty, "." or ".." component. */
bool rx_stick_path_safe(const char *stick_path);
/* Writes a file atomically, creating its directory. */
int  rx_stick_write(const char *path, const void *data, size_t size);
/* Bytes the export of `plan` to `dest` needs free, the files counted in
 * whole clusters of `cluster` bytes. */
int64_t rx_export_space_needed(const RxPlan *plan, const char *dest, int64_t cluster);

#endif /* RXBOX_EXPORT_PRIV_H */
