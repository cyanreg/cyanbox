/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* cb_ml_tag (cyanbox/ml.h): a genre, a colour and a comment from the model's
 * answers about a track.
 *
 * What the model can and cannot tell was measured on the author's library
 * (some 150 tracks: house, garage, techno, trance, J-core, bossa nova,
 * jazz, J-pop, ambient, orchestral; docs/ML.md has the numbers):
 *  - Asked for a number (tempo, key, "0 to 9" scales), it answers from
 *    habit: 111 BPM and C major for nearly everything, and 8 or 9 for the
 *    energy of anything with a beat. None of that is used.
 *  - Asked yes or no, the probability it gives "Yes" varies with the track
 *    far more finely, if with a bias of its own per question. Seven such
 *    questions span three directions: intensity ("intense", "calm",
 *    "chill-out", which agree at |r| 0.9), whether it is club music, and
 *    mood ("happy", "dark", "melancholic"). Each answer is taken as its
 *    log-odds, centred and scaled by its mean and spread over that
 *    library, so the biases cancel and the three are on one scale. They
 *    are the measures the library keeps.
 *  - Its genre is the right family (house, trance, techno, hard dance,
 *    jazz, J-pop) more often than the right subgenre: it hears bossa nova
 *    as jazz or samba, and electro house as J-core or big room. It is kept
 *    as it is said: a suggestion the user can override.
 *  - Who sings and in what language it tells well in words (instrumental
 *    albums come out instrumental), and its mood words are apt; both go
 *    into the comment. A yes/no question about vocals, oddly, is answered
 *    "no" nearly always.
 *
 * The colour is the model's choice among the legend's eight descriptions
 * (one per rekordbox colour, in the user's words), asked as a multiple-
 * choice question with a letter per description and scored, not sampled:
 * the probabilities of the eight letters as the first token of the
 * answer, read from the logits of one forward pass. So the quantisation
 * to eight colours is the model's, and the probabilities say how sure it
 * is. A model asked multiple choice favours some letters and places in
 * the list whatever they say (docs/ML.md measures how much), so the
 * question is asked in every rotation of the list, each description once
 * under each letter, and the answers combined by their geometric mean:
 * what is left is the descriptions' own pull. That pull (with no audio
 * the model leans to purple, red and orange, away from aqua and blue) is
 * left in: divided out, it tipped the bright tracks from orange to yellow
 * without the colours agreeing better with the model's own measures and
 * words (docs/ML.md has the numbers). This replaced a continuous hue
 * computed from the measures (intensity and mood as an angle on the
 * colour wheel), which the stick then quantised to the nearest of its
 * eight without the model's say.
 *
 * The colour question's length is the legend's, which the user writes:
 * the context left beside the audio for the questions is measured from
 * the questions themselves (with the longest answers they are allowed),
 * so a long legend cuts a long track shorter rather than leave its
 * question no room; a legend that would take more than a quarter of the
 * context is refused. A colour is only as good as the model's answering
 * with a letter: the letters hold 99.7 % of the answer or more on every
 * track measured (docs/ML.md), and under half the tag fails rather than
 * keep eight renormalised scraps.
 */

#include "cyanbox/ml.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "ml_priv.h"

/* ------------------------------------------------------------- questions */

enum {
    Y_INTENSE,
    Y_CALM,
    Y_CHILL,
    Y_CLUB,
    Y_HAPPY,
    Y_DARK,
    Y_MELANCHOLIC,
    NB_YES,
};

/* Each question, and the mean and spread of the log-odds of its "Yes" over
 * the library they were measured on. */
static const struct {
    const char *q;
    double mean, sd;
} yes_q[NB_YES] = {
    [Y_INTENSE]     = { "Is this an intense, high-energy track? Answer yes or no.", -1.210, 2.203 },
    [Y_CALM]        = { "Is this track calm? Answer yes or no.",                      -5.690, 1.715 },
    [Y_CHILL]       = { "Is this chill-out music? Answer yes or no.",                 -2.810, 1.712 },
    [Y_CLUB]        = { "Is this track made for dancing in a club? Answer yes or no.", 2.191, 2.654 },
    [Y_HAPPY]       = { "Is this track happy and uplifting? Answer yes or no.",       -1.739, 2.021 },
    [Y_DARK]        = { "Is this track dark? Answer yes or no.",                      -3.682, 2.071 },
    [Y_MELANCHOLIC] = { "Is this track melancholic? Answer yes or no.",               -3.220, 1.511 },
};

static const char *const q_genre =
    "What genre is this track? Answer with the genre name only, as specific as possible.";
static const char *const q_vocals =
    "Are there vocals in this track? If yes, say who sings and in what language, in a few words.";
static const char *const q_mood = "Describe the mood of this track in at most three words.";
static const char *const q_color = "Which of these descriptions fits this music best?";

