/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* cyanbox-eval: runs the beat-grid analyser over audio files and measures
 * what can be measured without ground truth (docs/BEATS.md, "Evaluation").
 *
 *   cyanbox-eval [options] FILE...
 *     -l, --list FILE         read paths from FILE (one per line)
 *     -j, --threads N         files analysed in parallel (default: cores / 2)
 *     --csv OUT               one row per file (the columns py_beats.py writes)
 *     --from CSV              do not analyse: read results from CSV
 *     --compare REF.csv       agreement statistics against another analyser's
 *                             CSV (dj's, from tests/analysis/py_beats.py)
 *     --dump-onsets BASE      for each file, BASE.<n>.onsets.csv (onset
 *                             signals) and BASE.<n>.beats.csv (beats,
 *                             attacks, grid)
 *     --starts                only where the music starts (no beat analysis):
 *                             one line per file, for scanning a library
 *     --bpm-range LO HI       restrict the tempo range
 *     --no-sections           one section only
 *     --no-dynamic            the best straight sections everywhere (no
 *                             beat-by-beat grid)
 *     -v                      the analyser's diagnostics per file
 *
 *     --ffmpeg                decode with the ffmpeg command line instead of cb_decode
 *     --all                   run the whole cb_analyse (native channels, waveforms
 *                             and key too), as the app does; the CSV adds the
 *                             key, its confidence and tuning, and a hash of the
 *                             packed waveforms
 *     --what MASK             with --all: only these stages (1 beats, 2 key, 4 waveforms)
 *     --mem                   print resident memory after decoding and the peak
 *                             after the analysis (meaningful with -j 1)
 *
 *     --cache DIR             keep each file's decoded audio (16-bit mono 44.1 kHz)
 *                             and its front end's arrays in DIR, and use them on
 *                             the next run: only the decisions run again
 *     --device NAME           the GPU for the front end (a part of its name)
 *     --no-gpu                the front end on the CPU (as CYANBOX_ANALYSIS_GPU=0)
 *
 * The spectral front end runs on the GPU when there is one (cyanbox/compute.h;
 * CYANBOX_ANALYSIS_GPU=1 makes its absence an error). Each file's time is
 * split into decoding (or loading from the cache), the front end (the
 * STFT) and the decisions (everything after it, the envelopes around the
 * beats included), in wall-clock time, printed with -v and summed at the
 * end, with the cache's hits.
 *
 * Audio is decoded by cb_decode, mono at the file's own rate, as
 * cb_analyse sees it (the analyser resamples). With --ffmpeg it is piped
 * from the ffmpeg command line at 44.1 kHz ("file:" prefix, so names
 * containing ':' work), exactly as dj decodes for its analysis, so that a
 * comparison with dj can see the same samples. The BPM tag, when a file
 * has one, is read with cb_probe and printed.
 *
 * Where the music starts (docs/BEATS.md): the CSV and -v give its time and
 * kind (hit, fade, swell, at once), and the grid's first beat, its number,
 * its first downbeat and the automatic cue (the first downbeat at or after
 * the start), so that the start of the music can be checked against beat
 * 1. The CSV's last columns are the evidence the start's section had
 * before its on/off-beat decision, for tuning the witnesses without
 * analysing again. */

#include "cyanbox/analysis.h"

#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <linux/magic.h>

#include "cyanbox/compute.h"
#include "src/analysis/beats_internal.h"

/* ------------------------------------------------------------- decoding */

/* Runs argv with stdout to a pipe and reads all of it. */
static int run_capture(char *const argv[], uint8_t **out, size_t *size)
{
    int fd[2];
    if (pipe(fd) < 0)
        return CB_ERROR(errno);
    pid_t pid = fork();
    if (pid < 0) {
        close(fd[0]);
        close(fd[1]);
        return CB_ERROR(errno);
    }
    if (pid == 0) {
        dup2(fd[1], 1);
        close(fd[0]);
        close(fd[1]);
        execvp(argv[0], argv);
        _exit(127);
    }
    close(fd[1]);
    size_t cap = 1 << 20, len = 0;
    uint8_t *buf = malloc(cap);
    int ret = 0;
    while (buf) {
        if (len == cap) {
            uint8_t *nb = realloc(buf, cap * 2);
            if (!nb) {
                free(buf);
                buf = NULL;
                break;
            }
            buf = nb;
            cap *= 2;
        }
        ssize_t r = read(fd[0], buf + len, cap - len);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            break;
        len += r;
    }
    close(fd[0]);
    int st = 0;
    waitpid(pid, &st, 0);
    if (!buf)
        ret = CB_ERROR(ENOMEM);
    else if (!WIFEXITED(st) || WEXITSTATUS(st))
        ret = CB_ERROR(EIO);
    if (ret < 0) {
        free(buf);
        return ret;
    }
    *out = buf;
    *size = len;
    return 0;
}

static int decode_ffmpeg(const char *path, float **x, int64_t *n)
{
    /* Raw 44.1 kHz mono f32 (test_beats -w) is read as is. */
    if (cb_str_ends(path, ".f32")) {
        uint8_t *buf = NULL;
        size_t size = 0;
        int ret = cb_file_read(path, &buf, &size);
        if (ret < 0)
            return ret;
        *x = (float *)buf;
        *n = size / sizeof(float);
        return 0;
    }
    char *in = cb_asprintf("file:%s", path);
    if (!in)
        return CB_ERROR(ENOMEM);
    char *argv[] = { "ffmpeg", "-v", "error", "-nostdin", "-i", in, "-map", "0:a:0",
                     "-ac", "1", "-ar", "44100", "-f", "f32le", "-", NULL };
    uint8_t *buf = NULL;
    size_t size = 0;
    int ret = run_capture(argv, &buf, &size);
    free(in);
    if (ret < 0)
        return ret;
    *x = (float *)buf;
    *n = size / sizeof(float);
    return 0;
}

static double read_tag_bpm(const char *path)
{
    CBProbe p;
    if (cb_probe(path, &p) < 0)
        return 0.0;
    const char *v = cb_probe_tag(&p, "bpm", "tbpm", "tempo", NULL);
    double bpm = v ? atof(v) : 0.0;
    cb_probe_free(&p);
    return bpm;
}

/* ---------------------------------------------------------------- cache */

/* --cache DIR: per input file, the decoded mono 44.1 kHz audio as 16-bit
 * PCM and the spectral front end's arrays (what cb_spectrum_compute makes,
 * as floats, exactly), so that a second run over the same files skips
 * decoding and the transforms and runs only what comes after them. A file
 * is found by a hash of its path, size, modification time (ns), the
 * decoder (cb_decode or --ffmpeg) and CB_FRONTEND_VERSION, and its header
 * repeats them all: a changed file, another decoder or a new front end is
 * a miss, and the entry is written again. About 6 MB a minute of audio:
 * 5.3 MB of PCM and 0.7 MB of the front end's arrays (21 and 3 MB for a
 * four-minute track).
 *
 * A run that fills the cache analyses the 16-bit copy it stores, not the
 * float decode, so that every run with the cache sees the same samples and
 * an unchanged analyser gives the same results from the first run on. A
 * mono mix of 16-bit stereo has one more bit than that; the copy is within
 * -96 dB of it, which can move the results the way the GPU's rounding
 * does (docs/BEATS.md, section 1), no more. The envelopes around the
 * beats are still cut on a hit: where they are cut depends on the beats. */

#define CACHE_FORMAT 1

typedef struct CacheHead {
    char     magic[4];              /* "CBFC" */
    uint32_t format, frontend, decoder;
    int64_t  size, mtime_ns;
    int64_t  n;                     /* samples at 44.1 kHz */
    int32_t  nb_frames, path_len;
    double   duration, fps;
} CacheHead;

static bool file_id(const char *path, int64_t *size, int64_t *mtime_ns)
{
    struct stat st;
    if (stat(path, &st) < 0)
        return false;
    *size = st.st_size;
    *mtime_ns = (int64_t)st.st_mtim.tv_sec * 1000000000 + st.st_mtim.tv_nsec;
    return true;
}

static char *cache_name(const char *dir, const char *path, int64_t size, int64_t mtime_ns,
                        uint32_t decoder)
{
    /* FNV-1a over everything that identifies the entry. */
    uint64_t h = 0xcbf29ce484222325ULL;
    const uint32_t fe = CB_FRONTEND_VERSION;
    const struct { const void *p; size_t n; } parts[] = {
        { path, strlen(path) }, { &size, sizeof(size) }, { &mtime_ns, sizeof(mtime_ns) },
        { &decoder, sizeof(decoder) }, { &fe, sizeof(fe) },
    };
    for (size_t k = 0; k < CB_ARRAY_ELEMS(parts); k++) {
        const uint8_t *b = parts[k].p;
        for (size_t i = 0; i < parts[k].n; i++)
            h = (h ^ b[i]) * 0x100000001b3ULL;
    }
    return cb_asprintf("%s/%016llx.rxfc", dir, (unsigned long long)h);
}

/* The arrays of a spectrum, in the order they are stored, and their
 * lengths. */
#define NB_SPEC 5
static void spec_arrays(CBSpectrum *s, float **arr[NB_SPEC], size_t len[NB_SPEC])
{
    arr[0] = &s->prof;
    arr[1] = &s->kick;
    arr[2] = &s->flux_full;
    arr[3] = &s->flux_mid;
    arr[4] = &s->flux_high;
    const size_t nf = (size_t)s->nb_frames;
    len[0] = nf * CB_BEAT_NPROF;
    len[1] = len[2] = len[3] = len[4] = nf;
}

/* Loads the entry of `path`; false when there is none (or it is not
 * this file's, or damaged). */
static bool cache_load(const char *dir, const char *path, uint32_t decoder, float **x, int64_t *n,
                       double *duration, CBSpectrum *spec)
{
    int64_t size, mtime;
    if (!file_id(path, &size, &mtime))
        return false;
    char *name = cache_name(dir, path, size, mtime, decoder);
    uint8_t *blob = NULL;
    size_t blob_size = 0;
    int ret = name ? cb_file_read(name, &blob, &blob_size) : CB_ERROR(ENOMEM);
    free(name);
    if (ret < 0)
        return false;

    CacheHead h;
    bool ok = blob_size >= sizeof(h);
    if (ok) {
        memcpy(&h, blob, sizeof(h));
        ok = !memcmp(h.magic, "CBFC", 4) && h.format == CACHE_FORMAT &&
             h.frontend == CB_FRONTEND_VERSION && h.decoder == decoder && h.size == size &&
             h.mtime_ns == mtime && h.n >= 0 && h.nb_frames >= 0 && h.path_len >= 0 &&
             (size_t)h.path_len == strlen(path);
    }
    size_t off = sizeof(h) + (ok ? (size_t)h.path_len : 0);
    off = CB_ALIGN(off, 8);
    if (ok)
        ok = !memcmp(blob + sizeof(h), path, h.path_len);
    CBSpectrum s = { .nb_frames = ok ? h.nb_frames : 0 };
    float **arr[NB_SPEC];
    size_t len[NB_SPEC];
    spec_arrays(&s, arr, len);
    size_t need = off + CB_ALIGN((size_t)(ok ? h.n : 0) * sizeof(int16_t), 8);
    for (int k = 0; k < NB_SPEC; k++)
        need += len[k] * sizeof(float);
    need += sizeof(s.mel_hz);
    ok = ok && need == blob_size;

    float *xs = ok ? malloc((size_t)CB_MAX(h.n, 1) * sizeof(*xs)) : NULL;
    ok = ok && xs;
    for (int k = 0; ok && k < NB_SPEC; k++) {
        *arr[k] = malloc(CB_MAX(len[k], 1) * sizeof(float));
        ok = *arr[k] != NULL;
    }
    if (!ok) {
        free(xs);
        cb_spectrum_free(&s);
        free(blob);
        return false;
    }

    const int16_t *pcm = (const int16_t *)(blob + off);
    for (int64_t i = 0; i < h.n; i++)
        xs[i] = pcm[i] / 32768.0f;
    off += CB_ALIGN((size_t)h.n * sizeof(int16_t), 8);
    for (int k = 0; k < NB_SPEC; k++) {
        memcpy(*arr[k], blob + off, len[k] * sizeof(float));
        off += len[k] * sizeof(float);
    }
    memcpy(s.mel_hz, blob + off, sizeof(s.mel_hz));
    s.fps = h.fps;
    free(blob);

    *x = xs;
    *n = h.n;
    *duration = h.duration;
    *spec = s;
    return true;
}

/* Rounds the signal to 16 bits in place, as the cache stores it. */
static void quantize16(float *x, int64_t n)
{
    for (int64_t i = 0; i < n; i++) {
        long q = lrintf(x[i] * 32768.0f);
        x[i] = (int16_t)CB_CLAMP(q, -32768L, 32767L) / 32768.0f;
    }
}

static int cache_store(const char *dir, const char *path, uint32_t decoder, const float *x,
                       int64_t n, double duration, CBSpectrum *spec)
{
    int64_t size, mtime;
    if (!file_id(path, &size, &mtime))
        return CB_ERROR(ENOENT);
    CacheHead h = {
        .magic     = { 'R', 'X', 'F', 'C' },
        .format    = CACHE_FORMAT,
        .frontend  = CB_FRONTEND_VERSION,
        .decoder   = decoder,
        .size      = size,
        .mtime_ns  = mtime,
        .n         = n,
        .nb_frames = spec->nb_frames,
        .path_len  = (int32_t)strlen(path),
        .duration  = duration,
        .fps       = spec->fps,
    };
    float **arr[NB_SPEC];
    size_t len[NB_SPEC];
    spec_arrays(spec, arr, len);
    size_t off = CB_ALIGN(sizeof(h) + (size_t)h.path_len, 8);
    size_t total = off + CB_ALIGN((size_t)n * sizeof(int16_t), 8);
    for (int k = 0; k < NB_SPEC; k++)
        total += len[k] * sizeof(float);
    total += sizeof(spec->mel_hz);
    uint8_t *blob = calloc(1, total);
    if (!blob)
        return CB_ERROR(ENOMEM);

    memcpy(blob, &h, sizeof(h));
    memcpy(blob + sizeof(h), path, h.path_len);
    int16_t *pcm = (int16_t *)(blob + off);
    for (int64_t i = 0; i < n; i++)
        pcm[i] = (int16_t)CB_CLAMP(lrintf(x[i] * 32768.0f), -32768L, 32767L);
    off += CB_ALIGN((size_t)n * sizeof(int16_t), 8);
    for (int k = 0; k < NB_SPEC; k++) {
        memcpy(blob + off, *arr[k], len[k] * sizeof(float));
        off += len[k] * sizeof(float);
    }
    memcpy(blob + off, spec->mel_hz, sizeof(spec->mel_hz));

    char *name = cache_name(dir, path, size, mtime, decoder);
    int ret = name ? cb_file_write_atomic(name, blob, total) : CB_ERROR(ENOMEM);
    free(name);
    free(blob);
    return ret;
}

/* The cache is meant for a disk: on a tmpfs it costs RAM. */
static void cache_check_dir(const char *dir)
{
    struct statfs fs;
    if (statfs(dir, &fs) == 0 && fs.f_type == TMPFS_MAGIC)
        fprintf(stderr, "cyanbox-eval: %s is on a tmpfs: the cache takes RAM there (about 24 MB "
                        "per four-minute track)\n", dir);
}

/* ----------------------------------------------------------- results */

#define MAX_SECS 64

typedef struct Result {
    char  *path;
    bool   ok;
    double duration;
    int    nb_secs;
    CBTempoSection secs[MAX_SECS];  /* the straight sections, and one entry per
                                     * dynamic passage (measured, its first beat) */
    CBGrid grid;                    /* the grid itself (empty when read from a CSV) */
    int    nb_dynamic;              /* measured beats of the grid */
    double bpm_lo, bpm_hi;          /* their local tempos' range */
    double downbeat;            /* first downbeat >= 0 */
    double confidence;
    unsigned flags;
    char   flagstr[160];
    double music_start;
    double ms;                  /* CPU time of the front end and the decisions */
    double dec_ms, fe_ms, beats_ms;     /* wall time of decoding, the front end, the decisions */
    bool   cached;              /* decoded audio and front end from --cache */
    /* --all: the key and the waveforms (FNV-1a of cb_waveforms_pack's blob) */
    bool   has_key, has_waves;
    char   key[CB_KEY_MAX];
    double key_confidence, key_tuning;
    int    wave_columns;
    uint64_t wave_hash;
    double tag_bpm;
    double closest;             /* closest two beats of the expanded grid, in periods */
    /* diagnostics */
    double octave_margin, phase_margin, kick_share, jitter, strong_on_grid;
    double full_on_grid, kick_delay, full_delay, scatter;
    double second_bpm;
    CBOctaveDecision oct;
    int    band;
    bool   shifted;
    /* where the music starts, and the grid there */
    int    start_kind;
    double audible, floor_db, loud_db, jump_db, rise, start_kick_ratio;
    double first_beat, first_down, cue;
    int    first_number;
    int    start_section;
    double start_kick, start_struct, start_hit, start_off, start_t0, start_period;
    long   start_half_beat;
    double start_scores[8];
} Result;

static const char *kind_name(int k)
{
    return k == CB_START_HIT ? "hit" : k == CB_START_FADE ? "fade" :
           k == CB_START_SWELL ? "swell" : "once";
}

/* The grid's first beat and its number, its first downbeat, and the
 * automatic cue (cb_analysis_auto_cues' rule: the first downbeat at or
 * after the start of the music, 50 ms of slack). */
static void grid_start(Result *r, const CBGrid *g)
{
    r->first_beat = r->first_down = r->cue = NAN;
    r->first_number = 0;
    CBBeat *b = NULL;
    int nb = 0;
    if (cb_grid_beats(g, r->duration, &b, &nb) < 0 || !nb)
        return;
    r->first_beat = b[0].time;
    r->first_number = b[0].number;
    for (int i = 0; i < nb; i++) {
        if (b[i].number != 1)
            continue;
        if (isnan(r->first_down))
            r->first_down = b[i].time;
        if (b[i].time >= r->music_start - 0.05) {
            r->cue = b[i].time;
            break;
        }
    }
    free(b);
}

static double grid_bpm(const Result *r)
{
    if (r->grid.nb_sections)
        return cb_grid_bpm_median(&r->grid, r->duration);
    return r->nb_secs ? r->secs[0].bpm : 0.0;
}

/* The section containing t. */
static int sec_at(const Result *r, double t)
{
    int i = 0;
    while (i + 1 < r->nb_secs && t >= r->secs[i + 1].start)
        i++;
    return i;
}

/* Beat nearest to t on the grid. */
static double nearest_beat(const Result *r, double t)
{
    if (r->grid.nb_sections)
        return cb_grid_nearest_beat(&r->grid, t, NULL);
    const CBTempoSection *s = &r->secs[sec_at(r, t)];
    double p = 60.0 / s->bpm;
    return s->start + round((t - s->start) / p) * p;
}

/* Downbeats: the count runs from the first section's start (beat 1)
 * through sections with beat 0 and restarts at explicit beat numbers. */
static double nearest_downbeat(const Result *r, double t)
{
    if (r->grid.nb_sections) {
        /* The downbeat nearest t: a bar either side of t's bar. */
        int bar, beat;
        cb_grid_bar_of(&r->grid, t, &bar, &beat);
        double best = NAN;
        for (int b = CB_MAX(bar - 1, 1); b <= bar + 1; b++) {
            double d = cb_grid_bar_time(&r->grid, b, 1);
            if (isnan(best) || fabs(d - t) < fabs(best - t))
                best = d;
        }
        return best;
    }
    int si = sec_at(r, t);
    int num = r->secs[0].beat ? r->secs[0].beat : 1;
    for (int i = 0; i < si; i++) {
        double p = 60.0 / r->secs[i].bpm;
        int beats = (int)lrint((r->secs[i + 1].start - r->secs[i].start) / p);
        num = r->secs[i + 1].beat ? r->secs[i + 1].beat : ((num - 1 + beats) % 4 + 4) % 4 + 1;
    }
    const CBTempoSection *s = &r->secs[si];
    double p = 60.0 / s->bpm;
    double first_down = s->start + ((5 - num) % 4) * p;
    double bar = 4 * p;
    return first_down + round((t - first_down) / bar) * bar;
}

static void flags_to_str(unsigned f, char *buf, size_t size)
{
    buf[0] = 0;
    for (int i = 0; i < 16; i++) {
        if (!(f & (1u << i)))
            continue;
        const char *n = cb_grid_flag_name((enum CBGridFlag)(1u << i));
        if (!n || !*n)
            continue;
        if (buf[0])
            strncat(buf, "+", size - strlen(buf) - 1);
        strncat(buf, n, size - strlen(buf) - 1);
    }
}

/* ---------------------------------------------------------- analysing */

typedef struct Job {
    char             **paths;
    int                nb;
    Result            *res;
    atomic_int         next;
    CBAnalysisOptions  opt;
    const char        *dump;
    const char        *cache;
    bool               ffmpeg;
    bool               mem;
    bool               all;
    bool               starts;
    unsigned           what;
} Job;

static double cpu_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

/* The closest two beats of the grid as a player gets it (cb_grid_beats),
 * in periods of the faster side: under a half, two beats are one hit. */
static double closest_beats(const CBGrid *g, double duration)
{
    CBBeat *b = NULL;
    int nb = 0;
    if (cb_grid_beats(g, duration, &b, &nb) < 0)
        return NAN;
    double m = INFINITY;
    for (int i = 1; i < nb; i++)
        m = fmin(m, (b[i].time - b[i - 1].time) / (60.0 / fmax(b[i].bpm, b[i - 1].bpm)));
    free(b);
    return m;
}

static int cmp_dbl(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

/* Of the strongest onsets of v (the top tenth of its local maxima), the
 * share locked to the grid: within 15 ms of their median offset from the
 * nearest beat (a band's flux has its own delay; what matters is that the
 * hits keep one distance from the grid). The median offset is returned in
 * *delay: near half a beat means the grid is on that band's off-beat. */
static double locked_to_grid(const CBOnsets *o, const float *v, const Result *r, double *delay)
{
    int n = o->nb, np = 0;
    double *pk = malloc((size_t)n * sizeof(*pk));
    double *off = malloc((size_t)n * sizeof(*off));
    *delay = 0.0;
    if (!pk || !off) {
        free(pk);
        free(off);
        return 0.0;
    }
    for (int i = 1; i + 1 < n; i++) {
        if (v[i] > v[i - 1] && v[i] >= v[i + 1] && v[i] > 0.0f)
            pk[np++] = v[i];
    }
    double frac = 0.0;
    if (np >= 20) {
        qsort(pk, np, sizeof(*pk), cmp_dbl);
        double thr = pk[(int)(0.9 * (np - 1))];
        int m = 0;
        for (int i = 1; i + 1 < n; i++) {
            if (!(v[i] > v[i - 1] && v[i] >= v[i + 1] && v[i] >= thr))
                continue;
            double den = v[i - 1] - 2.0 * v[i] + v[i + 1];
            double d = den < 0.0 ? 0.5 * (v[i - 1] - v[i + 1]) / den : 0.0;
            double t = (i + d) / o->fps;
            off[m++] = t - nearest_beat(r, t);
        }
        memcpy(pk, off, m * sizeof(*pk));
        qsort(pk, m, sizeof(*pk), cmp_dbl);
        *delay = pk[m / 2];
        int ok = 0;
        for (int i = 0; i < m; i++)
            ok += fabs(off[i] - *delay) <= 0.015;
        frac = m ? (double)ok / m : 0.0;
    }
    free(pk);
    free(off);
    return frac;
}

static void dump_files(const char *base, int idx, const CBBeatDebug *d, const Result *r)
{
    char *fn = cb_asprintf("%s.%d.onsets.csv", base, idx);
    FILE *f = fn ? fopen(fn, "w") : NULL;
    if (f) {
        fprintf(f, "time,full,low,mid,high,accent\n");
        for (int i = 0; i < d->ons.nb; i++)
            fprintf(f, "%.4f,%.3f,%.3f,%.3f,%.3f,%.3f\n", i / d->ons.fps, d->ons.full[i],
                    d->ons.low[i], d->ons.mid[i], d->ons.high[i], d->ons.accent[i]);
        fclose(f);
    }
    free(fn);
    fn = cb_asprintf("%s.%d.beats.csv", base, idx);
    f = fn ? fopen(fn, "w") : NULL;
    if (f) {
        fprintf(f, "index,tracked,attack,weight,kick,grid,residual_ms\n");
        for (int i = 0; i < d->nb_beats; i++) {
            const CBBeatObs *b = &d->beats[i];
            double g = nearest_beat(r, isnan(b->attack) ? b->time : b->attack);
            fprintf(f, "%d,%.5f,%.5f,%.4f,%.3f,%.5f,%.2f\n", b->index, b->time, b->attack,
                    b->weight, b->kick, g, isnan(b->attack) ? NAN : (b->attack - g) * 1e3);
        }
        fclose(f);
    }
    free(fn);
}

/* Resident memory now, and the process's peak so far, in MB. */
static double rss_mb(void)
{
    long pages = 0, res = 0;
    FILE *f = fopen("/proc/self/statm", "r");
    if (f) {
        if (fscanf(f, "%ld %ld", &pages, &res) != 2)
            res = 0;
        fclose(f);
    }
    return res * (double)sysconf(_SC_PAGESIZE) / 1048576.0;
}

/* The peak resident size since the last peak_reset (VmHWM). */
static double peak_mb(void)
{
    char line[256];
    double kb = 0.0;
    FILE *f = fopen("/proc/self/status", "r");
    if (!f)
        return 0.0;
    while (fgets(line, sizeof(line), f)) {
        if (!strncmp(line, "VmHWM:", 6))
            kb = atof(line + 6);
    }
    fclose(f);
    return kb / 1024.0;
}

/* Resets the peak to the current size (Linux: clear_refs 5), so that the
 * analysis's own peak is not hidden under the decoder's. */
static void peak_reset(void)
{
    FILE *f = fopen("/proc/self/clear_refs", "w");
    if (f) {
        fputs("5", f);
        fclose(f);
    }
}

/* --all: the whole cb_analyse, as the app runs it (native channels;
 * waveforms and key too). Fills the beat result only. */
/* The grid into the result: the straight sections as they are, a dynamic
 * passage as one measured entry (its first beat) with the count and the
 * tempo range of the measured beats kept beside. */
static void take_grid(Result *r, const CBGrid *g)
{
    r->ok = g->nb_sections > 0;
    r->nb_secs = 0;
    r->nb_dynamic = 0;
    r->bpm_lo = INFINITY;
    r->bpm_hi = 0.0;
    for (int i = 0; i < g->nb_sections; i++) {
        const CBTempoSection *s = &g->sections[i];
        if (s->measured) {
            r->nb_dynamic++;
            r->bpm_lo = fmin(r->bpm_lo, s->bpm);
            r->bpm_hi = fmax(r->bpm_hi, s->bpm);
            if (i > 0 && g->sections[i - 1].measured)
                continue;
        }
        if (r->nb_secs < MAX_SECS)
            r->secs[r->nb_secs++] = *s;
    }
    if (!r->nb_dynamic)
        r->bpm_lo = r->bpm_hi = 0.0;
    cb_grid_copy(&r->grid, g);
}

static int analyse_all(Job *job, Result *r, CBBeatResult *br)
{
    CBAudio au = { 0 };
    if (cb_decode(r->path, 0, 0, &au) < 0) {
        fprintf(stderr, "%s: decoding failed\n", r->path);
        return -1;
    }
    r->duration = (double)au.nb_frames / au.rate;
    double peak0 = peak_mb();
    peak_reset();
    double rss0 = rss_mb();
    CBAnalysis an;
    double t0 = cpu_ms();
    int ret = cb_analyse(&au, &job->opt, job->what ? job->what : CB_ANALYSE_ALL, &an);
    r->ms = cpu_ms() - t0;
    cb_audio_free(&au);
    if (job->mem)
        printf("memory: decoded %.0f MB resident (peak %.0f), peak after analysis %.0f MB: "
               "analysis %.0f MB above its input  %s\n", rss0, peak0, peak_mb(),
               peak_mb() - rss0, cb_path_base(r->path));
    if (ret < 0) {
        fprintf(stderr, "%s: analysis failed (%d)\n", r->path, ret);
        cb_analysis_free(&an);
        return -1;
    }
    if (an.done & CB_ANALYSE_KEY) {
        r->has_key = true;
        snprintf(r->key, sizeof(r->key), "%s", an.key.key);
        r->key_confidence = an.key.confidence;
        r->key_tuning = an.key.tuning_cents;
    }
    uint8_t *blob = NULL;
    size_t size = 0;
    if ((an.done & CB_ANALYSE_WAVES) && cb_waveforms_pack(&an.waves, &blob, &size) >= 0) {
        r->has_waves = true;
        r->wave_columns = an.waves.nb_detail;
        r->wave_hash = 0xcbf29ce484222325ULL;
        for (size_t i = 0; i < size; i++)
            r->wave_hash = (r->wave_hash ^ blob[i]) * 0x100000001b3ULL;
        free(blob);
    }
    *br = an.beats;
    an.beats = (CBBeatResult) { 0 };
    cb_analysis_free(&an);
    return 0;
}

/* --starts: where the music starts, without the beat analysis. */
static void start_one(Job *job, Result *r, const float *x, int64_t n, int rate)
{
    (void)job;
    float *own = NULL;
    int64_t n44 = n;
    if (rate != CB_BEAT_RATE) {
        if (cb_resample_mono(x, n, rate, CB_BEAT_RATE, &own, &n44) < 0)
            return;
        x = own;
    }
    CBMusicStart st;
    double t0 = cpu_ms();
    if (cb_music_start(x, n44, &st) == 0) {
        r->ok = true;
        r->music_start = st.start;
        r->audible = st.audible;
        r->start_kind = st.kind;
        r->floor_db = st.floor_db;
        r->loud_db = st.loud_db;
        r->jump_db = st.jump_db;
        r->rise = st.rise;
    }
    r->ms = cpu_ms() - t0;
    free(own);
}

static void analyse_one(Job *job, int i)
{
    Result *r = &job->res[i];
    r->path = job->paths[i];
    r->start_section = -1;
    if (job->all) {
        CBBeatResult br;
        if (analyse_all(job, r, &br) < 0)
            return;
        take_grid(r, &br.grid);
        r->closest = closest_beats(&br.grid, r->duration);
        r->confidence = br.confidence;
        r->octave_margin = br.octave_margin;
        r->phase_margin = br.phase_margin;
        r->flags = br.flags;
        flags_to_str(br.flags, r->flagstr, sizeof(r->flagstr));
        r->music_start = br.music_start;
        r->start_kind = br.start_kind;
        grid_start(r, &br.grid);
        if (r->ok) {
            r->downbeat = nearest_downbeat(r, 0.0);
            double bar = 4 * 60.0 / r->secs[0].bpm;
            while (r->downbeat < 0.0)
                r->downbeat += bar;
        }
        cb_beat_result_free(&br);
        return;
    }
    float *x = NULL;
    int64_t n = 0;
    int rate = CB_BEAT_RATE;
    CBSpectrum spec = { 0 };
    const uint32_t decoder = job->ffmpeg || cb_str_ends(r->path, ".f32");
    int64_t t0 = cb_time_us();
    r->cached = !job->starts && job->cache &&
                cache_load(job->cache, r->path, decoder, &x, &n, &r->duration, &spec);
    if (!r->cached) {
        if (decoder) {
            if (decode_ffmpeg(r->path, &x, &n) < 0) {
                fprintf(stderr, "%s: decoding failed\n", r->path);
                return;
            }
        } else {
            /* As cb_analyse sees it: mono at the file's own rate, on the
             * players' timeline. */
            CBAudio au = { 0 };
            if (cb_decode(r->path, 1, 0, &au) < 0) {
                fprintf(stderr, "%s: decoding failed\n", r->path);
                return;
            }
            x = au.samples;
            au.samples = NULL;
            n = au.nb_frames;
            rate = au.rate;
            cb_audio_free(&au);
        }
        r->duration = (double)n / rate;
        if (job->starts) {
            start_one(job, r, x, n, rate);
            free(x);
            return;
        }
        /* At 44.1 kHz, resampled as the analyser itself would. */
        if (rate != CB_BEAT_RATE) {
            float *x44 = NULL;
            int64_t n44 = 0;
            int ret = cb_resample_mono(x, n, rate, CB_BEAT_RATE, &x44, &n44);
            free(x);
            if (ret < 0) {
                fprintf(stderr, "%s: resampling failed\n", r->path);
                return;
            }
            x = x44;
            n = n44;
        }
        if (job->cache)
            quantize16(x, n);
    }
    r->dec_ms = (cb_time_us() - t0) / 1000.0;
    r->tag_bpm = cb_str_ends(r->path, ".f32") ? 0.0 : read_tag_bpm(r->path);

    CBBeatResult br;
    CBBeatDebug dbg;
    double peak0 = peak_mb();
    peak_reset();
    double rss0 = rss_mb();
    double c0 = cpu_ms();
    if (!r->cached) {
        int64_t t1 = cb_time_us();
        int ret = cb_spectrum_compute(x, n, job->opt.compute, &spec);
        r->fe_ms = (cb_time_us() - t1) / 1000.0;
        if (ret < 0) {
            fprintf(stderr, "%s: the front end failed (%d)\n", r->path, ret);
            free(x);
            return;
        }
        if (job->cache && (ret = cache_store(job->cache, r->path, decoder, x, n, r->duration,
                                             &spec)) < 0)
            fprintf(stderr, "%s: not cached: %s\n", r->path, strerror(-ret));
    }
    int64_t t2 = cb_time_us();
    int ret = cb_analyse_beats_spec(x, n, &spec, &job->opt, &br, job->mem ? NULL : &dbg);
    r->beats_ms = (cb_time_us() - t2) / 1000.0;
    r->ms = cpu_ms() - c0;
    if (job->mem) {
        printf("memory: decoded %.0f MB resident (peak %.0f), peak after analysis %.0f MB: "
               "analysis %.0f MB above its input  %s\n", rss0, peak0, peak_mb(),
               peak_mb() - rss0, cb_path_base(r->path));
        memset(&dbg, 0, sizeof(dbg));
    }
    free(x);
    if (ret < 0) {
        fprintf(stderr, "%s: analysis failed (%d)\n", r->path, ret);
        return;
    }
    take_grid(r, &br.grid);
    r->closest = closest_beats(&br.grid, r->duration);
    r->confidence = br.confidence;
    r->flags = br.flags;
    flags_to_str(br.flags, r->flagstr, sizeof(r->flagstr));
    r->music_start = br.music_start;
    r->start_kind = br.start_kind;
    grid_start(r, &br.grid);
    if (!job->mem) {
        r->audible = dbg.start.audible;
        r->floor_db = dbg.start.floor_db;
        r->loud_db = dbg.start.loud_db;
        r->jump_db = dbg.start.jump_db;
        r->rise = dbg.start.rise;
        r->start_kick_ratio = dbg.start_kick_ratio;
        r->start_section = dbg.start_section;
        r->start_kick = dbg.start_kick;
        r->start_struct = dbg.start_struct;
        r->start_hit = dbg.start_hit;
        r->start_half_beat = dbg.start_half_beat;
        r->start_off = dbg.start_off;
        r->start_t0 = dbg.start_t0;
        r->start_period = dbg.start_period;
        memcpy(r->start_scores, dbg.start_scores, sizeof(r->start_scores));
    }
    r->octave_margin = br.octave_margin;
    r->phase_margin = br.phase_margin;
    r->second_bpm = br.bpm_second_best;
    if (r->ok) {
        r->downbeat = nearest_downbeat(r, 0.0);
        double bar = 4 * 60.0 / r->secs[0].bpm;
        while (r->downbeat < 0.0)
            r->downbeat += bar;
        r->kick_share = dbg.kick_share;
        r->jitter = dbg.jitter;
        r->scatter = dbg.scatter;
        r->oct = dbg.octave;
        r->shifted = dbg.half_shift;
        r->band = dbg.band;
        if (dbg.ons.nb) {
            r->strong_on_grid = locked_to_grid(&dbg.ons, dbg.ons.low, r, &r->kick_delay);
            r->full_on_grid = locked_to_grid(&dbg.ons, dbg.ons.full, r, &r->full_delay);
        }
        if (job->dump)
            dump_files(job->dump, i, &dbg, r);
    }
    cb_beat_debug_free(&dbg);
    cb_beat_result_free(&br);
}

static void *worker(void *arg)
{
    Job *job = arg;
    while (1) {
        int i = atomic_fetch_add(&job->next, 1);
        if (i >= job->nb)
            break;
        analyse_one(job, i);
    }
    return NULL;
}

/* ----------------------------------------------------------------- CSV */

/* A straight section as start:bpm:beat; a dynamic passage as
 * D:start:bpm:beat (its first beat), and the count and tempo range of
 * all measured beats go in their own columns. */
static void sections_str(const Result *r, char *buf, size_t size)
{
    buf[0] = 0;
    for (int i = 0; i < r->nb_secs; i++) {
        char tmp[64];
        snprintf(tmp, sizeof(tmp), "%s%s%.5f:%.4f:%d", i ? "|" : "",
                 r->secs[i].measured ? "D:" : "", r->secs[i].start, r->secs[i].bpm,
                 r->secs[i].beat);
        strncat(buf, tmp, size - strlen(buf) - 1);
    }
}

static void csv_quote(FILE *f, const char *s)
{
    if (!strpbrk(s, ",\"\n")) {
        fputs(s, f);
        return;
    }
    fputc('"', f);
    for (; *s; s++) {
        if (*s == '"')
            fputc('"', f);
        fputc(*s, f);
    }
    fputc('"', f);
}

/* --beats DIR: each file's grid as a player gets it (cb_grid_beats), one
 * beat a line (time, number in the bar, bpm), in DIR/NNNNN.txt by the
 * file's place in the input. */
static void write_beats(const char *dir, const Result *res, int nb)
{
    for (int i = 0; i < nb; i++) {
        const Result *r = &res[i];
        CBBeat *b = NULL;
        int n = 0;
        if (!r->grid.nb_sections || cb_grid_beats(&r->grid, r->duration, &b, &n) < 0)
            continue;
        char *fn = cb_asprintf("%s/%05d.txt", dir, i);
        FILE *f = fn ? fopen(fn, "w") : NULL;
        if (!f) {
            fprintf(stderr, "%s: cannot write\n", fn ? fn : dir);
        } else {
            for (int k = 0; k < n; k++)
                fprintf(f, "%.5f %d %.4f\n", b[k].time, b[k].number, b[k].bpm);
            fclose(f);
        }
        free(fn);
        free(b);
    }
}

static void write_csv(const char *fn, const Result *res, int nb)
{
    FILE *f = fopen(fn, "w");
    if (!f) {
        fprintf(stderr, "%s: cannot write\n", fn);
        return;
    }
    fprintf(f, "path,duration,bpm,sections,downbeat,confidence,flags,music_start,ms,tag_bpm,"
               "start_kind,audible,floor_db,loud_db,rise,first_beat,first_number,first_down,"
               "cue,start_section,start_kick,start_struct,start_hit,start_half_beat,start_off,"
               "start_t0,start_period,start_scores,jump_db,start_kick_ratio,dynamic_beats,"
               "dynamic_bpm_lo,dynamic_bpm_hi,key,key_confidence,key_tuning,wave_columns,"
               "wave_hash,phase_margin,octave_margin\n");
    for (int i = 0; i < nb; i++) {
        const Result *r = &res[i];
        if (!r->ok)
            continue;
        char secs[MAX_SECS * 40];
        sections_str(r, secs, sizeof(secs));
        csv_quote(f, r->path);
        fprintf(f, ",%.3f,%.4f,%s,%.5f,%.3f,%s,%.4f,%.0f,", r->duration, grid_bpm(r), secs,
                r->downbeat, r->confidence, r->flagstr, r->music_start, r->ms);
        if (r->tag_bpm > 0)
            fprintf(f, "%g", r->tag_bpm);
        fprintf(f, ",%s,%.4f,%.1f,%.1f,%.3f,%.5f,%d,%.5f,%.5f,%d,%.4f,%.4f,%.2f,%ld,%.5f,%.6f,%.7f,",
                kind_name(r->start_kind), r->audible, r->floor_db, r->loud_db, r->rise,
                r->first_beat, r->first_number, r->first_down, r->cue, r->start_section,
                r->start_kick, r->start_struct, r->start_hit, r->start_half_beat, r->start_off,
                r->start_t0, r->start_period);
        for (int k = 0; k < 8; k++)
            fprintf(f, "%s%.4f", k ? ";" : "", r->start_scores[k]);
        fprintf(f, ",%.1f,%.3f,%d,%.3f,%.3f,", r->jump_db, r->start_kick_ratio, r->nb_dynamic,
                r->bpm_lo, r->bpm_hi);
        /* To the last bit: a change to the key or the waveforms shows */
        if (r->has_key)
            fprintf(f, "%s,%.17g,%.17g", r->key, r->key_confidence, r->key_tuning);
        else
            fputs(",,", f);
        if (r->has_waves)
            fprintf(f, ",%d,%016llx", r->wave_columns, (unsigned long long)r->wave_hash);
        else
            fputs(",,", f);
        fprintf(f, ",%.4f,%.4f\n", r->phase_margin, r->octave_margin);
    }
    fclose(f);
}

/* Splits one CSV line in place (quotes honoured). Returns the field count. */
static int csv_split(char *line, char **fields, int max)
{
    int n = 0;
    char *p = line;
    while (n < max) {
        char *w = p;
        fields[n++] = w;
        if (*p == '"') {
            p++;
            while (*p) {
                if (*p == '"' && p[1] == '"') {
                    *w++ = '"';
                    p += 2;
                } else if (*p == '"') {
                    p++;
                    break;
                } else {
                    *w++ = *p++;
                }
            }
            while (*p && *p != ',')
                p++;
        } else {
            while (*p && *p != ',' && *p != '\n' && *p != '\r')
                *w++ = *p++;
        }
        bool more = *p == ',';
        if (*p)
            p++;
        *w = 0;
        if (!more)
            break;
    }
    return n;
}

static int read_csv(const char *fn, Result **out)
{
    FILE *f = fopen(fn, "r");
    if (!f) {
        fprintf(stderr, "%s: cannot read\n", fn);
        return -1;
    }
    char *line = NULL;
    size_t cap = 0;
    int nb = 0, cr = 0;
    Result *res = NULL;
    bool header = true;
    while (getline(&line, &cap, f) > 0) {
        if (header) {
            header = false;
            continue;
        }
        char *fl[12];
        int nf = csv_split(line, fl, 12);
        if (nf < 9)
            continue;
        if (nb == cr) {
            cr = cr ? 2 * cr : 64;
            Result *nr = realloc(res, cr * sizeof(*nr));
            if (!nr)
                break;
            res = nr;
        }
        Result *r = &res[nb++];
        memset(r, 0, sizeof(*r));
        r->path = cb_strdup(fl[0]);
        r->duration = atof(fl[1]);
        r->closest = INFINITY;
        char *sp = fl[3];
        while (*sp && r->nb_secs < MAX_SECS) {
            CBTempoSection *s = &r->secs[r->nb_secs++];
            if (sp[0] == 'D' && sp[1] == ':') {
                s->measured = true;
                sp += 2;
            }
            s->start = strtod(sp, &sp);
            if (*sp == ':')
                sp++;
            s->bpm = strtod(sp, &sp);
            if (*sp == ':')
                sp++;
            s->beat = (int)strtol(sp, &sp, 10);
            if (*sp == '|')
                sp++;
            else
                break;
        }
        r->ok = r->nb_secs > 0 && r->secs[0].bpm > 0;
        r->downbeat = atof(fl[4]);
        r->confidence = atof(fl[5]);
        snprintf(r->flagstr, sizeof(r->flagstr), "%s", fl[6]);
        r->music_start = atof(fl[7]);
        r->ms = atof(fl[8]);
        r->tag_bpm = nf > 9 ? atof(fl[9]) : 0.0;
    }
    free(line);
    fclose(f);
    *out = res;
    return nb;
}

/* --------------------------------------------------------------- compare */

static double circ(double d, double m)
{
    d = fmod(d, m);
    if (d > m / 2)
        d -= m;
    if (d < -m / 2)
        d += m;
    return d;
}

static void compare(const Result *res, int nb, const char *ref_fn)
{
    Result *ref = NULL;
    int nr = read_csv(ref_fn, &ref);
    if (nr < 0)
        return;
    int n = 0, same = 0, octave = 0, other = 0, phase = 0, down15 = 0, down_beat = 0;
    int tagged = 0, tag_ours = 0, tag_ref = 0;
    printf("\n%-8s %-8s %-9s %-9s %s\n", "ours", "ref", "phase_ms", "down", "path");
    for (int i = 0; i < nb; i++) {
        const Result *a = &res[i];
        if (!a->ok)
            continue;
        const Result *b = NULL;
        for (int j = 0; j < nr; j++) {
            if (ref[j].ok && !strcmp(ref[j].path, a->path)) {
                b = &ref[j];
                break;
            }
        }
        if (!b)
            continue;
        n++;
        double ba = grid_bpm(a), bb = grid_bpm(b);
        bool eq = fabs(ba - bb) <= 0.1;
        double ratio = ba / bb;
        bool oct = !eq && (fabs(ratio - 2.0) < 0.02 || fabs(ratio - 0.5) < 0.005);
        same += eq;
        octave += oct;
        other += !eq && !oct;
        if (a->tag_bpm > 0 || b->tag_bpm > 0) {
            double tag = a->tag_bpm > 0 ? a->tag_bpm : b->tag_bpm;
            tagged++;
            tag_ours += fabs(ba - tag) <= 0.5;
            tag_ref += fabs(bb - tag) <= 0.5;
        }
        /* Phase and downbeat compared in the middle of the track, where
         * both grids have settled. */
        double mid = a->duration / 2;
        char ph[32] = "-", dn[32] = "-";
        if (eq || oct) {
            double p = 60.0 / fmax(ba, bb);
            double d = circ(nearest_beat(a, mid) - nearest_beat(b, mid), p);
            bool okp = fabs(d) <= 0.015;
            phase += okp;
            snprintf(ph, sizeof(ph), "%+.1f", d * 1e3);
            if (eq) {
                double dd = nearest_downbeat(a, mid) - nearest_downbeat(b, mid);
                double bar = 4 * 60.0 / ba;
                dd = circ(dd, bar);
                down15 += fabs(dd) <= 0.015;
                double beats = dd / (60.0 / ba);
                bool whole = fabs(beats - round(beats)) * 60.0 / ba <= 0.015;
                down_beat += whole;
                snprintf(dn, sizeof(dn), "%+.2fb", beats);
            }
            double dd = circ(nearest_downbeat(a, mid) - nearest_downbeat(b, mid), 4 * 60.0 / ba);
            if (okp && eq && fabs(dd) <= 0.015)
                continue;
        }
        printf("%-8.3f %-8.3f %-9s %-9s %s\n", ba, bb, ph, dn, cb_path_base(a->path));
    }
    printf("\ncompared %d files with %s\n", n, ref_fn);
    if (!n)
        return;
    printf("  bpm equal (0.1)        %3d  %5.1f %%\n", same, 100.0 * same / n);
    printf("  octave related         %3d  %5.1f %%\n", octave, 100.0 * octave / n);
    printf("  other                  %3d  %5.1f %%\n", other, 100.0 * other / n);
    printf("  phase within 15 ms     %3d  %5.1f %% (of bpm equal or octave)\n", phase,
           100.0 * phase / CB_MAX(same + octave, 1));
    printf("  downbeat within 15 ms  %3d  %5.1f %% (of bpm equal)\n", down15,
           100.0 * down15 / CB_MAX(same, 1));
    printf("  downbeat on a beat     %3d  %5.1f %% (of bpm equal; phase agrees)\n", down_beat,
           100.0 * down_beat / CB_MAX(same, 1));
    if (tagged)
        printf("  BPM tag agreement      ours %d/%d  ref %d/%d\n", tag_ours, tagged, tag_ref,
               tagged);
    for (int j = 0; j < nr; j++)
        free(ref[j].path);
    free(ref);
}

/* ------------------------------------------------------------------ main */

static void usage(void)
{
    fprintf(stderr, "usage: cyanbox-eval [-j N] [-l LIST] [--csv OUT] [--beats DIR] [--from CSV]\n"
                    "                  [--compare REF.csv]\n"
                    "                  [--dump-onsets BASE] [--bpm-range LO HI] [--no-sections]\n"
                    "                  [--no-dynamic]\n"
                    "                  [--ffmpeg] [--all] [--what MASK] [--mem] [--starts]\n"
                    "                  [--cache DIR] [--device NAME] [--no-gpu]\n"
                    "                  [-v|-vv] FILE...\n");
}

int main(int argc, char **argv)
{
    Job job = { 0 };
    cb_analysis_options_default(&job.opt);
    const char *csv = NULL, *beats = NULL, *from = NULL, *ref = NULL, *list = NULL;
    const char *device = NULL;
    bool no_gpu = false;
    int threads = (int)CB_MAX(sysconf(_SC_NPROCESSORS_ONLN) / 2, 1);
    bool verbose = false;
    char **paths = malloc(argc * sizeof(*paths));
    int np = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if ((!strcmp(a, "-j") || !strcmp(a, "--threads")) && i + 1 < argc) {
            threads = atoi(argv[++i]);
            threads = CB_MAX(threads, 1);
        } else if ((!strcmp(a, "-l") || !strcmp(a, "--list")) && i + 1 < argc) {
            list = argv[++i];
        } else if (!strcmp(a, "--csv") && i + 1 < argc) {
            csv = argv[++i];
        } else if (!strcmp(a, "--beats") && i + 1 < argc) {
            beats = argv[++i];
        } else if (!strcmp(a, "--from") && i + 1 < argc) {
            from = argv[++i];
        } else if (!strcmp(a, "--compare") && i + 1 < argc) {
            ref = argv[++i];
        } else if (!strcmp(a, "--dump-onsets") && i + 1 < argc) {
            job.dump = argv[++i];
        } else if (!strcmp(a, "--bpm-range") && i + 2 < argc) {
            job.opt.bpm_min = atof(argv[++i]);
            job.opt.bpm_max = atof(argv[++i]);
        } else if (!strcmp(a, "--ffmpeg")) {
            job.ffmpeg = true;
        } else if (!strcmp(a, "--mem")) {
            job.mem = true;
        } else if (!strcmp(a, "--all")) {
            job.all = true;
        } else if (!strcmp(a, "--starts")) {
            job.starts = true;
        } else if (!strcmp(a, "--what") && i + 1 < argc) {
            job.all = true;
            job.what = (unsigned)atoi(argv[++i]);
        } else if (!strcmp(a, "--no-sections")) {
            job.opt.allow_sections = false;
        } else if (!strcmp(a, "--no-dynamic")) {
            job.opt.allow_dynamic = false;
        } else if (!strcmp(a, "--cache") && i + 1 < argc) {
            job.cache = argv[++i];
        } else if (!strcmp(a, "--device") && i + 1 < argc) {
            device = argv[++i];
        } else if (!strcmp(a, "--no-gpu")) {
            no_gpu = true;
        } else if (!strcmp(a, "-v")) {
            verbose = true;
        } else if (!strcmp(a, "-vv")) {
            verbose = true;
            cb_log_set_level(CB_LOG_DEBUG);
        } else if (a[0] == '-' && a[1]) {
            usage();
            return 1;
        } else {
            paths[np++] = (char *)a;
        }
    }

    /* The front end's GPU, shared by the threads. */
    CBCompute *gpu = NULL;
    if (!from && !job.starts && !no_gpu && cb_compute_mode() != CB_COMPUTE_OFF) {
        CBComputeOptions co;
        cb_compute_options_default(&co);
        co.device = device;
        int ret = cb_compute_create(&gpu, &co);
        if (ret < 0 && cb_compute_mode() == CB_COMPUTE_REQUIRED) {
            fprintf(stderr, "cyanbox-eval: CYANBOX_ANALYSIS_GPU=1, but there is no GPU (%s)\n",
                    strerror(-ret));
            return 1;
        }
    }
    job.opt.compute = gpu;
    if (job.cache) {
        if (cb_mkdir_p(job.cache) < 0) {
            fprintf(stderr, "%s: cannot create the cache directory\n", job.cache);
            return 1;
        }
        cache_check_dir(job.cache);
    }

    Result *res = NULL;
    int nb = 0;
    if (from) {
        nb = read_csv(from, &res);
        if (nb < 0)
            return 1;
    } else {
        if (list) {
            FILE *f = fopen(list, "r");
            if (!f) {
                fprintf(stderr, "%s: cannot read\n", list);
                return 1;
            }
            char *line = NULL;
            size_t cap = 0;
            ssize_t len;
            while ((len = getline(&line, &cap, f)) > 0) {
                while (len && (line[len - 1] == '\n' || line[len - 1] == '\r'))
                    line[--len] = 0;
                if (!len)
                    continue;
                char **pn = realloc(paths, (np + argc + 1) * sizeof(*pn));
                if (!pn)
                    return 1;
                paths = pn;
                paths[np++] = cb_strdup(line);
            }
            free(line);
            fclose(f);
        }
        if (!np) {
            usage();
            return 1;
        }
        nb = np;
        res = calloc(nb, sizeof(*res));
        job.paths = paths;
        job.nb = nb;
        job.res = res;
        atomic_init(&job.next, 0);
        threads = CB_MIN(threads, nb);
        pthread_t *th = calloc(threads, sizeof(*th));
        for (int i = 0; i < threads; i++)
            pthread_create(&th[i], NULL, worker, &job);
        for (int i = 0; i < threads; i++)
            pthread_join(th[i], NULL);
        free(th);
    }

    if (job.starts) {
        /* One line per file: kind, start, threshold crossing, floor, loud
         * level, jump, rise (tab-separated), path. */
        for (int i = 0; i < nb; i++) {
            const Result *r = &res[i];
            if (!r->ok) {
                printf("none\t-\t-\t-\t-\t-\t%s\n", r->path ? r->path : "?");
                continue;
            }
            printf("%s\t%.4f\t%.4f\t%.1f\t%.1f\t%.1f\t%.3f\t%s\n", kind_name(r->start_kind),
                   r->music_start, r->audible, r->floor_db, r->loud_db, r->jump_db, r->rise,
                   r->path);
        }
        return 0;
    }

    double tot_ms = 0.0, tot_dur = 0.0;
    int ok = 0;
    for (int i = 0; i < nb; i++) {
        const Result *r = &res[i];
        if (!r->ok) {
            printf("%-60s  no grid\n", r->path ? cb_path_base(r->path) : "?");
            continue;
        }
        ok++;
        tot_ms += r->ms;
        tot_dur += r->duration;
        char secs[256] = "";
        if (r->nb_secs > 1 || r->nb_dynamic) {
            for (int s = 0; s < r->nb_secs && strlen(secs) < 200; s++) {
                char tmp[48];
                snprintf(tmp, sizeof(tmp), " %s%.2f@%.2f/%d", r->secs[s].measured ? "D" : "",
                         r->secs[s].start, r->secs[s].bpm, r->secs[s].beat);
                strcat(secs, tmp);
            }
        }
        if (r->nb_dynamic) {
            char tmp[64];
            snprintf(tmp, sizeof(tmp), "  [%d measured beats, %.1f-%.1f]", r->nb_dynamic,
                     r->bpm_lo, r->bpm_hi);
            if (strlen(secs) + strlen(tmp) < sizeof(secs))
                strcat(secs, tmp);
        }
        printf("%8.3f  db %7.3f  conf %.2f  %5.0f ms  %-30s", grid_bpm(r), r->downbeat,
               r->confidence, r->ms, r->flagstr);
        if (r->tag_bpm > 0)
            printf("  tag %g", r->tag_bpm);
        printf("  %s%s%s\n", cb_path_base(r->path), secs,
               r->closest < 0.5 ? "  NEAR-DUPLICATE BEATS" : "");
        if (!verbose || from)
            continue;
        printf("          time: %s %.0f ms, front end %.0f ms, decisions %.0f ms (wall)\n",
               r->cached ? "from the cache" : "decoding", r->dec_ms, r->fe_ms, r->beats_ms);
        printf("          2nd %.2f  octave x%g margin %.2f:", r->second_bpm, r->oct.factor,
               r->oct.margin);
        for (int l = 0; l < 3; l++)
            printf("  %.1f (snare %.2f/%.2f kick %.2f/%.2f)", r->oct.bpm[l],
                   r->oct.ev[l].balance, r->oct.ev[l].subdiv, r->oct.ev[l].kick_alt,
                   r->oct.ev[l].kick_sub);
        printf("\n          by the bar:");
        for (int l = 0; l < 3; l++)
            printf("  %.1f (kick %.2f, snare %.2f lone %.2f)", r->oct.bpm[l],
                   r->oct.ev[l].kick_bar, r->oct.ev[l].snare, r->oct.ev[l].snare_lone);
        printf("\n          the levels' beats from %.4f %.4f %.4f at %.4f BPM, read on %.1f-%.1f s",
               r->oct.t0[0], r->oct.t0[1], r->oct.t0[2], r->oct.bpm[1], r->oct.from, r->oct.to);
        printf("\n          phase %.2f%s  kick %.2f  jit %.1f ms  scatter %.1f ms  band %d"
               "  locked: kick %.2f at %+.1f ms, full %.2f at %+.1f ms\n",
               r->phase_margin, r->shifted ? " (moved)" : "", r->kick_share, r->jitter * 1e3,
               r->scatter * 1e3, r->band, r->strong_on_grid, r->kick_delay * 1e3, r->full_on_grid,
               r->full_delay * 1e3);
        printf("          start %.4f (%s", r->music_start, kind_name(r->start_kind));
        if (r->start_kind != CB_START_AT_ONCE)
            printf(", over the threshold at %.4f, floor %.1f dB, loud %.1f dB, jump %.1f dB, "
                   "rise %.3f s", r->audible, r->floor_db, r->loud_db, r->jump_db, r->rise);
        printf(")  first beat %.4f (beat %d)  beat 1 %.4f  cue %.4f\n", r->first_beat,
               r->first_number, r->first_down, r->cue);
        if (r->start_section >= 0)
            printf("          start's section %d: kick %+.2f structure %+.2f hit %+.2f "
                   "(half beat %ld, %+.1f ms; kick %.2f of the kick beats')\n",
                   r->start_section, r->start_kick, r->start_struct, r->start_hit,
                   r->start_half_beat, r->start_off * 1e3, r->start_kick_ratio);
    }
    if (ok && !from) {
        double sog = 0.0, sfg = 0.0, conf = 0.0;
        int agree = 0, dups = 0;
        for (int i = 0; i < nb; i++) {
            if (!res[i].ok)
                continue;
            dups += res[i].closest < 0.5;
            sog += res[i].strong_on_grid;
            sfg += res[i].full_on_grid;
            conf += res[i].confidence;
            /* Kick band and full band put their hits at the same place of
             * the beat (within 25 ms of each other's delay). */
            agree += fabs(res[i].kick_delay - res[i].full_delay) <= 0.025;
        }
        printf("\n%d files, %.0f ms CPU per file, %.1f ms per minute of audio\n"
               "mean confidence %.2f; strong onsets locked to the grid: kick band %.1f %%, "
               "full band %.1f %%; kick and full band agree on %d (%.0f %%)\n",
               ok, tot_ms / ok, tot_ms / (tot_dur / 60.0), conf / ok, 100.0 * sog / ok,
               100.0 * sfg / ok, agree, 100.0 * agree / ok);
        int dyn = 0;
        for (int i = 0; i < nb; i++)
            dyn += res[i].ok && res[i].nb_dynamic > 0;
        printf("grids with two beats closer than half a period: %d; dynamic grids: %d\n", dups,
               dyn);
        double dec = 0.0, fe = 0.0, dm = 0.0;
        int hits = 0;
        for (int i = 0; i < nb; i++) {
            if (!res[i].ok)
                continue;
            dec += res[i].dec_ms;
            fe += res[i].fe_ms;
            dm += res[i].beats_ms;
            hits += res[i].cached;
        }
        printf("per file, wall: decoding (or the cache) %.0f ms, front end %.0f ms (%s), "
               "decisions %.0f ms\n",
               dec / ok, fe / ok, gpu ? cb_compute_device_name(gpu) : "CPU", dm / ok);
        if (job.cache)
            printf("cache %s: %d of %d files from it\n", job.cache, hits, ok);
    }
    if (csv)
        write_csv(csv, res, nb);
    if (beats)
        write_beats(beats, res, nb);
    if (ref)
        compare(res, nb, ref);
    if (gpu) {
        int errors = cb_compute_validation_errors(gpu);
        cb_compute_destroy(&gpu);
        if (errors) {
            fprintf(stderr, "cyanbox-eval: %d Vulkan validation errors\n", errors);
            return 1;
        }
    }
    return 0;
}
