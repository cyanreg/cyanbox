/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* What cb_ml_tag (tag.c) makes of the model's answers in words: the genre,
 * who sings, the mood; and when an answer being generated is already all
 * that will be made of it (MLQuery.settled). */

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "ml_priv.h"

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
char *ml_genre_of(const char *answer)
{
    char *s = cb_strdup(answer);
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
    char *g = cb_strdup(t);
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
char *ml_vocals_of(const char *answer)
{
    static const char *const langs[] = {
        "Japanese", "English", "Korean", "Chinese", "Mandarin", "Cantonese", "Spanish",
        "Portuguese", "French", "German", "Italian", "Russian", "Latin", "Hindi", "Arabic",
    };
    if (!strncasecmp(answer, "no", 2) && !isalpha((unsigned char)answer[2]))
        return cb_strdup("Instrumental");

    bool female = has_word(answer, "female") || has_word(answer, "woman") ||
                  has_word(answer, "women") || has_word(answer, "girl");
    bool male = has_word(answer, "male") || has_word(answer, "man") || has_word(answer, "men");
    bool rap = has_word(answer, "rap") || has_word(answer, "rapper") || has_word(answer, "raps");
    bool choir = has_word(answer, "choir") || has_word(answer, "chorus");
    const char *who = rap ? "Rap" : choir ? "Choir" : female && male ? "Female and male vocals" :
                      female ? "Female vocals" : male ? "Male vocals" : "Vocals";
    const char *lang = NULL;
    for (size_t i = 0; i < CB_ARRAY_ELEMS(langs) && !lang; i++) {
        if (has_word(answer, langs[i]))
            lang = langs[i];
    }
    return lang ? cb_asprintf("%s, %s", who, lang) : cb_strdup(who);
}

/* "Bright, energetic, and uplifting." -> "Bright, energetic, uplifting". */
char *ml_mood_of(const char *answer)
{
    char *s = cb_strdup(answer);
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
    char *m = cb_strdup(trim(out));
    free(out);
    return m;
}

/* Whether an answer so far already says all that the functions above make
 * of it, whatever the model would say next: the rest is not generated
 * (MLQuery.settled), and the tags come out the same.
 *
 * ml_genre_of and ml_mood_of keep what comes before the first of `cuts`
 * after the leading spaces. Once something trim() would not take away
 * follows it, no continuation can trim the answer back to before it
 * either. */
static bool cut_settled(const char *answer, const char *cuts)
{
    while (isspace((unsigned char)*answer))
        answer++;
    const char *c = answer + strcspn(answer, cuts);
    if (!*c)
        return false;
    while (*++c) {
        if (!isspace((unsigned char)*c) && *c != '.')
            return true;
    }
    return false;
}

bool ml_genre_settled(const char *answer)
{
    return cut_settled(answer, "\n,;");
}

bool ml_mood_settled(const char *answer)
{
    return cut_settled(answer, "\n");
}

/* "No" and then something not a letter: ml_vocals_of says "Instrumental",
 * whatever follows. */
bool ml_vocals_settled(const char *answer)
{
    return !strncasecmp(answer, "no", 2) && answer[2] && !isalpha((unsigned char)answer[2]);
}
