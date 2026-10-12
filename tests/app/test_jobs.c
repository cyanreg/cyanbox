/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* The background jobs (rxbox/jobs.h) on a small WAV the ffmpeg command line
 * made (12 s, 48 kHz stereo, a 128 BPM kick): an analyse round trip with
 * the active track ids, a decode at the file's own rate and channels, an
 * artwork job on a file without art, a missing file, cancelling queued
 * requests, destroying a queue with work still in it, and the memory
 * guard: with a budget smaller than any analysis, two analyses on two
 * workers run one after the other (and both run), one waiting for memory
 * can be cancelled, and with the default budget the same two run at once.
 *
 * Usage: test_jobs FILE.wav */

#include "rxbox/jobs.h"

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

static RxJobResult *wait_result(RxJobs *j, int timeout_s)
{
    return rx_jobs_wait(j, timeout_s * 1000);
}

static void test_analyse(const char *wav)
{
    RxJobs *j = NULL;
    int ret = rx_jobs_create(&j, 2);
    CHECK(ret == 0, "rx_jobs_create: %s", strerror(-ret));
    if (ret < 0)
        return;

    RxAnalysisOptions opt;
    rx_analysis_options_default(&opt);
    uint64_t req = rx_jobs_analyse(j, 42, wav, &opt, RX_ANALYSE_ALL);
    CHECK((int64_t)req > 0, "request %lld", (long long)req);
    CHECK(rx_jobs_pending(j) == 1, "pending %d", rx_jobs_pending(j));

    /* While it runs, worker reports the track it analyses. */
    bool seen_active = false;
    RxJobResult *r = NULL;
    int64_t t0 = rx_time_us();
    while (!(r = rx_jobs_poll(j)) && rx_time_us() - t0 < 300 * 1000000LL) {
        uint64_t ids[4];
        int nb = rx_jobs_active_tracks(j, ids, 4);
        if (nb == 1 && ids[0] == 42)
            seen_active = true;
        usleep(500);
    }
    CHECK(r, "no analysis result");
    CHECK(seen_active, "the analysed track never showed as active");
    if (!r)
        goto end;

    CHECK(r->kind == RX_JOB_ANALYSE, "kind %d", r->kind);
    CHECK(r->track_id == 42, "track %llu", (unsigned long long)r->track_id);
    CHECK(r->request == req, "request %llu", (unsigned long long)r->request);
    CHECK(r->err == 0, "err %s", strerror(-r->err));
    CHECK(r->seconds > 0, "seconds %f", r->seconds);
    CHECK(r->analysis, "no analysis");
    if (r->analysis) {
        const RxAnalysis *a = r->analysis;
        CHECK(fabs(a->duration - 12.0) < 1e-6, "duration %f", a->duration);
        CHECK(a->done & RX_ANALYSE_WAVES, "done %#x", a->done);
        CHECK(abs(a->waves.nb_detail - 12 * RX_WAVE_COLUMNS_PER_SECOND) <= 2,
              "%d detail columns", a->waves.nb_detail);
        if (a->done & RX_ANALYSE_BEATS)
            printf("analysis: %.3f BPM, confidence %.2f, flags %#x\n",
                   rx_grid_bpm(&a->beats.grid), a->beats.confidence, a->beats.flags);
        if (a->done & RX_ANALYSE_KEY)
            printf("analysis: key \"%s\", confidence %.2f\n", a->key.key, a->key.confidence);
        printf("analysis: done %#x in %.2f s\n", a->done, r->seconds);
    }
    rx_job_result_free(r);

    CHECK(rx_jobs_pending(j) == 0, "pending %d", rx_jobs_pending(j));
    CHECK(rx_jobs_running(j) == 0, "running %d", rx_jobs_running(j));
    uint64_t ids[4];
    CHECK(rx_jobs_active_tracks(j, ids, 4) == 0, "tracks still active");

end:
    rx_jobs_destroy(j);
}

