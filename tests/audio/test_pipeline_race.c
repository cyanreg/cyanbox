/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* The races of a seek (src/audio/pipeline.c, cb_pipeline_seek): the thread
 * can take a seek and queue the chunks asked for before the call returns,
 * and they must come next, in order. Here the call waits 3 ms after
 * letting the thread go, which it always has taken the seek by then, so a
 * queue drained after that point loses the first chunk every time, and a
 * wake given up as the old generation's loses the one the thread was
 * blocked pushing into the full queue (as both were: the reader waited for
 * the chunk for ever, the deck's feed and Link's reads stuck under load).
 * 200 seeks, each followed a millisecond later by its first four chunks,
 * with a pipeline that never fills its queue and one that always does. */

#include <poll.h>
#include <stdio.h>
#include <unistd.h>

#define SEEK_RACE() usleep(3000)
#include "src/audio/pipeline.c"

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

static unsigned rnd(unsigned *seed)
{
    *seed = *seed * 1103515245u + 12345u;
    return *seed >> 4;
}

static void seeks(const char *path, double ahead)
{
    CBPipelineOptions o = { .format = { .sample = CB_SAMPLE_FLT }, .chunk = 256, .ahead = ahead };
    CBPipeline *p = NULL;
    CHECK(cb_pipeline_open(&p, path, &o) >= 0, "open %s", path);
    if (!p)
        return;
    int64_t nb = cb_pipeline_length(p) / 256;
    struct pollfd pfd = { .fd = cb_pipeline_fd(p), .events = POLLIN };
    unsigned seed = 5;
    int bad = 0, lost = 0;
    for (int i = 0; i < 200 && bad + lost < 5; i++) {
        int64_t k = (int64_t)(rnd(&seed) % (uint64_t)(nb - 4));
        cb_pipeline_seek(p, k);
        /* As the deck's feed does, back to its poll(): a push the wake
         * gave up on finds the queue still full */
        usleep(1000);
        for (int j = 0; j < 4; j++) {
            CBChunk *c = cb_pipeline_pop(p);
            while (!c && poll(&pfd, 1, 500) > 0) {
                uint64_t v;
                (void)!read(pfd.fd, &v, sizeof(v));
                c = cb_pipeline_pop(p);
            }
            lost += !c;
            bad += c && c->index != k + j;
            cb_chunk_unref(&c);
        }
    }
    cb_pipeline_free(&p);
    CHECK(!bad && !lost, "ahead %g s: chunks out of order after a seek %d, missing %d", ahead,
          bad, lost);
}

/* 10 s of a stereo 16-bit ramp at 44.1 kHz. */
static int write_wav(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;
    uint32_t n = 441000, data = n * 4;
    uint8_t h[44] = "RIFF....WAVEfmt \x10\0\0\0\x01\0\x02\0\x44\xac\0\0\x10\xb1\x02\0\x04\0\x10\0data....";
    uint32_t riff = 36 + data;
    memcpy(h + 4, &riff, 4);
    memcpy(h + 40, &data, 4);
    fwrite(h, 1, 44, f);
    for (uint32_t i = 0; i < n; i++) {
        int16_t s[2] = { (int16_t)(i % 30000), (int16_t)-(int16_t)(i % 30000) };
        fwrite(s, 2, 2, f);
    }
    return fclose(f) ? -1 : 0;
}

int main(void)
{
    const char *root = getenv("CB_TEST_DIR");
    char *dir = cb_path_join(root ? root : "test-media", "pipeline_race");
    char *wav = dir ? cb_path_join(dir, "ramp.wav") : NULL;
    if (!wav || cb_mkdir_p(dir) < 0 || write_wav(wav) < 0) {
        fprintf(stderr, "cannot write the test media\n");
        return 1;
    }
    seeks(wav, 60.0);
    seeks(wav, 0.0);
    free(wav);
    free(dir);
    if (failures) {
        fprintf(stderr, "%d failures\n", failures);
        return 1;
    }
    printf("pipeline race: all tests passed\n");
    return 0;
}
