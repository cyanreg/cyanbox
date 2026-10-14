/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* Text for a display that shows Latin-1 only (rxbox/common.h,
 * rx_utf8_latin1). A CDJ-900's main processor hands its display names as
 * 8-bit text, and whatever is not Latin-1 shows as '?': kana, kanji,
 * typographic and CJK punctuation alike (an emulated one, firmware 4.32,
 * 2026-10-15). So for such a display:
 *
 * - kana become romaji: Hepburn as it is typed (shi, chi, tsu, fu, ji;
 *   a doubled consonant for っ, tch before ch; n' before a vowel or y;
 *   ー repeats the vowel before it; を is wo; foreign sounds as fa, ti,
 *   vu, she), each run of kana capitalised where a word would start;
 * - fullwidth ASCII becomes ASCII, the ideographic space a space;
 * - typographic and CJK punctuation becomes its nearest Latin-1:
 *   ’ ', “ ", ‐ – — -, … ..., 、 ",", 。 ".", 「」『』 ", 【】 [], 《》 «»,
 *   〜 ~, ・ a space;
 * - a run of Japanese with kanji in it is read by MeCab (BSD; its system
 *   dictionary, IPADIC on Debian's mecab-ipadic-utf8, loaded the first
 *   time one is needed): its words apart, by their readings, particles
 *   and auxiliaries in lowercase (は as wa), an inflection or a suffix
 *   joined to its word, a name's suffix with a hyphen ("Yorokobi no
 *   Shirushi", "Sunao ni Narenainda", "Osoi-san"); a word it has no
 *   reading for (a name, mostly) kanji by kanji. Kana alone are not given
 *   to it: it cuts words it does not know into nonsense (いんどあ as
 *   いん ど あ). Without MeCab or a dictionary, kanji are left as they are;
 * - any other script (Greek, Cyrillic, Hangul, Latin beyond Latin-1) goes
 *   through ICU's Any-Latin, folded to ASCII where it is still not Latin-1
 *   (Москва as Moskva, Łódź as Lódz), when rxbox is built with ICU; never
 *   kanji, which it reads as Mandarin;
 * - the rest is left as it is (☆, which a CDJ-900 shows, ♥, ∞). */

#include "rxbox/common.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"

#if RXBOX_MECAB
#include <mecab.h>
#endif
#if RXBOX_ICU
#include <unicode/ustring.h>
#include <unicode/utrans.h>
#endif

/* ------------------------------------------------------------------ kana */

/* Hiragana U+3041..U+3096 (katakana U+30A1..U+30F6 are these + 0x60).
 * Small kana are marked with a leading '_'; っ is "_tsu". */
static const char *const kana[] = {
    "_a", "a", "_i", "i", "_u", "u", "_e", "e", "_o", "o",
    "ka", "ga", "ki", "gi", "ku", "gu", "ke", "ge", "ko", "go",
    "sa", "za", "shi", "ji", "su", "zu", "se", "ze", "so", "zo",
    "ta", "da", "chi", "ji", "_tsu", "tsu", "zu", "te", "de", "to", "do",
    "na", "ni", "nu", "ne", "no",
    "ha", "ba", "pa", "hi", "bi", "pi", "fu", "bu", "pu", "he", "be", "pe", "ho", "bo", "po",
    "ma", "mi", "mu", "me", "mo",
    "_ya", "ya", "_yu", "yu", "_yo", "yo",
    "ra", "ri", "ru", "re", "ro",
    "_wa", "wa", "wi", "we", "wo", "n", "vu", "_ka", "_ke",
};

