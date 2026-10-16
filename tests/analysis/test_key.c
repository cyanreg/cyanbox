/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* The key detector on music in known keys. Two sets, both synthesised here
 * in closed form (no fixtures on disk but the case lists):
 *
 * - tests/data/analysis/keys.txt: gen_fixtures.py's tonal(), dj's sine
 *   chord loops with a kick (i - iv - V - i and I - V - vi - IV), with dj's
 *   answers kept for information. dj names 5 of the 9 (its tuning estimate
 *   read every peak 60 Hz low); its port with that fixed named 7, missing
 *   G major as C and E major as Dbm (the kick's sweep tipped them).
 * - tests/data/analysis/keys_synth.txt (gen_key_fixtures.py synth): 216
 *   short tracks in all 24 keys, six progressions (three major, three
 *   minor), each plain, detuned and with a sparse bass, at several rates
 *   and tempos: band-limited sawtooth chords through a low-pass, a sawtooth
 *   bass line, a pitched kick, a clap and hats, arranged as dance tracks
 *   are (two bars of drums, two of the tonic chord, the loop four times
 *   with a pedal pluck on the tonic, the tonic again). See synth() and
 *   docs/KEY.md. `meson test` runs the quick subset marked in the list (44
 *   cases: every progression and variant in two keys, and the cases the
 *   previous detector got wrong); CYANBOX_KEY_FULL=1 runs all 216.
 *
 * Every case must come out in its true key, and the tuning estimate must
 * find the generated detune within 3 cents (it is within 2 on all of them;
 * the previous estimate was up to 7.6 cents off on the synthetic tracks).
 * The summary prints the plain accuracy and the MIREX score (1 same key,
 * 0.5 a fifth above, 0.3 the relative, 0.2 the parallel) per set, so a
 * build that breaks a case shows how far off it went.
 *
 * Also: names are dj's spellings ("Dbm", "F#m", "Bb"), silence has no key,
 * bad arguments are refused, and the confidence stays in 0..1.
 *
 * Other uses, for the evaluation (gen_key_fixtures.py):
 *
 *     test_key FILE|- RATE    the key of raw float32 mono samples
 *     test_key --features FILE|- RATE
 *                             the detector's chroma vectors of the whole
 *                             and of each 20-second segment, one line each
 *                             after a line with the tuning (the training
 *                             data of gen_key_fixtures.py train)
 *     test_key --synth NAME   writes that keys_synth.txt case to stdout as
 *                             raw float32 mono (its rate is in the list) */

#include <inttypes.h>
#include <math.h>
#include <stdio.h>

#include "cyanbox/analysis.h"
#include "cyanbox/common.h"

/* The detector's own source, for --features: the training data of its
 * templates must be exactly what it computes. cb_analyse_key is then
 * defined here, and the library's copy is not linked in. */
#include "../../src/analysis/key.c"

#define KEYS_FILE "tests/data/analysis/keys.txt"
#define SYNTH_FILE "tests/data/analysis/keys_synth.txt"

static const char *const MAJOR[12] = {
    "C", "Db", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B",
};
static const char *const MINOR[12] = {
    "Cm", "Dbm", "Dm", "Ebm", "Em", "Fm", "F#m", "Gm", "Abm", "Am", "Bbm", "Bm",
};

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            fails++;                                                    \
        }                                                               \
    } while (0)

/* ---------------------------------------------------------------- scoring */

static bool parse_key(const char *s, int *tonic, bool *minor)
{
    for (int i = 0; i < 12; i++) {
        if (!strcmp(s, MAJOR[i]) || !strcmp(s, MINOR[i])) {
            *tonic = i;
            *minor = !strcmp(s, MINOR[i]);
            return true;
        }
    }
    return false;
}

/* MIREX's weighted score, as mir_eval.key.weighted_score computes it */
static double mirex(const char *truth, const char *est)
{
    int rt, et;
    bool rm, em;
    if (!parse_key(truth, &rt, &rm) || !parse_key(est, &et, &em))
        return 0.0;
    int d = (et - rt + 12) % 12;
    if (rm == em)
        return d == 0 ? 1.0 : d == 7 ? 0.5 : 0.0;
    if (d == (rm ? 3 : 9))
        return 0.3;
    return d == 0 ? 0.2 : 0.0;
}

