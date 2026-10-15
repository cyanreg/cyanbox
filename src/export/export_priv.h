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

#include <sys/stat.h>

#define RX_SYNC_PATH "/PIONEER/rxbox/sync.txt"

/* ------------------------------------------------------------------ plan */

/* dj's _fat(_clean(s), limit): NFC, FAT-illegal characters to '_', trimmed,
 * cut to `limit` code points, trailing dots and spaces dropped, "_" when
 * nothing is left. */
char *rx_plan_fat_name(const char *s, size_t limit);
/* The file name stem a track gets on the stick before collision suffixes. */
char *rx_plan_usb_stem(const RXTrack *t);
/* Whether a stick path's file name is what the plan gives this track (the
 * stem, or its first 40 characters with a "-N" collision suffix): a guard
 * when a stick's track ids may not be ours. */
bool rx_plan_usb_name_matches(const RXTrack *t, const char *stick_path);
/* A path's key under FAT's and exFAT's case-insensitive names: up-cased
 * as exFAT's default up-case table does (plan.c); two paths with one key
 * are one file on the stick. */
char *rx_plan_fold_case(const char *s);

/* ----------------------------------------------------------- stick cues */

/* RXStickCue and rx_stick_cue_to_track: rxbox/export.h. */
void rx_stick_cue_from_plan(const RXPlanCue *c, RXStickCue *s);
/* A library cue as the export would write it. */
void rx_stick_cue_from_track(const RXCue *c, RXStickCue *s);
/* Same position, loop, name and colour (1 ms tolerance for rounding). */
bool rx_stick_cue_equal(const RXStickCue *a, const RXStickCue *b);

/* ----------------------------------------------------------- sync record */

typedef struct RXSyncTrack {
    uint64_t    id;
    RXStickCue *cues;
    int         nb_cues;
} RXSyncTrack;

typedef struct RXSyncRecord {
    char         uuid[17];
    char         library[33];    /* "" when the record has none */
    char         date[16];
    RXSyncTrack *tracks;         /* sorted by id */
    int          nb_tracks;
    struct { char name[24]; uint8_t *data; size_t size; } settings[4];
    int          nb_settings;
} RXSyncRecord;

int  rx_sync_record_read(const char *mount, RXSyncRecord *r);   /* -ENOENT when absent */
int  rx_sync_record_write(int root, const RXSyncRecord *r);
void rx_sync_record_free(RXSyncRecord *r);
const RXSyncTrack *rx_sync_record_track(const RXSyncRecord *r, uint64_t id);
/* A fresh record for a plan: new uuid, every track's cues as written. */
int  rx_sync_record_from_plan(const RXPlan *plan, RXSyncRecord *r);
/* Adds the settings files found on the stick now. */
int  rx_sync_record_add_settings(RXSyncRecord *r, int root);
/* The library's identity for the record: 32 hex digits made once and kept
 * in the library (setting "sync.library"). */
int  rx_sync_library_key(RXLibrary *lib, char key[33]);

/* ----------------------------------------------------------------- files */

/* The file system type of a mount point ("vfat", "exfat" ...), -ENOENT
 * when `mount` is not one. */
int  rx_stick_fs_type(const char *mount, char *buf, size_t size);
/* <mount><stick path>, allocating. */
char *rx_stick_file(const char *mount, const char *stick_path);
/* A path read from a stick (a database row, an analysis path) that names a
 * file on the stick: absolute, and no empty, "." or ".." component. */
bool rx_stick_path_safe(const char *stick_path);
/* The stick's files by stick path, below a descriptor of its root
 * (rx_stick_root, an O_PATH descriptor of the mount) and nowhere else: no
 * symbolic link is followed on the way and no other file system mounted
 * inside is entered (ELOOP, EXDEV; stick.c). Errors are negative errno
 * values; the descriptors returned are O_CLOEXEC. */
int  rx_stick_root(const char *mount);
/* openat(2) of a file on the stick; with O_CREAT the missing directories
 * on the way are made. A trailing link is refused, or with O_PATH |
 * O_NOFOLLOW opened as itself. */
int  rx_stick_open(int root, const char *stick_path, int flags, mode_t mode);
int  rx_stick_mkdirs(int root, const char *stick_dir);
/* The file itself, as lstat(2) would see it. */
int  rx_stick_stat(int root, const char *stick_path, struct stat *st);
/* unlinkat(2) in the file's directory; `flags` 0 or AT_REMOVEDIR. */
int  rx_stick_unlink(int root, const char *stick_path, int flags);
/* Writes a file in place, creating its directory; no temporary, no sync
 * (rx_stick_sync at the end of an export covers everything at once). */
int  rx_stick_write(int root, const char *stick_path, const void *data, size_t size);
/* A regular file's bytes; -EINVAL for anything else. */
int  rx_stick_read_file(int root, const char *stick_path, uint8_t **data, size_t *size);
/* Writes out everything pending on the stick's file system (syncfs). */
int  rx_stick_sync(const char *mount);
/* Bytes the export of `plan` to `dest` needs free, the files counted in
 * whole clusters of `cluster` bytes. */
int64_t rx_export_space_needed(const RXPlan *plan, const char *dest, int64_t cluster);

#endif /* RXBOX_EXPORT_PRIV_H */