/* Halfwidth katakana U+FF66..U+FF9D as their fullwidth ones. */
static const uint16_t halfwidth[] = {
    0x30F2, 0x30A1, 0x30A3, 0x30A5, 0x30A7, 0x30A9, 0x30E3, 0x30E5, 0x30E7, 0x30C3, 0x30FC,
    0x30A2, 0x30A4, 0x30A6, 0x30A8, 0x30AA, 0x30AB, 0x30AD, 0x30AF, 0x30B1, 0x30B3,
    0x30B5, 0x30B7, 0x30B9, 0x30BB, 0x30BD, 0x30BF, 0x30C1, 0x30C4, 0x30C6, 0x30C8,
    0x30CA, 0x30CB, 0x30CC, 0x30CD, 0x30CE, 0x30CF, 0x30D2, 0x30D5, 0x30D8, 0x30DB,
    0x30DE, 0x30DF, 0x30E0, 0x30E1, 0x30E2, 0x30E4, 0x30E6, 0x30E8,
    0x30E9, 0x30EA, 0x30EB, 0x30EC, 0x30ED, 0x30EF, 0x30F3,
};

#define LONG_MARK   0x30FC      /* ー */
#define SOKUON      0x3063      /* っ, as hiragana */

#define VA          0x10000     /* ヷ ヸ ヹ ヺ: VA + 0..3 */

static bool hiragana(uint32_t k)
{
    return k >= 0x3041 && k <= 0x3096;
}

/* A kana as hiragana (ヷ..ヺ as VA..VA + 3), ー as itself, 0 for anything
 * else. */
static uint32_t kana_of(uint32_t c)
{
    if (hiragana(c))
        return c;
    if (c >= 0x30A1 && c <= 0x30F6)
        return c - 0x60;
    if (c >= 0x30F7 && c <= 0x30FA)
        return VA + (c - 0x30F7);
    if (c == LONG_MARK || c == 0xFF70)
        return LONG_MARK;
    if (c >= 0xFF66 && c <= 0xFF9D)
        return kana_of(halfwidth[c - 0xFF66]);
    return 0;
}

/* The romaji of one hiragana or of VA..VA + 3; "" for anything else. */
static const char *romaji_of(uint32_t k)
{
    static const char *const v[] = { "va", "vi", "ve", "vo" };
    if (k >= VA && k < VA + 4)
        return v[k - VA];
    return hiragana(k) ? kana[k - 0x3041] : "";
}

/* Voices a kana for a halfwidth dakuten or handakuten after it (ｶﾞ is ガ). */
static uint32_t voiced(uint32_t k, bool semi)
{
    if (k == 0x3046 && !semi)
        return 0x3094;                                  /* う゛: ゔ */
    if (k >= 0x306F && k <= 0x307D && (k - 0x306F) % 3 == 0)
        return k + (semi ? 2 : 1);                      /* は row: ば, ぱ */
    if (semi)
        return k;
    if ((k >= 0x304B && k <= 0x3062 && (k - 0x304B) % 2 == 0) ||   /* か..ち */
        (k >= 0x3064 && k <= 0x3068 && (k - 0x3064) % 2 == 0))     /* つ..と */
        return k + 1;
    return k;
}

#define SYL 16                  /* room for a syllable's romaji */

/* The romaji of the syllable at k[0] (`n` kana from there, 1 or 2): a
 * small vowel or ya, yu, yo after it may join it (きゃ kya, しゃ sha, ふぁ
 * fa, てぃ ti, うぃ wi, いぇ ye). Returns the kana it took. */
