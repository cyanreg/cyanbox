/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* The waveform encoders against dj's: every tag byte of rx_waveforms_compute
 * is compared with what dj.analysis.waveform.compute wrote for the same
 * float32 samples (tests/data/analysis, made by gen_fixtures.py), plus the
 * column count rule, the firmware minimums, and the packed form (the
 * library's blob and the .rxw file): round trip, damaged data refused.
 *
 * rxbox deliberately differs from dj in two places (docs/OVERVIEW.md
 * section 3, item 2): PWAV heights are never below 2 and PWV2 never below 1,
 * because Nexus firmware rejects a preview with a zero column and rekordbox
 * never writes lower values. The same two clamps are applied to dj's bytes
 * before comparing, and the number of bytes they changed is printed.
 *
 * With arguments, `test_waveform SIGNAL.f32 TAGS` compares one pair (the
 * real-track check: gen_fixtures.py --real writes those). */

#include <math.h>
#include <stdio.h>
#include <unistd.h>

#include "rxbox/analysis.h"
#include "rxbox/common.h"

#define DATA_DIR "tests/data/analysis/"

typedef struct Fixture {
    int rate;
    int64_t nb_samples;
    int n;
    uint16_t gains[3];
    const uint8_t *pwav, *pwv2, *pwv4, *pwv6, *pwv3, *pwv5, *pwv7;
    uint8_t *buf;
} Fixture;

static uint32_t le32(const uint8_t *p)
{
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static int fixture_load(const char *path, Fixture *f)
{
    size_t size;
    *f = (Fixture) { 0 };
    int ret = rx_file_read(path, &f->buf, &size);
    if (ret < 0)
        return ret;
    if (size < 24 || memcmp(f->buf, "DJWV", 4))
        return RX_ERROR(EINVAL);
    f->rate = (int)le32(f->buf + 4);
    f->nb_samples = le32(f->buf + 8);
    f->n = (int)le32(f->buf + 12);
    for (int j = 0; j < 3; j++)
        f->gains[j] = (uint16_t)(f->buf[16 + 2 * j] | f->buf[17 + 2 * j] << 8);
    if (size != 24 + 400 + 100 + 7200 + 3600 + (size_t)f->n * 6)
        return RX_ERROR(EINVAL);
    const uint8_t *p = f->buf + 24;
    f->pwav = p;
    p += 400;
    f->pwv2 = p;
    p += 100;
    f->pwv4 = p;
    p += 7200;
    f->pwv6 = p;
    p += 3600;
    f->pwv3 = p;
    p += f->n;
    f->pwv5 = p;
    p += (size_t)f->n * 2;
    f->pwv7 = p;
    return 0;
}

static int signal_load(const char *path, float **x, int64_t *nb)
{
    uint8_t *buf;
    size_t size;
    int ret = rx_file_read(path, &buf, &size);
    if (ret < 0)
        return ret;
    *nb = (int64_t)(size / 4);
    *x = malloc(RX_MAX(size, 4));
    if (!*x) {
        free(buf);
        return RX_ERROR(ENOMEM);
    }
    /* The fixtures are little-endian float32, as is every host this builds on. */
    memcpy(*x, buf, size);
    free(buf);
    return 0;
}

static int count_diff(const char *tag, const uint8_t *a, const uint8_t *b, size_t size)
{
    int diff = 0;
    for (size_t i = 0; i < size; i++)
        diff += a[i] != b[i];
    printf("    %-5s %6d / %zu bytes differ\n", tag, diff, size);
    if (diff) {
        for (size_t i = 0, shown = 0; i < size && shown < 5; i++) {
            if (a[i] == b[i])
                continue;
            printf("          [%zu] rxbox %u dj %u\n", i, a[i], b[i]);
            shown++;
        }
    }
    return diff;
}

/* Compares one signal with its dj fixture; returns the number of differing bytes. */
static int compare(const char *f32_path, const char *tags_path)
{
    Fixture fx;
    float *x = NULL;
    int64_t nb;
    int ret = fixture_load(tags_path, &fx);
    if (ret < 0) {
        printf("  %s: cannot load (%s)\n", tags_path, strerror(-ret));
        free(fx.buf);
        return 1;
    }
    ret = signal_load(f32_path, &x, &nb);
    if (ret < 0 || nb != fx.nb_samples) {
        printf("  %s: cannot load or wrong length\n", f32_path);
        free(fx.buf);
        free(x);
        return 1;
    }

    RxWaveforms w;
    int64_t t0 = rx_time_us();
    ret = rx_waveforms_compute(x, nb, fx.rate, &w);
    int64_t t1 = rx_time_us();
    if (ret < 0) {
        printf("  %s: rx_waveforms_compute failed (%s)\n", f32_path, strerror(-ret));
        free(fx.buf);
        free(x);
        return 1;
    }
    printf("  %s: %d Hz, %lld samples, %d columns, %.1f ms\n", f32_path, fx.rate,
           (long long)nb, w.nb_detail, (t1 - t0) / 1000.0);

    /* dj's bytes with rxbox's firmware minimums applied. */
    uint8_t pwav[RX_WAVE_PWAV_COLUMNS], pwv2[RX_WAVE_PWV2_COLUMNS];
    int clamped = 0;
    for (int i = 0; i < RX_WAVE_PWAV_COLUMNS; i++) {
        pwav[i] = fx.pwav[i];
        if ((pwav[i] & 31) < 2) {
            pwav[i] = (uint8_t)((pwav[i] & ~31) | 2);
            clamped++;
        }
    }
    for (int i = 0; i < RX_WAVE_PWV2_COLUMNS; i++) {
        pwv2[i] = RX_MAX(fx.pwv2[i], 1);
        clamped += fx.pwv2[i] < 1;
    }
    printf("    firmware minimums changed %d of dj's preview bytes\n", clamped);

    int diff = 0;
    if (w.nb_detail != fx.n) {
        printf("    detail columns: rxbox %d dj %d\n", w.nb_detail, fx.n);
        diff++;
    } else {
        diff += count_diff("PWV3", w.pwv3, fx.pwv3, (size_t)fx.n);
        diff += count_diff("PWV5", w.pwv5, fx.pwv5, (size_t)fx.n * 2);
        diff += count_diff("PWV7", w.pwv7, fx.pwv7, (size_t)fx.n * 3);
    }
    diff += count_diff("PWAV", w.pwav, pwav, sizeof(pwav));
    diff += count_diff("PWV2", w.pwv2, pwv2, sizeof(pwv2));
    diff += count_diff("PWV4", w.pwv4, fx.pwv4, sizeof(w.pwv4));
    diff += count_diff("PWV6", w.pwv6, fx.pwv6, sizeof(w.pwv6));
    int gdiff = memcmp(w.gains, fx.gains, sizeof(w.gains)) != 0;
    printf("    PWVC  rxbox %u %u %u dj %u %u %u\n", w.gains[0], w.gains[1], w.gains[2],
           fx.gains[0], fx.gains[1], fx.gains[2]);
    diff += gdiff;

    rx_waveforms_free(&w);
    free(fx.buf);
    free(x);
    return diff;
}

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            fails++;                                                    \
        }                                                               \
    } while (0)

