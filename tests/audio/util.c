/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* Helpers for the audio tests, see util.h. */

#include "util.h"

#include <math.h>
#include <spawn.h>
#include <sys/wait.h>

#include <libavutil/log.h>

extern char **environ;

char *test_dir(const char *name)
{
    if (getenv("RX_TEST_VERBOSE"))
        rx_log_set_level(RX_LOG_DEBUG);
    else
        av_log_set_level(AV_LOG_FATAL);
    const char *root = getenv("RX_TEST_DIR");
    char *dir = rx_path_join(root ? root : "test-media", name);
    if (!dir || rx_mkdir_p(dir) < 0) {
        fprintf(stderr, "cannot create %s\n", dir ? dir : name);
        exit(1);
    }
    return dir;
}

char *test_path(const char *dir, const char *file)
{
    char *p = rx_path_join(dir, file);
    if (!p)
        exit(1);
    return p;
}

int run(const char *const argv[])
{
    pid_t pid;
    if (posix_spawnp(&pid, argv[0], NULL, NULL, (char *const *)argv, environ))
        return -1;
    int status;
    if (waitpid(pid, &status, 0) < 0)
        return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

int ffmpeg(const char *arg, ...)
{
    const char *argv[64] = { "ffmpeg", "-v", "error", "-nostdin", "-y" };
    int n = 5;
    va_list args;
    va_start(args, arg);
    for (const char *a = arg; a && n < 63; a = va_arg(args, const char *))
        argv[n++] = a;
    va_end(args);
    argv[n] = NULL;
    int ret = run(argv);
    if (ret)
        fprintf(stderr, "ffmpeg failed (%d)\n", ret);
    return ret;
}

bool have_encoder(const char *name)
{
    FILE *f = popen("ffmpeg -hide_banner -encoders 2>/dev/null", "r");
    if (!f)
        return false;
    char line[512];
    bool found = false;
    size_t n = strlen(name);
    while (fgets(line, sizeof(line), f)) {
        /* " A....D libmp3lame  ..." */
        const char *p = line + strspn(line, " ");
        p += strcspn(p, " ");
        p += strspn(p, " ");
        if (!strncmp(p, name, n) && (p[n] == ' ' || p[n] == '\n'))
            found = true;
    }
    pclose(f);
    return found;
}

int16_t *make_signal(int rate, int channels, int seconds, int64_t *frames)
{
    int64_t n = (int64_t)rate * seconds;
    int16_t *x = malloc((size_t)n * channels * sizeof(*x));
    if (!x)
        exit(1);
    for (int64_t i = 0; i < n; i++) {
        double s = sin(2.0 * M_PI * 440.0 * (double)i / rate);
        for (int c = 0; c < channels; c++) {
            double a = c == 0 ? 0.25 : 0.125;
            x[i * channels + c] = (int16_t)lrint(a * 32767.0 * s);
            if (i % (rate / 2) == 0)
                x[i * channels + c] = SIG_CLICK_VALUE;
        }
    }
    *frames = n;
    return x;
}

static void le16(uint8_t *p, unsigned v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void le32(uint8_t *p, uint32_t v)
{
    le16(p, v & 0xFFFF);
    le16(p + 2, v >> 16);
}

int write_wav(const char *path, int rate, int channels, const int16_t *pcm, int64_t frames)
{
    uint32_t data = (uint32_t)(frames * channels * 2);
    uint8_t h[44];
    memcpy(h, "RIFF", 4);
    le32(h + 4, 36 + data);
    memcpy(h + 8, "WAVEfmt ", 8);
    le32(h + 16, 16);
    le16(h + 20, 1);
    le16(h + 22, (unsigned)channels);
    le32(h + 24, (uint32_t)rate);
    le32(h + 28, (uint32_t)(rate * channels * 2));
    le16(h + 32, (unsigned)(channels * 2));
    le16(h + 34, 16);
    memcpy(h + 36, "data", 4);
    le32(h + 40, data);

    FILE *f = fopen(path, "wb");
    if (!f)
        return -errno;
    bool ok = fwrite(h, 1, sizeof(h), f) == sizeof(h) &&
              fwrite(pcm, 2, (size_t)frames * channels, f) == (size_t)frames * channels;
    return fclose(f) == 0 && ok ? 0 : -EIO;
}

int copy_file(const char *from, const char *to)
{
    uint8_t *d;
    size_t n;
    int ret = rx_file_read(from, &d, &n);
    if (ret < 0)
        return ret;
    FILE *f = fopen(to, "wb");
    if (!f) {
        free(d);
        return -errno;
    }
    bool ok = fwrite(d, 1, n, f) == n;
    free(d);
    return fclose(f) == 0 && ok ? 0 : -EIO;
}