typedef struct Tally {
    int nb, right;
    double score, conf;
} Tally;

static void tally(Tally *t, const char *truth, const CBKeyResult *k)
{
    t->nb++;
    t->right += !strcmp(truth, k->key);
    t->score += mirex(truth, k->key);
    t->conf += k->confidence;
}

static void tally_print(const char *what, const Tally *t)
{
    if (t->nb)
        printf("  %-12s %4d cases  accuracy %5.1f %%  MIREX %.3f  mean confidence %.2f\n", what,
               t->nb, 100.0 * t->right / t->nb, t->score / t->nb, t->conf / t->nb);
}

/* ------------------------------------------------------- dj's tonal loops */

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

/* ------------------------------------------------------ synthetic tracks */

/* The progressions, as (root above the tonic, minor triad) per bar. The
 * three minor ones are the commonest in dance music and J-pop: the aeolian
 * loop, the natural-minor i - iv - v and the Andalusian cadence (with the
 * major V of harmonic minor); the major ones the "axis" loop, ii - V - I and
 * the J-pop "royal road" IV - V - iii - vi, which never sounds the tonic
 * chord inside the loop. Two of them are played as offbeat stabs, the
 * others as a sustained pad. */
typedef struct Prog {
    const char *name;
    bool minor, stabs;
    int chord[4][2];
} Prog;

static const Prog PROGS[] = {
    { "axis",       false, false, { { 0, 0 }, { 7, 0 }, { 9, 1 }, { 5, 0 } } },   /* I V vi IV */
    { "twofive",    false, true,  { { 2, 1 }, { 7, 0 }, { 0, 0 }, { 0, 0 } } },   /* ii V I I */
    { "royal",      false, false, { { 5, 0 }, { 7, 0 }, { 4, 1 }, { 9, 1 } } },   /* IV V iii vi */
    { "aeolian",    true,  false, { { 0, 1 }, { 8, 0 }, { 3, 0 }, { 10, 0 } } },  /* i VI III VII */
    { "minorfive",  true,  true,  { { 0, 1 }, { 5, 1 }, { 7, 1 }, { 0, 1 } } },   /* i iv v i */
    { "andalusian", true,  false, { { 0, 1 }, { 10, 0 }, { 8, 0 }, { 7, 0 } } },  /* i VII VI V */
};

enum { VAR_PLAIN, VAR_DETUNED, VAR_SPARSE, NB_VARIANTS };
static const char *const VARIANTS[NB_VARIANTS] = { "plain", "detuned", "sparse" };

/* Bars: drums, tonic, the loop LOOPS times, tonic */
#define INTRO_DRUMS 2
#define INTRO_TONIC 2
#define LOOPS 4
#define OUTRO 2
#define NB_BARS (INTRO_DRUMS + INTRO_TONIC + 4 * LOOPS + OUTRO)

typedef struct Case {
    char name[48];
    int rate;
    double bpm;
    int tonic;
    bool minor;
    int prog, variant;
    double cents;
    unsigned seed;
    char truth[8];
    bool quick;
} Case;

/* A band-limited sawtooth: the naive ramp with a polyBLEP correction at
 * each wrap. */
static double saw(double *phase, double dt)
{
    double p = *phase, v = 2 * p - 1;
    if (p < dt) {
        double t = p / dt;
        v -= t + t - t * t - 1;
    } else if (p > 1 - dt) {
        double t = (p - 1) / dt;
        v -= t * t + t + t + 1;
    }
    p += dt;
    *phase = p >= 1 ? p - 1 : p;
    return v;
}

/* A state-variable filter (Zavalishin's trapezoidal form) */
typedef struct Svf {
    double a1, a2, a3, k, ic1, ic2;
} Svf;

static void svf_init(Svf *s, double fc, double q, int rate)
{
    double g = tan(M_PI * fc / rate);
    *s = (Svf) { .k = 1 / q };
    s->a1 = 1 / (1 + g * (g + s->k));
    s->a2 = g * s->a1;
    s->a3 = g * s->a2;
}