/* dj's test_waveform_columns, plus the minimums on silence. */
static int test_rules(void)
{
    int fails = 0;
    RxWaveforms w;

    /* 150 columns per second, ceil(samples * 150 / rate) of them */
    float *z = calloc(44101, sizeof(*z));
    if (!z)
        return 1;
    CHECK(rx_waveforms_compute(z, 44100, 44100, &w) == 0 && w.nb_detail == 150);
    rx_waveforms_free(&w);
    CHECK(rx_waveforms_compute(z, 44101, 44100, &w) == 0 && w.nb_detail == 151);
    /* Silence: dj writes zero heights, rxbox the firmware minimums. */
    for (int i = 0; i < RX_WAVE_PWAV_COLUMNS; i++)
        CHECK((w.pwav[i] & 31) == 2);
    for (int i = 0; i < RX_WAVE_PWV2_COLUMNS; i++)
        CHECK(w.pwv2[i] == 1);
    rx_waveforms_free(&w);
    CHECK(rx_waveforms_compute(z, 0, 44100, &w) == 0 && w.nb_detail == 1);
    rx_waveforms_free(&w);
    CHECK(rx_waveforms_compute(z, 100, 0, &w) == RX_ERROR(EINVAL));
    free(z);

    /* 60 Hz then 5 kHz at 48 kHz: bass dark, treble white */
    int rate = 48000, half = rate * 2;
    float *x = malloc(sizeof(*x) * half * 2);
    if (!x)
        return fails + 1;
    for (int i = 0; i < half; i++) {
        x[i] = (float)(0.9 * sin(2 * M_PI * 60 * i / rate));
        x[half + i] = (float)(0.9 * sin(2 * M_PI * 5000 * i / rate));
    }
    CHECK(rx_waveforms_compute(x, half * 2, rate, &w) == 0 && w.nb_detail == 600);
    if (w.nb_detail == 600) {
        for (int i = 5; i < 295; i++) {
            CHECK(w.pwv3[i] >> 5 == 0);
            CHECK(w.pwv3[300 + i] >> 5 == 7);
            /* a 60 Hz cycle is longer than a column */
            CHECK((w.pwv3[i] & 31) >= 25);
        }
        CHECK(w.pwv7[100 * 3 + 0] > w.pwv7[100 * 3 + 2]);
        CHECK(w.pwv7[400 * 3 + 2] > w.pwv7[400 * 3 + 0]);
    }
    rx_waveforms_free(&w);
    free(x);
    return fails;
}

