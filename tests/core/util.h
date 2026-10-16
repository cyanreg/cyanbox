/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* Shared by the core tests: a CHECK macro that reports and counts instead
 * of stopping (so one run shows every failure), a scratch directory under
 * the build tree ($CB_TEST_DIR, set by src/core/meson.build), and a WAV
 * writer with RIFF INFO tags so the library can be fed real files without
 * any encoder. */

#ifndef CYANBOX_TESTS_CORE_UTIL_H
#define CYANBOX_TESTS_CORE_UTIL_H

#include <math.h>
#include <stdio.h>

#include "cyanbox/common.h"

extern int test_failures;

#define CHECK(cond) do {                                                     \
    if (!(cond)) {                                                           \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        test_failures++;                                                     \
    }                                                                        \
} while (0)

#define CHECK_NEAR(a, b, tol) do {                                           \
    double a_ = (a), b_ = (b);                                               \
    if (!(fabs(a_ - b_) <= (tol))) {                                         \
        fprintf(stderr, "%s:%d: CHECK failed: %s = %.17g, expected %.17g (+-%g)\n", \
                __FILE__, __LINE__, #a, a_, b_, (double)(tol));              \
        test_failures++;                                                     \
    }                                                                        \
} while (0)

#define CHECK_STR(a, b) do {                                                 \
    const char *a_ = (a), *b_ = (b);                                         \
    if (!a_ || !b_ || strcmp(a_, b_)) {                                      \
        fprintf(stderr, "%s:%d: CHECK failed: %s = \"%s\", expected \"%s\"\n", \
                __FILE__, __LINE__, #a, a_ ? a_ : "(null)", b_ ? b_ : "(null)"); \
        test_failures++;                                                     \
    }                                                                        \
} while (0)

/* A fresh, empty directory $CB_TEST_DIR/<name> (malloc'd path). */
char *test_dir(const char *name);
/* rm -rf. */
void test_rm_rf(const char *path);
/* A 16-bit PCM WAV of `seconds` of a quiet tone, with optional INFO tags
 * (INAM title, IART artist, ICRD date). */
int test_write_wav(const char *path, double seconds, int rate, int channels,
                   const char *title, const char *artist, const char *date);
/* The same with an ICMT comment too (libavformat reads it as "comment"). */
int test_write_wav_comment(const char *path, double seconds, int rate, int channels,
                           const char *title, const char *artist, const char *date,
                           const char *comment);
/* A track of an album: INAM title, IPRD album and IPRT track number (which
 * libavformat reads as "album" and "track"; 0: no track number). */
int test_write_wav_album(const char *path, double seconds, int rate, int channels,
                         const char *title, const char *album, int track);
/* Exit status: 0, or 1 with a summary when anything failed. */
int test_done(const char *name);

#endif /* CYANBOX_TESTS_CORE_UTIL_H */