static void svf_run(Svf *s, double x, double *low, double *band)
{
    double v3 = x - s->ic2;
    double v1 = s->a1 * s->ic1 + s->a2 * v3;
    double v2 = s->ic2 + s->a2 * s->ic1 + s->a3 * v3;
    s->ic1 = 2 * v1 - s->ic1;
    s->ic2 = 2 * v2 - s->ic2;
    *low = v2;
    *band = v1;
}

static double noise(uint32_t *s)
{
    *s ^= *s << 13;
    *s ^= *s >> 17;
    *s ^= *s << 5;
    return *s / 2147483648.0 - 1.0;
}

static double midi_hz(double m, double detune)
{
    return 440.0 * pow(2.0, (m - 69) / 12) * detune;
}

/* The MIDI note of pitch class `pc` in [lo, lo + 12) */
static int place(int pc, int lo)
{
    return lo + ((pc - lo) % 12 + 12) % 12;
}

/* exp(-k / (tau * rate)) for k < len: the drums' and stabs' decays, by
 * sample, so the generator stays cheap */
static double *decay_table(int rate, double tau, int len)
{
    double *t = malloc(sizeof(*t) * len);
    if (t)
        for (int k = 0; k < len; k++)
            t[k] = exp(-k / (tau * rate));
    return t;
}