static bool same_waves(const RxWaveforms *a, const RxWaveforms *b)
{
    if (a->nb_detail != b->nb_detail || a->rate != b->rate)
        return false;
    size_t n = (size_t)a->nb_detail;
    return !memcmp(a->pwv3, b->pwv3, n) && !memcmp(a->pwv5, b->pwv5, n * 2) &&
           !memcmp(a->pwv7, b->pwv7, n * 3) && !memcmp(a->pwav, b->pwav, sizeof(a->pwav)) &&
           !memcmp(a->pwv2, b->pwv2, sizeof(a->pwv2)) &&
           !memcmp(a->pwv4, b->pwv4, sizeof(a->pwv4)) &&
           !memcmp(a->pwv6, b->pwv6, sizeof(a->pwv6)) &&
           !memcmp(a->gains, b->gains, sizeof(a->gains));
}

static void put32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> 8 * i);
}

/* The packed blob the library database stores (the .rxw layout in memory). */
static int test_pack(void)
{
    int fails = 0;
    float *x;
    int64_t nb;
    if (signal_load(DATA_DIR "short44k.f32", &x, &nb) < 0) {
        printf("FAIL: cannot read short44k.f32\n");
        return 1;
    }
    RxWaveforms w, r;
    CHECK(rx_waveforms_compute(x, nb, 44100, &w) == 0);
    free(x);

    uint8_t *data;
    size_t size;
    CHECK(rx_waveforms_pack(&w, &data, &size) == 0);
    if (!data) {
        rx_waveforms_free(&w);
        return fails + 1;
    }
    CHECK(size == 24 + 400 + 100 + 7200 + 3600 + (size_t)w.nb_detail * 6);
    CHECK(!memcmp(data, "RXWV", 4) && data[4] == 1 && le32(data + 8) == (uint32_t)w.nb_detail &&
          le32(data + 12) == 44100);
    CHECK(rx_waveforms_unpack(data, size, &r) == 0);
    CHECK(same_waves(&r, &w));
    rx_waveforms_free(&r);

    /* Refused, with nothing allocated: truncated, one byte too many, bad
     * magic, unknown version, a column count that disagrees with the size,
     * no columns, shorter than the header, no data */
    CHECK(rx_waveforms_unpack(data, size - 1, &r) == RX_ERROR(EINVAL) && !r.pwv3);
    uint8_t *big = malloc(size + 1);
    if (big) {
        memcpy(big, data, size);
        big[size] = 0;
        CHECK(rx_waveforms_unpack(big, size + 1, &r) == RX_ERROR(EINVAL) && !r.pwv3);
        free(big);
    }
    data[0] = 'X';
    CHECK(rx_waveforms_unpack(data, size, &r) == RX_ERROR(EINVAL));
    data[0] = 'R';
    put32(data + 4, 2);
    CHECK(rx_waveforms_unpack(data, size, &r) == RX_ERROR(EINVAL));
    put32(data + 4, 1);
    put32(data + 8, (uint32_t)w.nb_detail + 1);
    CHECK(rx_waveforms_unpack(data, size, &r) == RX_ERROR(EINVAL));
    put32(data + 8, 0);
    CHECK(rx_waveforms_unpack(data, 24 + 400 + 100 + 7200 + 3600, &r) == RX_ERROR(EINVAL));
    put32(data + 8, (uint32_t)w.nb_detail);
    CHECK(rx_waveforms_unpack(data, 23, &r) == RX_ERROR(EINVAL));
    CHECK(rx_waveforms_unpack(NULL, size, &r) == RX_ERROR(EINVAL));
    CHECK(rx_waveforms_unpack(data, size, &r) == 0 && same_waves(&r, &w));
    rx_waveforms_free(&r);
    free(data);

    /* Nothing to pack */
    RxWaveforms empty = { 0 };
    CHECK(rx_waveforms_pack(&empty, &data, &size) == RX_ERROR(EINVAL) && !data && !size);

    rx_waveforms_free(&w);
    return fails;
}

