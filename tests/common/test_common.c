/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* The helpers of src/common/common.c at their edges: mkdir -p on an empty
 * path and on a file, rx_path_join onto the root, rx_path_abs on paths
 * that do not exist (at the root, in a directory, bare),
 * rx_file_write_atomic's error paths and two threads saving the same file
 * at once, and rx_str_contains_fold with needles longer than its old
 * decode buffer and with invalid UTF-8 (in heap copies of exactly their
 * length, so ASan sees any read past the end). Everything happens in a
 * fresh directory under /tmp, removed at the end. */

#include "rxbox/common.h"

#include <dirent.h>
#include <ftw.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;

#define CHECK(cond, ...)                                              \
    do {                                                              \
        if (!(cond)) {                                                \
            fprintf(stderr, "%s:%d: %s: ", __FILE__, __LINE__, #cond); \
            fprintf(stderr, __VA_ARGS__);                             \
            fputc('\n', stderr);                                      \
            failures++;                                               \
        }                                                             \
    } while (0)

static char dir[64];

static char *in_dir(const char *name)
{
    char *p = rx_path_join(dir, name);
    if (!p)
        abort();
    return p;
}

/* Files in the test directory whose name contains `part`. */
static int count_files(const char *sub, const char *part)
{
    char *d = sub ? in_dir(sub) : rx_strdup(dir);
    DIR *dh = opendir(d);
    free(d);
    if (!dh)
        return -1;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(dh)))
        n += strstr(e->d_name, part) != NULL;
    closedir(dh);
    return n;
}

/* ------------------------------------------------------------------ mkdir */

static void test_mkdir(void)
{
    /* An empty path: an error, without reading past the one byte. */
    CHECK(rx_mkdir_p("") < 0, "empty path");

    char *nested = in_dir("a/b/c");
    CHECK(rx_mkdir_p(nested) == 0 && rx_dir_exists(nested), "nested");
    CHECK(rx_mkdir_p(nested) == 0, "again: already there");

    /* A file where the directory should be is not a directory. */
    char *file = in_dir("a/file");
    CHECK(rx_file_write_atomic(file, "x", 1) == 0, "file");
    CHECK(rx_mkdir_p(file) < 0, "mkdir -p on a file");
    char *below = in_dir("a/file/sub");
    CHECK(rx_mkdir_p(below) < 0, "below a file");
    free(below);
    free(file);
    free(nested);
}

/* ------------------------------------------------------------------ paths */

static void check_join(const char *a, const char *b, const char *want)
{
    char *j = rx_path_join(a, b);
    CHECK(j && !strcmp(j, want), "join(%s, %s) = %s, want %s", a, b, j, want);
    free(j);
}

static void test_path_abs(void)
{
    /* One '/' between the parts, the root included. */
    check_join("/", "x", "/x");
    check_join("//", "x", "/x");
    check_join("a/", "b", "a/b");
    check_join("a///", "b/c", "a/b/c");
    check_join("a", "/b", "/b");

    char *real_dir = rx_path_abs(dir);
    CHECK(real_dir && real_dir[0] == '/', "the test directory");

    /* Not there yet, in a directory that is. */
    char *missing = in_dir("not-yet.txt");
    char *abs = rx_path_abs(missing);
    char *want = rx_path_join(real_dir, "not-yet.txt");
    CHECK(abs && want && !strcmp(abs, want), "%s, want %s", abs, want);
    free(abs);
    free(want);
    free(missing);

    /* Not there, directly under the root: the directory part is "/". */
    abs = rx_path_abs("/rxbox-test-no-such-file");
    CHECK(abs && !strcmp(abs, "/rxbox-test-no-such-file"), "at the root: %s", abs);
    free(abs);

    /* A bare name that does not exist is in the working directory. */
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd))) {
        abs = rx_path_abs("rxbox-test-no-such-file");
        want = rx_path_join(cwd, "rxbox-test-no-such-file");
        CHECK(abs && want && !strcmp(abs, want), "bare: %s", abs);
        free(abs);
        free(want);
    }

    /* A missing directory part is a failure, as for realpath. */
    char *deep = in_dir("no/such/dir/file");
    CHECK(rx_path_abs(deep) == NULL, "missing directory");
    free(deep);
    free(real_dir);
}

/* ------------------------------------------------------------ atomic write */

#define WRITES 300
#define WRITE_SIZE 65536

typedef struct Writer {
    const char *path;
    char        byte;
    int         errors;
} Writer;

static void *writer(void *arg)
{
    Writer *w = arg;
    char *buf = malloc(WRITE_SIZE);
    if (!buf)
        abort();
    memset(buf, w->byte, WRITE_SIZE);
    for (int i = 0; i < WRITES; i++)
        w->errors += rx_file_write_atomic(w->path, buf, WRITE_SIZE) < 0;
    free(buf);
    return NULL;
}

