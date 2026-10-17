/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* cyanbox-beatthis: Beat This!'s frame activations for audio files, for
 * working out what to make of them (cyanbox/beatthis.h).
 *
 *   cyanbox-beatthis -m MODEL -o DIR [options] [FILE]...
 *
 * Input i's logits go to DIR/NNNNN.npy (i in five digits, from 0): float32
 * of shape (2, frames), the beat's and the downbeat's, 50 frames a second,
 * as the reference's Audio2Frames returns them (numpy.load reads them).
 * Files are decoded on -j threads ahead of the GPU.
 *
 * Decoding: cyanbox's by default (cb_decode: the players' timeline, the
 * channels' mean, resampled to 22050 Hz); --ffmpeg as the reference's
 * harness does it (ffmpeg to 44.1 kHz stereo, the channels' mean,
 * resampled); a FILE ending in .f32 is raw mono float at 22050 Hz, as is. */

#include <errno.h>
#include <getopt.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "cyanbox/audio.h"
#include "cyanbox/beatthis.h"

static void usage(void)
{
    fprintf(stderr,
            "usage: cyanbox-beatthis -m MODEL -o DIR [options] [FILE]...\n"
            "  -m, --model PATH   the GGUF file (beat-this-f16.gguf)\n"
            "  -o, --out DIR      where NNNNN.npy go, one an input in order\n"
            "  -l, --list FILE    inputs a line, after those on the command line\n"
            "  -j, --jobs N       decoding threads (default 4)\n"
            "  -b, --batch N      chunks of 30 s through the network at once (default 4)\n"
            "  -d, --device NAME  the GPU\n"
            "      --ffmpeg       decode as the reference's harness: ffmpeg, 44.1 kHz stereo,\n"
            "                     the mean, resampled (default: cyanbox's decoder)\n"
            "  -v                 debug logging\n");
}

typedef struct Slot {
    float *pcm;
    int64_t n;
    int ret;
    bool ready;
    double dec_ms;
} Slot;

typedef struct Job {
    char **paths;
    int nb;
    bool ffmpeg;
    Slot *slots;
    int next, consumed, ahead;
    pthread_mutex_t lock;
    pthread_cond_t cond;
} Job;

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
        len += (size_t)r;
    }
    close(fd[0]);
    int st = 0;
    waitpid(pid, &st, 0);
    if (!buf)
        return CB_ERROR(ENOMEM);
    if (!WIFEXITED(st) || WEXITSTATUS(st)) {
        free(buf);
        return CB_ERROR(EIO);
    }
    *out = buf;
    *size = len;
    return 0;
}

static int decode_ffmpeg(const char *path, CBAudio *out)
{
    char *in = cb_asprintf("file:%s", path);
    if (!in)
        return CB_ERROR(ENOMEM);
    char *argv[] = { "ffmpeg", "-v", "error", "-nostdin", "-i", in, "-map", "0:a:0",
                     "-ac", "2", "-ar", "44100", "-f", "f32le", "-", NULL };
    uint8_t *buf = NULL;
    size_t size = 0;
    int ret = run_capture(argv, &buf, &size);
    free(in);
    if (ret < 0)
        return ret;
    float *x = (float *)buf;
    int64_t n = (int64_t)(size / (2 * sizeof(float)));
    /* the mean of the channels, in place */
    for (int64_t i = 0; i < n; i++)
        x[i] = (float)(((double)x[2 * i] + x[2 * i + 1]) * 0.5);
    CBAudio mono = { .samples = x, .channels = 1, .rate = 44100, .nb_frames = n };
    ret = cb_audio_resample(&mono, CB_BEATTHIS_RATE, out);
    free(buf);
    return ret;
}

static int decode(const Job *job, const char *path, float **pcm, int64_t *n)
{
    int ret;
    if (cb_str_ends(path, ".f32")) {
        uint8_t *buf = NULL;
        size_t size = 0;
        if ((ret = cb_file_read(path, &buf, &size)) < 0)
            return ret;
        *pcm = (float *)buf;
        *n = (int64_t)(size / sizeof(float));
        return 0;
    }
    CBAudio au = { 0 };
    ret = job->ffmpeg ? decode_ffmpeg(path, &au) : cb_decode(path, 1, CB_BEATTHIS_RATE, &au);
    if (ret < 0)
        return ret;
    *pcm = au.samples;
    *n = au.nb_frames;
    return 0;
}

