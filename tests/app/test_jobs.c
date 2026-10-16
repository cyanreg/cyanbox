/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* The background jobs (cyanbox/jobs.h) on a small WAV the ffmpeg command line
 * made (12 s, 48 kHz stereo, a 128 BPM kick): an analyse round trip with
 * the active track ids, a decode at the file's own rate and channels, an
 * artwork job on a file without art, a missing file, cancelling queued
 * requests, destroying a queue with work still in it, and the memory
 * guard: with a budget smaller than any analysis, two analyses on two
 * workers run one after the other (and both run), one waiting for memory
 * can be cancelled, and with the default budget the same two run at once;
 * a tag batch whose model does not open. With --tag, the tag batch with the
 * model (src/app/jobs.c decodes each track while the one before it is
 * tagged): see test_tag.
 *
 * Usage: test_jobs FILE.wav
 *        test_jobs --tag A.wav B.wav (with $CYANBOX_ML_MODEL) */

#include "cyanbox/jobs.h"
#include "cyanbox/library.h"

#include <math.h>
#include <stdio.h>
#include <unistd.h>

static int failures;

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond);   \
            fprintf(stderr, __VA_ARGS__);                                     \
            fputc('\n', stderr);                                              \
            failures++;                                                       \
        }                                                                     \
    } while (0)

static CBJobResult *wait_result(CBJobs *j, int timeout_s)
{
    return cb_jobs_wait(j, timeout_s * 1000);
}

static void test_analyse(const char *wav)
{
    CBJobs *j = NULL;
    int ret = cb_jobs_create(&j, 2);
    CHECK(ret == 0, "cb_jobs_create: %s", strerror(-ret));
    if (ret < 0)
        return;

    CBAnalysisOptions opt;
    cb_analysis_options_default(&opt);
    uint64_t req = cb_jobs_analyse(j, 42, wav, &opt, CB_ANALYSE_ALL);
    CHECK((int64_t)req > 0, "request %lld", (long long)req);
    CHECK(cb_jobs_pending(j) == 1, "pending %d", cb_jobs_pending(j));

    /* While it runs, worker reports the track it analyses. */
    bool seen_active = false;
    CBJobResult *r = NULL;
    int64_t t0 = cb_time_us();
    while (!(r = cb_jobs_poll(j)) && cb_time_us() - t0 < 300 * 1000000LL) {
        uint64_t ids[4];
        int nb = cb_jobs_active_tracks(j, ids, 4);
        if (nb == 1 && ids[0] == 42)
            seen_active = true;
        usleep(500);
    }
    CHECK(r, "no analysis result");
    CHECK(seen_active, "the analysed track never showed as active");
    if (!r)
        goto end;

    CHECK(r->kind == CB_JOB_ANALYSE, "kind %d", r->kind);
    CHECK(r->track_id == 42, "track %llu", (unsigned long long)r->track_id);
    CHECK(r->request == req, "request %llu", (unsigned long long)r->request);
    CHECK(r->err == 0, "err %s", strerror(-r->err));
    CHECK(r->seconds > 0, "seconds %f", r->seconds);
    CHECK(r->analysis, "no analysis");
    if (r->analysis) {
        const CBAnalysis *a = r->analysis;
        CHECK(fabs(a->duration - 12.0) < 1e-6, "duration %f", a->duration);
        CHECK(a->done & CB_ANALYSE_WAVES, "done %#x", a->done);
        CHECK(abs(a->waves.nb_detail - 12 * CB_WAVE_COLUMNS_PER_SECOND) <= 2,
              "%d detail columns", a->waves.nb_detail);
        if (a->done & CB_ANALYSE_BEATS)
            printf("analysis: %.3f BPM, confidence %.2f, flags %#x\n",
                   cb_grid_bpm(&a->beats.grid), a->beats.confidence, a->beats.flags);
        if (a->done & CB_ANALYSE_KEY)
            printf("analysis: key \"%s\", confidence %.2f\n", a->key.key, a->key.confidence);
        printf("analysis: done %#x in %.2f s\n", a->done, r->seconds);
    }
    cb_job_result_free(r);

    CHECK(cb_jobs_pending(j) == 0, "pending %d", cb_jobs_pending(j));
    CHECK(cb_jobs_running(j) == 0, "running %d", cb_jobs_running(j));
    uint64_t ids[4];
    CHECK(cb_jobs_active_tracks(j, ids, 4) == 0, "tracks still active");

end:
    cb_jobs_destroy(j);
}

