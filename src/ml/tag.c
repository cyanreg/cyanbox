/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* rx_ml_tag (rxbox/ml.h): a genre, a hue and a comment from the model's
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
 *    library, so the biases cancel and the three are on one scale.
 *  - Its genre is the right family (house, trance, techno, hard dance,
 *    jazz, J-pop) more often than the right subgenre: it hears bossa nova
 *    as jazz or samba, and electro house as J-core or big room. It is kept
 *    as it is said: a suggestion the user can override.
 *  - Who sings and in what language it tells well in words (instrumental
 *    albums come out instrumental), and its mood words are apt; both go
 *    into the comment. A yes/no question about vocals, oddly, is answered
 *    "no" nearly always.
 *
 * The hue is the direction of the track on the plane of mood (dark to
 * bright) and intensity (calm to intense): Russell's circumplex of affect,
 * laid on the colour wheel with its usual associations, warm for intense
 * and cool for calm. Intense is red; intense and bright, orange and
 * yellow; bright and relaxed, green; calm, cyan; calm and sad, blue; dark,
 * violet; dark and intense, magenta. It is continuous in both, so similar
 * tracks get similar hues, and it does not depend on any library but the
 * one the constants were measured on. Near the middle of the plane, where
 * the direction is undefined and a small change would turn it round, it
 * fades into the hue of the average track (orange). Club music or not is
 * measured and kept, but left out of the hue: the genre says it. On the
 * library above, the hue of a whole track and of 150 s of its middle are
 * within 15 degrees of each other for half of the tracks and within 40 for
 * nine in ten; the rest are tracks whose middle is more intense than their
 * intro and outro, so rx_ml_tag listens to the whole track.
 */

#include "rxbox/ml.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

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

/* ------------------------------------------------------------------- hue */

static double wrap(double h)
{
    h = fmod(h, 360.0);
    return h < 0.0 ? h + 360.0 : h;
}

/* The signed angle from a to b the shorter way, in (-180, 180]. */
static double arc(double a, double b)
{
    double d = wrap(b - a);
    return d > 180.0 ? d - 360.0 : d;
}

/* The average track's hue, and the distance from the middle (in standard
 * deviations) within which a track fades into it. */
#define NEUTRAL_HUE    30.0
#define NEUTRAL_RADIUS 0.6

/* The hue for intensity i and mood v, both in the library's standard
 * deviations from its average: the angle on their plane, intense (straight
 * up) at 0 degrees, bright (right) at 90, calm (down) at 180, dark (left)
 * at 270. */
static double hue_of(double i, double v)
{
    double h = wrap(90.0 - atan2(i, v) * (180.0 / M_PI));
    double r = hypot(i, v);
    if (r < NEUTRAL_RADIUS)
        h = wrap(NEUTRAL_HUE + arc(NEUTRAL_HUE, h) * r / NEUTRAL_RADIUS);
    return h;
}

/* --------------------------------------------------------------- answers */

static char *trim(char *s)
{
    while (isspace((unsigned char)*s))
        s++;
    size_t n = strlen(s);
    while (n && (isspace((unsigned char)s[n - 1]) || s[n - 1] == '.'))
        s[--n] = 0;
    return s;
}

/* "progressive house" -> "Progressive House", "j-pop" -> "J-Pop": the first
 * letter of each word (after a space or a hyphen) up. */
static char *genre_of(const char *answer)
{
    char *s = rx_strdup(answer);
    if (!s)
        return NULL;
    char *t = trim(s);
    /* One line, no sentence: "This is ..." answers are cut at the first
     * newline or comma. */
    t[strcspn(t, "\n,;")] = 0;
    t = trim(t);
    if (!*t) {
        free(s);
        return NULL;
    }
    for (size_t i = 0; t[i]; i++) {
        if (i == 0 || t[i - 1] == ' ' || t[i - 1] == '-')
            t[i] = (char)toupper((unsigned char)t[i]);
    }
    char *g = rx_strdup(t);
    free(s);
    return g;
}

static bool has_word(const char *text, const char *word)
{
    size_t n = strlen(word);
    for (const char *p = text; (p = strcasestr(p, word)); p += n) {
        bool start = p == text || !isalpha((unsigned char)p[-1]);
        bool end = !isalpha((unsigned char)p[n]);
        if (start && end)
            return true;
    }
    return false;
}

/* "Yes, a female vocalist sings in Japanese." -> "Female vocals, Japanese";
 * "No, there are no vocals" -> "Instrumental". */
