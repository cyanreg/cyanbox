/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* The waveform encoders against dj's: every tag byte of cb_waveforms_compute
 * is compared with the reference bytes gen_fixtures.py wrote for the same
 * float32 samples (tests/data/analysis), plus the column count rule, the
 * firmware minimums, the three-band rules, and the packed form (the
 * library's blob and the .rxw file): round trip, damaged data refused,
 * older versions stale.
 *
 * PWAV, PWV2, PWV3, PWV4, PWV5 and PWVC are dj.analysis.waveform.compute's
 * bytes, with two deliberate differences (docs/OVERVIEW.md section 3,
 * item 2): PWAV heights are never below 2 and PWV2 never below 1, because
 * Nexus firmware rejects a preview with a zero column and rekordbox never
 * writes lower values. The same two clamps are applied to dj's bytes before
 * comparing, and the number of bytes they changed is printed.
 *
 * PWV6 and PWV7 in the fixtures are NOT dj's: they are rbxport's rules
 * (src/analysis/waveform.c), computed by gen_fixtures.py's three_band() on
 * whole numpy arrays, which the streaming C must match byte for byte. The
 * rules themselves are checked without fixtures: test_crossovers() on pure
 * tones against the filters' exact responses, test_overview_level() the
 * overview's normalisation to the track's level, test_peak() the detail's
 * peak per column.
 *
 * With arguments, `test_waveform SIGNAL.f32 TAGS` compares one pair (the
 * real-track check: gen_fixtures.py --real writes those). */

#include <complex.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "cyanbox/analysis.h"
#include "cyanbox/common.h"

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
    int ret = cb_file_read(path, &f->buf, &size);
    if (ret < 0)
        return ret;
    if (size < 24 || memcmp(f->buf, "DJWV", 4))
        return CB_ERROR(EINVAL);
    f->rate = (int)le32(f->buf + 4);
    f->nb_samples = le32(f->buf + 8);
    f->n = (int)le32(f->buf + 12);
    for (int j = 0; j < 3; j++)
        f->gains[j] = (uint16_t)(f->buf[16 + 2 * j] | f->buf[17 + 2 * j] << 8);
    if (size != 24 + 400 + 100 + 7200 + 3600 + (size_t)f->n * 6)
        return CB_ERROR(EINVAL);
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
    int ret = cb_file_read(path, &buf, &size);
    if (ret < 0)
        return ret;
    *nb = (int64_t)(size / 4);
    *x = malloc(CB_MAX(size, 4));
    if (!*x) {
        free(buf);
        return CB_ERROR(ENOMEM);
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
            printf("          [%zu] cyanbox %u dj %u\n", i, a[i], b[i]);
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

    CBWaveforms w;
    int64_t t0 = cb_time_us();
    ret = cb_waveforms_compute(x, nb, fx.rate, &w);
    int64_t t1 = cb_time_us();
    if (ret < 0) {
        printf("  %s: cb_waveforms_compute failed (%s)\n", f32_path, strerror(-ret));
        free(fx.buf);
        free(x);
        return 1;
    }
    printf("  %s: %d Hz, %lld samples, %d columns, %.1f ms\n", f32_path, fx.rate,
           (long long)nb, w.nb_detail, (t1 - t0) / 1000.0);

    /* dj's bytes with cyanbox's firmware minimums applied. */
    uint8_t pwav[CB_WAVE_PWAV_COLUMNS], pwv2[CB_WAVE_PWV2_COLUMNS];
    int clamped = 0;
    for (int i = 0; i < CB_WAVE_PWAV_COLUMNS; i++) {
        pwav[i] = fx.pwav[i];
        if ((pwav[i] & 31) < 2) {
            pwav[i] = (uint8_t)((pwav[i] & ~31) | 2);
            clamped++;
        }
    }
    for (int i = 0; i < CB_WAVE_PWV2_COLUMNS; i++) {
        pwv2[i] = CB_MAX(fx.pwv2[i], 1);
        clamped += fx.pwv2[i] < 1;
    }
    printf("    firmware minimums changed %d of dj's preview bytes\n", clamped);

    int diff = 0;
    if (w.nb_detail != fx.n) {
        printf("    detail columns: cyanbox %d dj %d\n", w.nb_detail, fx.n);
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
    printf("    PWVC  cyanbox %u %u %u dj %u %u %u\n", w.gains[0], w.gains[1], w.gains[2],
           fx.gains[0], fx.gains[1], fx.gains[2]);
    diff += gdiff;

    cb_waveforms_free(&w);
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

/* dj's synthetic kick (tests/synth.py): a 170 -> 50 Hz drop over 0.25 s */
static double kick(double t)
{
    if (t < 0 || t >= 0.25)
        return 0.0;
    return sin(2 * M_PI * (50 * t + 120 * (1 - exp(-40 * t)) / 40)) * exp(-12 * t);
}

/* A loud drum & bass style master in closed form, 174 BPM: a 55 Hz sub, a
 * reese (harmonics 3..24 of 55 Hz, slowly swelling) filling the mid band,
 * kicks on 1 and 3, a snare body on 2 and 4, sixteenth hats made of twenty
 * inharmonic partials from 3 to 15 kHz over a faint wash, all saturated
 * with tanh to a hot, dense level: something in every band, all the time,
 * at different rhythms. */
static float *loud_master(int rate, double secs, int64_t *nb)
{
    static const double snare[5] = { 180, 330, 670, 1230, 1870 };
    double beat = 60.0 / 174.0, hat[20];
    for (int k = 0; k < 20; k++)
        hat[k] = 3000 * pow(1.09, k);
    int64_t n = (int64_t)(secs * rate);
    float *x = malloc(sizeof(*x) * n);
    if (!x)
        return NULL;
    for (int64_t i = 0; i < n; i++) {
        double t = (double)i / rate;
        double v = 0.45 * sin(2 * M_PI * 55 * t) + 0.15 * sin(2 * M_PI * 110 * t);
        v += 0.8 * kick(fmod(t, 2 * beat));
        double hiss = 0.0;
        for (int k = 0; k < 20; k++)
            hiss += sin(2 * M_PI * hat[k] * t + 0.7 * k * k);
        hiss /= sqrt(10);
        v += 0.2 * hiss * (exp(-60 * fmod(t, beat / 4)) + 0.05);
        double body = 0.0;
        for (int k = 0; k < 5; k++)
            body += sin(2 * M_PI * snare[k] * t + k);
        v += 0.5 * body / 2 * exp(-25 * fmod(t + beat, 2 * beat));
        double reese = 0.0;
        for (int h = 3; h < 25; h++)
            reese += sin(2 * M_PI * 55 * h * t) / h;
        v += 0.35 * reese * (0.6 + 0.4 * sin(2 * M_PI * t / (8 * beat)));
        x[i] = (float)(tanh(1.5 * v) / tanh(1.5));
    }
    *nb = n;
    return x;
}

static float *tones(int rate, const double *freq, int nb_freq, double secs, double amp,
                    int64_t *nb)
{
    int64_t seg = (int64_t)(secs * rate), n = seg * nb_freq;
    float *x = malloc(sizeof(*x) * n);
    if (!x)
        return NULL;
    for (int s = 0; s < nb_freq; s++)
        for (int64_t i = 0; i < seg; i++)
            x[s * seg + i] = (float)(amp * sin(2 * M_PI * freq[s] * (double)i / rate));
    *nb = n;
    return x;
}

/* rbxport's one-pole low-pass at fc, (1 - c) / (1 - c e^-jw) with
 * c = exp(-2 pi fc / rate), at f Hz. */
static double complex onepole(double fc, double f, int rate)
{
    double c = exp(-2 * M_PI * fc / rate);
    return (1 - c) / (1 - c * cexp(-I * 2 * M_PI * f / rate));
}

/* The PWV7 bytes of a steady tone of amplitude `amp` at f Hz: the low band
 * is the 200 Hz low-pass, the mid what lies between it and the 2 kHz one,
 * the high what lies above; each band's peak to 0..255 truncated, halved. */
static void tone_pwv7(double f, double amp, int rate, unsigned out[3])
{
    double complex lo = onepole(200, f, rate), below = onepole(2000, f, rate);
    double level[3] = { cabs(lo), cabs(below - lo), cabs(1 - below) };
    for (int j = 0; j < 3; j++)
        out[j] = (unsigned)(255 * fmin(amp * level[j], 1.0)) >> 1;
}

/* The crossovers on pure tones: 100 Hz, 1 kHz and 8 kHz, 2 s each.
 *
 * In the detail every band takes the tone's level through its filters,
 * which are one-pole and so leaky: a tone shows most in its own band, but a
 * 1 kHz tone reaches the high band at about half its mid (the levels are
 * in the output). The bytes are checked against the filters' exact
 * responses: a sampled peak can only fall short of the true one, by a byte
 * or two at 8 kHz.
 *
 * In the overview the mid and high bands are Butterworth, and every band is
 * normalised over the whole track, so in each tone's stretch its own band
 * is at least three times either other one, and the farthest band (low at
 * 8 kHz, high at 100 Hz) about nothing. */
static int test_crossovers(void)
{
    static const double freq[3] = { 100, 1000, 8000 };
    int fails = 0, rate = 44100;
    int64_t nb;
    float *x = tones(rate, freq, 3, 2.0, 0.5, &nb);
    if (!x)
        return 1;
    CBWaveforms w;
    CHECK(cb_waveforms_compute(x, nb, rate, &w) == 0 && w.nb_detail == 900);
    free(x);
    if (w.nb_detail != 900) {
        cb_waveforms_free(&w);
        return fails + 1;
    }
    for (int s = 0; s < 3; s++) {
        unsigned e[3];
        tone_pwv7(freq[s], 0.5, rate, e);
        const uint8_t *d = &w.pwv7[(s * 300 + 150) * 3], *o = &w.pwv6[(s * 400 + 200) * 3];
        printf("%5.0f Hz at 0.5: PWV7 %3u %3u %3u (filters %3u %3u %3u), PWV6 %3u %3u %3u\n",
               freq[s], d[0], d[1], d[2], e[0], e[1], e[2], o[0], o[1], o[2]);
        /* Past the first 10 columns, where the filters settle */
        for (int k = s * 300 + 10; k < s * 300 + 300; k++) {
            for (int j = 0; j < 3; j++) {
                unsigned v = w.pwv7[k * 3 + j];
                CHECK(v <= e[j] && v + 2 >= e[j]);
            }
            CHECK(w.pwv7[k * 3 + s] >= w.pwv7[k * 3 + (s + 1) % 3] * 3 / 2);
            CHECK(w.pwv7[k * 3 + s] >= w.pwv7[k * 3 + (s + 2) % 3] * 3 / 2);
        }
        /* Past the trailing average, short of the next tone */
        for (int k = s * 400 + 2; k < s * 400 + 399; k++) {
            CHECK(w.pwv6[k * 3 + s] >= 3 * w.pwv6[k * 3 + (s + 1) % 3]);
            CHECK(w.pwv6[k * 3 + s] >= 3 * w.pwv6[k * 3 + (s + 2) % 3]);
        }
    }
    for (int k = 2; k < 399; k++) {
        CHECK(w.pwv6[k * 3 + 2] <= 1);
        CHECK(w.pwv6[(800 + k) * 3 + 0] <= 4);
    }
    cb_waveforms_free(&w);
    return fails;
}

/* The mean of every column's RMS, as the overview's level: the target its
 * bands are normalised to is 92 times this. Computed here from the
 * samples on the detail column grid. */
static double mean_rms(const float *x, int64_t nb, int rate)
{
    int64_t n = (int64_t)ceil((double)(nb * 150) / rate), start = 0, frames = 0;
    double level = 0.0;
    for (int64_t k = 0; k < n; k++) {
        int64_t end = CB_MIN((int64_t)floor((double)((k + 1) * rate) / 150 + 0.5), nb);
        double s = 0.0;
        for (int64_t i = start; i < end; i++)
            s += (double)x[i] * x[i];
        if (end > start) {
            level += sqrt(s / (double)(end - start));
            frames++;
        }
        start = end;
    }
    return frames ? level / frames : 0.0;
}

/* The overview's normalisation: on the loud master, 6 dB down so that
 * nothing reaches full scale, every band's mean over the 1200 columns is 92
 * times the mean column RMS (within rounding and the trailing average's
 * edge), and the overview of the same music at half that level is half as
 * high, where rekordbox-style per-band peak gains would draw both the same.
 * The detail is linear: half the level, half the bytes. */
static int test_overview_level(void)
{
    int fails = 0, rate = 44100;
    int64_t nb;
    float *x = loud_master(rate, 20.0, &nb);
    if (!x)
        return 1;
    CBWaveforms w, h;
    for (int64_t i = 0; i < nb; i++)
        x[i] *= 0.5f;
    CHECK(cb_waveforms_compute(x, nb, rate, &w) == 0);
    double target = 92 * mean_rms(x, nb, rate);
    for (int64_t i = 0; i < nb; i++)
        x[i] *= 0.5f;
    CHECK(cb_waveforms_compute(x, nb, rate, &h) == 0);
    free(x);

    double mean[3] = { 0 };
    int top = 0;
    for (int k = 0; k < CB_WAVE_PWV6_COLUMNS; k++) {
        for (int j = 0; j < 3; j++) {
            mean[j] += w.pwv6[k * 3 + j] / (double)CB_WAVE_PWV6_COLUMNS;
            top = CB_MAX(top, w.pwv6[k * 3 + j]);
            CHECK(abs(2 * h.pwv6[k * 3 + j] - w.pwv6[k * 3 + j]) <= 1);
        }
    }
    printf("overview of the loud master: target %.2f, band means %.2f %.2f %.2f, max %d\n",
           target, mean[0], mean[1], mean[2], top);
    CHECK(top < 127);
    for (int j = 0; j < 3; j++)
        CHECK(fabs(mean[j] - target) < 0.6);
    for (int64_t i = 0; i < (int64_t)w.nb_detail * 3; i++)
        CHECK(abs(2 * h.pwv7[i] - w.pwv7[i]) <= 2);
    cb_waveforms_free(&w);
    cb_waveforms_free(&h);
    return fails;
}

/* The detail takes each band's peak over the column, not its energy: a
 * 60-sample 8 kHz burst (Hann-shaped, so the filters follow it without an
 * onset transient) in a column of 294 shows at the level of a steady tone
 * in the high band, where a mean over the column would be under a tenth of
 * that and an RMS under a third, and nothing in the columns either side.
 * Past full scale the byte stays at 127. */
static int test_peak(void)
{
    int fails = 0, rate = 44100;
    float *x = calloc(rate, sizeof(*x));
    if (!x)
        return 1;
    /* Column 75 is [22050, 22344) */
    for (int i = 0; i < 60; i++) {
        double hann = 0.5 - 0.5 * cos(2 * M_PI * i / 60);
        x[22150 + i] = (float)(0.8 * hann * sin(2 * M_PI * 8000 * i / rate));
    }
    CBWaveforms w;
    CHECK(cb_waveforms_compute(x, rate, rate, &w) == 0 && w.nb_detail == 150);
    unsigned e[3];
    tone_pwv7(8000, 0.8, rate, e);
    printf("an 8 kHz burst of 60 samples at 0.8: PWV7 %u %u %u (a steady tone %u %u %u)\n",
           w.pwv7[75 * 3], w.pwv7[75 * 3 + 1], w.pwv7[75 * 3 + 2], e[0], e[1], e[2]);
    CHECK(w.pwv7[75 * 3 + 2] <= e[2] + 2 && w.pwv7[75 * 3 + 2] + 4 >= e[2]);
    for (int j = 0; j < 3; j++)
        CHECK(w.pwv7[74 * 3 + j] == 0 && w.pwv7[76 * 3 + j] <= 1);
    cb_waveforms_free(&w);

    /* 100 Hz at 1.5: its low band is 1.34, so 127; its mid 0.6 */
    for (int i = 0; i < rate; i++)
        x[i] = (float)(1.5 * sin(2 * M_PI * 100 * i / rate));
    CHECK(cb_waveforms_compute(x, rate, rate, &w) == 0 && w.nb_detail == 150);
    tone_pwv7(100, 1.5, rate, e);
    CHECK(e[0] == 127 && e[1] < 127);
    for (int k = 10; k < 150; k++) {
        CHECK(w.pwv7[k * 3] == 127);
        CHECK(w.pwv7[k * 3 + 1] <= e[1] && w.pwv7[k * 3 + 1] + 2 >= e[1]);
    }
    cb_waveforms_free(&w);
    free(x);
    return fails;
}

/* dj's test_waveform_columns, plus the minimums on silence. */
static int test_rules(void)
{
    int fails = 0;
    CBWaveforms w;

    /* 150 columns per second, ceil(samples * 150 / rate) of them */
    float *z = calloc(44101, sizeof(*z));
    if (!z)
        return 1;
    CHECK(cb_waveforms_compute(z, 44100, 44100, &w) == 0 && w.nb_detail == 150);
    cb_waveforms_free(&w);
    CHECK(cb_waveforms_compute(z, 44101, 44100, &w) == 0 && w.nb_detail == 151);
    /* Silence: dj writes zero heights, cyanbox the firmware minimums. */
    for (int i = 0; i < CB_WAVE_PWAV_COLUMNS; i++)
        CHECK((w.pwav[i] & 31) == 2);
    for (int i = 0; i < CB_WAVE_PWV2_COLUMNS; i++)
        CHECK(w.pwv2[i] == 1);
    /* and no three-band level: every band's mean is under rbxport's 1e-6 */
    for (int i = 0; i < CB_WAVE_PWV6_COLUMNS * 3; i++)
        CHECK(w.pwv6[i] == 0);
    for (int i = 0; i < w.nb_detail * 3; i++)
        CHECK(w.pwv7[i] == 0);
    cb_waveforms_free(&w);
    /* No samples: one empty column, and an overview of nothing */
    CHECK(cb_waveforms_compute(z, 0, 44100, &w) == 0 && w.nb_detail == 1);
    for (int i = 0; i < CB_WAVE_PWV6_COLUMNS * 3; i++)
        CHECK(w.pwv6[i] == 0);
    cb_waveforms_free(&w);
    /* Fewer samples than overview columns: those between samples are empty */
    z[5] = 0.5f;
    CHECK(cb_waveforms_compute(z, 10, 44100, &w) == 0 && w.nb_detail == 1);
    int lit = 0;
    for (int i = 0; i < CB_WAVE_PWV6_COLUMNS * 3; i++)
        lit += w.pwv6[i] > 0;
    CHECK(lit > 0 && w.pwv7[2] > 0);
    cb_waveforms_free(&w);
    z[5] = 0.0f;
    CHECK(cb_waveforms_compute(z, 100, 0, &w) == CB_ERROR(EINVAL));
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
    CHECK(cb_waveforms_compute(x, half * 2, rate, &w) == 0 && w.nb_detail == 600);
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
    cb_waveforms_free(&w);
    free(x);
    return fails;
}

static bool same_waves(const CBWaveforms *a, const CBWaveforms *b)
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
    CBWaveforms w, r;
    CHECK(cb_waveforms_compute(x, nb, 44100, &w) == 0);
    free(x);

    uint8_t *data;
    size_t size;
    CHECK(cb_waveforms_pack(&w, &data, &size) == 0);
    if (!data) {
        cb_waveforms_free(&w);
        return fails + 1;
    }
    CHECK(size == 24 + 400 + 100 + 7200 + 3600 + (size_t)w.nb_detail * 6);
    CHECK(!memcmp(data, "RXWV", 4) && le32(data + 4) == 4 && le32(data + 8) == (uint32_t)w.nb_detail &&
          le32(data + 12) == 44100);
    CHECK(cb_waveforms_unpack(data, size, &r) == 0);
    CHECK(same_waves(&r, &w));
    cb_waveforms_free(&r);

    /* The previews alone, as the library keeps them apart: the first
     * CB_WAVE_PACKED_PREVIEW bytes, the header's rate and gains with them,
     * no detail; any other length refused. */
    CHECK(CB_WAVE_PACKED_PREVIEW == 24 + 400 + 100 + 7200 + 3600);
    CHECK(cb_waveforms_unpack_previews(data, CB_WAVE_PACKED_PREVIEW, &r) == 0);
    CHECK(!r.nb_detail && !r.pwv3 && !r.pwv5 && !r.pwv7 && r.rate == 44100);
    CHECK(!memcmp(r.pwav, w.pwav, sizeof(w.pwav)) && !memcmp(r.pwv2, w.pwv2, sizeof(w.pwv2)) &&
          !memcmp(r.pwv4, w.pwv4, sizeof(w.pwv4)) && !memcmp(r.pwv6, w.pwv6, sizeof(w.pwv6)) &&
          !memcmp(r.gains, w.gains, sizeof(w.gains)));
    CHECK(cb_waveforms_unpack_previews(data, size, &r) == CB_ERROR(EINVAL));
    CHECK(cb_waveforms_unpack_previews(data, CB_WAVE_PACKED_PREVIEW - 1, &r) == CB_ERROR(EINVAL));
    put32(data + 4, 3);
    CHECK(cb_waveforms_unpack_previews(data, CB_WAVE_PACKED_PREVIEW, &r) == CB_ERROR(EPROTO));
    put32(data + 4, 4);

    /* Refused, with nothing allocated: truncated, one byte too many, bad
     * magic, another version, a column count that disagrees with the size,
     * no columns, shorter than the header, no data */
    CHECK(cb_waveforms_unpack(data, size - 1, &r) == CB_ERROR(EINVAL) && !r.pwv3);
    uint8_t *big = malloc(size + 1);
    if (big) {
        memcpy(big, data, size);
        big[size] = 0;
        CHECK(cb_waveforms_unpack(big, size + 1, &r) == CB_ERROR(EINVAL) && !r.pwv3);
        free(big);
    }
    data[0] = 'X';
    CHECK(cb_waveforms_unpack(data, size, &r) == CB_ERROR(EINVAL));
    data[0] = 'R';
    /* Another version is stale, not damaged: -EPROTO, so the app recomputes.
     * Versions 1 to 3 hold older three-band levels (dj's, then two fits to
     * rekordbox's bytes) in the same layout as version 4. */
    for (uint32_t v = 0; v < 6; v++) {
        if (v == 4)
            continue;
        put32(data + 4, v);
        CHECK(cb_waveforms_unpack(data, size, &r) == CB_ERROR(EPROTO) && !r.pwv3);
    }
    put32(data + 4, 4);
    put32(data + 8, (uint32_t)w.nb_detail + 1);
    CHECK(cb_waveforms_unpack(data, size, &r) == CB_ERROR(EINVAL));
    put32(data + 8, 0);
    CHECK(cb_waveforms_unpack(data, 24 + 400 + 100 + 7200 + 3600, &r) == CB_ERROR(EINVAL));
    put32(data + 8, (uint32_t)w.nb_detail);
    CHECK(cb_waveforms_unpack(data, 23, &r) == CB_ERROR(EINVAL));
    CHECK(cb_waveforms_unpack(NULL, size, &r) == CB_ERROR(EINVAL));
    CHECK(cb_waveforms_unpack(data, size, &r) == 0 && same_waves(&r, &w));
    cb_waveforms_free(&r);
    free(data);

    /* Nothing to pack */
    CBWaveforms empty = { 0 };
    CHECK(cb_waveforms_pack(&empty, &data, &size) == CB_ERROR(EINVAL) && !data && !size);

    cb_waveforms_free(&w);
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
    CBWaveforms w, r;
    CHECK(cb_waveforms_compute(x, nb, 11025, &w) == 0);
    free(x);

    char path[] = "/tmp/cyanbox-test-waveform-XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0);
    if (fd < 0) {
        cb_waveforms_free(&w);
        return fails;
    }
    close(fd);

    CHECK(cb_waveforms_save(&w, path) == 0);
    uint8_t *packed, *file;
    size_t packed_size, file_size;
    CHECK(cb_waveforms_pack(&w, &packed, &packed_size) == 0);
    CHECK(cb_file_read(path, &file, &file_size) == 0);
    CHECK(file_size == packed_size && !memcmp(file, packed, file_size));
    free(packed);
    free(file);
    CHECK(cb_waveforms_load(path, &r) == 0);
    CHECK(same_waves(&r, &w) && r.rate == 11025);
    cb_waveforms_free(&r);

    /* Damaged files are refused, not half read. */
    uint8_t *buf;
    size_t size;
    CHECK(cb_file_read(path, &buf, &size) == 0);
    CHECK(cb_file_write_atomic(path, buf, size - 1) == 0);
    CHECK(cb_waveforms_load(path, &r) == CB_ERROR(EINVAL) && !r.pwv3);
    buf[8]++;
    CHECK(cb_file_write_atomic(path, buf, size) == 0);
    CHECK(cb_waveforms_load(path, &r) == CB_ERROR(EINVAL));
    buf[8]--;
    /* A version 1 file (dj's three-band levels) or version 3 one (the last
     * fit to rekordbox's) is stale: -EPROTO */
    buf[4] = 1;
    CHECK(cb_file_write_atomic(path, buf, size) == 0);
    CHECK(cb_waveforms_load(path, &r) == CB_ERROR(EPROTO) && !r.pwv3);
    buf[4] = 3;
    CHECK(cb_file_write_atomic(path, buf, size) == 0);
    CHECK(cb_waveforms_load(path, &r) == CB_ERROR(EPROTO) && !r.pwv3);
    buf[4] = 4;
    buf[0] = 'X';
    CHECK(cb_file_write_atomic(path, buf, size) == 0);
    CHECK(cb_waveforms_load(path, &r) == CB_ERROR(EINVAL));
    CHECK(cb_file_write_atomic(path, buf, 10) == 0);
    CHECK(cb_waveforms_load(path, &r) == CB_ERROR(EINVAL));
    free(buf);
    unlink(path);
    CHECK(cb_waveforms_load(path, &r) == CB_ERROR(ENOENT));

    cb_waveforms_free(&w);
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
    printf("fixtures (byte differences per tag; dj's bytes after the firmware minimums,\n"
           "PWV6/PWV7 rbxport's rules):\n");
    for (size_t i = 0; i < CB_ARRAY_ELEMS(signals); i++) {
        char f32[256], tags[256];
        snprintf(f32, sizeof(f32), DATA_DIR "%s.f32", signals[i]);
        snprintf(tags, sizeof(tags), DATA_DIR "%s.tags", signals[i]);
        int diff = compare(f32, tags);
        if (diff)
            printf("FAIL %s: %d differences\n", signals[i], diff);
        fails += diff != 0;
    }
    fails += test_rules();
    fails += test_crossovers();
    fails += test_overview_level();
    fails += test_peak();
    fails += test_pack();
    fails += test_cache();
    if (fails) {
        printf("test_waveform: %d failures\n", fails);
        return 1;
    }
    printf("test_waveform: ok\n");
    return 0;
}