/* The longest answers cb_ml_tag lets the model give, in tokens. */
#define GENRE_TOKENS  24
#define VOCALS_TOKENS 40
#define MOOD_TOKENS   16

/* The letters' share of the colour's answer below which it is no answer. */
#define MIN_LETTER_MASS 0.5f

/* --------------------------------------------------------- multiple choice */

static const char *const letters[26] = {
    "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M",
    "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z",
};

/* "Question?\nA. first\nB. second\n...\nAnswer with the letter only.",
 * malloc'd. */
static char *pick_prompt(const char *question, const char *const *options, int nb)
{
    size_t size = strlen(question) + 64;
    for (int i = 0; i < nb; i++)
        size += strlen(options[i]) + 8;
    char *prompt = malloc(size);
    if (!prompt)
        return NULL;
    size_t n = (size_t)snprintf(prompt, size, "%s\n", question);
    for (int i = 0; i < nb; i++)
        n += (size_t)snprintf(prompt + n, size - n, "%s. %s\n", letters[i], options[i]);
    snprintf(prompt + n, size - n, "Answer with the letter only.");
    return prompt;
}

int cb_ml_pick(CBML *ml, const char *question, const char *const *options, int nb,
               float *probs, float *mass)
{
    if (nb < 2 || nb > 26)
        return CB_ERROR(EINVAL);
    char *prompt = pick_prompt(question, options, nb);
    if (!prompt)
        return CB_ERROR(ENOMEM);
    int ret = cb_ml_choose(ml, prompt, NULL, letters, nb, probs, mass);
    free(prompt);
    return ret;
}

/* Rotation r of the legend: description (r + k) mod N under the k-th
 * letter. */
static void rotate(const char *const legend[CB_TRACK_COLORS], int r,
                   const char *opts[CB_TRACK_COLORS])
{
    for (int k = 0; k < CB_TRACK_COLORS; k++)
        opts[k] = legend[(r + k) % CB_TRACK_COLORS];
}

/* The colour's question in `orders` rotations, as queries for ml_ask_many:
 * rotation r's prompt in prompts[r] (malloc'd, freed by the caller), its
 * logits into logits + r * ML_VOCAB. Rotation r lists description (r + k)
 * mod N under the k-th letter: over all N rotations each description has
 * every letter and every place once. */
static int color_queries(const char *const legend[CB_TRACK_COLORS], int orders, MLQuery *q,
                         char **prompts, float *logits)
{
    for (int r = 0; r < orders; r++) {
        const char *opts[CB_TRACK_COLORS];
        rotate(legend, r, opts);
        if (!(prompts[r] = pick_prompt(q_color, opts, CB_TRACK_COLORS)))
            return CB_ERROR(ENOMEM);
        q[r] = (MLQuery) { .question = prompts[r], .logits = logits + (size_t)r * ML_VOCAB };
    }
    return 0;
}

/* The colour from the logits of the colour's questions (color_queries),
 * as cb_ml_color returns it. */
static int color_of(CBML *ml, const float *logits, int orders, float probs[CB_TRACK_COLORS],
                    float *mass)
{
    enum { N = CB_TRACK_COLORS };
    int32_t ids[N];
    int ret = ml_choice_ids(ml, letters, N, ids);
    if (ret < 0)
        return ret;
    double lp[N] = { 0 };
    float mass_sum = 0.0f;
    for (int r = 0; r < orders; r++) {
        float p[N], m;
        if ((ret = ml_choice_probs(logits + (size_t)r * ML_VOCAB, ids, N, p, &m)) < 0)
            return ret;
        for (int k = 0; k < N; k++)
            lp[(r + k) % N] += log(fmax(p[k], 1e-30));
        mass_sum += m;
    }
    /* The geometric mean, renormalised: the model's preference for a
     * letter or a place multiplies the probability of whatever stands
     * there, and over every rotation each description has had each
     * letter's factor once, so the factors cancel in the product. (The
     * plain mean leaves the favoured letters' share in; on the acceptance
     * tracks it also picked less consistently, docs/ML.md.) */
    double mx = -INFINITY, z = 0.0;
    for (int i = 0; i < N; i++)
        mx = fmax(mx, lp[i] / orders);
    for (int i = 0; i < N; i++)
        z += exp(lp[i] / orders - mx);
    int best = 0;
    for (int i = 0; i < N; i++) {
        probs[i] = (float)(exp(lp[i] / orders - mx) / z);
        if (probs[i] > probs[best])
            best = i;
    }
    cb_debug("ml: colour %d (%.2f); the letters held %.2f of the answer", best + 1, probs[best],
             mass_sum / orders);
    if (mass)
        *mass = mass_sum / orders;
    return best + 1;
}

