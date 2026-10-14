/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

#ifndef RXBOX_COMMON_H
#define RXBOX_COMMON_H

/* Shared by every module: error convention, small helpers, logging.
 *
 * Errors: functions return 0 on success and a negative errno on failure
 * (-ENOMEM, -EINVAL, -ENOENT, -EIO ...). RX_ERROR(e) builds one. Functions
 * that return a count return >= 0 on success. Nothing here allocates
 * global state; a module that needs state owns it in a context struct. */

#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define RX_ERROR(e) (-(e))

#define RX_ARRAY_ELEMS(a) (sizeof(a) / sizeof((a)[0]))
#define RX_MIN(a, b) ((a) < (b) ? (a) : (b))
#define RX_MAX(a, b) ((a) > (b) ? (a) : (b))
#define RX_CLAMP(x, lo, hi) RX_MIN(RX_MAX(x, lo), hi)
#define RX_ALIGN(x, a) (((x) + (a) - 1) / (a) * (a))

#define RX_UNUSED __attribute__((unused))
#define RX_PRINTF(fmt, args) __attribute__((format(printf, fmt, args)))
#define RX_WARN_UNUSED __attribute__((warn_unused_result))

/* ------------------------------------------------------------------ logging */

enum RXLogLevel {
    RX_LOG_ERROR = 0,
    RX_LOG_WARN,
    RX_LOG_INFO,
    RX_LOG_DEBUG,
};

/* Default sink: stderr, at RX_LOG_INFO. `rx_log_set_level` raises or lowers
 * it; `rx_log_set_sink` redirects every line (the interface shows them in
 * the status bar). Thread safe. */
void rx_log(enum RXLogLevel level, const char *fmt, ...) RX_PRINTF(2, 3);
void rx_vlog(enum RXLogLevel level, const char *fmt, va_list args);
void rx_log_set_level(enum RXLogLevel level);
enum RXLogLevel rx_log_level(void);
typedef void (*rx_log_sink)(void *opaque, enum RXLogLevel level, const char *line);
void rx_log_set_sink(rx_log_sink sink, void *opaque);

#define rx_err(...)   rx_log(RX_LOG_ERROR, __VA_ARGS__)
#define rx_warn(...)  rx_log(RX_LOG_WARN, __VA_ARGS__)
#define rx_info(...)  rx_log(RX_LOG_INFO, __VA_ARGS__)
#define rx_debug(...) rx_log(RX_LOG_DEBUG, __VA_ARGS__)

/* ------------------------------------------------------------------ strings */

/* UTF-8 (src/common/utf8.c). rx_utf8_next decodes one code point and
 * advances, an invalid byte giving U+FFFD and advancing by one (Python's
 * decode(errors="replace")); rx_utf8_put encodes one into buf (4 bytes of
 * room) and returns its length; rx_utf8_length counts code points. */
uint32_t rx_utf8_next(const char **p);
int      rx_utf8_put(char *buf, uint32_t c);
size_t   rx_utf8_length(const char *s);
/* Invalid bytes replaced by U+FFFD, then Unicode NFC (Unicode 16): every
 * text rxbox keeps or gives out. Allocated; NULL when out of memory. */
char    *rx_utf8_nfc(const char *s);
/* Whether s is NFC already (what rx_utf8_nfc would give back). */
bool     rx_utf8_is_nfc(const char *s);
/* Whether s is UTF-8 throughout. */
bool     rx_utf8_valid(const char *s);
/* s for a display that shows Latin-1 only (a CDJ-900's; src/common/latin1.c):
 * Japanese as romaji (kanji through MeCab, when there is a dictionary),
 * other scripts through ICU, fullwidth ASCII as ASCII, typographic and CJK
 * punctuation as its nearest Latin-1, the rest as it is. Allocated; NULL
 * when nothing changes (or out of memory). Thread-safe. */
char    *rx_utf8_latin1(const char *s);
/* Whether rx_utf8_latin1 reads kanji: built with MeCab, its dictionary
 * found (loads it, the first time). */
bool     rx_utf8_latin1_reads_kanji(void);

