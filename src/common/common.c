/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* The helpers of rxbox/common.h: strings, paths, files, time and the
 * growable array. Nothing here knows about tracks. */

#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>
#include <stdckdint.h>

#include "rxbox/common.h"

/* ------------------------------------------------------------------ strings */

char *rx_strdup(const char *s)
{
    if (!s)
        return NULL;
    size_t n = strlen(s) + 1;
    char *d = malloc(n);
    if (d)
        memcpy(d, s, n);
    return d;
}

char *rx_strndup(const char *s, size_t n)
{
    if (!s)
        return NULL;
    size_t len = strnlen(s, n);
    char *d = malloc(len + 1);
    if (!d)
        return NULL;
    memcpy(d, s, len);
    d[len] = '\0';
    return d;
}

char *rx_vasprintf(const char *fmt, va_list args)
{
    va_list copy;
    va_copy(copy, args);
    int n = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    if (n < 0)
        return NULL;
    char *s = malloc((size_t)n + 1);
    if (!s)
        return NULL;
    vsnprintf(s, (size_t)n + 1, fmt, args);
    return s;
}

char *rx_asprintf(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    char *s = rx_vasprintf(fmt, args);
    va_end(args);
    return s;
}

bool rx_str_starts(const char *s, const char *pfx)
{
    return s && pfx && strncmp(s, pfx, strlen(pfx)) == 0;
}

bool rx_str_ends(const char *s, const char *sfx)
{
    if (!s || !sfx)
        return false;
    size_t ls = strlen(s), lx = strlen(sfx);
    return lx <= ls && memcmp(s + ls - lx, sfx, lx) == 0;
}

int rx_strcasecmp(const char *a, const char *b)
{
    while (*a && *b) {
        int ca = tolower((unsigned char)*a), cb = tolower((unsigned char)*b);
        if (ca != cb)
            return ca - cb;
        a++;
        b++;
    }
    return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}

/* Decodes one UTF-8 code point; invalid bytes are returned as themselves
 * so a search never stalls on bad input. */