static int syllable(const uint32_t *k, int n, char out[SYL])
{
    const char *r = romaji_of(k[0]);
    if (r[0] == '_')
        r++;                                            /* a small one alone */
    snprintf(out, SYL, "%s", r);
    if (n < 2 || !hiragana(k[0]) || !hiragana(k[1]))
        return 1;
    const char *s = romaji_of(k[1]);
    if (s[0] != '_' || !strchr("aiueoy", s[1]))        /* not っ ゕ ゖ ゎ */
        return 1;
    s++;
    size_t len = strlen(r);
    char last = r[len - 1], head[8];
    snprintf(head, sizeof(head), "%.*s", (int)len - 1, r);
    bool palatal = !strcmp(head, "sh") || !strcmp(head, "ch") || !strcmp(head, "j");
    bool f = !strcmp(r, "fu") || !strcmp(r, "vu");
    bool t = !strcmp(r, "te") || !strcmp(r, "de");
    if (s[0] == 'y') {
        if (last == 'i' && len >= 2)
            snprintf(out, SYL, "%s%s", head, palatal ? s + 1 : s);   /* kya, sha, ja */
        else if (t || f)
            snprintf(out, SYL, "%c%s", r[0], s);          /* てゅ tyu, ふゅ fyu */
        else
            return 1;
    } else if (f) {
        snprintf(out, SYL, "%c%s", r[0], s);              /* ふぁ fa, ゔぃ vi */
    } else if (!strcmp(r, "tsu")) {
        snprintf(out, SYL, "ts%s", s);                    /* つぁ tsa */
    } else if (t && s[0] == 'i') {
        snprintf(out, SYL, "%ci", r[0]);                  /* てぃ ti, でぃ di */
    } else if ((!strcmp(r, "to") || !strcmp(r, "do")) && s[0] == 'u') {
        snprintf(out, SYL, "%cu", r[0]);                  /* とぅ tu, どぅ du */
    } else if (!strcmp(r, "u") && s[0] != 'u') {
        snprintf(out, SYL, "w%s", s);                     /* うぃ wi, うぇ we, うぉ wo */
    } else if ((!strcmp(r, "ku") || !strcmp(r, "gu")) && s[0] == 'a') {
        snprintf(out, SYL, "%cwa", r[0]);                 /* くぁ kwa, ぐぁ gwa */
    } else if (last == 'i' && s[0] == 'e') {
        if (len == 1)
            snprintf(out, SYL, "ye");                     /* いぇ */
        else
            snprintf(out, SYL, "%s%se", head, palatal ? "" : "y");  /* しぇ she, きぇ kye */
    } else {
        return 1;                                       /* あぁ: two */
    }
    return 2;
}

/* ------------------------------------------------------------ the output */

typedef struct Out {
    char  *p;
    size_t n, cap;
    bool   oom;
} Out;

static void put(Out *o, const char *s, size_t n)
{
    if (o->oom)
        return;
    if (o->n + n + 1 > o->cap) {
        size_t cap = (o->cap + n + 1) * 2;
        char *p = realloc(o->p, cap);
        if (!p) {
            o->oom = true;
            return;
        }
        o->p = p;
        o->cap = cap;
    }
    memcpy(o->p + o->n, s, n);
    o->n += n;
    o->p[o->n] = 0;
}

static void puts_(Out *o, const char *s)
{
    put(o, s, strlen(s));
}

static void putc_(Out *o, uint32_t c)
{
    char b[4];
    put(o, b, (size_t)rx_utf8_put(b, c));
}

static char last_byte(const Out *o)
{
    return o->n ? o->p[o->n - 1] : 0;
}

/* Whether a word starts here: at the start, after a space or an opening
 * mark. */
static bool word_start(const Out *o)
{
    char c = last_byte(o);
    if (o->n >= 2 && (uint8_t)o->p[o->n - 2] == 0xC2 && (uint8_t)c == 0xAB)
        return true;                                    /* « */
    return !c || strchr(" \t([{<\"'-~/", c);
}

static bool is_vowel(char c)
{
    return c && strchr("aeiou", c);
}

static char lower(char c)
{
    return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
}

/* A run of kana (`k`, `n` of them, as kana_of gives them, and the marks
 * ゛ ゜ ゝ ゞ) as romaji, capitalised with `cap`. */
