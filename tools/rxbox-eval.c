/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* rxbox-eval: runs the beat-grid analyser over audio files and measures
 * what can be measured without ground truth (docs/BEATS.md, "Evaluation").
 *
 *   rxbox-eval [options] FILE...
 *     -l, --list FILE         read paths from FILE (one per line)
 *     -j, --threads N         files analysed in parallel (default: cores / 2)
 *     --csv OUT               one row per file (the columns py_beats.py writes)
 *     --from CSV              do not analyse: read results from CSV
 *     --compare REF.csv       agreement statistics against another analyser's
 *                             CSV (dj's, from tests/analysis/py_beats.py)
 *     --dump-onsets BASE      for each file, BASE.<n>.onsets.csv (onset
 *                             signals) and BASE.<n>.beats.csv (beats,
 *                             attacks, grid)
 *     --bpm-range LO HI       restrict the tempo range
 *     --no-sections           one section only
 *     -v                      the analyser's diagnostics per file
 *
 *     --ffmpeg                decode with the ffmpeg command line instead of rx_decode
 *
 * Audio is decoded by rx_decode, mono at the file's own rate, as
 * rx_analyse sees it (the analyser resamples). With --ffmpeg it is piped
 * from the ffmpeg command line at 44.1 kHz ("file:" prefix, so names
 * containing ':' work), exactly as dj decodes for its analysis, so that a
 * comparison with dj can see the same samples. The BPM tag, when a file
 * has one, is read with rx_probe and printed. */

#include "rxbox/analysis.h"

#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "src/analysis/beats_internal.h"

/* ------------------------------------------------------------- decoding */

/* Runs argv with stdout to a pipe and reads all of it. */
static int run_capture(char *const argv[], uint8_t **out, size_t *size)
{
    int fd[2];
    if (pipe(fd) < 0)
        return RX_ERROR(errno);
    pid_t pid = fork();
    if (pid < 0) {
        close(fd[0]);
        close(fd[1]);
        return RX_ERROR(errno);
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
        ret = RX_ERROR(ENOMEM);
    else if (!WIFEXITED(st) || WEXITSTATUS(st))
        ret = RX_ERROR(EIO);
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
    if (rx_str_ends(path, ".f32")) {
        uint8_t *buf = NULL;
        size_t size = 0;
        int ret = rx_file_read(path, &buf, &size);
        if (ret < 0)
            return ret;
        *x = (float *)buf;
        *n = size / sizeof(float);
        return 0;
    }
    char *in = rx_asprintf("file:%s", path);
    if (!in)
        return RX_ERROR(ENOMEM);
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
    RxProbe p;
    if (rx_probe(path, &p) < 0)
        return 0.0;
    const char *v = rx_probe_tag(&p, "bpm", "tbpm", "tempo", NULL);
    double bpm = v ? atof(v) : 0.0;
    rx_probe_free(&p);
    return bpm;
}

/* ----------------------------------------------------------- results */

#define MAX_SECS 64

typedef struct Result {
    char  *path;
    bool   ok;
    double duration;
    int    nb_secs;
    RxTempoSection secs[MAX_SECS];
    double downbeat;            /* first downbeat >= 0 */
    double confidence;
    unsigned flags;
    char   flagstr[160];
    double first_onset;
    double ms;
    double tag_bpm;
    /* diagnostics */
    double octave_margin, phase_margin, kick_share, jitter, strong_on_grid;
    double full_on_grid, kick_delay, full_delay;
    double second_bpm;
    RxOctaveDecision oct;
    int    band;
    bool   shifted;
} Result;

static double grid_bpm(const Result *r)
{
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
    const RxTempoSection *s = &r->secs[sec_at(r, t)];
    double p = 60.0 / s->bpm;
    return s->start + round((t - s->start) / p) * p;
}

/* Downbeats: the count runs from the first section's start (beat 1)
 * through sections with beat 0 and restarts at explicit beat numbers. */
static double nearest_downbeat(const Result *r, double t)
{
    int si = sec_at(r, t);
    int num = r->secs[0].beat ? r->secs[0].beat : 1;
    for (int i = 0; i < si; i++) {
        double p = 60.0 / r->secs[i].bpm;
        int beats = (int)lrint((r->secs[i + 1].start - r->secs[i].start) / p);
        num = r->secs[i + 1].beat ? r->secs[i + 1].beat : ((num - 1 + beats) % 4 + 4) % 4 + 1;
    }
    const RxTempoSection *s = &r->secs[si];
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
        const char *n = rx_grid_flag_name((enum RxGridFlag)(1u << i));
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
    RxAnalysisOptions  opt;
    const char        *dump;
    bool               ffmpeg;
} Job;

static double cpu_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
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
static double locked_to_grid(const RxOnsets *o, const float *v, const Result *r, double *delay)
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

static void dump_files(const char *base, int idx, const RxBeatDebug *d, const Result *r)
{
    char *fn = rx_asprintf("%s.%d.onsets.csv", base, idx);
    FILE *f = fn ? fopen(fn, "w") : NULL;
    if (f) {
        fprintf(f, "time,full,low,mid,high,accent\n");
        for (int i = 0; i < d->ons.nb; i++)
            fprintf(f, "%.4f,%.3f,%.3f,%.3f,%.3f,%.3f\n", i / d->ons.fps, d->ons.full[i],
                    d->ons.low[i], d->ons.mid[i], d->ons.high[i], d->ons.accent[i]);
        fclose(f);
    }
    free(fn);
    fn = rx_asprintf("%s.%d.beats.csv", base, idx);
    f = fn ? fopen(fn, "w") : NULL;
    if (f) {
        fprintf(f, "index,tracked,attack,weight,kick,grid,residual_ms\n");
        for (int i = 0; i < d->nb_beats; i++) {
            const RxBeatObs *b = &d->beats[i];
            double g = nearest_beat(r, isnan(b->attack) ? b->time : b->attack);
            fprintf(f, "%d,%.5f,%.5f,%.4f,%.3f,%.5f,%.2f\n", b->index, b->time, b->attack,
                    b->weight, b->kick, g, isnan(b->attack) ? NAN : (b->attack - g) * 1e3);
        }
        fclose(f);
    }
    free(fn);
}

static void analyse_one(Job *job, int i)
{
    Result *r = &job->res[i];
    r->path = job->paths[i];
    float *x = NULL;
    int64_t n = 0;
    int rate = 44100;
    RxAudio au = { 0 };
    if (job->ffmpeg || rx_str_ends(r->path, ".f32")) {
        if (decode_ffmpeg(r->path, &x, &n) < 0) {
            fprintf(stderr, "%s: decoding failed\n", r->path);
            return;
        }
    } else {
        /* As rx_analyse sees it: mono at the file's own rate, on the
         * players' timeline; the analyser resamples. */
        if (rx_decode(r->path, 1, 0, &au) < 0) {
            fprintf(stderr, "%s: decoding failed\n", r->path);
            return;
        }
        x = au.samples;
        au.samples = NULL;
        n = au.nb_frames;
        rate = au.rate;
        rx_audio_free(&au);
    }
    r->duration = (double)n / rate;
    r->tag_bpm = rx_str_ends(r->path, ".f32") ? 0.0 : read_tag_bpm(r->path);

    RxBeatResult br;
    RxBeatDebug dbg;
    double t0 = cpu_ms();
    int ret = rx_analyse_beats_dbg(x, n, rate, &job->opt, &br, &dbg);
    r->ms = cpu_ms() - t0;
    free(x);
    if (ret < 0) {
        fprintf(stderr, "%s: analysis failed (%d)\n", r->path, ret);
        return;
    }
    r->ok = br.grid.nb_sections > 0;
    r->nb_secs = RX_MIN(br.grid.nb_sections, MAX_SECS);
    memcpy(r->secs, br.grid.sections, r->nb_secs * sizeof(*r->secs));
    r->confidence = br.confidence;
    r->flags = br.flags;
    flags_to_str(br.flags, r->flagstr, sizeof(r->flagstr));
    r->first_onset = br.first_onset;
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
        r->oct = dbg.octave;
        r->shifted = dbg.half_shift;
        r->band = dbg.band;
        r->strong_on_grid = locked_to_grid(&dbg.ons, dbg.ons.low, r, &r->kick_delay);
        r->full_on_grid = locked_to_grid(&dbg.ons, dbg.ons.full, r, &r->full_delay);
        if (job->dump)
            dump_files(job->dump, i, &dbg, r);
    }
    rx_beat_debug_free(&dbg);
    rx_beat_result_free(&br);
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

static void sections_str(const Result *r, char *buf, size_t size)
{
    buf[0] = 0;
    for (int i = 0; i < r->nb_secs; i++) {
        char tmp[64];
        snprintf(tmp, sizeof(tmp), "%s%.5f:%.4f:%d", i ? "|" : "", r->secs[i].start,
                 r->secs[i].bpm, r->secs[i].beat);
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

static void write_csv(const char *fn, const Result *res, int nb)
{
    FILE *f = fopen(fn, "w");
    if (!f) {
        fprintf(stderr, "%s: cannot write\n", fn);
        return;
    }
    fprintf(f, "path,duration,bpm,sections,downbeat,confidence,flags,first_onset,ms,tag_bpm\n");
    for (int i = 0; i < nb; i++) {
        const Result *r = &res[i];
        if (!r->ok)
            continue;
        char secs[MAX_SECS * 40];
        sections_str(r, secs, sizeof(secs));
        csv_quote(f, r->path);
        fprintf(f, ",%.3f,%.4f,%s,%.5f,%.3f,%s,%.3f,%.0f,", r->duration, grid_bpm(r), secs,
                r->downbeat, r->confidence, r->flagstr, r->first_onset, r->ms);
        if (r->tag_bpm > 0)
            fprintf(f, "%g", r->tag_bpm);
        fputc('\n', f);
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
        r->path = rx_strdup(fl[0]);
        r->duration = atof(fl[1]);
        char *sp = fl[3];
        while (*sp && r->nb_secs < MAX_SECS) {
            RxTempoSection *s = &r->secs[r->nb_secs++];
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
        r->first_onset = atof(fl[7]);
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
        printf("%-8.3f %-8.3f %-9s %-9s %s\n", ba, bb, ph, dn, rx_path_base(a->path));
    }
    printf("\ncompared %d files with %s\n", n, ref_fn);
    if (!n)
        return;
    printf("  bpm equal (0.1)        %3d  %5.1f %%\n", same, 100.0 * same / n);
    printf("  octave related         %3d  %5.1f %%\n", octave, 100.0 * octave / n);
    printf("  other                  %3d  %5.1f %%\n", other, 100.0 * other / n);
    printf("  phase within 15 ms     %3d  %5.1f %% (of bpm equal or octave)\n", phase,
           100.0 * phase / RX_MAX(same + octave, 1));
    printf("  downbeat within 15 ms  %3d  %5.1f %% (of bpm equal)\n", down15,
           100.0 * down15 / RX_MAX(same, 1));
    printf("  downbeat on a beat     %3d  %5.1f %% (of bpm equal; phase agrees)\n", down_beat,
           100.0 * down_beat / RX_MAX(same, 1));
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
    fprintf(stderr, "usage: rxbox-eval [-j N] [-l LIST] [--csv OUT] [--from CSV]\n"
                    "                  [--compare REF.csv]\n"
                    "                  [--dump-onsets BASE] [--bpm-range LO HI] [--no-sections]\n"
                    "                  [--ffmpeg] [-v|-vv] FILE...\n");
}

int main(int argc, char **argv)
{
    Job job = { 0 };
    rx_analysis_options_default(&job.opt);
    const char *csv = NULL, *from = NULL, *ref = NULL, *list = NULL;
    int threads = (int)RX_MAX(sysconf(_SC_NPROCESSORS_ONLN) / 2, 1);
    bool verbose = false;
    char **paths = malloc(argc * sizeof(*paths));
    int np = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if ((!strcmp(a, "-j") || !strcmp(a, "--threads")) && i + 1 < argc) {
            threads = atoi(argv[++i]);
            threads = RX_MAX(threads, 1);
        } else if ((!strcmp(a, "-l") || !strcmp(a, "--list")) && i + 1 < argc) {
            list = argv[++i];
        } else if (!strcmp(a, "--csv") && i + 1 < argc) {
            csv = argv[++i];
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
        } else if (!strcmp(a, "--no-sections")) {
            job.opt.allow_sections = false;
        } else if (!strcmp(a, "-v")) {
            verbose = true;
        } else if (!strcmp(a, "-vv")) {
            verbose = true;
            rx_log_set_level(RX_LOG_DEBUG);
        } else if (a[0] == '-' && a[1]) {
            usage();
            return 1;
        } else {
            paths[np++] = (char *)a;
        }
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
                paths[np++] = rx_strdup(line);
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
        threads = RX_MIN(threads, nb);
        pthread_t *th = calloc(threads, sizeof(*th));
        for (int i = 0; i < threads; i++)
            pthread_create(&th[i], NULL, worker, &job);
        for (int i = 0; i < threads; i++)
            pthread_join(th[i], NULL);
        free(th);
    }

    double tot_ms = 0.0, tot_dur = 0.0;
    int ok = 0;
    for (int i = 0; i < nb; i++) {
        const Result *r = &res[i];
        if (!r->ok) {
            printf("%-60s  no grid\n", r->path ? rx_path_base(r->path) : "?");
            continue;
        }
        ok++;
        tot_ms += r->ms;
        tot_dur += r->duration;
        char secs[256] = "";
        if (r->nb_secs > 1) {
            for (int s = 0; s < r->nb_secs && strlen(secs) < 200; s++) {
                char tmp[48];
                snprintf(tmp, sizeof(tmp), " %.2f@%.2f/%d", r->secs[s].start, r->secs[s].bpm,
                         r->secs[s].beat);
                strcat(secs, tmp);
            }
        }
        printf("%8.3f  db %7.3f  conf %.2f  %5.0f ms  %-30s", grid_bpm(r), r->downbeat,
               r->confidence, r->ms, r->flagstr);
        if (r->tag_bpm > 0)
            printf("  tag %g", r->tag_bpm);
        printf("  %s%s\n", rx_path_base(r->path), secs);
        if (!verbose || from)
            continue;
        printf("          2nd %.2f  octave x%g margin %.2f:", r->second_bpm, r->oct.factor,
               r->oct.margin);
        for (int l = 0; l < 3; l++)
            printf("  %.1f (snare %.2f/%.2f kick %.2f/%.2f)", r->oct.bpm[l],
                   r->oct.ev[l].balance, r->oct.ev[l].subdiv, r->oct.ev[l].kick_alt,
                   r->oct.ev[l].kick_sub);
        printf("\n          phase %.2f%s  kick %.2f  jit %.1f ms  band %d"
               "  locked: kick %.2f at %+.1f ms, full %.2f at %+.1f ms\n",
               r->phase_margin, r->shifted ? " (moved)" : "", r->kick_share, r->jitter * 1e3,
               r->band, r->strong_on_grid, r->kick_delay * 1e3, r->full_on_grid,
               r->full_delay * 1e3);
    }
    if (ok && !from) {
        double sog = 0.0, sfg = 0.0, conf = 0.0;
        int agree = 0;
        for (int i = 0; i < nb; i++) {
            if (!res[i].ok)
                continue;
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
    }
    if (csv)
        write_csv(csv, res, nb);
    if (ref)
        compare(res, nb, ref);
    return 0;
}
