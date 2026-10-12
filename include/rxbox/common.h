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

enum RxLogLevel {
    RX_LOG_ERROR = 0,
    RX_LOG_WARN,
    RX_LOG_INFO,
    RX_LOG_DEBUG,
};

/* Default sink: stderr, at RX_LOG_INFO. `rx_log_set_level` raises or lowers
 * it; `rx_log_set_sink` redirects every line (the interface shows them in
 * the status bar). Thread safe. */
void rx_log(enum RxLogLevel level, const char *fmt, ...) RX_PRINTF(2, 3);
void rx_vlog(enum RxLogLevel level, const char *fmt, va_list args);
void rx_log_set_level(enum RxLogLevel level);
enum RxLogLevel rx_log_level(void);
typedef void (*rx_log_sink)(void *opaque, enum RxLogLevel level, const char *line);
void rx_log_set_sink(rx_log_sink sink, void *opaque);

#define rx_err(...)   rx_log(RX_LOG_ERROR, __VA_ARGS__)
#define rx_warn(...)  rx_log(RX_LOG_WARN, __VA_ARGS__)
#define rx_info(...)  rx_log(RX_LOG_INFO, __VA_ARGS__)
#define rx_debug(...) rx_log(RX_LOG_DEBUG, __VA_ARGS__)

/* ------------------------------------------------------------------ strings */

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
/* UTF-8 aware, case-insensitive "needle in haystack" for the search box.
 * Folds ASCII case and full-width ASCII; everything else compares as is. */
bool rx_str_contains_fold(const char *haystack, const char *needle);

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
/* Writes a file atomically: temp file next to it, fsync, rename. */
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
typedef struct RxArray {
    void  *data;
    size_t nb;        /* elements in use */
    size_t cap;       /* elements allocated */
    size_t elem;      /* element size in bytes */
} RxArray;

void  rx_array_init(RxArray *a, size_t elem_size);
/* Appends a copy of `elem` (or zeroed storage when NULL); returns the new
 * element or NULL on ENOMEM. */
void *rx_array_push(RxArray *a, const void *elem);
void *rx_array_at(const RxArray *a, size_t i);
int   rx_array_reserve(RxArray *a, size_t cap);
void  rx_array_remove(RxArray *a, size_t i);        /* keeps order */
void  rx_array_clear(RxArray *a);                   /* keeps storage */
void  rx_array_free(RxArray *a);

#endif /* RXBOX_COMMON_H */