static uint32_t utf8_next(const char **p)
{
    const unsigned char *s = (const unsigned char *)*p;
    uint32_t c = s[0];
    int n = 0;

    if (c >= 0xF0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80 && (s[3] & 0xC0) == 0x80) {
        c = ((c & 0x07) << 18) | ((s[1] & 0x3F) << 12) | ((s[2] & 0x3F) << 6) | (s[3] & 0x3F);
        n = 4;
    } else if (c >= 0xE0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
        c = ((c & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
        n = 3;
    } else if (c >= 0xC0 && (s[1] & 0xC0) == 0x80) {
        c = ((c & 0x1F) << 6) | (s[1] & 0x3F);
        n = 2;
    } else {
        n = 1;
    }
    *p += n;
    return c;
}

/* Folds ASCII case, full-width ASCII (U+FF01..FF5E) to ASCII, and the
 * ideographic space to a space: enough for searching a library of
 * Japanese and English titles. */
static uint32_t fold(uint32_t c)
{
    if (c >= 0xFF01 && c <= 0xFF5E)
        c = c - 0xFF01 + 0x21;
    if (c == 0x3000)
        c = ' ';
    if (c < 128)
        c = (uint32_t)tolower((int)c);
    return c;
}

/* The whole needle matches at q. Both sides are decoded as they are
 * compared, so a needle of any length is compared in full. */
static bool match_at(const char *q, const char *n)
{
    while (*n) {
        if (!*q || fold(utf8_next(&q)) != fold(utf8_next(&n)))
            return false;
    }
    return true;
}

bool rx_str_contains_fold(const char *haystack, const char *needle)
{
    if (!needle || !*needle)
        return true;
    if (!haystack)
        return false;

    for (const char *h = haystack; *h; utf8_next(&h)) {
        if (match_at(h, needle))
            return true;
    }
    return false;
}

/* ------------------------------------------------------------- paths & files */

char *rx_path_join(const char *a, const char *b)
{
    if (!a || !*a)
        return rx_strdup(b);
    if (!b || !*b)
        return rx_strdup(a);
    if (b[0] == '/')
        return rx_strdup(b);
    size_t la = strlen(a);
    while (la > 1 && a[la - 1] == '/')
        la--;
    /* Only the root itself still ends in '/': "/" + "x" is "/x". */
    return rx_asprintf("%.*s%s%s", (int)la, a, a[la - 1] == '/' ? "" : "/", b);
}

const char *rx_path_base(const char *path)
{
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

const char *rx_path_ext(const char *path, char *buf, size_t size)
{
    const char *base = rx_path_base(path);
    const char *dot = strrchr(base, '.');
    if (!dot || dot == base || size == 0) {
        if (size)
            buf[0] = '\0';
        return buf;
    }
    size_t i = 0;
    while (dot[i] && i + 1 < size) {
        buf[i] = (char)tolower((unsigned char)dot[i]);
        i++;
    }
    buf[i] = '\0';
    return buf;
}

char *rx_path_abs(const char *path)
{
    char *expanded = rx_path_expand(path);
    if (!expanded)
        return NULL;
    char *abs = realpath(expanded, NULL);
    if (!abs) {
        /* The file may not exist yet: resolve the directory part, which
         * for "/name" is the root. */
        char *slash = strrchr(expanded, '/');
        if (slash) {
            *slash = '\0';
            char *dir = realpath(slash == expanded ? "/" : expanded, NULL);
            if (dir) {
                abs = rx_path_join(dir, slash + 1);
                free(dir);
            }
        } else {
            char cwd[PATH_MAX];
            if (getcwd(cwd, sizeof(cwd)))
                abs = rx_path_join(cwd, expanded);
        }
    }
    free(expanded);
    return abs;
}

char *rx_path_expand(const char *path)
{
    if (path && path[0] == '~' && (path[1] == '/' || path[1] == '\0')) {
        const char *home = getenv("HOME");
        if (home && *home)
            return rx_path_join(home, path + 1 + (path[1] == '/'));
    }
    return rx_strdup(path);
}

int rx_mkdir_p(const char *path)
{
    /* The loop below starts after the first byte, so "" needs its own
     * answer: mkdir("")'s. */
    if (!path || !*path)
        return RX_ERROR(ENOENT);
    char *tmp = rx_strdup(path);
    if (!tmp)
        return RX_ERROR(ENOMEM);
    int ret = 0;
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        if (mkdir(tmp, 0755) < 0 && errno != EEXIST) {
            ret = RX_ERROR(errno);
            break;
        }
        *p = '/';
    }
    if (!ret && mkdir(tmp, 0755) < 0) {
        /* EEXIST is success only when what is there is a directory (a
         * file in the middle of the path fails the next mkdir instead). */
        int err = errno;
        if (err != EEXIST)
            ret = RX_ERROR(err);
        else if (!rx_dir_exists(tmp))
            ret = RX_ERROR(ENOTDIR);
    }
    free(tmp);
    return ret;
}

int rx_file_read(const char *path, uint8_t **out, size_t *size)
{
    *out = NULL;
    *size = 0;
    FILE *f = fopen(path, "rb");
    if (!f)
        return RX_ERROR(errno);
    int ret = 0;
    size_t cap = 0, len = 0;
    uint8_t *buf = NULL;
    while (1) {
        if (len + 65536 + 1 > cap) {
            size_t ncap = cap ? cap * 2 : 262144;
            uint8_t *nb = realloc(buf, ncap);
            if (!nb) {
                ret = RX_ERROR(ENOMEM);
                break;
            }
            buf = nb;
            cap = ncap;
        }
        size_t n = fread(buf + len, 1, cap - len - 1, f);
        len += n;
        if (n == 0) {
            if (ferror(f))
                ret = RX_ERROR(EIO);
            break;
        }
    }
    fclose(f);
    if (ret) {
        free(buf);
        return ret;
    }
    buf[len] = '\0';
    *out = buf;
    *size = len;
    return 0;
}

int rx_file_write_atomic(const char *path, const void *data, size_t size)
{
    /* The temporary is named after the thread, not the process: two
     * threads saving the same file at once would otherwise truncate and
     * rename each other's temporary. (The main thread's id is the pid.) */
    char *tmp = rx_asprintf("%s.tmp-%ld", path, (long)gettid());
    if (!tmp)
        return RX_ERROR(ENOMEM);
    int ret = 0;
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        ret = RX_ERROR(errno);
        goto out;
    }
    const uint8_t *p = data;
    size_t left = size;
    while (left) {
        ssize_t n = write(fd, p, left);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            ret = RX_ERROR(errno);
            break;
        }
        p += n;
        left -= (size_t)n;
    }
    if (!ret && fsync(fd) < 0)
        ret = RX_ERROR(errno);
    if (close(fd) < 0 && !ret)
        ret = RX_ERROR(errno);
    if (!ret && rename(tmp, path) < 0)
        ret = RX_ERROR(errno);
    if (ret) {
        unlink(tmp);
    } else {
        /* The rename is durable only once the directory entry is: fsync
         * the parent, or a crash can bring back the old name with the
         * new data already gone. Failing to open the directory (a FAT
         * stick refuses O_RDONLY on some kernels) is not an error for the
         * file itself. */
        char *dir = rx_strdup(path);
        if (dir) {
            char *slash = strrchr(dir, '/');
            int dfd = open(slash && slash != dir ? (*slash = '\0', dir) : ".", O_RDONLY | O_DIRECTORY);
            if (dfd >= 0) {
                fsync(dfd);
                close(dfd);
            }
            free(dir);
        }
    }
out:
    free(tmp);
    return ret;
}

