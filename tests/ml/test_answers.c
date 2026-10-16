/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* What the tags make of the model's answers in words (src/ml/answers.c),
 * and when an answer being generated is settled: for every prefix of
 * random answers, byte by byte, that the settled functions call settled,
 * the genre, vocals and mood must be those of the whole answer (the tags
 * stop generating there, and must come out as without stopping). No GPU. */

#include <stdio.h>

#include "ml_priv.h"

static int failures;

static uint64_t rng = 0x2545f4914f6cdd1dull;

static uint32_t rand32(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return (uint32_t)(rng >> 32);
}

static bool same(const char *a, const char *b)
{
    return (!a && !b) || (a && b && !strcmp(a, b));
}

static void expect(const char *what, char *got, const char *want)
{
    if (!same(got, want)) {
        printf("FAIL %s: \"%s\", not \"%s\"\n", what, got ? got : "(null)", want ? want : "(null)");
        failures++;
    }
    free(got);
}

static void expect_settled(const char *what, bool got, bool want)
{
    if (got != want) {
        printf("FAIL %s: %s\n", what, got ? "settled" : "not settled");
        failures++;
    }
}

static void test_examples(void)
{
    expect("genre", ml_genre_of("progressive house"), "Progressive House");
    expect("genre", ml_genre_of("j-pop"), "J-Pop");
    expect("genre", ml_genre_of(" Tech House, with elements of trance."), "Tech House");
    expect("genre", ml_genre_of("\n."), NULL);
    expect("vocals", ml_vocals_of("Yes, a female vocalist sings in Japanese."),
           "Female vocals, Japanese");
    expect("vocals", ml_vocals_of("No, there are no vocals"), "Instrumental");
    expect("vocals", ml_vocals_of("None that I can hear, a man raps"), "Rap");
    expect("mood", ml_mood_of("Bright, energetic, and uplifting."), "Bright, energetic, uplifting");

    expect_settled("genre \"Tech House\"", ml_genre_settled("Tech House"), false);
    expect_settled("genre \"Tech House,\"", ml_genre_settled("Tech House,"), false);
    expect_settled("genre \"Tech House. \"", ml_genre_settled("Tech House;. "), false);
    expect_settled("genre \"Tech House, with\"", ml_genre_settled("Tech House, with"), true);
    expect_settled("genre \"\\nHouse\"", ml_genre_settled("\nHouse"), false);
    expect_settled("vocals \"No\"", ml_vocals_settled("No"), false);
    expect_settled("vocals \"None\"", ml_vocals_settled("None"), false);
    expect_settled("vocals \"No,\"", ml_vocals_settled("No,"), true);
    expect_settled("vocals \"Yes,\"", ml_vocals_settled("Yes,"), false);
    expect_settled("mood \"Calm and \\n\"", ml_mood_settled("Calm and \n"), false);
    expect_settled("mood \"Calm and \\nx\"", ml_mood_settled("Calm and \nx"), true);
    expect_settled("mood \"Calm, warm\"", ml_mood_settled("Calm, warm"), false);
}

/* Answers made of the pieces the functions look for: words they match,
 * "and " they drop, and every character they cut or trim at. */
static void test_prefixes(void)
{
    static const char *const pieces[] = {
        "house", "House", "Tech", "and", "and ", " and ", "no", "No", "NO", "Yes", "None",
        "female", "male", "man", "rap", "choir", "Japanese", "English", "calm", "Dreamy",
        "j-pop", "-", " ", "  ", "\t", ",", ", ", ".", "...", "\n", "\n\n", ";", "x",
    };
    static const struct {
        const char *name;
        char *(*of)(const char *);
        bool (*settled)(const char *);
    } fns[] = {
        { "genre", ml_genre_of, ml_genre_settled },
        { "vocals", ml_vocals_of, ml_vocals_settled },
        { "mood", ml_mood_of, ml_mood_settled },
    };
    int checked[3] = { 0 }, failed = 0;
    for (int n = 0; n < 50000 && failed < 10; n++) {
        char a[256] = "";
        size_t len = 0;
        int nb = 1 + (int)(rand32() % 10);
        for (int i = 0; i < nb; i++) {
            const char *p = pieces[rand32() % CB_ARRAY_ELEMS(pieces)];
            if (len + strlen(p) >= sizeof(a))
                break;
            memcpy(a + len, p, strlen(p) + 1);
            len += strlen(p);
        }
        for (size_t f = 0; f < CB_ARRAY_ELEMS(fns); f++) {
            char *whole = fns[f].of(a);
            for (size_t k = 0; k <= len; k++) {
                char pre[256];
                memcpy(pre, a, k);
                pre[k] = 0;
                if (!fns[f].settled(pre))
                    continue;
                checked[f]++;
                char *part = fns[f].of(pre);
                if (!same(part, whole) && failed++ < 10)
                    printf("FAIL %s settled at \"%s\": \"%s\", the whole \"%s\": \"%s\"\n",
                           fns[f].name, pre, part ? part : "(null)", a, whole ? whole : "(null)");
                free(part);
            }
            free(whole);
        }
    }
    printf("settled prefixes checked: genre %d, vocals %d, mood %d\n", checked[0], checked[1],
           checked[2]);
    failures += failed;
    for (int f = 0; f < 3; f++)
        failures += !checked[f];
}

int main(void)
{
    test_examples();
    test_prefixes();
    printf("%s\n", failures ? "FAILED" : "all ok");
    return failures ? 1 : 0;
}