/* save/load: the packed blob in a file */
static int test_cache(void)
{
    int fails = 0;
    float *x;
    int64_t nb;
    if (signal_load(DATA_DIR "odd11k.f32", &x, &nb) < 0) {
        printf("FAIL: cannot read odd11k.f32\n");
        return 1;
    }
    RxWaveforms w, r;
    CHECK(rx_waveforms_compute(x, nb, 11025, &w) == 0);
    free(x);

    char path[] = "/tmp/rxbox-test-waveform-XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0);
    if (fd < 0) {
        rx_waveforms_free(&w);
        return fails;
    }
    close(fd);

    CHECK(rx_waveforms_save(&w, path) == 0);
    uint8_t *packed, *file;
    size_t packed_size, file_size;
    CHECK(rx_waveforms_pack(&w, &packed, &packed_size) == 0);
    CHECK(rx_file_read(path, &file, &file_size) == 0);
    CHECK(file_size == packed_size && !memcmp(file, packed, file_size));
    free(packed);
    free(file);
    CHECK(rx_waveforms_load(path, &r) == 0);
    CHECK(same_waves(&r, &w) && r.rate == 11025);
    rx_waveforms_free(&r);

    /* Damaged files are refused, not half read. */
    uint8_t *buf;
    size_t size;
    CHECK(rx_file_read(path, &buf, &size) == 0);
    CHECK(rx_file_write_atomic(path, buf, size - 1) == 0);
    CHECK(rx_waveforms_load(path, &r) == RX_ERROR(EINVAL) && !r.pwv3);
    buf[8]++;
    CHECK(rx_file_write_atomic(path, buf, size) == 0);
    CHECK(rx_waveforms_load(path, &r) == RX_ERROR(EINVAL));
    buf[8]--;
    buf[0] = 'X';
    CHECK(rx_file_write_atomic(path, buf, size) == 0);
    CHECK(rx_waveforms_load(path, &r) == RX_ERROR(EINVAL));
    CHECK(rx_file_write_atomic(path, buf, 10) == 0);
    CHECK(rx_waveforms_load(path, &r) == RX_ERROR(EINVAL));
    free(buf);
    unlink(path);
    CHECK(rx_waveforms_load(path, &r) == RX_ERROR(ENOENT));

    rx_waveforms_free(&w);
    return fails;
}

int main(int argc, char **argv)
{
    if (argc == 3)
        return compare(argv[1], argv[2]) ? 1 : 0;

    static const char *const signals[] = {
        "mix44k", "cols48k", "odd11k", "short44k", "silence44k",
    };
    int fails = 0;
    printf("dj fixtures (byte differences per tag, after the firmware minimums):\n");
    for (size_t i = 0; i < RX_ARRAY_ELEMS(signals); i++) {
        char f32[256], tags[256];
        snprintf(f32, sizeof(f32), DATA_DIR "%s.f32", signals[i]);
        snprintf(tags, sizeof(tags), DATA_DIR "%s.tags", signals[i]);
        int diff = compare(f32, tags);
        if (diff)
            printf("FAIL %s: %d differences\n", signals[i], diff);
        fails += diff != 0;
    }
    fails += test_rules();
    fails += test_pack();
    fails += test_cache();
    if (fails) {
        printf("test_waveform: %d failures\n", fails);
        return 1;
    }
    printf("test_waveform: ok\n");
    return 0;
}