static void romanise(Out *o, const uint32_t *k, int n, bool cap)
{
    bool first = true, gem = false;
    for (int i = 0; i < n;) {
        uint32_t c = k[i];
        if (c == LONG_MARK) {
            /* the vowel before again (パーティー paatii); alone, a dash */
            char v = lower(last_byte(o));
            if (first)
                puts_(o, "-");
            else if (is_vowel(v))
                put(o, &v, 1);
            i++;
            continue;
        }
        if (c == SOKUON) {
            gem = true;
            i++;
            continue;
        }
        /* ゝ ゞ: the kana before, again (voiced); ゛ ゜ alone: nothing */
        if ((c == 0x309D || c == 0x309E) && i > 0 && hiragana(k[i - 1]))
            c = c == 0x309E ? voiced(k[i - 1], false) : k[i - 1];
        if (!hiragana(c) && !(c >= VA && c < VA + 4)) {
            i++;
            continue;
        }
        uint32_t two[2] = { c, i + 1 < n ? k[i + 1] : 0 };
        char syl[SYL], word[SYL + 2];
        i += syllable(two, i + 1 < n ? 2 : 1, syl);
        /* っ: the consonant after it doubled, t before ch (マッチ matchi) */
        size_t w = 0;
        if (gem && syl[0] && !is_vowel(syl[0]) && syl[0] != 'n')
            word[w++] = syl[0] == 'c' ? 't' : syl[0];
        gem = false;
        /* ん before a vowel or y: n' (きんえん kin'en, しんや shin'ya) */
        if (!strcmp(syl, "n") && i < n && hiragana(k[i])) {
            const char *r = romaji_of(k[i]);
            r += r[0] == '_';
            if (is_vowel(r[0]) || r[0] == 'y')
                snprintf(syl, sizeof(syl), "n'");
        }
        snprintf(word + w, sizeof(word) - w, "%s", syl);
        if (first && cap && word[0] >= 'a' && word[0] <= 'z')
            word[0] = (char)(word[0] - 'a' + 'A');
        puts_(o, word);
        first = false;
    }
}

/* ----------------------------------------------------------- punctuation */

/* The Latin-1 of a punctuation mark or symbol, NULL when it has none. */
static const char *mark(uint32_t c)
{
    switch (c) {
    case 0x2018: case 0x2019: case 0x201A: case 0x201B: case 0x2032: return "'";
    case 0x201C: case 0x201D: case 0x201E: case 0x201F: case 0x2033: case 0x3003: return "\"";
    case 0x300C: case 0x300D: case 0x300E: case 0x300F: return "\"";
    case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014: case 0x2015: case 0x2212:
    case 0x2500: case 0x2501: return "-";
    case 0x2026: return "...";
    case 0x2025: return "..";
    case 0x3000: return " ";
    case 0x3001: case 0xFF64: return ",";
    case 0x3002: case 0xFF61: return ".";
    case 0x3010: case 0x3016: case 0x3018: case 0x301A: return "[";
    case 0x3011: case 0x3017: case 0x3019: case 0x301B: return "]";
    case 0x3014: return "(";
    case 0x3015: return ")";
    case 0x3008: return "<";
    case 0x3009: return ">";
    case 0x300A: return "\xC2\xAB";                     /* « */
    case 0x300B: return "\xC2\xBB";                     /* » */
    case 0x301C: case 0x301D: case 0x301E: return "~";
    case 0x30FB: case 0xFF65: return " ";
    case 0xFF62: return "\"";                           /* halfwidth 「 」 */
    case 0xFF63: return "\"";
    }
    return NULL;
}

/* -------------------------------------------------------------- readings */

/* Kanji, and the marks that stand for them (々 〆 〇). */
static bool kanji(uint32_t c)
{
    return (c >= 0x4E00 && c <= 0x9FFF) || (c >= 0x3400 && c <= 0x4DBF) ||
           (c >= 0xF900 && c <= 0xFAFF) || (c >= 0x20000 && c <= 0x3134F) ||
           (c >= 0x3005 && c <= 0x3007);
}

/* What a run for MeCab is made of: kanji and fullwidth kana (・ is
 * punctuation). */
static bool japanese(uint32_t c)
{
    return kanji(c) || (c >= 0x3041 && c <= 0x309F) || (c >= 0x30A1 && c <= 0x30FF && c != 0x30FB);
}

static bool alnum(char c)
{
    return (c >= '0' && c <= '9') || ((c | 0x20) >= 'a' && (c | 0x20) <= 'z');
}