static void test_write_atomic(void)
{
    /* Into a directory that does not exist: the error, and no temporary. */
    char *nowhere = in_dir("nowhere/file");
    CHECK(rx_file_write_atomic(nowhere, "x", 1) == RX_ERROR(ENOENT), "missing directory");
    free(nowhere);

    /* Over a directory: rename fails, and the temporary is removed. */
    char *sub = in_dir("w");
    CHECK(rx_mkdir_p(sub) == 0, "mkdir");
    char *over = in_dir("w/dir");
    CHECK(rx_mkdir_p(over) == 0, "mkdir");
    CHECK(rx_file_write_atomic(over, "x", 1) < 0, "over a directory");
    CHECK(count_files("w", ".tmp") == 0, "no temporary left");

    /* Two threads saving the same file: every save succeeds and the file
     * is always one of them whole, never a mix or a truncation. */
    char *path = in_dir("w/shared");
    Writer w[2] = {
        { .path = path, .byte = 'A' },
        { .path = path, .byte = 'B' },
    };
    pthread_t th[2];
    for (int i = 0; i < 2; i++)
        pthread_create(&th[i], NULL, writer, &w[i]);
    for (int i = 0; i < 2; i++)
        pthread_join(th[i], NULL);
    CHECK(w[0].errors == 0 && w[1].errors == 0, "failed saves: %d %d", w[0].errors, w[1].errors);

    uint8_t *data;
    size_t size;
    CHECK(rx_file_read(path, &data, &size) == 0, "read back");
    if (data) {
        size_t same = 0;
        while (same < size && data[same] == data[0])
            same++;
        CHECK(size == WRITE_SIZE && same == size, "%zu bytes, %zu of them like the first",
              size, same);
        free(data);
    }
    CHECK(count_files("w", ".tmp") == 0, "no temporary left");

    free(path);
    free(over);
    free(sub);
}

/* ---------------------------------------------------------------- search */

/* A heap copy of exactly the string, so a read past its NUL is caught. */
static char *exact(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = malloc(n);
    if (!d)
        abort();
    memcpy(d, s, n);
    return d;
}

static bool contains(const char *h, const char *n)
{
    char *hh = exact(h), *nn = exact(n);
    bool r = rx_str_contains_fold(hh, nn);
    free(hh);
    free(nn);
    return r;
}

static void test_contains_fold(void)
{
    CHECK(contains("Daft Punk", "PUNK"), "ASCII case");
    CHECK(contains("ＤＡＦＴ", "daft"), "full width");
    CHECK(contains("a\xe3\x80\x80" "b", "a b"), "ideographic space");
    CHECK(!contains("Daft", "Punk"), "absent");
    CHECK(contains("x", "") && !rx_str_contains_fold(NULL, "x"), "empty and NULL");

    /* Needles longer than 256 characters compare in full. */
    char hay[400], needle[400];
    memset(hay, 'a', 300);
    hay[300] = 'c';
    hay[301] = '\0';
    memset(needle, 'a', 299);
    needle[299] = 'b';
    needle[300] = '\0';
    CHECK(!contains(hay, needle), "a 300 character needle that differs at the end");
    needle[299] = 'a';
    CHECK(contains(hay, needle), "and one that matches");
    needle[299] = 'A';
    needle[300] = 'C';
    needle[301] = '\0';
    CHECK(contains(hay, needle), "folded, to the end of the haystack");
    memset(needle, 'a', 302);
    needle[302] = '\0';
    CHECK(!contains(hay, needle), "longer than the run");

    /* Invalid UTF-8: truncated sequences at the end of either string,
     * stray continuation bytes, bytes that are never valid. Nothing may
     * be read past the end, and ASCII next to them is still found. */
    static const char *const bad[] = {
        "\xf0", "\xf0\x9f", "\xf0\x9f\x8e", "\xe2\x82", "\xc3", "\x80", "\xbf\xbf",
        "\xff\xfe", "\xc0\x80", "abc\xf0", "abc\xe2\x82", "\xf8\x88\x80\x80\x80",
    };
    for (size_t i = 0; i < RX_ARRAY_ELEMS(bad); i++) {
        for (size_t j = 0; j < RX_ARRAY_ELEMS(bad); j++)
            contains(bad[i], bad[j]);
        CHECK(contains(bad[i], bad[i]), "%zu finds itself", i);
        char *h = rx_asprintf("%sKey%s", bad[i], bad[i]);
        CHECK(h && contains(h, "key"), "%zu: ASCII between invalid bytes", i);
        free(h);
    }
    CHECK(contains("\x80" "abc", "ABC") && contains("abc\xc3", "\xc3"), "stray bytes");
}

static int fuzzy(const char *text, const char *query)
{
    int s = 0;
    return rx_fuzzy_match(text, query, &s, NULL, 0, NULL) ? s : -1;
}