static void test_decode_and_artwork(const char *wav)
{
    CBJobs *j = NULL;
    int ret = cb_jobs_create(&j, 2);
    CHECK(ret == 0, "cb_jobs_create: %s", strerror(-ret));
    if (ret < 0)
        return;

    uint64_t dec = cb_jobs_decode(j, 7, wav, 0);
    CHECK((int64_t)dec > 0, "decode request %lld", (long long)dec);
    CBJobResult *r = wait_result(j, 60);
    CHECK(r, "no decode result");
    if (r) {
        CHECK(r->kind == CB_JOB_DECODE && r->request == dec && r->track_id == 7 && r->deck == 0,
              "kind %d request %llu deck %d", r->kind, (unsigned long long)r->request, r->deck);
        CHECK(r->err == 0 && r->packets && !r->audio, "err %s", strerror(-r->err));
        if (r->packets) {
            /* The file's packets, decoded as the deck plays them, at the
             * file's own rate and channels: no resampling, no downmix. */
            CBPackets *pk = r->packets;
            CHECK(cb_packets_rate(pk) == 48000, "rate %d", cb_packets_rate(pk));
            CHECK(cb_packets_channels(pk) == 2, "channels %d", cb_packets_channels(pk));
            CHECK(cb_packets_length(pk, NULL) == 12 * 48000, "%lld frames",
                  (long long)cb_packets_length(pk, NULL));
            CBPacketDecoder *d = NULL;
            float *x = malloc(sizeof(float) * 2 * 48000);
            float peak = 0;
            if (x && cb_packets_decoder(pk, &d) >= 0) {
                for (int64_t at = 0; at < 12 * 48000; at += 48000) {
                    CHECK(cb_packets_decode(d, at, 48000, x, 2) == 48000, "decode at %lld",
                          (long long)at);
                    for (int i = 0; i < 2 * 48000; i++)
                        peak = fmaxf(peak, fabsf(x[i]));
                }
            }
            CHECK(peak > 0.5f && peak <= 1.0f, "peak %f", peak);
            cb_packets_decoder_free(d);
            free(x);
        }
        cb_job_result_free(r);
    }

    /* A WAV has no cover and no cover file sits next to it. */
    uint64_t art = cb_jobs_artwork(j, 7, wav, 64);
    CHECK((int64_t)art > 0, "artwork request %lld", (long long)art);
    r = wait_result(j, 30);
    CHECK(r, "no artwork result");
    if (r) {
        CHECK(r->kind == CB_JOB_ARTWORK && r->request == art, "kind %d", r->kind);
        CHECK(r->err == CB_ERROR(ENOENT), "err %d (%s)", r->err, strerror(-r->err));
        CHECK(!r->rgba && !r->jpeg, "art from nowhere");
        cb_job_result_free(r);
    }

    /* A missing file is an error result, not silence. */
    uint64_t miss = cb_jobs_analyse(j, 9, "/nonexistent/cyanbox/track.flac", NULL, CB_ANALYSE_ALL);
    r = wait_result(j, 30);
    CHECK(r && r->request == miss && r->err < 0 && !r->analysis, "missing file result");
    cb_job_result_free(r);

    CHECK((int64_t)cb_jobs_analyse(j, 9, wav, NULL, 0) < 0, "nothing to analyse accepted");
    cb_jobs_destroy(j);
}

