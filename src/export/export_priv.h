/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* What the export module's files share: the plan's source and path rules
 * (dj's export.py: _needs_transcode, usb_path, _fat, _clean), cues in the
 * form the stick stores them (ms, colour code), and the sync record the
 * export leaves on the stick.
 *
 * The sync record, /PIONEER/cyanbox/sync.txt, is what makes a gig's changes
 * tellable from the library's: it lists, per exported track, the cues as
 * written to the ANLZ files, and the settings files as they were on the
 * stick after the export. Whatever differs from it later was written by a
 * player. Plain text, one item per line:
 *
 *   cyanbox-sync 1
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
 * are the same, and a record without the line (an older cyanbox) is treated
 * as no record. */

#ifndef CYANBOX_EXPORT_PRIV_H
#define CYANBOX_EXPORT_PRIV_H

#include "cyanbox/export.h"

#include <sys/stat.h>

#define CB_SYNC_PATH "/PIONEER/cyanbox/sync.txt"

/* ------------------------------------------------------------------ plan */

/* dj's _fat(_clean(s), limit): NFC, FAT-illegal characters to '_', trimmed,
 * cut to `limit` code points, trailing dots and spaces dropped, "_" when
 * nothing is left. */
char *cb_plan_fat_name(const char *s, size_t limit);
/* The file name stem a track gets on the stick before collision suffixes. */
char *cb_plan_usb_stem(const CBTrack *t);
/* Whether a stick path's file name is what the plan gives this track (the
 * stem, or its first 40 characters with a "-N" collision suffix): a guard
 * when a stick's track ids may not be ours. */
bool cb_plan_usb_name_matches(const CBTrack *t, const char *stick_path);
/* A path's key under FAT's and exFAT's case-insensitive names: up-cased
 * as exFAT's default up-case table does (plan.c); two paths with one key
 * are one file on the stick. */
char *cb_plan_fold_case(const char *s);

/* ----------------------------------------------------------- stick cues */

/* CBStickCue and cb_stick_cue_to_track: cyanbox/export.h. */
void cb_stick_cue_from_plan(const CBPlanCue *c, CBStickCue *s);
/* A library cue as the export would write it. */
void cb_stick_cue_from_track(const CBCue *c, CBStickCue *s);
/* Same position, loop, name and colour (1 ms tolerance for rounding). */
bool cb_stick_cue_equal(const CBStickCue *a, const CBStickCue *b);

/* ----------------------------------------------------------- sync record */

typedef struct CBSyncTrack {
    uint64_t    id;
    CBStickCue *cues;
    int         nb_cues;
} CBSyncTrack;

typedef struct CBSyncRecord {
    char         uuid[17];
    char         library[33];    /* "" when the record has none */
    char         date[16];
    CBSyncTrack *tracks;         /* sorted by id */
    int          nb_tracks;
    struct { char name[24]; uint8_t *data; size_t size; } settings[4];
    int          nb_settings;
} CBSyncRecord;

int  cb_sync_record_read(const char *mount, CBSyncRecord *r);   /* -ENOENT when absent */
int  cb_sync_record_write(int root, const CBSyncRecord *r);
void cb_sync_record_free(CBSyncRecord *r);
const CBSyncTrack *cb_sync_record_track(const CBSyncRecord *r, uint64_t id);
/* A fresh record for a plan: new uuid, every track's cues as written. */
int  cb_sync_record_from_plan(const CBPlan *plan, CBSyncRecord *r);
/* Adds the settings files found on the stick now. */
int  cb_sync_record_add_settings(CBSyncRecord *r, int root);
/* The library's identity for the record: 32 hex digits made once and kept
 * in the library (setting "sync.library"). */
int  cb_sync_library_key(CBLibrary *lib, char key[33]);

/* ----------------------------------------------------------------- files */

/* The file system type of a mount point ("vfat", "exfat" ...), -ENOENT
 * when `mount` is not one. */
int  cb_stick_fs_type(const char *mount, char *buf, size_t size);
/* <mount><stick path>, allocating. */
char *cb_stick_file(const char *mount, const char *stick_path);
/* A path read from a stick (a database row, an analysis path) that names a
 * file on the stick: absolute, and no empty, "." or ".." component. */
bool cb_stick_path_safe(const char *stick_path);
/* The stick's files by stick path, below a descriptor of its root
 * (cb_stick_root, an O_PATH descriptor of the mount) and nowhere else: no
 * symbolic link is followed on the way and no other file system mounted
 * inside is entered (ELOOP, EXDEV; stick.c). Errors are negative errno
 * values; the descriptors returned are O_CLOEXEC. */
int  cb_stick_root(const char *mount);
/* openat(2) of a file on the stick; with O_CREAT the missing directories
 * on the way are made. A trailing link is refused, or with O_PATH |
 * O_NOFOLLOW opened as itself. */
int  cb_stick_open(int root, const char *stick_path, int flags, mode_t mode);
int  cb_stick_mkdirs(int root, const char *stick_dir);
/* The file itself, as lstat(2) would see it. */
int  cb_stick_stat(int root, const char *stick_path, struct stat *st);
/* unlinkat(2) in the file's directory; `flags` 0 or AT_REMOVEDIR. */
int  cb_stick_unlink(int root, const char *stick_path, int flags);
/* Writes a file in place, creating its directory; no temporary, no sync
 * (cb_stick_sync at the end of an export covers everything at once). */
int  cb_stick_write(int root, const char *stick_path, const void *data, size_t size);
/* A regular file's bytes; -EINVAL for anything else. */
int  cb_stick_read_file(int root, const char *stick_path, uint8_t **data, size_t *size);
/* Writes out everything pending on the stick's file system (syncfs). */
int  cb_stick_sync(const char *mount);
/* Bytes the export of `plan` to `dest` needs free, the files counted in
 * whole clusters of `cluster` bytes. */
int64_t cb_export_space_needed(const CBPlan *plan, const char *dest, int64_t cluster);

#endif /* CYANBOX_EXPORT_PRIV_H */