/* Kana text as lowercase romaji; NULL when something in it is not kana. */
static char *romaji(const char *s, size_t len)
{
    uint32_t *k = malloc((len + 1) * sizeof(*k));
    if (!k)
        return NULL;
    int n = 0;
    for (const char *p = s; p < s + len;) {
        uint32_t c = rx_utf8_next(&p);
        uint32_t kc = kana_of(c);
        if (!kc && !(c >= 0x309B && c <= 0x309E)) {
            free(k);
            return NULL;
        }
        k[n++] = kc ? kc : c;
    }
    Out o = { 0 };
    romanise(&o, k, n, false);
    free(k);
    if (o.oom || !o.p) {
        free(o.p);
        return o.oom ? NULL : rx_strdup("");
    }
    return o.p;
}

#if RXBOX_MECAB
static mecab_t *tagger;
#endif
#if RXBOX_ICU
static UTransliterator *latin;
#endif
static pthread_once_t readers_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t readers_lock = PTHREAD_MUTEX_INITIALIZER;   /* neither is thread-safe */

/* MeCab with its system dictionary (/etc/mecabrc) and ICU's transform,
 * once, the first time one is needed; kept until the process ends. */
static void readers_open(void)
{
#if RXBOX_MECAB
    tagger = mecab_new2("");
    if (!tagger)
        rx_info("romaji: MeCab found no dictionary (/etc/mecabrc; Debian's mecab-ipadic-utf8): "
                "kanji go as they are");
#endif
#if RXBOX_ICU
    UErrorCode st = U_ZERO_ERROR;
    UChar id[16], rules[64];
    u_strFromUTF8(id, 16, NULL, "rxbox-latin1", -1, &st);
    u_strFromUTF8(rules, 64, NULL, "::Any-Latin; ::[^\\u0000-\\u00FF] Latin-ASCII;", -1, &st);
    UParseError pe;
    if (U_SUCCESS(st))
        latin = utrans_openU(id, -1, UTRANS_FORWARD, rules, -1, &pe, &st);
    if (U_FAILURE(st)) {
        rx_info("romaji: no ICU transform (%s): other scripts go as they are", u_errorName(st));
        latin = NULL;
    }
#endif
}

bool rx_utf8_latin1_reads_kanji(void)
{
#if RXBOX_MECAB
    pthread_once(&readers_once, readers_open);
    return tagger;
#else
    return false;
#endif
}

#if RXBOX_MECAB
/* How a word goes next to the one before it. */
enum Join {
    J_WORD,                     /* apart, capitalised */
    J_FUNC,                     /* apart, lowercase: particles, auxiliaries */
    J_JOIN,                     /* joined: an inflection, a suffix */
    J_HYPHEN,                   /* joined with a hyphen: さん, ちゃん */
};

typedef struct Word {
    const char *surface;        /* into the run */
    size_t      len;
    char       *roman;          /* lowercase romaji; NULL: the surface as it is */
    enum Join   join;
} Word;

/* Field i of a MeCab feature (IPADIC: part of speech, three subdivisions,
 * conjugation type and form, base form, reading, pronunciation); false
 * when it is missing or "*". */
static bool field(const char *f, int i, char *out, size_t size)
{
    for (; i > 0 && f; i--)
        if ((f = strchr(f, ',')))
            f++;
    size_t n = f ? strcspn(f, ",") : 0;
    n = RX_MIN(n, size - 1);
    if (f)
        memcpy(out, f, n);
    out[n] = 0;
    return n && strcmp(out, "*");
}

