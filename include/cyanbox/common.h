/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

#ifndef CYANBOX_COMMON_H
#define CYANBOX_COMMON_H

/* Shared by every module: error convention, small helpers, logging.
 *
 * Errors: functions return 0 on success and a negative errno on failure
 * (-ENOMEM, -EINVAL, -ENOENT, -EIO ...). CB_ERROR(e) builds one. Functions
 * that return a count return >= 0 on success. Nothing here allocates
 * global state; a module that needs state owns it in a context struct. */

#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define CB_ERROR(e) (-(e))

#define CB_ARRAY_ELEMS(a) (sizeof(a) / sizeof((a)[0]))
#define CB_MIN(a, b) ((a) < (b) ? (a) : (b))
#define CB_MAX(a, b) ((a) > (b) ? (a) : (b))
#define CB_CLAMP(x, lo, hi) CB_MIN(CB_MAX(x, lo), hi)
#define CB_ALIGN(x, a) (((x) + (a) - 1) / (a) * (a))

#define CB_UNUSED __attribute__((unused))
#define CB_PRINTF(fmt, args) __attribute__((format(printf, fmt, args)))
#define CB_WARN_UNUSED __attribute__((warn_unused_result))

/* ------------------------------------------------------------------ logging */

enum CBLogLevel {
    CB_LOG_ERROR = 0,
    CB_LOG_WARN,
    CB_LOG_INFO,
    CB_LOG_DEBUG,
};

/* Default sink: stderr, at CB_LOG_INFO. `cb_log_set_level` raises or lowers
 * it; `cb_log_set_sink` redirects every line (the interface shows them in
 * the status bar). Thread safe. */
void cb_log(enum CBLogLevel level, const char *fmt, ...) CB_PRINTF(2, 3);
void cb_vlog(enum CBLogLevel level, const char *fmt, va_list args);
void cb_log_set_level(enum CBLogLevel level);
enum CBLogLevel cb_log_level(void);
typedef void (*cb_log_sink)(void *opaque, enum CBLogLevel level, const char *line);
void cb_log_set_sink(cb_log_sink sink, void *opaque);

#define cb_err(...)   cb_log(CB_LOG_ERROR, __VA_ARGS__)
#define cb_warn(...)  cb_log(CB_LOG_WARN, __VA_ARGS__)
#define cb_info(...)  cb_log(CB_LOG_INFO, __VA_ARGS__)
#define cb_debug(...) cb_log(CB_LOG_DEBUG, __VA_ARGS__)

/* ------------------------------------------------------------------ strings */

/* UTF-8 (src/common/utf8.c). cb_utf8_next decodes one code point and
 * advances, an invalid byte giving U+FFFD and advancing by one (Python's
 * decode(errors="replace")); cb_utf8_put encodes one into buf (4 bytes of
 * room) and returns its length; cb_utf8_length counts code points. */
uint32_t cb_utf8_next(const char **p);
int      cb_utf8_put(char *buf, uint32_t c);
size_t   cb_utf8_length(const char *s);
/* Invalid bytes replaced by U+FFFD, then Unicode NFC (Unicode 16): every
 * text cyanbox keeps or gives out. Allocated; NULL when out of memory. */
char    *cb_utf8_nfc(const char *s);
/* Whether s is NFC already (what cb_utf8_nfc would give back). */
bool     cb_utf8_is_nfc(const char *s);
/* Whether s is UTF-8 throughout. */
bool     cb_utf8_valid(const char *s);
/* s for a display that shows Latin-1 only (a CDJ-900's; src/common/latin1.c):
 * Japanese as romaji (kanji through MeCab, when there is a dictionary),
 * other scripts through ICU, fullwidth ASCII as ASCII, typographic and CJK
 * punctuation as its nearest Latin-1, the rest as it is. Allocated; NULL
 * when nothing changes (or out of memory). Thread-safe. */
char    *cb_utf8_latin1(const char *s);
/* Whether cb_utf8_latin1 reads kanji: built with MeCab, its dictionary
 * found (loads it, the first time). */
bool     cb_utf8_latin1_reads_kanji(void);