static void test_decode_and_artwork(const char *wav)
{
    RxJobs *j = NULL;
    int ret = rx_jobs_create(&j, 2);
    CHECK(ret == 0, "rx_jobs_create: %s", strerror(-ret));
    if (ret < 0)
        return;

    uint64_t dec = rx_jobs_decode(j, 7, wav, 0);
    CHECK((int64_t)dec > 0, "decode request %lld", (long long)dec);
    RxJobResult *r = wait_result(j, 60);
    CHECK(r, "no decode result");
    if (r) {
        CHECK(r->kind == RX_JOB_DECODE && r->request == dec && r->track_id == 7 && r->deck == 0,
              "kind %d request %llu deck %d", r->kind, (unsigned long long)r->request, r->deck);
        CHECK(r->err == 0 && r->audio, "err %s", strerror(-r->err));
        if (r->audio) {
            /* The file's own samples: no resampling, no downmix. */
            const RxAudio *a = r->audio;
            CHECK(a->rate == 48000, "rate %d", a->rate);
            CHECK(a->channels == 2, "channels %d", a->channels);
            CHECK(a->nb_frames == 12 * 48000, "%lld frames", (long long)a->nb_frames);
            float peak = 0;
            for (int64_t i = 0; i < a->nb_frames * a->channels; i++)
                peak = fmaxf(peak, fabsf(a->samples[i]));
            CHECK(peak > 0.5f && peak <= 1.0f, "peak %f", peak);
        }
        rx_job_result_free(r);
    }

    /* A WAV has no cover and no cover file sits next to it. */
    uint64_t art = rx_jobs_artwork(j, 7, wav, 64);
    CHECK((int64_t)art > 0, "artwork request %lld", (long long)art);
    r = wait_result(j, 30);
    CHECK(r, "no artwork result");
    if (r) {
        CHECK(r->kind == RX_JOB_ARTWORK && r->request == art, "kind %d", r->kind);
        CHECK(r->err == RX_ERROR(ENOENT), "err %d (%s)", r->err, strerror(-r->err));
        CHECK(!r->rgba && !r->jpeg, "art from nowhere");
        rx_job_result_free(r);
    }

    /* A missing file is an error result, not silence. */
    uint64_t miss = rx_jobs_analyse(j, 9, "/nonexistent/rxbox/track.flac", NULL, RX_ANALYSE_ALL);
    r = wait_result(j, 30);
    CHECK(r && r->request == miss && r->err < 0 && !r->analysis, "missing file result");
    rx_job_result_free(r);

    CHECK((int64_t)rx_jobs_analyse(j, 9, wav, NULL, 0) < 0, "nothing to analyse accepted");
    rx_jobs_destroy(j);
}