static void test_cancel(const char *wav)
{
    CBJobs *j = NULL;
    int ret = cb_jobs_create(&j, 1);
    CHECK(ret == 0, "cb_jobs_create: %s", strerror(-ret));
    if (ret < 0)
        return;

    /* The one worker is busy with the analysis, so the decodes queue. */
    uint64_t first = cb_jobs_analyse(j, 1, wav, NULL, CB_ANALYSE_ALL);
    int64_t t0 = cb_time_us();
    while (!cb_jobs_running(j) && cb_time_us() - t0 < 10 * 1000000LL)
        usleep(100);
    uint64_t r2 = cb_jobs_decode(j, 2, wav, 0);
    uint64_t r3 = cb_jobs_decode(j, 3, wav, 0);
    uint64_t r4 = cb_jobs_decode(j, 4, wav, 0);
    cb_jobs_cancel(j, r2);
    cb_jobs_cancel(j, r3);
    cb_jobs_cancel(j, r3);              /* twice is harmless */
    cb_jobs_cancel(j, first);           /* running: finishes and is reported */
    CHECK(cb_jobs_pending(j) == 2, "pending %d after cancelling two", cb_jobs_pending(j));

    bool got_first = false, got_r4 = false;
    t0 = cb_time_us();
    while (cb_time_us() - t0 < 300 * 1000000LL) {
        CBJobResult *r = cb_jobs_wait(j, 100);
        if (!r) {
            if (!cb_jobs_pending(j))
                break;
            continue;
        }
        CHECK(r->request != r2 && r->request != r3, "a cancelled request (%llu) ran",
              (unsigned long long)r->request);
        got_first |= r->request == first;
        got_r4 |= r->request == r4;
        cb_job_result_free(r);
    }
    CHECK(got_first, "the running job was not reported");
    CHECK(got_r4, "the job after the cancelled ones did not run");
    CHECK(cb_jobs_pending(j) == 0, "pending %d", cb_jobs_pending(j));

    /* Request ids keep increasing. */
    uint64_t r5 = cb_jobs_decode(j, 5, wav, 0);
    CHECK(r5 > r4, "request %llu after %llu", (unsigned long long)r5, (unsigned long long)r4);
    cb_jobs_cancel_all(j);
    cb_jobs_destroy(j);

    /* Destroying with work queued: the queued jobs are dropped, the running
     * one finishes, nothing leaks into a hang. */
    ret = cb_jobs_create(&j, 1);
    CHECK(ret == 0, "cb_jobs_create: %s", strerror(-ret));
    if (ret < 0)
        return;
    for (int i = 0; i < 6; i++)
        cb_jobs_analyse(j, 10 + i, wav, NULL, CB_ANALYSE_ALL);
    t0 = cb_time_us();
    cb_jobs_destroy(j);
    double took = (cb_time_us() - t0) / 1e6;
    printf("destroy with 6 queued analyses took %.2f s\n", took);
}

/* Two analyses on two workers with the budget given (NULL: the default),
 * sampling the queue until both results are in: the most jobs running at
 * once, whether both tracks were ever active together, and the first and
 * last time each was seen active. With the default budget, returns whether
 * they were seen running at once (checked by the caller). */
static bool budget_round(const char *wav, const char *budget_mb)
{
    if (budget_mb)
        setenv("CYANBOX_ANALYSIS_BUDGET_MB", budget_mb, 1);
    else
        unsetenv("CYANBOX_ANALYSIS_BUDGET_MB");
    CBJobs *j = NULL;
    int ret = cb_jobs_create(&j, 2);
    unsetenv("CYANBOX_ANALYSIS_BUDGET_MB");
    CHECK(ret == 0, "cb_jobs_create: %s", strerror(-ret));
    if (ret < 0)
        return true;

    const uint64_t ids[2] = { 101, 102 };
    for (int i = 0; i < 2; i++)
        CHECK((int64_t)cb_jobs_analyse(j, ids[i], wav, NULL, CB_ANALYSE_ALL) > 0, "request %d", i);

    int got = 0, max_running = 0;
    bool together = false;
    int64_t first[2] = { 0 }, last[2] = { 0 };
    int64_t t0 = cb_time_us();
    while (got < 2 && cb_time_us() - t0 < 300 * 1000000LL) {
        CBJobResult *r = cb_jobs_poll(j);
        if (r) {
            CHECK(r->err == 0 && r->analysis, "track %llu: %s", (unsigned long long)r->track_id,
                  strerror(-r->err));
            cb_job_result_free(r);
            got++;
            continue;
        }
        max_running = CB_MAX(max_running, cb_jobs_running(j));
        uint64_t active[4];
        int nb = cb_jobs_active_tracks(j, active, 4);
        int64_t now = cb_time_us();
        for (int i = 0; i < nb; i++) {
            for (int k = 0; k < 2; k++) {
                if (active[i] != ids[k])
                    continue;
                if (!first[k])
                    first[k] = now;
                last[k] = now;
            }
        }
        together |= nb == 2;
        usleep(200);
    }
    CHECK(got == 2, "%d of 2 analyses reported (budget %s MB)", got,
          budget_mb ? budget_mb : "default");

    if (budget_mb) {
        /* Every analysis needs more than the budget: one at a time, and
         * the one waiting was not counted as running meanwhile. */
        CHECK(max_running == 1, "%d analyses ran at once", max_running);
        CHECK(!together, "both tracks were active at once");
        CHECK(first[0] && first[1], "a track never ran (seen at %lld and %lld)",
              (long long)first[0], (long long)first[1]);
        CHECK(last[0] < first[1] || last[1] < first[0],
              "the analyses overlapped: %lld..%lld and %lld..%lld us",
              (long long)(first[0] - t0), (long long)(last[0] - t0),
              (long long)(first[1] - t0), (long long)(last[1] - t0));
        if (first[0] && first[1])
            printf("budget %s MB: one after the other, done %.2f s and %.2f s in\n", budget_mb,
                   (CB_MIN(last[0], last[1]) - t0) / 1e6, (CB_MAX(last[0], last[1]) - t0) / 1e6);
    }
    cb_jobs_destroy(j);
    return max_running == 2 && together;
}