/* rx_fuzzy_match, the Add dialog's finder: words in any order, letters in
 * order with gaps, folded; a letter at the start of a component or a word
 * and a run of letters rank higher, a word in the last component higher
 * still; the hits are byte offsets; invalid UTF-8 and long texts are safe. */
static void test_fuzzy(void)
{
    const char *felic = "AD:HOUSE 11 [FLAC]/2.01 - Felicidade.flac";
    int hits[16], nh = 0, s = 0;
    CHECK(rx_fuzzy_match(felic, "felic", &s, hits, 16, &nh) && nh == 5, "felic: %d hits", nh);
    const char *at = strstr(felic, "Felic");
    for (int i = 0; i < nh; i++)
        CHECK(hits[i] == (int)(at - felic) + i, "hit %d at %d", i, hits[i]);
    CHECK(fuzzy(felic, "adhouse 11") >= 0, "letters apart, two words");
    CHECK(fuzzy(felic, "11 adhouse") >= 0, "words in any order");
    CHECK(fuzzy(felic, "flac felicidade xyz") < 0, "every word must be there");
    CHECK(fuzzy(felic, "edadicilef") < 0, "letters in order");
    CHECK(fuzzy(felic, "") == 0 && fuzzy(felic, "   ") == 0, "an empty query matches");

    CHECK(fuzzy("Dub Techno", "dub") > fuzzy("Studio Rubber", "dub"), "a word's start ranks higher");
    CHECK(fuzzy("x/Felicidade.flac", "felic") > fuzzy("x/F-e-l-i-c.flac", "felic"),
          "a run ranks higher");
    CHECK(fuzzy("Live/Album.flac", "album") > fuzzy("Album/Live.flac", "album"),
          "the last component ranks higher");
    CHECK(fuzzy("DeepHouse", "h") > fuzzy("deephouse", "h"), "a case change starts a word");
    CHECK(fuzzy("x/2.01 - Felicidade.flac", "felic") > fuzzy("x/Felix the Cat", "felic"),
          "a word found whole ranks higher");
    CHECK(fuzzy("ＤＡＦＴ ＰＵＮＫ/one.flac", "daft punk") >= 0, "full width folds");

    /* Byte offsets after multibyte characters. */
    CHECK(rx_fuzzy_match("夏恋/Summer.flac", "sum", NULL, hits, 16, &nh) && nh == 3 &&
          hits[0] == 7 && hits[1] == 8 && hits[2] == 9, "hits after UTF-8: %d %d", nh, hits[0]);
    /* At most max_hits, but the match is still the whole query's. */
    CHECK(rx_fuzzy_match(felic, "felicidade", NULL, hits, 3, &nh) && nh == 3, "max_hits");

    /* A text longer than the 1024 characters looked at: its end counts. */
    char *long_path = malloc(3000);
    if (long_path) {
        memset(long_path, 'a', 2000);
        strcpy(long_path + 2000, "/target.flac");
        CHECK(rx_fuzzy_match(long_path, "target", &s, hits, 16, &nh) && nh == 6 &&
              hits[0] == 2001, "the end of a long text: %d", nh ? hits[0] : -1);
        long_path[0] = 'z';
        CHECK(fuzzy(long_path, "zt") < 0, "its start is not looked at");
        free(long_path);
    }

    /* Invalid UTF-8 on either side: no read past the end, no hang. */
    static const char *const bad[] = {
        "\xf0", "\xf0\x9f", "\xe2\x82", "\xc3", "\x80", "\xff\xfe", "abc\xf0", "\xc0\x80",
    };
    for (size_t i = 0; i < RX_ARRAY_ELEMS(bad); i++) {
        for (size_t j = 0; j < RX_ARRAY_ELEMS(bad); j++)
            fuzzy(bad[i], bad[j]);
        char *t = rx_asprintf("%sKey/name%s", bad[i], bad[i]);
        CHECK(t && fuzzy(t, "key name") >= 0, "%zu: ASCII between invalid bytes", i);
        free(t);
    }
    CHECK(!rx_fuzzy_match(NULL, "a", NULL, NULL, 0, NULL) &&
          !rx_fuzzy_match("a", NULL, NULL, NULL, 0, NULL), "NULL");
}

static int remove_entry(const char *path, const struct stat *st, int flag, struct FTW *ftw)
{
    return remove(path);
}

int main(void)
{
    snprintf(dir, sizeof(dir), "/tmp/rxbox-test-common-XXXXXX");
    if (!mkdtemp(dir)) {
        perror("mkdtemp");
        return 1;
    }

    test_mkdir();
    test_path_abs();
    test_write_atomic();
    test_contains_fold();
    test_fuzzy();

    if (nftw(dir, remove_entry, 16, FTW_DEPTH | FTW_PHYS) < 0)
        fprintf(stderr, "could not remove %s\n", dir);

    if (failures) {
        fprintf(stderr, "common_helpers: %d failures\n", failures);
        return 1;
    }
    printf("common_helpers: all tests passed\n");
    return 0;
}
