/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* rxbox-ml: asks MOSS-Audio about audio files, for trying prompts out.
 *
 *   rxbox-ml -m MODEL [-q QUESTION]... [-c QUESTION]... [options] [FILE]...
 *
 * Each file is decoded to 16 kHz mono, listened to once, and every -q is
 * asked of it (greedy answers); every -c is asked for a digit, and the
 * probabilities of 0 to 9 and their mean are printed. With no file, the
 * questions are asked without audio. */

#include <getopt.h>
#include <math.h>
#include <stdio.h>

#include "rxbox/audio.h"
#include "rxbox/ml.h"

static void usage(void)
{
    fprintf(stderr,
            "usage: rxbox-ml -m MODEL [options] [FILE]...\n"
            "  -m, --model PATH      the GGUF file\n"
            "  -q, --ask TEXT        a question, answered in words (repeatable)\n"
            "  -c, --digit TEXT      a question answered with a digit 0-9 (repeatable)\n"
            "  -r, --rank Q|A|B|...  the answers A, B ... to Q, most likely first\n"
            "  -y, --yes TEXT        a yes/no question: the probability of yes (repeatable)\n"
            "  -t, --tag             the library's tags: genre, hue, comment\n"
            "  -n, --tokens N        longest answer (default 256)\n"
            "  -s, --start SEC       listen from SEC seconds in (negative: from the middle)\n"
            "  -l, --length SEC      listen to SEC seconds at most\n"
            "  -d, --device NAME     the GPU\n"
            "  -v, --verbose\n");
}

/* "Q|A|B|..." -> the answers by probability, with each one's. */
static void rank(RXML *ml, const char *spec)
{
    char *s = rx_strdup(spec);
    const char *ans[256];
    double lp[256];
    int idx[256], nb = 0;
    char *save = NULL;
    char *q = strtok_r(s, "|", &save);
    for (char *a; nb < 256 && (a = strtok_r(NULL, "|", &save));)
        ans[nb++] = a;
    int ret = rx_ml_rank(ml, q, NULL, ans, nb, lp);
    if (ret < 0) {
        fprintf(stderr, "rxbox-ml: ranking: %s\n", strerror(-ret));
        free(s);
        return;
    }
    double z = 0.0;
    for (int i = 0; i < nb; i++) {
        idx[i] = i;
        z += exp(lp[i]);
    }
    for (int i = 1; i < nb; i++) {
        for (int j = i; j > 0 && lp[idx[j]] > lp[idx[j - 1]]; j--) {
            int t = idx[j];
            idx[j] = idx[j - 1];
            idx[j - 1] = t;
        }
    }
    printf("R: %s\n  ", q);
    for (int i = 0; i < RX_MIN(nb, 6); i++)
        printf(" %s %.2f", ans[idx[i]], exp(lp[idx[i]]) / z);
    printf("  (mass %.2f)\n", z);
    free(s);
}

