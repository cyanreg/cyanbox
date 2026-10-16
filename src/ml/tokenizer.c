/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* Qwen2's tokenizer (the decoder is Qwen3, whose tokenizer it is): GPT-2's
 * byte-level BPE with Qwen2's pre-tokenizer, from the vocabulary and the
 * merges the GGUF carries (tokenizer.ggml.tokens, .merges).
 *
 * Text is split by the pre-tokenizer's regular expression, done by hand
 * here:
 *   (?i:'s|'t|'re|'ve|'m|'ll|'d)|[^\r\n\p{L}\p{N}]?\p{L}+|\p{N}
 *   | ?[^\s\p{L}\p{N}]+[\r\n]*|\s*[\r\n]+|\s+(?!\S)|\s+
 * Its Unicode classes are exact for ASCII; above it, the CJK and general
 * punctuation blocks and the usual spaces are told apart, and everything
 * else counts as a letter. The prompts are ours and in English, so that
 * is all they need; a title in a prompt would be tokenized close to, if
 * not always exactly as, the model was trained on.
 *
 * Each piece's bytes become GPT-2's printable stand-ins (bytes_to_unicode)
 * and are merged pair by pair, the pair of lowest rank first. */

#include "ml_priv.h"

#include <stdio.h>

/* ------------------------------------------------------------- hash table */

typedef struct Entry {
    const char *key;
    int32_t     value;
} Entry;

typedef struct Table {
    Entry   *e;
    uint32_t mask;
} Table;

static uint32_t hash(const char *s, size_t n)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++)
        h = (h ^ (uint8_t)s[i]) * 16777619u;
    return h;
}

static int table_init(Table *t, size_t nb)
{
    uint32_t size = 1;
    while (size < nb * 2)
        size <<= 1;
    t->e = calloc(size, sizeof(*t->e));
    t->mask = size - 1;
    return t->e ? 0 : CB_ERROR(ENOMEM);
}

static void table_put(Table *t, const char *key, int32_t value)
{
    uint32_t i = hash(key, strlen(key)) & t->mask;
    while (t->e[i].key) {
        if (!strcmp(t->e[i].key, key))
            return;             /* the first of equals wins */
        i = (i + 1) & t->mask;
    }
    t->e[i] = (Entry) { key, value };
}

static int32_t table_get(const Table *t, const char *key, size_t n)
{
    uint32_t i = hash(key, n) & t->mask;
    while (t->e[i].key) {
        if (!strncmp(t->e[i].key, key, n) && !t->e[i].key[n])
            return t->e[i].value;
        i = (i + 1) & t->mask;
    }
    return -1;
}

/* -------------------------------------------------------------- tokenizer */

struct MLTokenizer {
    const MLGguf *f;
    char **tokens;
    int    nb_tokens;
    Table  vocab;               /* token -> id */
    Table  merges;              /* "a b" -> rank */
    uint16_t byte_cp[256];      /* byte -> its stand-in code point */
    int16_t  cp_byte[512];      /* and back, -1 when none */
};

static void byte_map(MLTokenizer *t)
{
    for (int i = 0; i < 512; i++)
        t->cp_byte[i] = -1;
    int n = 0;
    for (int b = 0; b < 256; b++) {
        bool printable = (b >= 33 && b <= 126) || (b >= 161 && b <= 172) || (b >= 174);
        t->byte_cp[b] = printable ? b : 256 + n++;
        t->cp_byte[t->byte_cp[b]] = b;
    }
}

int ml_tok_create(MLTokenizer **out, const MLGguf *f)
{
    const MLValue *tok = ml_gguf_value(f, "tokenizer.ggml.tokens");
    const MLValue *mer = ml_gguf_value(f, "tokenizer.ggml.merges");
    if (!tok || tok->type != ML_GGUF_STRINGS || !mer || mer->type != ML_GGUF_STRINGS)
        return CB_ERROR(EINVAL);

    MLTokenizer *t = calloc(1, sizeof(*t));
    if (!t)
        return CB_ERROR(ENOMEM);
    t->f = f;
    t->tokens = tok->strs;
    t->nb_tokens = (int)tok->nb_strs;
    byte_map(t);

    int ret;
    if ((ret = table_init(&t->vocab, tok->nb_strs)) < 0 ||
        (ret = table_init(&t->merges, mer->nb_strs)) < 0) {
        ml_tok_free(&t);
        return ret;
    }
    for (int64_t i = 0; i < tok->nb_strs; i++)
        table_put(&t->vocab, tok->strs[i], (int32_t)i);
    for (int64_t i = 0; i < mer->nb_strs; i++)
        table_put(&t->merges, mer->strs[i], (int32_t)i);

    *out = t;
    return 0;
}

