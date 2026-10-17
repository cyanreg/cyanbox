/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* Beat This! on the GPU (cyanbox/beatthis.h) against its reference code:
 * the logits of 40 s of a made beat (kicks accented on the downbeat, snares
 * on two and four, hats on the offbeats, a bass) against what beat_this
 * (PyTorch) makes of the same samples, beatthis_ref.h, written by
 * gen_beatthis_ref.py. Forty seconds are two chunks of the network, which
 * overlap: the frames where they meet come from the first, as the
 * reference's "keep_first". The same audio a second time gives the same
 * logits, bit for bit; one second (a chunk shorter than the network's)
 * gives as many frames as the reference says and all of them set.
 *
 * Needs the model, $CYANBOX_BEATTHIS_MODEL (beat-this-f16.gguf; meson
 * passes models/'s), and a GPU that can run it; skipped (77) without
 * either. */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "cyanbox/beatthis.h"
#include "beatthis_ref.h"

static int failures;

#define CHECK(c, ...) do {                                                   \
        if (!(c)) {                                                          \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                      \
            printf(__VA_ARGS__);                                             \
            printf("\n");                                                    \
            failures++;                                                      \
        }                                                                    \
    } while (0)

/* gen_beatthis_ref.py's make_audio, sample for sample. */
static float *make_audio(int64_t n)
{
    float *pcm = malloc((size_t)n * sizeof(*pcm));
    if (!pcm)
        return NULL;
    uint32_t r = 12345;
    const double beat = 60.0 / 128.0;
    for (int64_t i = 0; i < n; i++) {
        double t = (double)i / CB_BEATTHIS_RATE;
        double tb = fmod(t, beat);
        int b = (int)(t / beat);
        r = r * 1664525u + 1013904223u;
        double noise = (double)(r >> 8) / (1 << 24) * 2.0 - 1.0;
        double accent = b % 4 == 0 ? 1.0 : 0.6;
        double kick = sin(2 * M_PI * (50.0 + 80.0 * exp(-tb * 30.0)) * tb) * exp(-tb * 8.0);
        double snare = b % 2 == 1 ? noise * exp(-tb * 20.0) * 0.4 : 0.0;
        double toff = fmod(t + beat / 2, beat);
        double hat = noise * exp(-toff * 60.0) * 0.2;
        double bass = sin(2 * M_PI * ((b / 4) % 2 == 0 ? 55.0 : 65.41) * t) * 0.25;
        pcm[i] = (float)(0.5 * accent * kick + snare + hat + bass);
    }
    return pcm;
}

int main(void)
{
    const char *path = getenv("CYANBOX_BEATTHIS_MODEL");
    struct stat st;
    if (!path || !path[0] || stat(path, &st) < 0) {
        printf("no model ($CYANBOX_BEATTHIS_MODEL): skipped\n");
        return 77;
    }
    CBBeatThis *bt = NULL;
    int ret = cb_beatthis_open(&bt, path, NULL);
    if (ret == CB_ERROR(ENODEV)) {
        printf("no GPU that runs the model: skipped\n");
        return 77;
    }
    if (ret < 0) {
        printf("FAIL: cb_beatthis_open: %d\n", ret);
        return 1;
    }

    int64_t n = (int64_t)40 * CB_BEATTHIS_RATE;
    float *pcm = make_audio(n);
    float *beat = NULL, *down = NULL, *beat2 = NULL, *down2 = NULL;
    int nf = 0, nf2 = 0;
    CHECK(pcm, "no memory");
    ret = pcm ? cb_beatthis_run(bt, pcm, n, &beat, &down, &nf) : -1;
    CHECK(ret == 0, "cb_beatthis_run: %d", ret);
    CHECK(nf == REF_FRAMES, "%d frames, the reference has %d", nf, REF_FRAMES);
    if (ret == 0 && nf == REF_FRAMES) {
        /* f32 sums in another order than PyTorch's: 3e-4 at most on music */
        double worst = 0.0;
        int at = -1, nb = 0, flips = 0;
        for (int i = 0; i < REF_FRAMES; i += REF_STEP, nb++) {
            float rb = ref_beat[i / REF_STEP], rd = ref_down[i / REF_STEP];
            double d = fmax(fabs(beat[i] - rb), fabs(down[i] - rd));
            if (d > worst) {
                worst = d;
                at = i;
            }
            flips += fabs(rb) > 0.05 && (beat[i] > 0) != (rb > 0);
        }
        printf("%d frames against the reference: largest difference %.5f (frame %d)\n", nb,
               worst, at);
        CHECK(worst < 2e-3, "frame %d is %.5f off the reference", at, worst);
        CHECK(!flips, "%d frames on the other side of 0 from the reference", flips);

        ret = cb_beatthis_run(bt, pcm, n, &beat2, &down2, &nf2);
        CHECK(ret == 0 && nf2 == nf, "the second run: %d, %d frames", ret, nf2);
        if (ret == 0 && nf2 == nf)
            CHECK(!memcmp(beat, beat2, (size_t)nf * sizeof(*beat)) &&
                  !memcmp(down, down2, (size_t)nf * sizeof(*down)),
                  "the second run's logits differ");
    }
    free(beat);
    free(down);
    free(beat2);
    free(down2);

    /* one second: a single chunk of its own length and the borders */
    int64_t n1 = CB_BEATTHIS_RATE;
    ret = cb_beatthis_run(bt, pcm, n1, &beat, &down, &nf);
    CHECK(ret == 0 && nf == 1 + (int)(n1 / 441), "one second: %d, %d frames", ret, nf);
    int unset = 0;
    for (int i = 0; ret == 0 && i < nf; i++)
        unset += beat[i] <= -1000.0f || down[i] <= -1000.0f;
    CHECK(!unset, "one second: %d frames not set", unset);
    free(beat);
    free(down);

    free(pcm);
    cb_beatthis_close(&bt);
    if (failures)
        printf("%d failures\n", failures);
    else
        printf("ok\n");
    return failures ? 1 : 0;
}
