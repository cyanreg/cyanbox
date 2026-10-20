/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* Reading a track through its mapping (src/audio/input.c).
 *
 * - libavformat over the mapping gives what cb_decode gave over the file
 *   protocol: test_pipeline and test_decode check every format; here a
 *   source over a WAV reads it whole, its samples the file's.
 * - A file cut short while it is read: a source past the new end fails
 *   with -EIO, after what libavformat holds already, and the program goes
 *   on (the pages past the end raise SIGBUS); a pipeline ends with an error
 *   chunk; the MP3 and FLAC scanners of a mapping cut short fail with
 *   -EIO.
 * - A SIGBUS that is not a read of a mapping still ends the program (a
 *   child touches a page past the end of a mapping of its own, outside
 *   the guard, once the guard's handler is installed). */

#include "cyanbox/audio.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include "src/audio/internal.h"
#include "util.h"

static int failures;

static void fresh_wav(const char *path, const int16_t *pcm, int64_t n)
{
    CHECK(write_wav(path, SIG_RATE, 2, pcm, n) >= 0, "write %s", path);
}

/* What a WAV of n frames holds before frame `frames` ends, in bytes. */
static off_t wav_bytes(int64_t frames)
{
    return 44 + (off_t)frames * 4;
}

static void check_source(const char *path, const int16_t *pcm, int64_t n)
{
    fresh_wav(path, pcm, n);
    CBAudioFormat f = { .sample = CB_SAMPLE_S16 };
    CBSource *s = NULL;
    CHECK(cb_source_open(&s, path, &f) >= 0, "open %s", path);
    if (!s)
        return;
    int16_t *x = malloc((size_t)n * 4);
    int64_t got = 0;
    while (got < n) {
        int r = cb_source_read(s, (uint8_t *[]) { (uint8_t *)(x + 2 * got) }, (int)CB_MIN(n - got, 4096));
        if (r <= 0)
            break;
        got += r;
    }
    CHECK(got == n && !memcmp(x, pcm, (size_t)n * 4), "source: %lld frames of %lld, the file's %d",
          (long long)got, (long long)n, got == n && !memcmp(x, pcm, (size_t)n * 4));
    cb_source_free(&s);

    /* Cut to two seconds after the first is read */
    CHECK(cb_source_open(&s, path, &f) >= 0, "open %s again", path);
    if (!s) {
        free(x);
        return;
    }
    got = 0;
    int r = 0;
    while (got < SIG_RATE) {
        r = cb_source_read(s, (uint8_t *[]) { (uint8_t *)(x + 2 * got) }, 1024);
        if (r <= 0)
            break;
        got += r;
    }
    int64_t cut = 2 * SIG_RATE;
    CHECK(truncate(path, wav_bytes(cut)) == 0, "truncate %s", path);
    while (got < n) {
        r = cb_source_read(s, (uint8_t *[]) { (uint8_t *)(x + 2 * got) }, 1024);
        if (r <= 0)
            break;
        got += r;
    }
    /* What libavformat holds already (the packets avformat_find_stream_info
     * read ahead) and what is left of the file, the file's, then -EIO */
    CHECK(r == CB_ERROR(EIO) && got < n && !memcmp(x, pcm, (size_t)got * 4),
          "cut short: %lld frames, then %d", (long long)got, r);
    cb_source_free(&s);
    free(x);
}

static void check_pipeline(const char *path, const int16_t *pcm, int64_t n)
{
    fresh_wav(path, pcm, n);
    CBPipelineOptions o = { .format = { .sample = CB_SAMPLE_S16 }, .chunk = 4096, .ahead = 0.5 };
    CBPipeline *p = NULL;
    CHECK(cb_pipeline_open(&p, path, &o) >= 0, "pipeline: open %s", path);
    if (!p)
        return;
    CHECK(truncate(path, wav_bytes(3 * SIG_RATE)) == 0, "truncate %s", path);
    int err = 0;
    bool last = false;
    int64_t t0 = cb_time_us();
    while (!last && cb_time_us() - t0 < 10000000) {
        CBChunk *c = cb_pipeline_pop(p);
        if (!c) {
            usleep(1000);
            continue;
        }
        last = c->last;
        err = c->err;
        cb_chunk_unref(&c);
    }
    CHECK(last && err == CB_ERROR(EIO), "pipeline: ends %d with %d", last, err);
    cb_pipeline_free(&p);
}

static void check_scanners(const char *dir, const char *wav, const int16_t *pcm, int64_t n)
{
    fresh_wav(wav, pcm, n);
    const char *kinds[] = { "mp3", "flac" };
    for (int i = 0; i < 2; i++) {
        char name[16];
        snprintf(name, sizeof(name), "cut.%s", kinds[i]);
        char *path = test_path(dir, name);
        if (ffmpeg("-i", wav, path, NULL)) {
            CHECK(false, "encode %s", name);
            free(path);
            continue;
        }
        CBMap m;
        CHECK(cb_audio_map(path, &m) >= 0, "map %s", name);
        CHECK(truncate(path, (off_t)(m.size / 4)) == 0, "truncate %s", name);
        int ret;
        if (i == 0) {
            CBMp3Info info;
            ret = cb_mp3_scan_mapped(path, &m, &info);
            CHECK(!info.valid && !info.nb_frames, "%s: what was scanned kept", name);
        } else {
            CBFlacInfo info;
            ret = cb_flac_scan_mapped(path, &m, &info);
            CHECK(!info.valid && !info.frames && !info.nb_frames, "%s: what was scanned kept", name);
            cb_flac_info_free(&info);
        }
        CHECK(ret == CB_ERROR(EIO), "%s cut short: %d", name, ret);
        cb_audio_unmap(&m);
        free(path);
    }
}

static int touch_past(void *opaque)
{
    return 0;
}

static void check_other_sigbus(const char *path, const int16_t *pcm, int64_t n)
{
    fresh_wav(path, pcm, n);
    pid_t pid = fork();
    if (pid == 0) {
        /* The guard's handler installed, then a page past the end of a
         * mapping no guarded read is reading */
        alarm(10);
        CBMap m;
        if (cb_audio_map(path, &m) < 0)
            _exit(2);
        cb_audio_guarded(&m, touch_past, NULL);
        if (truncate(path, 4096) < 0)
            _exit(3);
        volatile uint8_t v = m.data[m.size - 1];
        (void)v;
        _exit(0);
    }
    int status = 0;
    CHECK(pid > 0 && waitpid(pid, &status, 0) == pid, "fork");
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGBUS,
          "a SIGBUS outside a read: exited %d, signal %d", WIFEXITED(status) ? WEXITSTATUS(status) : -1,
          WIFSIGNALED(status) ? WTERMSIG(status) : 0);
}

int main(void)
{
    char *dir = test_dir("input");
    int64_t n;
    int16_t *pcm = make_signal(SIG_RATE, 2, SIG_SECONDS, &n);
    char *wav = test_path(dir, "sig.wav");

    check_source(wav, pcm, n);
    check_pipeline(wav, pcm, n);
    check_scanners(dir, wav, pcm, n);
    check_other_sigbus(wav, pcm, n);

    free(pcm);
    free(wav);
    free(dir);
    if (failures) {
        fprintf(stderr, "%d failures\n", failures);
        return 1;
    }
    printf("input: all tests passed\n");
    return 0;
}
