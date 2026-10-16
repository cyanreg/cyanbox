/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* Questions asked of the model together (ml_ask_many) against the same
 * questions asked one at a time: the logits bit for bit, the answers to
 * the byte. Among them a question of a few tokens (the GEMV's), one of
 * over LLM_CHUNK tokens (read in two chunks), answers of one token and of
 * many, and an answer cut short once settled; and all of them twice over,
 * more than the cache has room for beside the audio at once. The audio is
 * made here: 40 s of a beat, a bass line and a chord.
 *
 * Needs the model, $CYANBOX_ML_MODEL (the 4 GB GGUF file), and a GPU that
 * can run it; skipped (77) without either. */

#include <math.h>
#include <stdio.h>
#include <sys/stat.h>

#include "ml_priv.h"

static int failures;

static float *make_audio(int64_t n)
{
    float *pcm = malloc((size_t)n * sizeof(*pcm));
    if (!pcm)
        return NULL;
    uint32_t r = 12345;
    const double beat = 60.0 / 124.0;
    static const double chord[3] = { 220.0, 261.63, 329.63 };
    static const double bass[4] = { 55.0, 55.0, 65.41, 49.0 };
    for (int64_t i = 0; i < n; i++) {
        double t = (double)i / CB_ML_RATE;
        double tb = fmod(t, beat);
        int b = (int)(t / beat);
        r = r * 1664525u + 1013904223u;
        double noise = (double)(r >> 8) / (1 << 24) * 2.0 - 1.0;
        double kick = sin(2 * M_PI * (50.0 + 80.0 * exp(-tb * 30.0)) * tb) * exp(-tb * 8.0);
        double toff = fmod(t + beat / 2, beat);
        double hat = noise * exp(-toff * 60.0) * 0.3;
        double bs = sin(2 * M_PI * bass[(b / 4) % 4] * t) * 0.3;
        double pad = 0.0;
        for (int k = 0; k < 3; k++)
            pad += sin(2 * M_PI * chord[k] * t) * 0.08;
        pcm[i] = (float)(0.5 * kick + hat + bs + pad);
    }
    return pcm;
}

static bool any_text(const char *answer)
{
    return answer[0] != 0;
}

/* The questions of the test, each with its own logits buffer when it is
 * scored. */
#define NQ 9

static void make_queries(MLQuery *q, float *logits, const char *long_q)
{
    static const char *const pick =
        "Which of these descriptions fits this music best?\nA. dark and intense, hard\n"
        "B. intense, peak time\nC. energetic and bright, driving\nD. bright, uplifting\n"
        "E. groovy and relaxed, warm\nF. calm, smooth\nG. deep, melancholic\n"
        "H. calm and dark, ambient\nAnswer with the letter only.";
    MLQuery all[NQ] = {
        { .question = "Is this track dark? Answer yes or no." },
        { .question = pick },
        { .question = "Hi" },
        { .question = "What genre is this track? Answer with the genre name only.",
          .answer_wanted = true, .max_tokens = 24 },
        { .question = long_q },
        { .question = "Describe this music.", .answer_wanted = true, .max_tokens = 40 },
        { .question = "Is there a beat?", .answer_wanted = true, .max_tokens = 1 },
        { .question = "Hi", .answer_wanted = true, .max_tokens = 6 },
        { .question = "Describe this music.", .answer_wanted = true, .max_tokens = 40,
          .settled = any_text },
    };
    for (int i = 0; i < NQ; i++) {
        q[i] = all[i];
        if (!q[i].answer_wanted)
            q[i].logits = logits + (size_t)i * ML_VOCAB;
    }
}

static void compare(const char *what, const MLQuery *got, const MLQuery *want)
{
    int before = failures;
    for (int i = 0; i < NQ; i++) {
        if (want[i].logits && memcmp(got[i].logits, want[i].logits, (size_t)ML_VOCAB * 4)) {
            int k = 0;
            while (got[i].logits[k] == want[i].logits[k])
                k++;
            printf("FAIL %s: question %d's logits differ (%d: %.9g vs %.9g)\n", what, i, k,
                   got[i].logits[k], want[i].logits[k]);
            failures++;
        }
        if (want[i].answer_wanted &&
            (!got[i].answer || !want[i].answer || strcmp(got[i].answer, want[i].answer))) {
            printf("FAIL %s: question %d's answer \"%s\", alone \"%s\"\n", what, i,
                   got[i].answer ? got[i].answer : "(null)",
                   want[i].answer ? want[i].answer : "(null)");
            failures++;
        }
    }
    printf("%-40s %s\n", what, failures > before ? "FAIL" : "ok, bit for bit");
}

int main(void)
{
    const char *path = getenv("CYANBOX_ML_MODEL");
    struct stat st;
    cb_log_set_level(CB_LOG_DEBUG);
    if (!path || !path[0] || stat(path, &st) < 0) {
        printf("no model ($CYANBOX_ML_MODEL): skipped\n");
        return 77;
    }
    CBMLOptions o;
    cb_ml_options_default(&o);
    o.max_context = 2048;
    CBML *ml = NULL;
    int ret = cb_ml_open(&ml, path, &o);
    if (ret == CB_ERROR(ENODEV)) {
        printf("no device for the model: skipped\n");
        return 77;
    }
    if (ret < 0) {
        printf("FAIL cannot open %s: %s\n", path, strerror(-ret));
        return 1;
    }

    int64_t n = 40 * CB_ML_RATE;
    float *pcm = make_audio(n);
    char *long_q = calloc(1, 8192);
    float *alone_l = malloc((size_t)NQ * ML_VOCAB * 4), *together_l = malloc((size_t)2 * NQ * ML_VOCAB * 4);
    if (!pcm || !long_q || !alone_l || !together_l) {
        printf("FAIL out of memory\n");
        return 1;
    }
    for (int i = 0; i < 200; i++)
        strcat(long_q, "Listen closely. ");
    strcat(long_q, "Is this music? Answer yes or no.");
    if ((ret = cb_ml_listen(ml, pcm, n, NULL)) < 0) {
        printf("FAIL listening: %s\n", strerror(-ret));
        return 1;
    }

    /* One at a time, as cb_ml_choose and cb_ml_ask ask. */
    MLQuery alone[NQ], together[2 * NQ];
    make_queries(alone, alone_l, long_q);
    for (int i = 0; i < NQ; i++) {
        if ((ret = ml_ask_many(ml, &alone[i], 1, NULL)) < 0) {
            printf("FAIL question %d alone: %s\n", i, strerror(-ret));
            return 1;
        }
    }
    printf("question 4 is %d tokens; the genre: \"%s\"; the music: \"%s\"\n",
           ml_question_tokens(ml, long_q, NULL), alone[3].answer, alone[5].answer);
    if (ml_question_tokens(ml, long_q, NULL) <= 512 || ml_question_tokens(ml, "Hi", NULL) > 8) {
        printf("FAIL the questions are not of the lengths the test needs\n");
        failures++;
    }
    if (strncmp(alone[8].answer, alone[5].answer, strlen(alone[8].answer)) ||
        !alone[8].answer[0] || !strcmp(alone[8].answer, alone[5].answer)) {
        printf("FAIL the settled answer \"%s\" is not the start of \"%s\"\n", alone[8].answer,
               alone[5].answer);
        failures++;
    }

    /* Together, then twice over. */
    make_queries(together, together_l, long_q);
    if ((ret = ml_ask_many(ml, together, NQ, NULL)) < 0) {
        printf("FAIL the questions together: %s\n", strerror(-ret));
        return 1;
    }
    compare("together", together, alone);
    for (int i = 0; i < NQ; i++)
        free(together[i].answer);
    make_queries(together, together_l, long_q);
    make_queries(together + NQ, together_l + (size_t)NQ * ML_VOCAB, long_q);
    if ((ret = ml_ask_many(ml, together, 2 * NQ, NULL)) < 0) {
        printf("FAIL the questions twice over: %s\n", strerror(-ret));
        return 1;
    }
    compare("twice over, in more than one batch", together, alone);
    compare("twice over, the second", together + NQ, alone);

    for (int i = 0; i < NQ; i++) {
        free(alone[i].answer);
        free(together[i].answer);
        free(together[NQ + i].answer);
    }
    free(pcm);
    free(long_q);
    free(alone_l);
    free(together_l);
    cb_ml_close(&ml);
    printf("%s\n", failures ? "FAILED" : "all ok");
    return failures ? 1 : 0;
}