static void test_budget(const char *wav)
{
    budget_round(wav, "1");
    /* The control: the default budget holds both, so they run at once.
     * Seeing that takes the second worker starting within the first's
     * tenth of a second, so a loaded machine gets three tries. */
    bool together = false;
    for (int i = 0; i < 3 && !together; i++)
        together = budget_round(wav, NULL);
    CHECK(together, "the default budget never ran both analyses at once");

    /* An analysis waiting for memory has not started: cancelling it drops
     * it, and only the running one is reported. */
    setenv("CYANBOX_ANALYSIS_BUDGET_MB", "1", 1);
    CBJobs *j = NULL;
    int ret = cb_jobs_create(&j, 2);
    unsetenv("CYANBOX_ANALYSIS_BUDGET_MB");
    CHECK(ret == 0, "cb_jobs_create: %s", strerror(-ret));
    if (ret < 0)
        return;
    uint64_t req[2] = {
        cb_jobs_analyse(j, 201, wav, NULL, CB_ANALYSE_ALL),
        cb_jobs_analyse(j, 202, wav, NULL, CB_ANALYSE_ALL),
    };
    uint64_t active = 0;
    int64_t t0 = cb_time_us();
    while (!active && cb_time_us() - t0 < 30 * 1000000LL) {
        if (cb_jobs_active_tracks(j, &active, 1) != 1)
            active = 0;
        usleep(100);
    }
    CHECK(active == 201 || active == 202, "no analysis started");
    uint64_t running = active == 202 ? req[1] : req[0];
    uint64_t waiting = active == 202 ? req[0] : req[1];
    CHECK(cb_jobs_running(j) == 1 && cb_jobs_pending(j) == 2, "running %d, pending %d",
          cb_jobs_running(j), cb_jobs_pending(j));
    cb_jobs_cancel(j, waiting);
    CHECK(cb_jobs_pending(j) == 1, "pending %d after cancelling the waiting one",
          cb_jobs_pending(j));
    int got = 0;
    t0 = cb_time_us();
    while (cb_time_us() - t0 < 300 * 1000000LL) {
        CBJobResult *r = cb_jobs_wait(j, 100);
        if (!r) {
            if (!cb_jobs_pending(j))
                break;
            continue;
        }
        CHECK(r->request == running, "request %llu reported, the cancelled one is %llu",
              (unsigned long long)r->request, (unsigned long long)waiting);
        got++;
        cb_job_result_free(r);
    }
    CHECK(got == 1, "%d results", got);
    cb_jobs_destroy(j);
}

/* The tag batch with a model that does not open: the first track decodes
 * on a thread of its own while the model loads, and the batch still ends
 * with the one result that says so, nothing left running. */
static void test_tag_no_model(const char *wav)
{
    CBJobs *j = NULL;
    int ret = cb_jobs_create(&j, 1);
    CHECK(ret == 0, "cb_jobs_create: %s", strerror(-ret));
    if (ret < 0)
        return;
    for (uint64_t id = 1; id <= 3; id++)
        CHECK((int64_t)cb_jobs_tag(j, "/nonexistent/model.gguf", NULL, id, wav) > 0, "queued");
    CBJobResult *r = wait_result(j, 60);
    CHECK(r && r->kind == CB_JOB_TAG && !r->track_id && r->err < 0, "the model's failure");
    cb_job_result_free(r);
    int64_t t0 = cb_time_us();
    while (cb_jobs_tag_progress(j, NULL, NULL, NULL) && cb_time_us() - t0 < 10000000)
        usleep(1000);
    CHECK(!cb_jobs_tag_progress(j, NULL, NULL, NULL), "the batch has not ended");
    r = wait_result(j, 1);
    CHECK(!r, "a result after the failure");
    cb_job_result_free(r);
    cb_jobs_destroy(j);
}

static bool same_tags(const CBMLTags *a, const CBMLTags *b)
{
    return a && b && a->color == b->color && !memcmp(a->colors, b->colors, sizeof(a->colors)) &&
           a->intensity == b->intensity && a->club == b->club && a->valence == b->valence &&
           a->vocals == b->vocals &&
           !strcmp(a->comment ? a->comment : "", b->comment ? b->comment : "") &&
           !strcmp(a->genre ? a->genre : "", b->genre ? b->genre : "");
}

