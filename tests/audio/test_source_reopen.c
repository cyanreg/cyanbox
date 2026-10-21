/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* A seek to the start of a 16-bit PCM WAV (src/audio/source.c,
 * open_input): the source opens the file again, and libavformat would
 * hold back its first 32 packets (512 kB of 44.1 kHz stereo) to look for
 * DTS or AC-3 in them, as the first open did. Opened again it reads no
 * further than the first packet before the first frames come out, and
 * they are the file's. source.c is built into the test, to see how far
 * libavformat read. (test_pipeline checks a seek to 0 of every format
 * against reading in order.) */

#include <stdio.h>

#include "src/audio/source.c"
#include "util.h"

static int failures;

int main(void)
{
    char *dir = test_dir("source_reopen");
    int64_t n;
    int16_t *pcm = make_signal(SIG_RATE, 2, SIG_SECONDS, &n);
    char *wav = test_path(dir, "sig.wav");
    CHECK(write_wav(wav, SIG_RATE, 2, pcm, n) >= 0, "write %s", wav);

    CBAudioFormat f = { .sample = CB_SAMPLE_S16 };
    CBSource *s = NULL;
    CHECK(cb_source_open(&s, wav, &f) >= 0, "open %s", wav);
    if (!s)
        return 1;
    /* The first open probes: what the test tells apart */
    int64_t first = avio_tell(s->fmt->pb);
    CHECK(first >= 32 * 16384, "the first open read %lld bytes", (long long)first);

    int16_t x[4096 * 2];
    uint8_t *dst[1] = { (uint8_t *)x };
    CHECK(cb_source_seek(s, n - 4096) >= 0 && cb_source_read(s, dst, 4096) == 4096, "the end");
    CHECK(cb_source_seek(s, 0) >= 0, "a seek to 0");
    int got = cb_source_read(s, dst, 4096);
    int64_t read = avio_tell(s->fmt->pb);
    CHECK(got == 4096 && !memcmp(x, pcm, sizeof(x)), "after a seek to 0: %d frames, the file's %d",
          got, got == 4096 && !memcmp(x, pcm, sizeof(x)));
    CHECK(read <= 44 + 2 * 16384, "after a seek to 0, read to %lld", (long long)read);
    printf("source_reopen: the first open read %lld bytes, a seek to 0 to %lld\n", (long long)first,
           (long long)read);

    cb_source_free(&s);
    free(pcm);
    free(wav);
    free(dir);
    if (failures) {
        fprintf(stderr, "%d failures\n", failures);
        return 1;
    }
    printf("source_reopen: all tests passed\n");
    return 0;
}
