/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* The helpers of util.h. */

#include "util.h"

#include <ftw.h>
#include <stdint.h>
#include <unistd.h>

int test_failures;

static int rm_one(const char *path, const struct stat *st, int flag, struct FTW *ftw)
{
    remove(path);
    return 0;
}

void test_rm_rf(const char *path)
{
    nftw(path, rm_one, 16, FTW_DEPTH | FTW_PHYS);
}

char *test_dir(const char *name)
{
    const char *base = getenv("CB_TEST_DIR");
    char *dir = cb_asprintf("%s/%s", base && *base ? base : "test-tmp", name);
    if (!dir)
        return NULL;
    test_rm_rf(dir);
    if (cb_mkdir_p(dir) < 0) {
        fprintf(stderr, "cannot create %s\n", dir);
        free(dir);
        return NULL;
    }
    char *abs = cb_path_abs(dir);
    free(dir);
    return abs;
}

static void put_le(FILE *f, uint32_t v, int bytes)
{
    for (int i = 0; i < bytes; i++)
        fputc((int)(v >> (8 * i)) & 0xff, f);
}

static uint32_t info_size(const char *s)
{
    if (!s)
        return 0;
    uint32_t n = (uint32_t)strlen(s) + 1;
    return 8 + n + (n & 1);
}

static void put_info(FILE *f, const char *id, const char *s)
{
    if (!s)
        return;
    uint32_t n = (uint32_t)strlen(s) + 1;
    fwrite(id, 1, 4, f);
    put_le(f, n, 4);
    fwrite(s, 1, n, f);
    if (n & 1)
        fputc(0, f);
}

int test_write_wav(const char *path, double seconds, int rate, int channels,
                   const char *title, const char *artist, const char *date)
{
    return test_write_wav_comment(path, seconds, rate, channels, title, artist, date, NULL);
}

/* The WAV with INFO tags given as id and value pairs (a NULL value: left
 * out). */
static int write_wav(const char *path, double seconds, int rate, int channels,
                     const char *const ids[], const char *const values[], int nb_tags)
{
    char *dir = cb_strdup(path);
    *strrchr(dir, '/') = '\0';
    cb_mkdir_p(dir);
    free(dir);

    FILE *f = fopen(path, "wb");
    if (!f)
        return CB_ERROR(errno);
    uint32_t frames = (uint32_t)(seconds * rate);
    uint32_t data = frames * (uint32_t)channels * 2;
    uint32_t list = 0;
    for (int i = 0; i < nb_tags; i++)
        list += info_size(values[i]);
    if (list)
        list += 4;

    fwrite("RIFF", 1, 4, f);
    put_le(f, 4 + 8 + 16 + (list ? 8 + list : 0) + 8 + data, 4);
    fwrite("WAVEfmt ", 1, 8, f);
    put_le(f, 16, 4);
    put_le(f, 1, 2);
    put_le(f, (uint32_t)channels, 2);
    put_le(f, (uint32_t)rate, 4);
    put_le(f, (uint32_t)(rate * channels * 2), 4);
    put_le(f, (uint32_t)(channels * 2), 2);
    put_le(f, 16, 2);
    if (list) {
        fwrite("LIST", 1, 4, f);
        put_le(f, list, 4);
        fwrite("INFO", 1, 4, f);
        for (int i = 0; i < nb_tags; i++)
            put_info(f, ids[i], values[i]);
    }
    fwrite("data", 1, 4, f);
    put_le(f, data, 4);
    for (uint32_t i = 0; i < frames; i++) {
        int16_t v = (int16_t)(3000 * sin(2 * M_PI * 440.0 * i / rate));
        for (int c = 0; c < channels; c++)
            put_le(f, (uint16_t)v, 2);
    }
    return fclose(f) ? CB_ERROR(EIO) : 0;
}

int test_write_wav_comment(const char *path, double seconds, int rate, int channels,
                           const char *title, const char *artist, const char *date,
                           const char *comment)
{
    const char *const ids[] = { "INAM", "IART", "ICRD", "ICMT" };
    const char *const values[] = { title, artist, date, comment };
    return write_wav(path, seconds, rate, channels, ids, values, 4);
}

int test_write_wav_album(const char *path, double seconds, int rate, int channels,
                         const char *title, const char *album, int track)
{
    char number[16];
    snprintf(number, sizeof(number), "%d", track);
    const char *const ids[] = { "INAM", "IPRD", "IPRT" };
    const char *const values[] = { title, album, track > 0 ? number : NULL };
    return write_wav(path, seconds, rate, channels, ids, values, 3);
}

int test_done(const char *name)
{
    if (test_failures) {
        fprintf(stderr, "%s: %d check%s failed\n", name, test_failures, test_failures == 1 ? "" : "s");
        return 1;
    }
    printf("%s: all checks passed\n", name);
    return 0;
}