static void *decoder(void *arg)
{
    Job *job = arg;
    while (1) {
        pthread_mutex_lock(&job->lock);
        while (job->next < job->nb && job->next >= job->consumed + job->ahead)
            pthread_cond_wait(&job->cond, &job->lock);
        if (job->next >= job->nb) {
            pthread_mutex_unlock(&job->lock);
            break;
        }
        int i = job->next++;
        pthread_mutex_unlock(&job->lock);

        Slot *s = &job->slots[i];
        int64_t t0 = cb_time_us();
        s->ret = decode(job, job->paths[i], &s->pcm, &s->n);
        s->dec_ms = (cb_time_us() - t0) / 1000.0;

        pthread_mutex_lock(&job->lock);
        s->ready = true;
        pthread_cond_broadcast(&job->cond);
        pthread_mutex_unlock(&job->lock);
    }
    return NULL;
}

/* numpy's format 1.0: float32, C order, shape (2, n). */
static int write_npy(const char *fn, const float *a, const float *b, int n)
{
    char head[128];
    int len = snprintf(head, sizeof(head),
                       "{'descr': '<f4', 'fortran_order': False, 'shape': (2, %d), }", n);
    int total = CB_ALIGN(10 + len + 1, 64);
    FILE *f = fopen(fn, "wb");
    if (!f)
        return CB_ERROR(errno);
    uint16_t hl = (uint16_t)(total - 10);
    fwrite("\x93NUMPY\x01\x00", 1, 8, f);
    fputc(hl & 0xff, f);
    fputc(hl >> 8, f);
    fwrite(head, 1, (size_t)len, f);
    for (int i = 10 + len; i < total - 1; i++)
        fputc(' ', f);
    fputc('\n', f);
    fwrite(a, sizeof(float), (size_t)n, f);
    fwrite(b, sizeof(float), (size_t)n, f);
    int ret = ferror(f) ? CB_ERROR(EIO) : 0;
    if (fclose(f) != 0 && !ret)
        ret = CB_ERROR(errno);
    return ret;
}

static int read_list(const char *fn, char ***paths, int *nb, int *cap)
{
    FILE *f = fopen(fn, "r");
    if (!f)
        return CB_ERROR(errno);
    char line[8192];
    while (fgets(line, sizeof(line), f)) {
        size_t l = strlen(line);
        while (l && (line[l - 1] == '\n' || line[l - 1] == '\r'))
            line[--l] = 0;
        if (!l || line[0] == '#')
            continue;
        if (*nb == *cap) {
            int nc = *cap ? *cap * 2 : 64;
            char **np = realloc(*paths, (size_t)nc * sizeof(*np));
            if (!np) {
                fclose(f);
                return CB_ERROR(ENOMEM);
            }
            *paths = np;
            *cap = nc;
        }
        if (!((*paths)[(*nb)++] = strdup(line))) {
            fclose(f);
            return CB_ERROR(ENOMEM);
        }
    }
    fclose(f);
    return 0;
}

