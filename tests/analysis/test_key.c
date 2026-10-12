/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* The key detector on chord progressions in known keys (the closed form of
 * gen_fixtures.py's tonal(), computed here again): each must come out in
 * the key it was generated in, and the tuning estimate must find the detune
 * it was generated with (tests/data/analysis/keys.txt has the cases).
 *
 * rxbox fixes dj's tuning estimate, which reads every spectral peak 60 Hz
 * low (src/analysis/key.c), so it is checked against the truth, not against
 * dj; dj's answers are printed for information. dj gets 4 of the 9 wrong:
 * B-flat major as Eb, G major as C, E major (-20 cents) as Dbm, and A minor
 * (+30 cents) as F, with tuning estimates off by 10..40 cents.
 *
 * Two misses remain after the fix, listed in keys.txt as known: G major
 * reads as C and E major as Dbm. Both are major I - V - vi - IV, where the
 * true key, its IV and its relative minor correlate within ~0.02 with the
 * Temperley profiles, and the synthetic kick tips them: it sounds for more
 * than half of every beat, so the 9-frame median keeps it, and its pitch
 * sweep adds a chroma tilt peaking at C/Db/D. Without the kick both are
 * right. A known miss must give exactly the listed wrong key (anything else
 * fails), and if it starts giving the true key the test says so.
 *
 * Also: names are dj's spellings ("Dbm", "F#m", "Bb"), silence has no key,
 * bad arguments are refused.
 *
 * With arguments, `test_key SIGNAL.f32 RATE` prints the key of one raw
 * float32 mono file (the informational real-track comparison with
 * gen_fixtures.py --real). */

#include <math.h>
#include <stdio.h>

#include "rxbox/analysis.h"
#include "rxbox/common.h"

#define KEYS_FILE "tests/data/analysis/keys.txt"

static const char *const MAJOR[12] = {
    "C", "Db", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B",
};
static const char *const MINOR[12] = {
    "Cm", "Dbm", "Dm", "Ebm", "Em", "Fm", "F#m", "Gm", "Abm", "Am", "Bbm", "Bm",
};

/* dj's synthetic kick: a 170 -> 50 Hz drop over 0.25 s */
static double kick(double t)
{
    if (t < 0 || t >= 0.25)
        return 0.0;
    double phase = 2 * M_PI * (50 * t + 120 * (1 - exp(-40 * t)) / 40);
    return sin(phase) * exp(-12 * t);
}

/* gen_fixtures.py's tonal(): i - iv - V - i or I - V - vi - IV, chord tones
 * with an octave partial, a bass from bar 4, a kick on every beat. */
static float *tonal(int rate, int tonic, bool minor, double cents, int64_t *nb)
{
    static const int prog[2][4][3] = {
        { { 0, 4, 7 }, { 7, 11, 14 }, { 9, 12, 16 }, { 5, 9, 12 } },
        { { 0, 3, 7 }, { 5, 8, 12 }, { 7, 11, 14 }, { 0, 3, 7 } },
    };
    double period = 60.0 / 124.0, bar = 4 * period;
    double detune = pow(2.0, cents / 1200);
    int64_t n = (int64_t)rint(16 * bar * rate);
    float *x = malloc(sizeof(*x) * n);
    if (!x)
        return NULL;
    for (int64_t i = 0; i < n; i++) {
        double t = (double)i / rate;
        double b = floor(t / bar), u = t / bar - b;
        int k = (int)b % 4;
        double swell = pow(sin(M_PI * u), 0.1), v = 0.0;
        for (int j = 0; j < 3; j++) {
            double f = 440.0 * pow(2.0, (48 + tonic + prog[minor][k][j] - 69) / 12.0) * detune;
            v += 0.25 * (sin(2 * M_PI * f * t) + 0.3 * sin(4 * M_PI * f * t)) / 3 * swell;
        }
        if (b >= 4) {
            double f = 440.0 * pow(2.0, (36 + tonic + prog[minor][k][0] - 69) / 12.0) * detune;
            v += 0.3 * (sin(2 * M_PI * f * t) + 0.3 * sin(4 * M_PI * f * t));
        }
        v += 0.8 * kick(fmod(t, period));
        x[i] = (float)v;
    }
    *nb = n;
    return x;
}

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            fails++;                                                    \
        }                                                               \
    } while (0)