int cb_ml_color(CBML *ml, const char *const legend[CB_TRACK_COLORS], int orders,
                float probs[CB_TRACK_COLORS], float *mass)
{
    orders = CB_CLAMP(orders, 1, CB_TRACK_COLORS);
    MLQuery q[CB_TRACK_COLORS];
    char *prompts[CB_TRACK_COLORS] = { 0 };
    float *logits = malloc((size_t)orders * ML_VOCAB * sizeof(*logits));
    int ret = logits ? color_queries(legend, orders, q, prompts, logits) : CB_ERROR(ENOMEM);
    if (ret >= 0)
        ret = ml_ask_many(ml, q, orders, NULL);
    if (ret >= 0)
        ret = color_of(ml, logits, orders, probs, mass);
    for (int r = 0; r < orders; r++)
        free(prompts[r]);
    free(logits);
    return ret;
}

uint32_t cb_ml_legend_hash(const char *const legend[CB_TRACK_COLORS])
{
    uint32_t h = 2166136261u;
    for (int i = 0; i < CB_TRACK_COLORS; i++) {
        /* Each description with its end, so "a" "bc" is not "ab" "c". */
        const char *s = legend[i] ? legend[i] : "";
        do {
            h ^= (unsigned char)*s;
            h *= 16777619u;
        } while (*s++);
    }
    return h ? h : 1;
}

/* --------------------------------------------------------------- tagging */

void cb_ml_tags_free(CBMLTags *t)
{
    free(t->genre);
    free(t->comment);
    *t = (CBMLTags) { 0 };
}

static double logit(double p)
{
    p = CB_CLAMP(p, 1e-4, 1.0 - 1e-4);
    return log(p / (1.0 - p));
}

static float unit(double z)
{
    return (float)(1.0 / (1.0 + exp(-1.7 * z)));
}

/* The most context any of cb_ml_tag's questions takes after the audio,
 * with its answer: the colour question, in its every rotation, grows with
 * the legend. */
static int question_room(CBML *ml, const char *const legend[CB_TRACK_COLORS])
{
    int room = 0, ret;
    for (int i = 0; i < NB_YES; i++) {
        if ((ret = ml_question_tokens(ml, yes_q[i].q, NULL)) < 0)
            return ret;
        room = CB_MAX(room, ret);
    }
    static const struct {
        const char *const *q;
        int answer;
    } asks[] = {
        { &q_genre, GENRE_TOKENS }, { &q_vocals, VOCALS_TOKENS }, { &q_mood, MOOD_TOKENS },
    };
    for (size_t i = 0; i < CB_ARRAY_ELEMS(asks); i++) {
        if ((ret = ml_question_tokens(ml, *asks[i].q, NULL)) < 0)
            return ret;
        room = CB_MAX(room, ret + asks[i].answer);
    }
    for (int r = 0; r < CB_ML_COLOR_ORDERS; r++) {
        const char *opts[CB_TRACK_COLORS];
        rotate(legend, r, opts);
        char *prompt = pick_prompt(q_color, opts, CB_TRACK_COLORS);
        if (!prompt)
            return CB_ERROR(ENOMEM);
        ret = ml_question_tokens(ml, prompt, NULL);
        free(prompt);
        if (ret < 0)
            return ret;
        room = CB_MAX(room, ret);
    }
    return room;
}