/* Duplicates (NULL in -> NULL out). Returns NULL on ENOMEM. */
char *cb_strdup(const char *s);
char *cb_strndup(const char *s, size_t n);
/* Allocating printf. Returns NULL on ENOMEM. */
char *cb_asprintf(const char *fmt, ...) CB_PRINTF(1, 2);
char *cb_vasprintf(const char *fmt, va_list args);
/* True when `s` starts with / ends with `pfx` / `sfx`. */
bool cb_str_starts(const char *s, const char *pfx);
bool cb_str_ends(const char *s, const char *sfx);
/* Case-insensitive ASCII compare, like strcasecmp. */
int cb_strcasecmp(const char *a, const char *b);
/* Natural order, case-insensitive (ASCII): a run of digits compares as the
 * number it writes, so "AD:HOUSE 3" comes before "AD:HOUSE 10" and
 * "Track 2" before "Track 10". Between names that differ only in leading
 * zeros the one with fewer comes first ("1" before "01"); everything else
 * compares as cb_strcasecmp does. 0 only for names cb_strcasecmp calls
 * equal. */
int cb_strnatcasecmp(const char *a, const char *b);
/* UTF-8 aware, case-insensitive "needle in haystack" for the search box.
 * Folds ASCII case and full-width ASCII; everything else compares as is. */
bool cb_str_contains_fold(const char *haystack, const char *needle);
/* Fuzzy matching, as fzf does it, for the Add dialog's finder: every word
 * of `query` (split at spaces) must be found in `text` with its letters in
 * order, gaps allowed, folded as cb_str_contains_fold folds. Each letter
 * found scores, more at the start of a path component or a word (after a
 * separator, a case change or a digit run) and right after the letter
 * before; each letter skipped costs; a word found whole, or wholly in the
 * last path component, scores more. The best placement of each word counts. Returns
 * false when a word is not found; *score (may be NULL) is the sum. `hits`
 * (may be NULL) gets the byte offsets in `text` of the letters matched, at
 * most max_hits, their count in *nb_hits. Only the last 1024 characters of
 * `text` and the first 256 of `query` are looked at. */
bool cb_fuzzy_match(const char *text, const char *query, int *score, int *hits, int max_hits,
                    int *nb_hits);

/* ------------------------------------------------------------- paths & files */

/* Joins `a` and `b` with one '/', allocating. NULL on ENOMEM. */
char *cb_path_join(const char *a, const char *b);
/* The part after the last '/', pointing into `path`. */
const char *cb_path_base(const char *path);
/* The extension including the dot, lower-cased into `buf`; "" if none. */
const char *cb_path_ext(const char *path, char *buf, size_t size);
/* Absolute, normalised path (realpath). NULL on failure. */
char *cb_path_abs(const char *path);
/* Expands a leading "~/". Allocating. */
char *cb_path_expand(const char *path);
/* mkdir -p. 0 or a negative errno. */
int cb_mkdir_p(const char *path);
/* Reads a whole file. `*out` is malloc'd with one extra NUL byte. */
int cb_file_read(const char *path, uint8_t **out, size_t *size);
/* Writes a file: open, write, close, nothing else. */
int cb_file_write(const char *path, const void *data, size_t size);
/* Writes a file atomically: temp file next to it, fsync, rename. For the
 * library's own files; a stick is synced once at the end of an export. */
int cb_file_write_atomic(const char *path, const void *data, size_t size);
bool cb_file_exists(const char *path);
bool cb_dir_exists(const char *path);
/* Modification time in seconds since the epoch, 0 if missing. */
int64_t cb_file_mtime(const char *path);
int64_t cb_file_size(const char *path);
/* Free bytes on the file system holding `path`; <0 on error. */
int64_t cb_fs_free_bytes(const char *path);

/* ------------------------------------------------------------------- time */

/* Monotonic microseconds. */
int64_t cb_time_us(void);
/* Wall clock seconds since the epoch. */
int64_t cb_time_now(void);
/* "m:ss.mmm" style formatting into buf; returns buf. */
const char *cb_fmt_time(double seconds, char *buf, size_t size);

/* ------------------------------------------------------------- dynamic array */

/* A minimal growable array of fixed-size elements, used throughout for
 * lists that are built then read. Elements are plain memory. */
typedef struct CBArray {
    void  *data;
    size_t nb;        /* elements in use */
    size_t cap;       /* elements allocated */
    size_t elem;      /* element size in bytes */
} CBArray;

void  cb_array_init(CBArray *a, size_t elem_size);
/* Appends a copy of `elem` (or zeroed storage when NULL); returns the new
 * element or NULL on ENOMEM. */
void *cb_array_push(CBArray *a, const void *elem);
void *cb_array_at(const CBArray *a, size_t i);
int   cb_array_reserve(CBArray *a, size_t cap);
void  cb_array_remove(CBArray *a, size_t i);        /* keeps order */
void  cb_array_clear(CBArray *a);                   /* keeps storage */
void  cb_array_free(CBArray *a);

#endif /* CYANBOX_COMMON_H */