void ml_tok_free(MLTokenizer **pt)
{
    MLTokenizer *t = *pt;
    if (!t)
        return;
    free(t->vocab.e);
    free(t->merges.e);
    free(t);
    *pt = NULL;
}

int32_t ml_tok_id(const MLTokenizer *t, const char *token)
{
    return table_get(&t->vocab, token, strlen(token));
}

/* ---------------------------------------------------------------- classes */

static uint32_t utf8_at(const char *s, size_t n, size_t i, int *len)
{
    const uint8_t *p = (const uint8_t *)s + i;
    uint32_t c = p[0];
    int l = c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
    if (i + l > n)
        l = 1;
    if (l > 1) {
        c &= 0x3f >> (l - 1);
        for (int k = 1; k < l; k++)
            c = (c << 6) | (p[k] & 0x3f);
    }
    *len = l;
    return c;
}

enum { C_LETTER, C_NUMBER, C_SPACE, C_OTHER };

static int cls(uint32_t c)
{
    if (c < 0x80) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))
            return C_LETTER;
        if (c >= '0' && c <= '9')
            return C_NUMBER;
        if (c == ' ' || (c >= '\t' && c <= '\r'))
            return C_SPACE;
        return C_OTHER;
    }
    if (c == 0x85 || c == 0xa0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200a) ||
        c == 0x2028 || c == 0x2029 || c == 0x202f || c == 0x205f || c == 0x3000)
        return C_SPACE;
    if (c >= 0xff10 && c <= 0xff19)
        return C_NUMBER;
    if ((c >= 0xa1 && c <= 0xbf && c != 0xaa && c != 0xb5 && c != 0xba) || c == 0xd7 ||
        c == 0xf7 || (c >= 0x2000 && c <= 0x2bff) || (c >= 0x3000 && c <= 0x303f) ||
        (c >= 0xff00 && c <= 0xff0f) || (c >= 0xff1a && c <= 0xff20) ||
        (c >= 0xff3b && c <= 0xff40) || (c >= 0xff5b && c <= 0xff65))
        return C_OTHER;
    return C_LETTER;
}

static bool is_nl(uint32_t c)
{
    return c == '\r' || c == '\n';
}

/* The length in bytes of the pre-tokenizer's match at s[i]. */
static size_t match(const char *s, size_t n, size_t i)
{
    int l0;
    uint32_t c0 = utf8_at(s, n, i, &l0);

    /* 's 't 're 've 'm 'll 'd, any case. */
    if (c0 == '\'' && i + 1 < n) {
        char a = (char)(s[i + 1] | 0x20);
        char b = i + 2 < n ? (char)(s[i + 2] | 0x20) : 0;
        if (a == 's' || a == 't' || a == 'm' || a == 'd')
            return 2;
        if ((a == 'r' && b == 'e') || (a == 'v' && b == 'e') || (a == 'l' && b == 'l'))
            return 3;
    }

    /* [^\r\n\p{L}\p{N}]?\p{L}+ */
    {
        size_t j = i;
        int k0 = cls(c0);
        if (k0 != C_LETTER && k0 != C_NUMBER && !is_nl(c0))
            j += l0;
        size_t start = j;
        while (j < n) {
            int l;
            uint32_t c = utf8_at(s, n, j, &l);
            if (cls(c) != C_LETTER)
                break;
            j += l;
        }
        if (j > start)
            return j - i;
    }

    /* \p{N} */
    if (cls(c0) == C_NUMBER)
        return l0;

    /*  ?[^\s\p{L}\p{N}]+[\r\n]* */
    {
        size_t j = i;
        if (c0 == ' ')
            j++;
        size_t start = j;
        while (j < n) {
            int l;
            uint32_t c = utf8_at(s, n, j, &l);
            if (cls(c) != C_OTHER)
                break;
            j += l;
        }
        if (j > start) {
            while (j < n && is_nl((uint8_t)s[j]))
                j++;
            return j - i;
        }
    }

    /* The run of white space here, and where its last newline ends. */
    size_t j = i, nl_end = 0, last = i;
    while (j < n) {
        int l;
        uint32_t c = utf8_at(s, n, j, &l);
        if (cls(c) != C_SPACE)
            break;
        last = j;
        j += l;
        if (is_nl(c))
            nl_end = j;
    }
    if (j == i)
        return l0;              /* nothing matched: one character alone */

    /* \s*[\r\n]+ */
    if (nl_end)
        return nl_end - i;
    /* \s+(?!\S): all of it at the end, else all but its last character */
    if (j == n || last == i)
        return j - i;
    return last - i;
}