static char *vocals_of(const char *answer)
{
    static const char *const langs[] = {
        "Japanese", "English", "Korean", "Chinese", "Mandarin", "Cantonese", "Spanish",
        "Portuguese", "French", "German", "Italian", "Russian", "Latin", "Hindi", "Arabic",
    };
    if (!strncasecmp(answer, "no", 2) && !isalpha((unsigned char)answer[2]))
        return rx_strdup("Instrumental");

    bool female = has_word(answer, "female") || has_word(answer, "woman") ||
                  has_word(answer, "women") || has_word(answer, "girl");
    bool male = has_word(answer, "male") || has_word(answer, "man") || has_word(answer, "men");
    bool rap = has_word(answer, "rap") || has_word(answer, "rapper") || has_word(answer, "raps");
    bool choir = has_word(answer, "choir") || has_word(answer, "chorus");
    const char *who = rap ? "Rap" : choir ? "Choir" : female && male ? "Female and male vocals" :
                      female ? "Female vocals" : male ? "Male vocals" : "Vocals";
    const char *lang = NULL;
    for (size_t i = 0; i < RX_ARRAY_ELEMS(langs) && !lang; i++) {
        if (has_word(answer, langs[i]))
            lang = langs[i];
    }
    return lang ? rx_asprintf("%s, %s", who, lang) : rx_strdup(who);
}

/* "Bright, energetic, and uplifting." -> "Bright, energetic, uplifting". */
static char *mood_of(const char *answer)
{
    char *s = rx_strdup(answer);
    if (!s)
        return NULL;
    char *t = trim(s);
    t[strcspn(t, "\n")] = 0;
    char *out = calloc(1, strlen(t) + 1);
    if (!out) {
        free(s);
        return NULL;
    }
    size_t o = 0;
    for (const char *p = t; *p;) {
        if (!strncasecmp(p, "and ", 4) && (p == t || p[-1] == ' ')) {
            p += 4;
            continue;
        }
        char c = (char)(o == 0 ? toupper((unsigned char)*p) : tolower((unsigned char)*p));
        out[o++] = c;
        p++;
    }
    out[o] = 0;
    free(s);
    char *m = rx_strdup(trim(out));
    free(out);
    return m;
}

/* --------------------------------------------------------------- tagging */

void rx_ml_tags_free(RXMLTags *t)
{
    free(t->genre);
    free(t->comment);
    *t = (RXMLTags) { 0 };
}

static double logit(double p)
{
    p = RX_CLAMP(p, 1e-4, 1.0 - 1e-4);
    return log(p / (1.0 - p));
}

static float unit(double z)
{
    return (float)(1.0 / (1.0 + exp(-1.7 * z)));
}

int rx_ml_tag(RXML *ml, const float *pcm, int64_t nb_samples, RXMLTags *tags, RXCancel *cancel)
{
    *tags = (RXMLTags) { 0 };
    int ret = rx_ml_listen(ml, pcm, nb_samples, cancel);
    if (ret < 0)
        return ret;

    static const char *const yn[2] = { "Yes", "No" };
    double z[NB_YES];
    for (int i = 0; i < NB_YES; i++) {
        float p[2];
        if ((ret = rx_ml_choose(ml, yes_q[i].q, NULL, yn, 2, p, NULL)) < 0)
            return ret;
        z[i] = (logit(p[0]) - yes_q[i].mean) / yes_q[i].sd;
        if (rx_cancelled(cancel))
            return RX_ERROR(ECANCELED);
    }
    double in = (z[Y_INTENSE] - z[Y_CALM] - z[Y_CHILL]) / 3.0;
    double club = z[Y_CLUB];
    double val = (z[Y_HAPPY] - z[Y_DARK] - z[Y_MELANCHOLIC]) / 3.0;

    char *genre = NULL, *vocals = NULL, *mood = NULL;
    if ((ret = rx_ml_ask(ml, q_genre, 24, &genre)) < 0 ||
        (ret = rx_ml_ask(ml, q_vocals, 40, &vocals)) < 0 ||
        (ret = rx_ml_ask(ml, q_mood, 16, &mood)) < 0)
        goto end;

    tags->genre = genre_of(genre);
    char *v = vocals_of(vocals), *m = mood_of(mood);
    tags->vocals = v && strcmp(v, "Instrumental") ? 1.0f : 0.0f;
    tags->comment = v && m && *m ? rx_asprintf("%s. %s.", v, m) : rx_strdup(v ? v : "");
    free(v);
    free(m);
    if (!tags->comment) {
        ret = RX_ERROR(ENOMEM);
        goto end;
    }
    tags->hue = hue_of(in, val);
    tags->intensity = unit(in);
    tags->club = unit(club);
    tags->valence = unit(val);
    rx_debug("ml: intensity %+.2f club %+.2f mood %+.2f -> hue %.0f", in, club, val, tags->hue);

end:
    free(genre);
    free(vocals);
    free(mood);
    if (ret < 0)
        rx_ml_tags_free(tags);
    return ret;
}