static void test_cancel(const char *wav)
{
    RxJobs *j = NULL;
    int ret = rx_jobs_create(&j, 1);
    CHECK(ret == 0, "rx_jobs_create: %s", strerror(-ret));
    if (ret < 0)
        return;

    /* The one worker is busy with the analysis, so the decodes queue. */
    uint64_t first = rx_jobs_analyse(j, 1, wav, NULL, RX_ANALYSE_ALL);
    int64_t t0 = rx_time_us();
    while (!rx_jobs_running(j) && rx_time_us() - t0 < 10 * 1000000LL)
        usleep(100);
    uint64_t r2 = rx_jobs_decode(j, 2, wav, 0);
    uint64_t r3 = rx_jobs_decode(j, 3, wav, 0);
    uint64_t r4 = rx_jobs_decode(j, 4, wav, 0);
    rx_jobs_cancel(j, r2);
    rx_jobs_cancel(j, r3);
    rx_jobs_cancel(j, r3);              /* twice is harmless */
    rx_jobs_cancel(j, first);           /* running: finishes and is reported */
    CHECK(rx_jobs_pending(j) == 2, "pending %d after cancelling two", rx_jobs_pending(j));

    bool got_first = false, got_r4 = false;
    t0 = rx_time_us();
    while (rx_time_us() - t0 < 300 * 1000000LL) {
        RxJobResult *r = rx_jobs_wait(j, 100);
        if (!r) {
            if (!rx_jobs_pending(j))
                break;
            continue;
        }
        CHECK(r->request != r2 && r->request != r3, "a cancelled request (%llu) ran",
              (unsigned long long)r->request);
        got_first |= r->request == first;
        got_r4 |= r->request == r4;
        rx_job_result_free(r);
    }
    CHECK(got_first, "the running job was not reported");
    CHECK(got_r4, "the job after the cancelled ones did not run");
    CHECK(rx_jobs_pending(j) == 0, "pending %d", rx_jobs_pending(j));

    /* Request ids keep increasing. */
    uint64_t r5 = rx_jobs_decode(j, 5, wav, 0);
    CHECK(r5 > r4, "request %llu after %llu", (unsigned long long)r5, (unsigned long long)r4);
    rx_jobs_cancel_all(j);
    rx_jobs_destroy(j);

    /* Destroying with work queued: the queued jobs are dropped, the running
     * one finishes, nothing leaks into a hang. */
    ret = rx_jobs_create(&j, 1);
    CHECK(ret == 0, "rx_jobs_create: %s", strerror(-ret));
    if (ret < 0)
        return;
    for (int i = 0; i < 6; i++)
        rx_jobs_analyse(j, 10 + i, wav, NULL, RX_ANALYSE_ALL);
    t0 = rx_time_us();
    rx_jobs_destroy(j);
    double took = (rx_time_us() - t0) / 1e6;
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
        setenv("RXBOX_ANALYSIS_BUDGET_MB", budget_mb, 1);
    else
        unsetenv("RXBOX_ANALYSIS_BUDGET_MB");
    RxJobs *j = NULL;
    int ret = rx_jobs_create(&j, 2);
    unsetenv("RXBOX_ANALYSIS_BUDGET_MB");
    CHECK(ret == 0, "rx_jobs_create: %s", strerror(-ret));
    if (ret < 0)
        return true;

    const uint64_t ids[2] = { 101, 102 };
    for (int i = 0; i < 2; i++)
        CHECK((int64_t)rx_jobs_analyse(j, ids[i], wav, NULL, RX_ANALYSE_ALL) > 0, "request %d", i);

    int got = 0, max_running = 0;
    bool together = false;
    int64_t first[2] = { 0 }, last[2] = { 0 };
    int64_t t0 = rx_time_us();
    while (got < 2 && rx_time_us() - t0 < 300 * 1000000LL) {
        RxJobResult *r = rx_jobs_poll(j);
        if (r) {
            CHECK(r->err == 0 && r->analysis, "track %llu: %s", (unsigned long long)r->track_id,
                  strerror(-r->err));
            rx_job_result_free(r);
            got++;
            continue;
        }
        max_running = RX_MAX(max_running, rx_jobs_running(j));
        uint64_t active[4];
        int nb = rx_jobs_active_tracks(j, active, 4);
        int64_t now = rx_time_us();
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
                   (RX_MIN(last[0], last[1]) - t0) / 1e6, (RX_MAX(last[0], last[1]) - t0) / 1e6);
    }
    rx_jobs_destroy(j);
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
    setenv("RXBOX_ANALYSIS_BUDGET_MB", "1", 1);
    RxJobs *j = NULL;
    int ret = rx_jobs_create(&j, 2);
    unsetenv("RXBOX_ANALYSIS_BUDGET_MB");
    CHECK(ret == 0, "rx_jobs_create: %s", strerror(-ret));
    if (ret < 0)
        return;
    uint64_t req[2] = {
        rx_jobs_analyse(j, 201, wav, NULL, RX_ANALYSE_ALL),
        rx_jobs_analyse(j, 202, wav, NULL, RX_ANALYSE_ALL),
    };
    uint64_t active = 0;
    int64_t t0 = rx_time_us();
    while (!active && rx_time_us() - t0 < 30 * 1000000LL) {
        if (rx_jobs_active_tracks(j, &active, 1) != 1)
            active = 0;
        usleep(100);
    }
    CHECK(active == 201 || active == 202, "no analysis started");
    uint64_t running = active == 202 ? req[1] : req[0];
    uint64_t waiting = active == 202 ? req[0] : req[1];
    CHECK(rx_jobs_running(j) == 1 && rx_jobs_pending(j) == 2, "running %d, pending %d",
          rx_jobs_running(j), rx_jobs_pending(j));
    rx_jobs_cancel(j, waiting);
    CHECK(rx_jobs_pending(j) == 1, "pending %d after cancelling the waiting one",
          rx_jobs_pending(j));
    int got = 0;
    t0 = rx_time_us();
    while (rx_time_us() - t0 < 300 * 1000000LL) {
        RxJobResult *r = rx_jobs_wait(j, 100);
        if (!r) {
            if (!rx_jobs_pending(j))
                break;
            continue;
        }
        CHECK(r->request == running, "request %llu reported, the cancelled one is %llu",
              (unsigned long long)r->request, (unsigned long long)waiting);
        got++;
        rx_job_result_free(r);
    }
    CHECK(got == 1, "%d results", got);
    rx_jobs_destroy(j);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: test_jobs FILE.wav\n");
        return 2;
    }
    const char *wav = argv[1];

    test_analyse(wav);
    test_decode_and_artwork(wav);
    test_cancel(wav);
    test_budget(wav);

    if (failures) {
        fprintf(stderr, "%d failure%s\n", failures, failures == 1 ? "" : "s");
        return 1;
    }
    printf("jobs: OK\n");
    return 0;
}