static int test_synthetic(void)
{
    int fails = 0, nb_cases = 0, nb_true = 0, nb_dj_true = 0;
    FILE *f = fopen(KEYS_FILE, "r");
    if (!f) {
        printf("FAIL: cannot open %s\n", KEYS_FILE);
        return 1;
    }
    char line[256];
    printf("%-10s %6s %6s  %-4s  %-4s %8s %8s  |  dj: %-4s %8s %8s\n", "signal", "rate", "cents",
           "true", "rx", "conf", "cents", "key", "conf", "cents");
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#')
            continue;
        char name[32], truth[8], known[8], dj_key[8];
        int rate, tonic, minor;
        double cents, dj_conf, dj_tuning;
        if (sscanf(line, "%31s %d %d %d %lf %7s %7s %7s %lf %lf", name, &rate, &tonic, &minor,
                   &cents, truth, known, dj_key, &dj_conf, &dj_tuning) != 10) {
            printf("FAIL: bad line in %s: %s", KEYS_FILE, line);
            fails++;
            continue;
        }
        /* keys.txt and rxbox spell the keys the same way */
        CHECK(!strcmp(truth, minor ? MINOR[tonic] : MAJOR[tonic]));

        int64_t nb;
        float *x = tonal(rate, tonic, minor, cents, &nb);
        if (!x)
            return fails + 1;
        RxKeyResult k;
        int64_t t0 = rx_time_us();
        int ret = rx_analyse_key(x, nb, rate, &k);
        int64_t t1 = rx_time_us();
        free(x);
        CHECK(ret == 0);

        bool right = !strcmp(k.key, truth);
        const char *note = "";
        if (strcmp(known, "-")) {
            note = right ? "  (listed as a known miss, now right: update keys.txt)" : "  (known miss)";
            CHECK(right || !strcmp(k.key, known));
        } else {
            CHECK(right);
        }
        printf("%-10s %6d %+6.0f  %-4s  %-4s %8.4f %+8.2f  |      %-4s %8.4f %+8.2f  %.0f ms%s\n",
               name, rate, cents, truth, k.key, k.confidence, k.tuning_cents, dj_key, dj_conf,
               100 * dj_tuning, (t1 - t0) / 1000.0, note);
        /* A real tuning estimate: the generated detune, to a few cents */
        CHECK(fabs(k.tuning_cents - cents) < 3);
        nb_cases++;
        nb_true += right;
        nb_dj_true += !strcmp(dj_key, truth);
    }
    fclose(f);
    printf("%d signals: rxbox finds the true key of %d, dj of %d\n", nb_cases, nb_true,
           nb_dj_true);
    CHECK(nb_cases >= 9);
    return fails;
}

static int test_edges(void)
{
    int fails = 0;
    RxKeyResult k;

    /* Silence has no key */
    float *z = calloc(44100 * 5, sizeof(*z));
    if (!z)
        return 1;
    CHECK(rx_analyse_key(z, 44100 * 5, 44100, &k) == 0);
    CHECK(k.key[0] == '\0' && k.confidence == 0 && k.tuning_cents == 0);
    CHECK(rx_analyse_key(z, 0, 44100, &k) == 0 && k.key[0] == '\0');
    CHECK(rx_analyse_key(z, 100, 0, &k) == RX_ERROR(EINVAL));
    CHECK(rx_analyse_key(NULL, 100, 44100, &k) == RX_ERROR(EINVAL));
    free(z);

    /* A second of a chord, shorter than the median span: still a key, and
     * one of dj's spellings */
    int64_t nb;
    float *x = tonal(48000, 9, true, 0.0, &nb);
    if (!x)
        return fails + 1;
    CHECK(rx_analyse_key(x, 48000, 48000, &k) == 0);
    bool known = false;
    for (int i = 0; i < 12; i++)
        known |= !strcmp(k.key, MAJOR[i]) || !strcmp(k.key, MINOR[i]);
    CHECK(known);
    free(x);
    return fails;
}

static int key_of_file(const char *path, int rate)
{
    uint8_t *buf;
    size_t size;
    int ret = rx_file_read(path, &buf, &size);
    if (ret < 0) {
        printf("cannot read %s: %s\n", path, strerror(-ret));
        return 1;
    }
    float *x = malloc(RX_MAX(size, 4));
    if (!x) {
        free(buf);
        return 1;
    }
    memcpy(x, buf, size);
    free(buf);
    RxKeyResult k;
    int64_t t0 = rx_time_us();
    ret = rx_analyse_key(x, (int64_t)(size / 4), rate, &k);
    int64_t t1 = rx_time_us();
    free(x);
    if (ret < 0) {
        printf("rx_analyse_key failed: %s\n", strerror(-ret));
        return 1;
    }
    printf("%d %s %.6f %.6f %.0f ms\n", rate, k.key, k.confidence, k.tuning_cents / 100,
           (t1 - t0) / 1000.0);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 3)
        return key_of_file(argv[1], atoi(argv[2]));

    int fails = test_synthetic() + test_edges();
    if (fails) {
        printf("test_key: %d failures\n", fails);
        return 1;
    }
    printf("test_key: ok\n");
    return 0;
}