int main(int argc, char **argv)
{
    static const struct option opts[] = {
        { "model",   required_argument, NULL, 'm' },
        { "ask",     required_argument, NULL, 'q' },
        { "digit",   required_argument, NULL, 'c' },
        { "rank",    required_argument, NULL, 'r' },
        { "yes",     required_argument, NULL, 'y' },
        { "tag",     no_argument,       NULL, 't' },
        { "tokens",  required_argument, NULL, 'n' },
        { "start",   required_argument, NULL, 's' },
        { "length",  required_argument, NULL, 'l' },
        { "device",  required_argument, NULL, 'd' },
        { "verbose", no_argument,       NULL, 'v' },
        { 0 },
    };
    const char *model = getenv("RXBOX_ML_MODEL");
    const char *asks[64], *digits[64], *ranks[16], *yeses[64];
    int nb_asks = 0, nb_digits = 0, nb_ranks = 0, nb_yeses = 0, max_tokens = 256;
    double start = 0.0, length = 0.0;
    bool tag = false;
    RXMLOptions o;
    rx_ml_options_default(&o);

    int c;
    while ((c = getopt_long(argc, argv, "m:q:c:r:y:tn:s:l:d:v", opts, NULL)) != -1) {
        switch (c) {
        case 'm': model = optarg; break;
        case 'q': if (nb_asks < 64) asks[nb_asks++] = optarg; break;
        case 'c': if (nb_digits < 64) digits[nb_digits++] = optarg; break;
        case 'r': if (nb_ranks < 16) ranks[nb_ranks++] = optarg; break;
        case 'y': if (nb_yeses < 64) yeses[nb_yeses++] = optarg; break;
        case 't': tag = true; break;
        case 'n': max_tokens = atoi(optarg); break;
        case 's': start = atof(optarg); break;
        case 'l': length = atof(optarg); break;
        case 'd': o.device = optarg; break;
        case 'v': rx_log_set_level(RX_LOG_DEBUG); break;
        default: usage(); return 2;
        }
    }
    if (!model || (!nb_asks && !nb_digits && !nb_ranks && !nb_yeses && !tag)) {
        usage();
        return 2;
    }

    RXML *ml;
    int ret = rx_ml_open(&ml, model, &o);
    if (ret < 0) {
        fprintf(stderr, "rxbox-ml: cannot open %s: %s\n", model, strerror(-ret));
        return 1;
    }

    static const char *const nums[10] = { "0", "1", "2", "3", "4", "5", "6", "7", "8", "9" };
    int nb_files = argc - optind;
    for (int f = 0; f < RX_MAX(nb_files, 1); f++) {
        if (nb_files) {
            /* Absolute: libavformat takes "AD:HOUSE 10/..." for a URL. */
            char *path = rx_path_abs(argv[optind + f]);
            if (!path) {
                fprintf(stderr, "rxbox-ml: %s: %s\n", argv[optind + f], strerror(errno));
                continue;
            }
            RXAudio a;
            int64_t t0 = rx_time_us();
            if ((ret = rx_decode(path, 1, RX_ML_RATE, &a)) < 0) {
                fprintf(stderr, "rxbox-ml: %s: %s\n", path, strerror(-ret));
                free(path);
                continue;
            }
            double dec_ms = (rx_time_us() - t0) * 1e-3;
            int64_t s0 = (int64_t)(start * RX_ML_RATE);
            int64_t n = length > 0 ? (int64_t)(length * RX_ML_RATE) : a.nb_frames;
            if (start < 0)
                s0 = RX_MAX(0, a.nb_frames / 2 - n / 2);
            s0 = RX_CLAMP(s0, 0, a.nb_frames);
            n = RX_MIN(n, a.nb_frames - s0);
            RXMLTags tags;
            if (tag)
                ret = rx_ml_tag(ml, a.samples + s0, n, &tags, NULL);
            else
                ret = rx_ml_listen(ml, a.samples + s0, n, NULL);
            rx_audio_free(&a);
            double listen_ms, ask_ms;
            rx_ml_times(ml, &listen_ms, &ask_ms);
            printf("== %s (%.1f s from %.1f s; decode %.0f ms, listen %.0f ms)\n", path,
                   n / (double)RX_ML_RATE, s0 / (double)RX_ML_RATE, dec_ms, listen_ms);
            if (tag && ret >= 0) {
                printf("T: hue %5.1f | intensity %.2f club %.2f mood %.2f | %s | %s  (questions %.0f ms)\n",
                       tags.hue, tags.intensity, tags.club, tags.valence,
                       tags.genre ? tags.genre : "-", tags.comment, ask_ms);
                rx_ml_tags_free(&tags);
            }
            free(path);
            if (ret < 0) {
                fprintf(stderr, "rxbox-ml: listening: %s\n", strerror(-ret));
                continue;
            }
        } else {
            rx_ml_forget(ml);
        }

        for (int i = 0; i < nb_asks; i++) {
            char *ans = NULL;
            int64_t t0 = rx_time_us();
            if ((ret = rx_ml_ask(ml, asks[i], max_tokens, &ans)) < 0) {
                fprintf(stderr, "rxbox-ml: asking: %s\n", strerror(-ret));
                continue;
            }
            printf("Q: %s\nA: %s\n   (%.0f ms)\n", asks[i], ans, (rx_time_us() - t0) * 1e-3);
            free(ans);
        }
        for (int i = 0; i < nb_digits; i++) {
            float p[10], mass;
            if ((ret = rx_ml_choose(ml, digits[i], NULL, nums, 10, p, &mass)) < 0) {
                fprintf(stderr, "rxbox-ml: asking: %s\n", strerror(-ret));
                continue;
            }
            double mean = 0.0;
            for (int d = 0; d < 10; d++)
                mean += d * p[d];
            printf("D: %s\n   mean %.2f (mass %.2f) [", digits[i], mean, mass);
            for (int d = 0; d < 10; d++)
                printf("%s%.2f", d ? " " : "", p[d]);
            printf("]\n");
        }
        for (int i = 0; i < nb_ranks; i++)
            rank(ml, ranks[i]);
        static const char *const yn[2] = { "Yes", "No" };
        for (int i = 0; i < nb_yeses; i++) {
            float p[2], mass;
            if ((ret = rx_ml_choose(ml, yeses[i], NULL, yn, 2, p, &mass)) < 0) {
                fprintf(stderr, "rxbox-ml: asking: %s\n", strerror(-ret));
                continue;
            }
            printf("Y: %s\n   yes %.3f (mass %.2f)\n", yeses[i], p[0], mass);
        }
        fflush(stdout);
    }

    rx_ml_close(&ml);
    return 0;
}