/* --------------------------------------------------------------------- BPE */

typedef struct Sym {
    char *s;                    /* the stand-ins' UTF-8 */
    size_t n;
} Sym;

static int push_id(int32_t **ids, int *nb, int *cap, int32_t id)
{
    if (*nb == *cap) {
        int c = CB_MAX(*cap * 2, 64);
        int32_t *p = realloc(*ids, c * sizeof(*p));
        if (!p)
            return CB_ERROR(ENOMEM);
        *ids = p;
        *cap = c;
    }
    (*ids)[(*nb)++] = id;
    return 0;
}

static int bpe(const MLTokenizer *t, const char *piece, size_t len, int32_t **ids, int *nb,
               int *cap)
{
    /* Each byte as its stand-in, as UTF-8: at most 2 bytes each. */
    char *buf = malloc(len * 2 + 1);
    Sym *sym = calloc(CB_MAX(len, 1), sizeof(*sym));
    char *pair = malloc(len * 4 + 2);
    if (!buf || !sym || !pair) {
        free(buf);
        free(sym);
        free(pair);
        return CB_ERROR(ENOMEM);
    }

    size_t o = 0;
    int ns = 0;
    for (size_t i = 0; i < len; i++) {
        uint16_t cp = t->byte_cp[(uint8_t)piece[i]];
        sym[ns].s = buf + o;
        if (cp < 0x80) {
            buf[o++] = (char)cp;
        } else {
            buf[o++] = (char)(0xc0 | (cp >> 6));
            buf[o++] = (char)(0x80 | (cp & 0x3f));
        }
        sym[ns].n = buf + o - sym[ns].s;
        ns++;
    }

    /* Symbols are runs of buf, so a merge is a join of two neighbours. */
    while (ns > 1) {
        int best = -1;
        int32_t best_rank = INT32_MAX;
        for (int i = 0; i + 1 < ns; i++) {
            size_t n = sym[i].n + 1 + sym[i + 1].n;
            memcpy(pair, sym[i].s, sym[i].n);
            pair[sym[i].n] = ' ';
            memcpy(pair + sym[i].n + 1, sym[i + 1].s, sym[i + 1].n);
            int32_t r = table_get(&t->merges, pair, n);
            if (r >= 0 && r < best_rank) {
                best_rank = r;
                best = i;
            }
        }
        if (best < 0)
            break;
        sym[best].n += sym[best + 1].n;
        memmove(&sym[best + 1], &sym[best + 2], (ns - best - 2) * sizeof(*sym));
        ns--;
    }

    int ret = 0;
    for (int i = 0; i < ns && ret >= 0; i++) {
        int32_t id = table_get(&t->vocab, sym[i].s, sym[i].n);
        if (id < 0) {
            cb_warn("ml: no token for a piece of the prompt");
            continue;
        }
        ret = push_id(ids, nb, cap, id);
    }

    free(buf);
    free(sym);
    free(pair);
    return ret;
}

int ml_tok_encode(const MLTokenizer *t, const char *text, int32_t **ids, int *nb, int *cap)
{
    size_t n = strlen(text);
    for (size_t i = 0; i < n;) {
        size_t l = match(text, n, i);
        int ret = bpe(t, text + i, l, ids, nb, cap);
        if (ret < 0)
            return ret;
        i += l;
    }
    return 0;
}

int ml_tok_decode(const MLTokenizer *t, int32_t id, char **out, size_t *len, size_t *cap)
{
    if (id < 0 || id >= t->nb_tokens)
        return CB_ERROR(EINVAL);
    const char *s = t->tokens[id];
    size_t n = strlen(s);
    if (*len + n + 1 > *cap) {
        size_t c = CB_MAX(*cap * 2, *len + n + 64);
        char *p = realloc(*out, c);
        if (!p)
            return CB_ERROR(ENOMEM);
        *out = p;
        *cap = c;
    }

    /* Special tokens (<|im_end|> ...) are not byte-level: as they are. */
    bool special = n > 4 && s[0] == '<' && s[1] == '|' && s[n - 1] == '>' && s[n - 2] == '|';
    for (size_t i = 0; i < n;) {
        int l;
        uint32_t cp = utf8_at(s, n, i, &l);
        int b = !special && cp < 512 ? t->cp_byte[cp] : -1;
        if (b >= 0) {
            (*out)[(*len)++] = (char)b;
        } else {
            memcpy(*out + *len, s + i, l);
            *len += l;
        }
        i += l;
    }
    (*out)[*len] = 0;
    return 0;
}
