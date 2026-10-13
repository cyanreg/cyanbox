/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* rxbox-ml: asks MOSS-Audio about audio files, for trying prompts out.
 *
 *   rxbox-ml -m MODEL [-q QUESTION]... [-c QUESTION]... [options] [FILE]...
 *
 * Each file is decoded to 16 kHz mono, listened to once, and every -q is
 * asked of it (greedy answers); every -c is asked for a digit, and the
 * probabilities of 0 to 9 and their mean are printed. With no file, the
 * questions are asked without audio.
 *
 * -t tags as the library does, with the default legend (rxbox/colors.h)
 * or -L's. -b checks the colour question for a bias of order: it is asked
 * in each rotation of the legend and in the reverse order, one line each
 * with the probabilities by colour (pink .. purple), for a script to
 * compare (docs/ML.md, The colour). */

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
            "  -t, --tag             the library's tags: colour, genre, comment\n"
            "  -L, --legend A|B|...  the eight colours' descriptions, pink to purple\n"
            "  -b, --bias            the colour question in every rotation and reversed\n"
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

/* rx_track_color_names, which is the formats module's: not linked here. */
static const char *const color_names[RX_TRACK_COLORS] = {
    "pink", "red", "orange", "yellow", "green", "aqua", "blue", "purple",
};

/* The colour question in the order `order` (indices into the legend), its
 * probabilities back in the legend's order. */
static int color_in_order(RXML *ml, const char *const *legend, const int *order, float *p,
                          float *mass)
{
    const char *opts[RX_TRACK_COLORS];
    float q[RX_TRACK_COLORS];
    for (int k = 0; k < RX_TRACK_COLORS; k++)
        opts[k] = legend[order[k]];
    int ret = rx_ml_color(ml, opts, 1, q, mass);
    for (int k = 0; ret >= 0 && k < RX_TRACK_COLORS; k++)
        p[order[k]] = q[k];
    return ret;
}

static void print_probs(const char *what, const float *p, float mass)
{
    printf("B: %-5s", what);
    for (int i = 0; i < RX_TRACK_COLORS; i++)
        printf(" %.4f", p[i]);
    printf("  mass %.3f\n", mass);
}

static void bias(RXML *ml, const char *const *legend)
{
    int order[RX_TRACK_COLORS];
    float p[RX_TRACK_COLORS], mass;
    double listen_ms, ask0, ask1;
    rx_ml_times(ml, &listen_ms, &ask0);
    for (int r = 0; r < RX_TRACK_COLORS; r++) {
        for (int k = 0; k < RX_TRACK_COLORS; k++)
            order[k] = (r + k) % RX_TRACK_COLORS;
        if (color_in_order(ml, legend, order, p, &mass) < 0)
            return;
        char what[16];
        snprintf(what, sizeof(what), "rot%d", r);
        print_probs(what, p, mass);
    }
    rx_ml_times(ml, &listen_ms, &ask1);
    for (int k = 0; k < RX_TRACK_COLORS; k++)
        order[k] = RX_TRACK_COLORS - 1 - k;
    if (color_in_order(ml, legend, order, p, &mass) >= 0)
        print_probs("rev", p, mass);
    printf("B: %.0f ms for the %d rotations\n", ask1 - ask0, RX_TRACK_COLORS);
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
        { "legend",  required_argument, NULL, 'L' },
        { "bias",    no_argument,       NULL, 'b' },
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
    bool tag = false, check_bias = false;
    const char *legend[RX_TRACK_COLORS] = {
        RX_TRACK_VIBE_PINK, RX_TRACK_VIBE_RED,   RX_TRACK_VIBE_ORANGE, RX_TRACK_VIBE_YELLOW,
        RX_TRACK_VIBE_GREEN, RX_TRACK_VIBE_AQUA, RX_TRACK_VIBE_BLUE,   RX_TRACK_VIBE_PURPLE,
    };
    char *legend_arg = NULL;
    RXMLOptions o;
    rx_ml_options_default(&o);

    int c;
    while ((c = getopt_long(argc, argv, "m:q:c:r:y:tL:bn:s:l:d:v", opts, NULL)) != -1) {
        switch (c) {
        case 'm': model = optarg; break;
        case 'q': if (nb_asks < 64) asks[nb_asks++] = optarg; break;
        case 'c': if (nb_digits < 64) digits[nb_digits++] = optarg; break;
        case 'r': if (nb_ranks < 16) ranks[nb_ranks++] = optarg; break;
        case 'y': if (nb_yeses < 64) yeses[nb_yeses++] = optarg; break;
        case 't': tag = true; break;
        case 'b': check_bias = true; break;
        case 'L': {
            free(legend_arg);
            legend_arg = rx_strdup(optarg);
            char *save = NULL, *d = strtok_r(legend_arg, "|", &save);
            for (int i = 0; i < RX_TRACK_COLORS && d; i++, d = strtok_r(NULL, "|", &save))
                legend[i] = d;
            break;
        }
        case 'n': max_tokens = atoi(optarg); break;
        case 's': start = atof(optarg); break;
        case 'l': length = atof(optarg); break;
        case 'd': o.device = optarg; break;
        case 'v': rx_log_set_level(RX_LOG_DEBUG); break;
        default: usage(); return 2;
        }
    }
    if (!model || (!nb_asks && !nb_digits && !nb_ranks && !nb_yeses && !tag && !check_bias)) {
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
                ret = rx_ml_tag(ml, a.samples + s0, n, legend, &tags, NULL);
            else
                ret = rx_ml_listen(ml, a.samples + s0, n, NULL);
            rx_audio_free(&a);
            double listen_ms, ask_ms;
            rx_ml_times(ml, &listen_ms, &ask_ms);
            printf("== %s (%.1f s from %.1f s; decode %.0f ms, listen %.0f ms)\n", path,
                   n / (double)RX_ML_RATE, s0 / (double)RX_ML_RATE, dec_ms, listen_ms);
            if (tag && ret >= 0) {
                int second = tags.color == 1 ? 2 : 1;
                for (int i = 1; i <= RX_TRACK_COLORS; i++) {
                    if (i != tags.color && tags.colors[i - 1] > tags.colors[second - 1])
                        second = i;
                }
                printf("T: %s %.2f (%s %.2f) | intensity %.2f club %.2f mood %.2f | %s | %s  "
                       "(questions %.0f ms)\n", color_names[tags.color - 1],
                       tags.colors[tags.color - 1], color_names[second - 1],
                       tags.colors[second - 1], tags.intensity, tags.club, tags.valence,
                       tags.genre ? tags.genre : "-", tags.comment, ask_ms);
                printf("P:");
                for (int i = 0; i < RX_TRACK_COLORS; i++)
                    printf(" %.4f", tags.colors[i]);
                printf("\n");
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
        if (check_bias)
            bias(ml, legend);
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
    free(legend_arg);
    return 0;
}