static enum Join join_of(const char *pos, const char *pos1, const char *pos2, const char *prev,
                         const char *prev1)
{
    bool inflected = !strcmp(prev, "動詞") || !strcmp(prev, "形容詞") || !strcmp(prev, "助動詞") ||
                     (!strcmp(prev, "名詞") && !strcmp(prev1, "非自立"));
    if (!strcmp(pos, "助詞"))
        return !strcmp(pos1, "接続助詞") && inflected ? J_JOIN : J_FUNC;   /* koete; no, wa */
    if (!strcmp(pos, "助動詞"))
        return inflected ? J_JOIN : J_FUNC;                 /* narenai; makka na */
    if (!strcmp(pos1, "接尾"))
        return !strcmp(pos2, "人名") ? J_HYPHEN : J_JOIN;   /* osoi-san; shoutengai */
    if (!strcmp(pos, "名詞") && !strcmp(pos1, "非自立") && inflected)
        return J_JOIN;                                      /* narenain(da) */
    return J_WORD;
}

/* A word MeCab has no reading for (a name, mostly): each kanji by its own
 * reading, 々 the one before it again, what has none as it is. With the
 * lock held, the run's nodes done with. NULL when out of memory. */
static char *by_kanji(const char *s, size_t len)
{
    Out o = { 0 };
    char prev[64] = "";
    for (const char *p = s; p < s + len;) {
        const char *at = p;
        uint32_t c = rx_utf8_next(&p);
        char *r = NULL, reading[64];
        if (c == 0x3005 && prev[0]) {
            r = rx_strdup(prev);
        } else if (kanji(c)) {
            const mecab_node_t *n = mecab_sparse_tonode2(tagger, at, (size_t)(p - at));
            while (n && n->stat != MECAB_NOR_NODE)
                n = n->next;
            if (n && n->length == (size_t)(p - at) && field(n->feature, 7, reading, sizeof(reading)))
                r = romaji(reading, strlen(reading));
        } else {
            r = romaji(at, (size_t)(p - at));
        }
        if (r) {
            puts_(&o, r);
            snprintf(prev, sizeof(prev), "%s", r);
            free(r);
        } else {
            put(&o, at, (size_t)(p - at));
            prev[0] = 0;
        }
    }
    if (o.oom) {
        free(o.p);
        return NULL;
    }
    return o.p ? o.p : rx_strdup("");
}
#endif

/* A run of Japanese with kanji in it by MeCab's words and readings (see
 * the top). False without MeCab or its dictionary. */
static bool read_run(Out *o, const char *run, size_t len)
{
#if RXBOX_MECAB
    if (!rx_utf8_latin1_reads_kanji())
        return false;
    Word *w = NULL;
    int nb = 0, cap = 0;
    char prev[32] = "", prev1[32] = "";
    bool prefix = false;
    pthread_mutex_lock(&readers_lock);
    for (const mecab_node_t *n = mecab_sparse_tonode2(tagger, run, len); n; n = n->next) {
        if (n->stat == MECAB_BOS_NODE || n->stat == MECAB_EOS_NODE || !n->length)
            continue;
        if (nb == cap) {
            Word *g = realloc(w, (size_t)(cap = cap * 2 + 8) * sizeof(*w));
            if (!g) {
                o->oom = true;
                break;
            }
            w = g;
        }
        char pos[32], pos1[32], pos2[32], reading[256];
        field(n->feature, 0, pos, sizeof(pos));
        field(n->feature, 1, pos1, sizeof(pos1));
        field(n->feature, 2, pos2, sizeof(pos2));
        Word *x = &w[nb++];
        *x = (Word) { .surface = n->surface, .len = n->length,
                      .join = prefix ? J_JOIN : join_of(pos, pos1, pos2, prev, prev1) };
        prefix = !strcmp(pos, "接頭詞");
        /* a particle by its sound: は wa, へ e */
        bool particle = !strcmp(pos, "助詞");
        if (n->stat != MECAB_UNK_NODE && field(n->feature, particle ? 8 : 7, reading, sizeof(reading)))
            x->roman = romaji(reading, strlen(reading));
        if (!x->roman)
            x->roman = romaji(n->surface, n->length);
        snprintf(prev, sizeof(prev), "%s", pos);
        snprintf(prev1, sizeof(prev1), "%s", pos1);
    }
    for (int i = 0; i < nb; i++)
        if (!w[i].roman)
            w[i].roman = by_kanji(w[i].surface, w[i].len);
    pthread_mutex_unlock(&readers_lock);

    for (int i = 0; i < nb; i++) {
        enum Join j = w[i].join;
        bool after = alnum(last_byte(o)), start = word_start(o);
        if (!i && !after && (j == J_JOIN || j == J_HYPHEN))
            j = J_WORD;
        if (j == J_HYPHEN)
            puts_(o, "-");
        else if (j != J_JOIN && (i ? !start : after))
            puts_(o, " ");
        /* a title's first word capitalised, whatever it is */
        bool up = j == J_WORD || (!i && start);
        if (w[i].roman) {
            if (up && w[i].roman[0] >= 'a' && w[i].roman[0] <= 'z')
                w[i].roman[0] = (char)(w[i].roman[0] - 'a' + 'A');
            puts_(o, w[i].roman);
        } else {
            put(o, w[i].surface, w[i].len);
        }
        free(w[i].roman);
    }
    free(w);
    return true;
#else
    (void)o, (void)run, (void)len;
    return false;
#endif
}