int main(int argc, char **argv)
{
    static const struct option opts[] = {
        { "model",  required_argument, NULL, 'm' },
        { "out",    required_argument, NULL, 'o' },
        { "list",   required_argument, NULL, 'l' },
        { "jobs",   required_argument, NULL, 'j' },
        { "batch",  required_argument, NULL, 'b' },
        { "device", required_argument, NULL, 'd' },
        { "ffmpeg", no_argument,       NULL, 'F' },
        { NULL, 0, NULL, 0 },
    };
    const char *model = NULL, *out = NULL, *list = NULL;
    int jobs = 4;
    CBBeatThisOptions o;
    cb_beatthis_options_default(&o);
    Job job = { .ahead = 0 };
    int c;
    while ((c = getopt_long(argc, argv, "m:o:l:j:b:d:v", opts, NULL)) != -1) {
        switch (c) {
        case 'm': model = optarg; break;
        case 'o': out = optarg; break;
        case 'l': list = optarg; break;
        case 'j': jobs = CB_MAX(atoi(optarg), 1); break;
        case 'b': o.batch = atoi(optarg); break;
        case 'd': o.device = optarg; break;
        case 'F': job.ffmpeg = true; break;
        case 'v': cb_log_set_level(CB_LOG_DEBUG); break;
        default:
            usage();
            return 1;
        }
    }
    if (!model || !out) {
        usage();
        return 1;
    }
    int cap = 0;
    for (int i = optind; i < argc; i++) {
        if (job.nb == cap) {
            cap = cap ? cap * 2 : 64;
            job.paths = realloc(job.paths, (size_t)cap * sizeof(*job.paths));
            if (!job.paths)
                return 1;
        }
        job.paths[job.nb++] = strdup(argv[i]);
    }
    if (list && read_list(list, &job.paths, &job.nb, &cap) < 0) {
        fprintf(stderr, "%s: cannot read\n", list);
        return 1;
    }
    if (!job.nb) {
        usage();
        return 1;
    }
    if (mkdir(out, 0755) < 0 && errno != EEXIST) {
        fprintf(stderr, "%s: %s\n", out, strerror(errno));
        return 1;
    }

    CBBeatThis *bt = NULL;
    int ret = cb_beatthis_open(&bt, model, &o);
    if (ret < 0) {
        fprintf(stderr, "%s: cannot open the model: %s\n", model, strerror(-ret));
        return 1;
    }

    job.slots = calloc((size_t)job.nb, sizeof(*job.slots));
    job.ahead = jobs + 2;
    pthread_mutex_init(&job.lock, NULL);
    pthread_cond_init(&job.cond, NULL);
    pthread_t *th = calloc((size_t)jobs, sizeof(*th));
    for (int i = 0; i < jobs; i++)
        pthread_create(&th[i], NULL, decoder, &job);

    int64_t t0 = cb_time_us();
    double audio_s = 0.0, dec_ms = 0.0, mel_ms = 0.0, net_ms = 0.0;
    int failed = 0;
    for (int i = 0; i < job.nb; i++) {
        Slot *s = &job.slots[i];
        pthread_mutex_lock(&job.lock);
        while (!s->ready)
            pthread_cond_wait(&job.cond, &job.lock);
        pthread_mutex_unlock(&job.lock);

        if (s->ret < 0) {
            fprintf(stderr, "%s: decoding failed: %s\n", job.paths[i], strerror(-s->ret));
            failed++;
        } else {
            float *beat = NULL, *down = NULL;
            int nf = 0;
            ret = cb_beatthis_run(bt, s->pcm, s->n, &beat, &down, &nf);
            if (ret < 0) {
                fprintf(stderr, "%s: the model failed: %s\n", job.paths[i], strerror(-ret));
                failed++;
            } else {
                char *fn = cb_asprintf("%s/%05d.npy", out, i);
                if (!fn || write_npy(fn, beat, down, nf) < 0) {
                    fprintf(stderr, "%s: cannot write\n", fn ? fn : out);
                    failed++;
                }
                free(fn);
                double m, nt;
                cb_beatthis_times(bt, &m, &nt);
                audio_s += (double)s->n / CB_BEATTHIS_RATE;
                dec_ms += s->dec_ms;
                mel_ms += m;
                net_ms += nt;
                fprintf(stderr,
                        "%d/%d %.1f s: decoded %.0f ms, spectrogram %.0f ms, network %.0f ms  %s\n",
                        i + 1, job.nb, (double)s->n / CB_BEATTHIS_RATE, s->dec_ms, m, nt,
                        cb_path_base(job.paths[i]));
            }
            free(beat);
            free(down);
        }
        free(s->pcm);
        s->pcm = NULL;
        pthread_mutex_lock(&job.lock);
        job.consumed = i + 1;
        pthread_cond_broadcast(&job.cond);
        pthread_mutex_unlock(&job.lock);
    }
    for (int i = 0; i < jobs; i++)
        pthread_join(th[i], NULL);
    double wall = (cb_time_us() - t0) / 1e6;
    printf("%d files, %.1f min of audio in %.1f s on %s: per minute of audio, decoding %.0f ms "
           "(on %d threads), spectrogram %.1f ms, network %.1f ms\n",
           job.nb - failed, audio_s / 60, wall, cb_beatthis_device_name(bt),
           dec_ms / (audio_s / 60), jobs, mel_ms / (audio_s / 60), net_ms / (audio_s / 60));

    cb_beatthis_close(&bt);
    for (int i = 0; i < job.nb; i++)
        free(job.paths[i]);
    free(job.paths);
    free(job.slots);
    free(th);
    return failed ? 1 : 0;
}