static float *synth(const Case *c, int64_t *nb)
{
    const Prog *p = &PROGS[c->prog];
    int rate = c->rate;
    double period = 60.0 / c->bpm, bar = 4 * period, eighth = period / 2;
    double detune = pow(2.0, c->cents / 1200);
    int64_t n = (int64_t)(NB_BARS * bar * rate);
    int beat_len = (int)(period * rate) + 2, kick_len = rate / 4;
    float *x = malloc(sizeof(*x) * n);
    double *kick_tab = malloc(sizeof(*kick_tab) * kick_len);
    double *clap_tab = decay_table(rate, 0.08, beat_len);
    double *hat_tab = decay_table(rate, 0.04, beat_len);
    double *stab_tab = decay_table(rate, 0.09, beat_len);
    double *pluck_tab = decay_table(rate, 0.12, beat_len);
    if (!x || !kick_tab || !clap_tab || !hat_tab || !stab_tab || !pluck_tab) {
        free(x);
        x = NULL;
        goto end;
    }
    for (int k = 0; k < kick_len; k++)
        kick_tab[k] = kick((double)k / rate);

    Svf pad_lp, bass_lp, clap_bp, pluck_lp;
    svf_init(&pad_lp, p->stabs ? 2800 : 2000, 0.8, rate);
    svf_init(&pluck_lp, 3000, 0.9, rate);
    svf_init(&bass_lp, 350, 2.0, rate);
    svf_init(&clap_bp, 1200, 1.0, rate);
    double pad_ph[8] = { 0 }, pad_dt[8] = { 0 }, bass_ph = 0, bass_dt = 0, lo, band;
    double pluck_ph = 0, pluck_dt = midi_hz(place(c->tonic, 72), detune) / rate;
    uint32_t rng = c->seed | 1;
    double last_noise = 0;
    int cur_bar = -1, cur_note = -1;

    for (int64_t i = 0; i < n; i++) {
        double t = (double)i / rate;
        int b = (int)(t / bar);
        double tb = t - b * bar;
        int beat = CB_MIN((int)(tb / period), 3);
        double tq = tb - beat * period;
        int kq = (int)(tq * rate);

        /* The chord of this bar: the tonic triad outside the loop */
        int root = 0, minor = p->minor;
        if (b >= INTRO_DRUMS + INTRO_TONIC && b < NB_BARS - OUTRO) {
            int k = (b - INTRO_DRUMS - INTRO_TONIC) % 4;
            root = p->chord[k][0];
            minor = p->chord[k][1];
        }
        int pc = c->tonic + root;
        if (b != cur_bar) {
            /* Close voicing in G3..F#4, the root doubled an octave up, two
             * saws per note detuned by +-6 cents (symmetric: no shift in
             * the tuning) */
            int notes[4] = {
                place(pc, 55), place(pc + (minor ? 3 : 4), 55), place(pc + 7, 55),
                place(pc, 67),
            };
            for (int v = 0; v < 4; v++) {
                double f = midi_hz(notes[v], detune);
                pad_dt[2 * v] = f * pow(2.0, 6 / 1200.0) / rate;
                pad_dt[2 * v + 1] = f * pow(2.0, -6 / 1200.0) / rate;
            }
            cur_bar = b;
        }
        bool tonal_on = b >= INTRO_DRUMS;

        /* Pad or stabs */
        double env = 0;
        if (tonal_on) {
            if (p->stabs) {
                double ts = tq >= eighth ? tq - eighth : tq + eighth;
                /* No tail before the first stab */
                bool first = b == INTRO_DRUMS && beat == 0 && tq < eighth;
                int ks = CB_MIN((int)(ts * rate), beat_len - 1);
                env = first ? 0 : fmin(1, ts / 0.003) * stab_tab[ks] * 1.6;
            } else {
                env = fmin(1, tb / 0.02) * fmax(0, fmin(1, (bar - tb) / 0.06));
            }
        }
        double v = 0;
        for (int o = 0; o < 8; o++)
            v += saw(&pad_ph[o], pad_dt[o]);
        svf_run(&pad_lp, v * env, &lo, &band);
        double out = 0.05 * lo;

        /* Bass: rolling eighths with octave jumps, or one note a bar */
        double benv = 0;
        int bnote = place(pc, 33);
        if (tonal_on) {
            if (c->variant == VAR_SPARSE) {
                double gate = 0.9 * period;
                benv = tb < gate ? fmin(1, tb / 0.004) * fmin(1, (gate - tb) / 0.01) : 0;
            } else {
                int e = (int)(tb / eighth);
                double te = tb - e * eighth, gate = 0.7 * eighth;
                benv = te < gate ? fmin(1, te / 0.004) * fmin(1, (gate - te) / 0.01) : 0;
                bnote += (e & 1) * 12;
            }
        }
        if (bnote != cur_note) {
            bass_dt = midi_hz(bnote, detune) / rate;
            cur_note = bnote;
        }
        svf_run(&bass_lp, saw(&bass_ph, bass_dt) * benv, &lo, &band);
        out += 0.3 * lo;

        /* In the loop, a pluck on the tonic (C5..B5) on the second and
         * fourth sixteenths of every beat: the pedal riff dance tracks
         * carry over their chords, and what tells an aeolian loop from its
         * relative major's vi - IV - I - V, which has the same notes */
        if (b >= INTRO_DRUMS + INTRO_TONIC && b < NB_BARS - OUTRO) {
            double sixteenth = period / 4;
            int q = (int)(tq / sixteenth);
            /* Since the last pluck: this sixteenth's, or the one before */
            double tp = tq - q * sixteenth + (q & 1 ? 0 : sixteenth);
            int kp = CB_MIN((int)(tp * rate), beat_len - 1);
            bool before_first = q == 0 && beat == 0 && b == INTRO_DRUMS + INTRO_TONIC;
            double penv = before_first ? 0 : fmin(1, tp / 0.002) * pluck_tab[kp];
            svf_run(&pluck_lp, saw(&pluck_ph, pluck_dt) * penv, &lo, &band);
            out += 0.12 * lo;
        }

        /* Drums: kick on every beat, clap on 2 and 4, offbeat hats */
        if (kq < kick_len)
            out += 0.8 * kick_tab[kq];
        double w = noise(&rng);
        double clap = beat & 1 ? clap_tab[CB_MIN(kq, beat_len - 1)] : 0;
        svf_run(&clap_bp, w * clap, &lo, &band);
        out += 0.5 * band;
        double hat = tq >= eighth ? hat_tab[CB_MIN((int)((tq - eighth) * rate), beat_len - 1)] : 0;
        out += 0.12 * (w - last_noise) * hat;
        last_noise = w;

        x[i] = (float)out;
    }
    *nb = n;
end:
    free(kick_tab);
    free(clap_tab);
    free(hat_tab);
    free(stab_tab);
    free(pluck_tab);
    return x;
}