/* A run of what a Latin-1 display has no glyph for and is neither
 * Japanese nor punctuation, through ICU (see the top). False without it.
 * Sets *changed when the text came out otherwise. */
static bool icu_run(Out *o, const char *s, size_t len, bool *changed)
{
#if RXBOX_ICU
    pthread_once(&readers_once, readers_open);
    if (!latin)
        return false;
    UErrorCode st = U_ZERO_ERROR;
    int32_t n = 0, cap = (int32_t)RX_MIN(len * 8 + 16, (size_t)INT32_MAX / 2);
    UChar *b = malloc((size_t)cap * sizeof(*b));
    char *u = NULL;
    if (b)
        u_strFromUTF8(b, cap, &n, s, (int32_t)len, &st);
    int32_t limit = n;
    if (b && U_SUCCESS(st)) {
        pthread_mutex_lock(&readers_lock);
        utrans_transUChars(latin, b, &n, cap, 0, &limit, &st);
        pthread_mutex_unlock(&readers_lock);
    }
    int32_t m = 0;
    if (b && U_SUCCESS(st) && (u = malloc((size_t)n * 3 + 1)))
        u_strToUTF8(u, n * 3 + 1, &m, b, n, &st);
    bool ok = b && u && U_SUCCESS(st);
    if (ok) {
        put(o, u, (size_t)m);
        *changed |= (size_t)m != len || memcmp(u, s, len);
    }
    free(b);
    free(u);
    return ok;
#else
    (void)o, (void)s, (void)len, (void)changed;
    return false;
#endif
}

/* Japanese punctuation brings its own spacing, its Latin-1 needs a space:
 * after a closing mark where text follows (回レ！雪月花: "Kai Re!
 * Setsugekka"), before an opening one where text precedes. */
static bool opening(uint32_t c)
{
    return c == 0xFF08 || c == 0xFF3B || c == 0xFF5B || c == 0x3008 || c == 0x300A ||
           c == 0x300C || c == 0x300E || c == 0x3010 || c == 0x3014 || c == 0x3016 ||
           c == 0x3018 || c == 0x301A || c == 0xFF62;
}

static bool closing(uint32_t c)
{
    return c == 0xFF09 || c == 0xFF3D || c == 0xFF5D || c == 0x3009 || c == 0x300B ||
           c == 0x300D || c == 0x300F || c == 0x3011 || c == 0x3015 || c == 0x3017 ||
           c == 0x3019 || c == 0x301B || c == 0xFF63 || c == 0x3001 || c == 0x3002 ||
           c == 0xFF01 || c == 0xFF1F || c == 0xFF0C || c == 0xFF1A || c == 0xFF1B;
}