/* Duplicates (NULL in -> NULL out). Returns NULL on ENOMEM. */
char *rx_strdup(const char *s);
char *rx_strndup(const char *s, size_t n);
/* Allocating printf. Returns NULL on ENOMEM. */
char *rx_asprintf(const char *fmt, ...) RX_PRINTF(1, 2);
char *rx_vasprintf(const char *fmt, va_list args);
/* True when `s` starts with / ends with `pfx` / `sfx`. */
bool rx_str_starts(const char *s, const char *pfx);
bool rx_str_ends(const char *s, const char *sfx);
/* Case-insensitive ASCII compare, like strcasecmp. */
int rx_strcasecmp(const char *a, const char *b);
/* Natural order, case-insensitive (ASCII): a run of digits compares as the
 * number it writes, so "AD:HOUSE 3" comes before "AD:HOUSE 10" and
 * "Track 2" before "Track 10". Between names that differ only in leading
 * zeros the one with fewer comes first ("1" before "01"); everything else
 * compares as rx_strcasecmp does. 0 only for names rx_strcasecmp calls
 * equal. */
int rx_strnatcasecmp(const char *a, const char *b);
/* UTF-8 aware, case-insensitive "needle in haystack" for the search box.
 * Folds ASCII case and full-width ASCII; everything else compares as is. */
bool rx_str_contains_fold(const char *haystack, const char *needle);
/* Fuzzy matching, as fzf does it, for the Add dialog's finder: every word
 * of `query` (split at spaces) must be found in `text` with its letters in
 * order, gaps allowed, folded as rx_str_contains_fold folds. Each letter
 * found scores, more at the start of a path component or a word (after a
 * separator, a case change or a digit run) and right after the letter
 * before; each letter skipped costs; a word found whole, or wholly in the
 * last path component, scores more. The best placement of each word counts. Returns
 * false when a word is not found; *score (may be NULL) is the sum. `hits`
 * (may be NULL) gets the byte offsets in `text` of the letters matched, at
 * most max_hits, their count in *nb_hits. Only the last 1024 characters of
 * `text` and the first 256 of `query` are looked at. */
bool rx_fuzzy_match(const char *text, const char *query, int *score, int *hits, int max_hits,
                    int *nb_hits);

/* ------------------------------------------------------------- paths & files */

/* Joins `a` and `b` with one '/', allocating. NULL on ENOMEM. */
char *rx_path_join(const char *a, const char *b);
/* The part after the last '/', pointing into `path`. */
const char *rx_path_base(const char *path);
/* The extension including the dot, lower-cased into `buf`; "" if none. */
const char *rx_path_ext(const char *path, char *buf, size_t size);
/* Absolute, normalised path (realpath). NULL on failure. */
char *rx_path_abs(const char *path);
/* Expands a leading "~/". Allocating. */
char *rx_path_expand(const char *path);
/* mkdir -p. 0 or a negative errno. */
int rx_mkdir_p(const char *path);
/* Reads a whole file. `*out` is malloc'd with one extra NUL byte. */
int rx_file_read(const char *path, uint8_t **out, size_t *size);
/* Writes a file: open, write, close, nothing else. */
int rx_file_write(const char *path, const void *data, size_t size);
/* Writes a file atomically: temp file next to it, fsync, rename. For the
 * library's own files; a stick is synced once at the end of an export. */
int rx_file_write_atomic(const char *path, const void *data, size_t size);
bool rx_file_exists(const char *path);
bool rx_dir_exists(const char *path);
/* Modification time in seconds since the epoch, 0 if missing. */
int64_t rx_file_mtime(const char *path);
int64_t rx_file_size(const char *path);
/* Free bytes on the file system holding `path`; <0 on error. */
int64_t rx_fs_free_bytes(const char *path);

/* ------------------------------------------------------------------- time */

/* Monotonic microseconds. */
int64_t rx_time_us(void);
/* Wall clock seconds since the epoch. */
int64_t rx_time_now(void);
/* "m:ss.mmm" style formatting into buf; returns buf. */
const char *rx_fmt_time(double seconds, char *buf, size_t size);

/* ------------------------------------------------------------- dynamic array */

/* A minimal growable array of fixed-size elements, used throughout for
 * lists that are built then read. Elements are plain memory. */
typedef struct RXArray {
    void  *data;
    size_t nb;        /* elements in use */
    size_t cap;       /* elements allocated */
    size_t elem;      /* element size in bytes */
} RXArray;

void  rx_array_init(RXArray *a, size_t elem_size);
/* Appends a copy of `elem` (or zeroed storage when NULL); returns the new
 * element or NULL on ENOMEM. */
void *rx_array_push(RXArray *a, const void *elem);
void *rx_array_at(const RXArray *a, size_t i);
int   rx_array_reserve(RXArray *a, size_t cap);
void  rx_array_remove(RXArray *a, size_t i);        /* keeps order */
void  rx_array_clear(RXArray *a);                   /* keeps storage */
void  rx_array_free(RXArray *a);

#endif /* RXBOX_COMMON_H */