static int read_cases(Case **out, int *nb_out)
{
    FILE *f = fopen(SYNTH_FILE, "r");
    if (!f) {
        printf("FAIL: cannot open %s\n", SYNTH_FILE);
        return CB_ERROR(ENOENT);
    }
    Case *cases = NULL;
    int nb = 0, cap = 0, ret = 0;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n')
            continue;
        if (nb == cap) {
            cap = cap ? 2 * cap : 256;
            Case *tmp = realloc(cases, sizeof(*cases) * cap);
            if (!tmp) {
                ret = CB_ERROR(ENOMEM);
                break;
            }
            cases = tmp;
        }
        Case *c = &cases[nb];
        char prog[16], variant[16];
        int minor, quick;
        if (sscanf(line, "%47s %d %lf %d %d %15s %15s %lf %u %7s %d", c->name, &c->rate, &c->bpm,
                   &c->tonic, &minor, prog, variant, &c->cents, &c->seed, c->truth, &quick) != 11) {
            printf("FAIL: bad line in %s: %s", SYNTH_FILE, line);
            ret = CB_ERROR(EINVAL);
            break;
        }
        c->minor = minor;
        c->quick = quick;
        c->prog = c->variant = -1;
        for (int i = 0; i < (int)CB_ARRAY_ELEMS(PROGS); i++)
            if (!strcmp(prog, PROGS[i].name))
                c->prog = i;
        for (int i = 0; i < NB_VARIANTS; i++)
            if (!strcmp(variant, VARIANTS[i]))
                c->variant = i;
        if (c->prog < 0 || c->variant < 0 || PROGS[c->prog].minor != c->minor) {
            printf("FAIL: bad case in %s: %s", SYNTH_FILE, line);
            ret = CB_ERROR(EINVAL);
            break;
        }
        nb++;
    }
    fclose(f);
    if (ret < 0) {
        free(cases);
        return ret;
    }
    *out = cases;
    *nb_out = nb;
    return 0;
}

/* ------------------------------------------------------------------ tests */

static int test_tonal(void)
{
    int fails = 0, nb_dj_true = 0;
    Tally all = { 0 };
    FILE *f = fopen(KEYS_FILE, "r");
    if (!f) {
        printf("FAIL: cannot open %s\n", KEYS_FILE);
        return 1;
    }
    char line[256];
    printf("%-10s %6s %6s  %-4s  %-4s %6s %7s  |  dj: %-4s %6s %7s\n", "signal", "rate", "cents",
           "true", "cb", "conf", "cents", "key", "conf", "cents");
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
        /* keys.txt and cyanbox spell the keys the same way */
        CHECK(!strcmp(truth, minor ? MINOR[tonic] : MAJOR[tonic]));

        int64_t nb;
        float *x = tonal(rate, tonic, minor, cents, &nb);
        if (!x)
            return fails + 1;
        CBKeyResult k;
        int64_t t0 = cb_time_us();
        int ret = cb_analyse_key(x, nb, rate, &k);
        int64_t t1 = cb_time_us();
        free(x);
        CHECK(ret == 0);

        bool right = !strcmp(k.key, truth);
        const char *note = "";
        if (strcmp(known, "-")) {
            note = right ? "  (listed as a known miss, now right: update keys.txt)"
                         : "  (known miss)";
            CHECK(right || !strcmp(k.key, known));
        } else {
            CHECK(right);
        }
        printf("%-10s %6d %+6.0f  %-4s  %-4s %6.3f %+7.2f  |      %-4s %6.3f %+7.2f  %.0f ms%s\n",
               name, rate, cents, truth, k.key, k.confidence, k.tuning_cents, dj_key, dj_conf,
               100 * dj_tuning, (t1 - t0) / 1000.0, note);
        /* A real tuning estimate: the generated detune, to a few cents */
        CHECK(fabs(k.tuning_cents - cents) < 3);
        CHECK(k.confidence >= 0 && k.confidence <= 1);
        tally(&all, truth, &k);
        nb_dj_true += !strcmp(dj_key, truth);
    }
    fclose(f);
    printf("dj's tonal loops: cyanbox finds the true key of %d of %d, dj of %d\n", all.right,
           all.nb, nb_dj_true);
    tally_print("tonal", &all);
    CHECK(all.nb >= 9);
    return fails;
}