bool rx_file_exists(const char *path)
{
    struct stat st;
    return path && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

bool rx_dir_exists(const char *path)
{
    struct stat st;
    return path && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

int64_t rx_file_mtime(const char *path)
{
    struct stat st;
    if (!path || stat(path, &st) < 0)
        return 0;
    return (int64_t)st.st_mtime;
}

int64_t rx_file_size(const char *path)
{
    struct stat st;
    if (!path || stat(path, &st) < 0)
        return -1;
    return (int64_t)st.st_size;
}

int64_t rx_fs_free_bytes(const char *path)
{
    struct statvfs vfs;
    if (statvfs(path, &vfs) < 0)
        return RX_ERROR(errno);
    return (int64_t)vfs.f_bavail * (int64_t)vfs.f_frsize;
}

/* ------------------------------------------------------------------- time */

int64_t rx_time_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

int64_t rx_time_now(void)
{
    return (int64_t)time(NULL);
}

const char *rx_fmt_time(double seconds, char *buf, size_t size)
{
    bool neg = seconds < 0;
    if (neg)
        seconds = -seconds;
    int whole = (int)seconds;
    int ms = (int)((seconds - whole) * 1000.0 + 0.5);
    if (ms >= 1000) {
        whole++;
        ms -= 1000;
    }
    if (whole >= 3600)
        snprintf(buf, size, "%s%d:%02d:%02d.%03d", neg ? "-" : "", whole / 3600, whole / 60 % 60, whole % 60, ms);
    else
        snprintf(buf, size, "%s%d:%02d.%03d", neg ? "-" : "", whole / 60, whole % 60, ms);
    return buf;
}

/* ------------------------------------------------------------- dynamic array */

void rx_array_init(RxArray *a, size_t elem_size)
{
    memset(a, 0, sizeof(*a));
    a->elem = elem_size;
}

int rx_array_reserve(RxArray *a, size_t cap)
{
    if (cap <= a->cap)
        return 0;
    size_t bytes;
    if (ckd_mul(&bytes, cap, a->elem))
        return RX_ERROR(ENOMEM);
    void *d = realloc(a->data, bytes);
    if (!d)
        return RX_ERROR(ENOMEM);
    a->data = d;
    a->cap = cap;
    return 0;
}

void *rx_array_push(RxArray *a, const void *elem)
{
    if (a->nb == a->cap && rx_array_reserve(a, a->cap ? a->cap * 2 : 16) < 0)
        return NULL;
    uint8_t *slot = (uint8_t *)a->data + a->nb * a->elem;
    if (elem)
        memcpy(slot, elem, a->elem);
    else
        memset(slot, 0, a->elem);
    a->nb++;
    return slot;
}

void *rx_array_at(const RxArray *a, size_t i)
{
    return i < a->nb ? (uint8_t *)a->data + i * a->elem : NULL;
}

void rx_array_remove(RxArray *a, size_t i)
{
    if (i >= a->nb)
        return;
    uint8_t *d = a->data;
    memmove(d + i * a->elem, d + (i + 1) * a->elem, (a->nb - i - 1) * a->elem);
    a->nb--;
}

void rx_array_clear(RxArray *a)
{
    a->nb = 0;
}

void rx_array_free(RxArray *a)
{
    free(a->data);
    a->data = NULL;
    a->nb = a->cap = 0;
}