int cb_ml_tag(CBML *ml, const float *pcm, int64_t nb_samples,
              const char *const legend[CB_TRACK_COLORS], CBMLTags *tags, CBCancel *cancel)
{
    *tags = (CBMLTags) { 0 };
    int room = question_room(ml, legend);
    if (room < 0)
        return room;
    /* A quarter of the context: the audio keeps three quarters, about
     * eight minutes. The interface's fields (127 bytes a colour) stay
     * well under it in any script. */
    if (room > ml_max_context(ml) / 4) {
        cb_err("ml: the colour legend is too long: its question takes %d tokens, at most %d "
               "fit beside the audio (shorten the settings color.pink ... color.purple)",
               room, ml_max_context(ml) / 4);
        return CB_ERROR(E2BIG);
    }
    int64_t t0 = cb_time_us();
    int ret = ml_listen(ml, pcm, nb_samples, room, cancel);
    if (ret < 0)
        return ret;

    /* Every question at once (ml_ask_many): the seven yes/no, the colour's
     * in every order, and the three in words. */
    enum {
        Q_YES = 0,
        Q_COLOR = Q_YES + NB_YES,
        Q_GENRE = Q_COLOR + CB_ML_COLOR_ORDERS,
        Q_VOCALS,
        Q_MOOD,
        NB_Q,
    };
    MLQuery q[NB_Q] = { 0 };
    char *prompts[CB_ML_COLOR_ORDERS] = { 0 };
    float *logits = malloc((size_t)Q_GENRE * ML_VOCAB * sizeof(*logits));
    if (!logits) {
        ret = CB_ERROR(ENOMEM);
        goto end;
    }
    for (int i = 0; i < NB_YES; i++)
        q[Q_YES + i] = (MLQuery) {
            .question = yes_q[i].q, .logits = logits + (size_t)(Q_YES + i) * ML_VOCAB,
        };
    if ((ret = color_queries(legend, CB_ML_COLOR_ORDERS, q + Q_COLOR, prompts,
                             logits + (size_t)Q_COLOR * ML_VOCAB)) < 0)
        goto end;
    q[Q_GENRE] = (MLQuery) {
        .question = q_genre, .answer_wanted = true, .max_tokens = GENRE_TOKENS,
        .settled = ml_genre_settled,
    };
    q[Q_VOCALS] = (MLQuery) {
        .question = q_vocals, .answer_wanted = true, .max_tokens = VOCALS_TOKENS,
        .settled = ml_vocals_settled,
    };
    q[Q_MOOD] = (MLQuery) {
        .question = q_mood, .answer_wanted = true, .max_tokens = MOOD_TOKENS,
        .settled = ml_mood_settled,
    };
    if ((ret = ml_ask_many(ml, q, NB_Q, cancel)) < 0)
        goto end;

    static const char *const yn[2] = { "Yes", "No" };
    int32_t yn_ids[2];
    if ((ret = ml_choice_ids(ml, yn, 2, yn_ids)) < 0)
        goto end;
    double z[NB_YES];
    for (int i = 0; i < NB_YES; i++) {
        float p[2];
        if ((ret = ml_choice_probs(q[Q_YES + i].logits, yn_ids, 2, p, NULL)) < 0)
            goto end;
        z[i] = (logit(p[0]) - yes_q[i].mean) / yes_q[i].sd;
    }
    double in = (z[Y_INTENSE] - z[Y_CALM] - z[Y_CHILL]) / 3.0;
    double club = z[Y_CLUB];
    double val = (z[Y_HAPPY] - z[Y_DARK] - z[Y_MELANCHOLIC]) / 3.0;

    float mass;
    if ((ret = color_of(ml, logits + (size_t)Q_COLOR * ML_VOCAB, CB_ML_COLOR_ORDERS, tags->colors,
                        &mass)) < 0)
        goto end;
    if (!(mass >= MIN_LETTER_MASS)) {
        cb_err("ml: the letters held only %.0f %% of the model's answer to the colour "
               "question; the legend's words may not read as a list to it", mass * 100.0);
        ret = CB_ERROR(EIO);
        goto end;
    }
    tags->color = ret;
    tags->legend = cb_ml_legend_hash(legend);

    const char *genre = q[Q_GENRE].answer, *vocals = q[Q_VOCALS].answer;
    const char *mood = q[Q_MOOD].answer;
    tags->genre = ml_genre_of(genre);
    char *v = ml_vocals_of(vocals), *m = ml_mood_of(mood);
    tags->vocals = v && strcmp(v, "Instrumental") ? 1.0f : 0.0f;
    tags->comment = v && m && *m ? cb_asprintf("%s. %s.", v, m) : cb_strdup(v ? v : "");
    free(v);
    free(m);
    if (!tags->comment) {
        ret = CB_ERROR(ENOMEM);
        goto end;
    }
    tags->intensity = unit(in);
    tags->club = unit(club);
    tags->valence = unit(val);
    cb_debug("ml: intensity %+.2f club %+.2f mood %+.2f, colour %d", in, club, val, tags->color);
    const MLStats *st = ml_stats(ml);
    cb_debug("ml: tagged in %.0f ms: encoder %.0f, %d tokens of audio read %.0f, %d of questions "
             "%.0f, %d of answers %.0f", (cb_time_us() - t0) * 1e-3, st->encode_ms,
             st->prefill_tokens, st->prefill_ms, st->read_tokens, st->read_ms, st->answer_tokens,
             st->answer_ms);
    cb_debug("ml: colours %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f; intensity %.6f club %.6f "
             "mood %.6f; genre \"%s\" vocals \"%s\" mood \"%s\"", tags->colors[0], tags->colors[1],
             tags->colors[2], tags->colors[3], tags->colors[4], tags->colors[5], tags->colors[6],
             tags->colors[7], tags->intensity, tags->club, tags->valence, genre, vocals, mood);

end:
    for (int i = 0; i < NB_Q; i++)
        free(q[i].answer);
    for (int r = 0; r < CB_ML_COLOR_ORDERS; r++)
        free(prompts[r]);
    free(logits);
    if (ret < 0)
        cb_ml_tags_free(tags);
    return ret;
}