static int test_synth(void)
{
    Case *cases;
    int nb_cases, fails = 0, nb_run = 0;
    if (read_cases(&cases, &nb_cases) < 0)
        return 1;
    const char *full = getenv("CYANBOX_KEY_FULL");
    bool all_cases = full && strcmp(full, "0");

    Tally all = { 0 }, by_var[NB_VARIANTS] = { 0 }, by_prog[CB_ARRAY_ELEMS(PROGS)] = { 0 };
    int64_t us = 0;
    double secs = 0;
    printf("\n%-24s %6s %4s %6s  %-4s  %-4s %6s %7s\n", "track", "rate", "bpm", "cents", "true",
           "cb", "conf", "cents");
    for (int i = 0; i < nb_cases; i++) {
        const Case *c = &cases[i];
        int tonic;
        bool minor;
        CHECK(parse_key(c->truth, &tonic, &minor) && tonic == c->tonic && minor == c->minor);
        if (!c->quick && !all_cases)
            continue;
        nb_run++;

        int64_t nb;
        float *x = synth(c, &nb);
        if (!x) {
            free(cases);
            return fails + 1;
        }
        CBKeyResult k;
        int64_t t0 = cb_time_us();
        int ret = cb_analyse_key(x, nb, c->rate, &k);
        us += cb_time_us() - t0;
        secs += (double)nb / c->rate;
        free(x);
        CHECK(ret == 0);

        bool right = !strcmp(k.key, c->truth);
        bool tuned = fabs(k.tuning_cents - c->cents) < 3;
        printf("%-24s %6d %4.0f %+6.0f  %-4s  %-4s %6.3f %+7.2f%s%s\n", c->name, c->rate, c->bpm,
               c->cents, c->truth, k.key, k.confidence, k.tuning_cents, right ? "" : "  WRONG",
               tuned ? "" : "  TUNING");
        if (!right) {
            printf("FAIL: %s is %s, not %s\n", c->name, k.key, c->truth);
            fails++;
        }
        if (!tuned) {
            printf("FAIL: %s tuning %+.2f cents, generated %+.0f\n", c->name, k.tuning_cents,
                   c->cents);
            fails++;
        }
        CHECK(k.confidence >= 0 && k.confidence <= 1);
        tally(&all, c->truth, &k);
        tally(&by_var[c->variant], c->truth, &k);
        tally(&by_prog[c->prog], c->truth, &k);
    }
    printf("synthetic tracks, %s (%.0f ms per minute of audio):\n",
           all_cases ? "all" : "the quick subset; CYANBOX_KEY_FULL=1 runs all",
           us / 1000.0 / (secs / 60));
    tally_print("all", &all);
    for (int i = 0; i < NB_VARIANTS; i++)
        tally_print(VARIANTS[i], &by_var[i]);
    for (int i = 0; i < (int)CB_ARRAY_ELEMS(PROGS); i++)
        tally_print(PROGS[i].name, &by_prog[i]);
    CHECK(nb_cases >= 216 && nb_run >= 40);
    free(cases);
    return fails;
}

static int test_edges(void)
{
    int fails = 0;
    CBKeyResult k;

    /* Silence has no key */
    float *z = calloc(44100 * 5, sizeof(*z));
    if (!z)
        return 1;
    CHECK(cb_analyse_key(z, 44100 * 5, 44100, &k) == 0);
    CHECK(k.key[0] == '\0' && k.confidence == 0 && k.tuning_cents == 0);
    CHECK(cb_analyse_key(z, 0, 44100, &k) == 0 && k.key[0] == '\0');
    CHECK(cb_analyse_key(z, 100, 0, &k) == CB_ERROR(EINVAL));
    CHECK(cb_analyse_key(NULL, 100, 44100, &k) == CB_ERROR(EINVAL));
    free(z);

    /* A second of a chord, shorter than any smoothing span: still a key,
     * one of dj's spellings */
    int64_t nb;
    float *x = tonal(48000, 9, true, 0.0, &nb);
    if (!x)
        return fails + 1;
    CHECK(cb_analyse_key(x, 48000, 48000, &k) == 0);
    int tonic;
    bool minor;
    CHECK(parse_key(k.key, &tonic, &minor));
    CHECK(k.confidence >= 0 && k.confidence <= 1);
    free(x);
    return fails;
}