/* Text a space goes between: letters, digits, Japanese, an opening mark. */
static bool texty(uint32_t c)
{
    return (c < 0x80 && alnum((char)c)) || (c >= 0xC0 && c <= 0xFF && c != 0xD7 && c != 0xF7) ||
           japanese(c) || kana_of(c) || opening(c) || (c >= 0xFF10 && c <= 0xFF19) ||
           (c >= 0xFF21 && c <= 0xFF3A) || (c >= 0xFF41 && c <= 0xFF5A);
}

/* After romaji, a space where Latin text or a bracket follows straight on
 * (愛MAX: "Ai MAX"; 雪月花(Remix): "Setsugekka (Remix)"). */
static void space_after(Out *o, const char *next)
{
    if (alnum(*next) || *next == '(' || *next == '[')
        puts_(o, " ");
}

/* What goes to icu_run: above Latin-1, and nothing this file knows
 * better (kana, kanji, punctuation, fullwidth forms) or the CDJ-900 shows
 * (☆). */
static bool other(uint32_t c)
{
    return c > 0xFF && !japanese(c) && !kana_of(c) && !mark(c) && !(c >= 0xFF01 && c <= 0xFF9F) &&
           c != 0x2606;
}

/* -------------------------------------------------------------------- it */

char *rx_utf8_latin1(const char *s)
{
    if (!s)
        return NULL;
    bool any = false;
    for (const unsigned char *p = (const unsigned char *)s; *p && !any; p++)
        any = *p >= 0x80;
    if (!any)
        return NULL;

    Out o = { 0 };
    bool changed = false;
    size_t cap = strlen(s) + 1;
    uint32_t *run = malloc(cap * sizeof(*run));
    if (!run)
        return NULL;
    for (const char *p = s; *p;) {
        const char *at = p;
        uint32_t c = rx_utf8_next(&p);
        /* Japanese with kanji in it: MeCab's words */
        if (japanese(c)) {
            const char *q = p;
            bool has_kanji = kanji(c);
            while (*q) {
                const char *r = q;
                uint32_t d = rx_utf8_next(&r);
                if (!japanese(d))
                    break;
                has_kanji |= kanji(d);
                q = r;
            }
            if (has_kanji && read_run(&o, at, (size_t)(q - at))) {
                space_after(&o, q);
                p = q;
                changed = true;
                continue;
            }
        }
        if (other(c)) {
            const char *q = p;
            while (*q) {
                const char *r = q;
                if (!other(rx_utf8_next(&r)))
                    break;
                q = r;
            }
            if (icu_run(&o, at, (size_t)(q - at), &changed)) {
                p = q;
                continue;
            }
        }
        if (kana_of(c) || (c >= 0x309B && c <= 0x309E)) {
            int n = 0;
            const char *q = at;
            while (*q) {
                const char *r = q;
                uint32_t d = rx_utf8_next(&r);
                uint32_t kd = kana_of(d);
                if (kd) {
                    run[n++] = kd;
                } else if ((d == 0xFF9E || d == 0xFF9F) && n) {
                    run[n - 1] = voiced(run[n - 1], d == 0xFF9F);
                } else if (d >= 0x309B && d <= 0x309E) {
                    run[n++] = d;
                } else {
                    break;
                }
                q = r;
            }
            romanise(&o, run, n, word_start(&o));
            space_after(&o, q);
            p = q;
            changed = true;
            continue;
        }
        const char *m = mark(c);
        if (m || (c >= 0xFF01 && c <= 0xFF5E)) {
            char b = (char)(c - 0xFEE0);                /* fullwidth ASCII */
            if (opening(c) && alnum(last_byte(&o)))
                puts_(&o, " ");
            if (m)
                puts_(&o, m);
            else
                put(&o, &b, 1);
            if (closing(c) && *p) {
                const char *r = p;
                if (texty(rx_utf8_next(&r)))
                    puts_(&o, " ");
            }
            changed = true;
        } else {
            putc_(&o, c);
        }
    }
    free(run);
    if (o.oom || !changed) {
        free(o.p);
        return NULL;
    }
    return o.p;
}
