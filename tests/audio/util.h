/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* Helpers for the audio tests: a scratch directory under the build
 * directory, running the ffmpeg CLI without a shell, and the known test
 * signal (a 440 Hz sine with a one-sample click every half second). */

#ifndef RXBOX_TESTS_AUDIO_UTIL_H
#define RXBOX_TESTS_AUDIO_UTIL_H

#include <stdio.h>

#include "rxbox/common.h"

#define CHECK(cond, ...)                                                        \
    do {                                                                        \
        if (!(cond)) {                                                          \
            fprintf(stderr, "%s:%d: CHECK(%s) failed: ", __FILE__, __LINE__, #cond); \
            fprintf(stderr, __VA_ARGS__);                                       \
            fputc('\n', stderr);                                                \
            failures++;                                                         \
        }                                                                       \
    } while (0)

/* The known signal */
#define SIG_RATE    44100
#define SIG_SECONDS 10
#define SIG_CLICK   (SIG_RATE / 2)          /* a click every this many samples */
#define SIG_CLICK_VALUE 31129               /* 0.95 of full scale */

/* $RX_TEST_DIR/name, created; malloc'd. Exits when it cannot be made.
 * Also quiets libav's own log (the tests feed it damaged files on purpose)
 * unless RX_TEST_VERBOSE is set, which also turns rxbox's log to debug. */
char *test_dir(const char *name);
/* dir/file, malloc'd */
char *test_path(const char *dir, const char *file);
/* Runs argv (searched in PATH), returns its exit status or -1. */
int run(const char *const argv[]);
/* ffmpeg -v error -nostdin -y <args...>; NULL-terminated, returns its status. */
int ffmpeg(const char *arg, ...);
/* True when the ffmpeg CLI has an encoder of that name. */
bool have_encoder(const char *name);
/* The signal: frames x channels interleaved s16. Channel 0 is the sine at
 * 0.25, channel 1 at 0.125; every SIG_CLICK samples all channels hold
 * SIG_CLICK_VALUE. */
int16_t *make_signal(int rate, int channels, int seconds, int64_t *frames);
int write_wav(const char *path, int rate, int channels, const int16_t *pcm, int64_t frames);
int copy_file(const char *from, const char *to);

#endif /* RXBOX_TESTS_AUDIO_UTIL_H */