/* The tag batch with the model ($CYANBOX_ML_MODEL): a, a missing file, b
 * and a again, each decoded while the one before it is tagged. Each gets
 * its own audio: a's tags twice the same, and the same as cb_ml_tag's of
 * a decoded here (with the batch's model closed first: one model on a
 * GPU at a time). */
static int test_tag(const char *model, const char *a, const char *b)
{
    CBJobs *j = NULL;
    int ret = cb_jobs_create(&j, 1);
    CHECK(ret == 0, "cb_jobs_create: %s", strerror(-ret));
    if (ret < 0)
        return 1;
    cb_jobs_set_gpu(j, true, NULL);
    const char *paths[4] = { a, "/nonexistent/track.wav", b, a };
    CBMLTags *tags[4] = { 0 };
    for (int i = 0; i < 4; i++)
        CHECK((int64_t)cb_jobs_tag(j, model, NULL, (uint64_t)i + 1, paths[i]) > 0, "queued");
    for (int n = 0; n < 4; n++) {
        CBJobResult *r = wait_result(j, 300);
        CHECK(r && r->kind == CB_JOB_TAG, "a tag result");
        if (!r)
            break;
        if (!r->track_id) {
            fprintf(stderr, "the model does not open (%s): skipped\n", strerror(-r->err));
            cb_job_result_free(r);
            cb_jobs_destroy(j);
            return 77;
        }
        int i = (int)r->track_id - 1;
        CHECK(i >= 0 && i < 4 && (i == 1 ? r->err < 0 : r->err == 0 && r->tags),
              "track %d: %s", i + 1, strerror(-r->err));
        if (i >= 0 && i < 4 && r->tags) {
            tags[i] = r->tags;
            r->tags = NULL;
        }
        cb_job_result_free(r);
    }
    cb_jobs_destroy(j);

    CHECK(same_tags(tags[0], tags[3]), "a's tags differ from one time to the next");
    CHECK(tags[2] && !same_tags(tags[0], tags[2]), "b's tags are a's");

    CBML *ml = NULL;
    CBAudio audio = { 0 };
    CBMLTags direct = { 0 };
    ret = cb_ml_open(&ml, model, NULL);
    CHECK(ret == 0, "cb_ml_open: %s", strerror(-ret));
    if (ret == 0 && (ret = cb_decode(a, 1, CB_ML_RATE, &audio)) == 0) {
        const char *legend[CB_TRACK_COLORS];
        for (int i = 0; i < CB_TRACK_COLORS; i++)
            legend[i] = cb_config_vibe_default(i + 1);
        ret = cb_ml_tag(ml, audio.samples, audio.nb_frames, legend, &direct, NULL);
        CHECK(ret >= 0, "cb_ml_tag: %s", strerror(-ret));
        CHECK(same_tags(tags[0], &direct), "the batch's tags of a are not cb_ml_tag's");
    }
    cb_ml_tags_free(&direct);
    cb_audio_free(&audio);
    cb_ml_close(&ml);
    for (int i = 0; i < 4; i++) {
        if (tags[i])
            cb_ml_tags_free(tags[i]);
        free(tags[i]);
    }
    return failures ? 1 : 0;
}

int main(int argc, char **argv)
{
    if (argc == 4 && !strcmp(argv[1], "--tag")) {
        const char *model = getenv("CYANBOX_ML_MODEL");
        if (!model || access(model, R_OK)) {
            printf("no model ($CYANBOX_ML_MODEL): skipped\n");
            return 77;
        }
        int ret = test_tag(model, argv[2], argv[3]);
        printf("jobs, tagging: %s\n", ret == 77 ? "skipped" : ret ? "FAILED" : "OK");
        return ret;
    }
    if (argc < 2) {
        fprintf(stderr, "usage: test_jobs FILE.wav\n"
                        "       test_jobs --tag A.wav B.wav (with $CYANBOX_ML_MODEL)\n");
        return 2;
    }
    const char *wav = argv[1];

    test_analyse(wav);
    test_decode_and_artwork(wav);
    test_cancel(wav);
    test_budget(wav);
    test_tag_no_model(wav);

    if (failures) {
        fprintf(stderr, "%d failure%s\n", failures, failures == 1 ? "" : "s");
        return 1;
    }
    printf("jobs: OK\n");
    return 0;
}