/* ------------------------------------------------------------ other uses */

static float *read_samples(const char *path, int64_t *nb)
{
    uint8_t *buf = NULL;
    size_t size = 0;
    if (!strcmp(path, "-")) {
        size_t cap = 1 << 22, r;
        buf = malloc(cap);
        while (buf && (r = fread(buf + size, 1, cap - size, stdin)) > 0) {
            size += r;
            if (size == cap) {
                uint8_t *tmp = realloc(buf, cap *= 2);
                if (!tmp)
                    free(buf);
                buf = tmp;
            }
        }
        if (!buf)
            return NULL;
    } else {
        int ret = cb_file_read(path, &buf, &size);
        if (ret < 0) {
            printf("cannot read %s: %s\n", path, strerror(-ret));
            return NULL;
        }
    }
    float *x = malloc(CB_MAX(size, 4));
    if (x)
        memcpy(x, buf, size);
    free(buf);
    *nb = (int64_t)(size / 4);
    return x;
}

static int key_of_file(const char *path, int rate)
{
    int64_t nb;
    float *x = read_samples(path, &nb);
    if (!x)
        return 1;
    CBKeyResult k;
    int64_t t0 = cb_time_us();
    int ret = cb_analyse_key(x, nb, rate, &k);
    int64_t t1 = cb_time_us();
    free(x);
    if (ret < 0) {
        printf("cb_analyse_key failed: %s\n", strerror(-ret));
        return 1;
    }
    printf("%d %s %.6f %.6f %.0f ms\n", rate, k.key[0] ? k.key : "-", k.confidence,
           k.tuning_cents / 100, (t1 - t0) / 1000.0);
    return 0;
}

static int features_of_file(const char *path, int rate)
{
    int64_t nb;
    float *x = read_samples(path, &nb);
    if (!x || rate <= 0)
        return 1;
    KeyFrames kf;
    int ret = key_frames(x, nb, rate, &kf);
    free(x);
    if (ret < 0)
        return 1;
    double v[NB_FEAT];
    int64_t seg = CB_MAX(llrint(SEG_SECONDS * kf.fs / HOP), 1);
    printf("%.6f %.3f %" PRId64 "\n", kf.tuning, kf.fs, seg);
    for (int64_t f0 = -1; f0 < kf.nb_frames; f0 = f0 < 0 ? 0 : f0 + seg) {
        int64_t a = f0 < 0 ? 0 : f0, b = f0 < 0 ? kf.nb_frames : CB_MIN(f0 + seg, kf.nb_frames);
        if (f0 > 0 && b - a < seg / 2)
            break;
        key_chroma(&kf, a, b, v);
        for (int i = 0; i < NB_FEAT; i++)
            printf("%s%.6g", i ? " " : "", v[i]);
        printf("\n");
    }
    key_frames_free(&kf);
    return 0;
}

static int write_synth(const char *name)
{
    Case *cases;
    int nb_cases;
    if (read_cases(&cases, &nb_cases) < 0)
        return 1;
    int ret = 1;
    for (int i = 0; i < nb_cases; i++) {
        if (strcmp(cases[i].name, name))
            continue;
        int64_t nb;
        float *x = synth(&cases[i], &nb);
        if (x && fwrite(x, sizeof(*x), nb, stdout) == (size_t)nb)
            ret = 0;
        free(x);
        break;
    }
    free(cases);
    return ret;
}

int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "--synth"))
        return write_synth(argv[2]);
    if (argc == 4 && !strcmp(argv[1], "--features"))
        return features_of_file(argv[2], atoi(argv[3]));
    if (argc == 3)
        return key_of_file(argv[1], atoi(argv[2]));

    int fails = test_tonal() + test_synth() + test_edges();
    if (fails) {
        printf("test_key: %d failures\n", fails);
        return 1;
    }
    printf("test_key: ok\n");
    return 0;
}
