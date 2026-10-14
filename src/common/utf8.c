/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* UTF-8 and Unicode NFC (rxbox/common.h). Every text rxbox keeps or gives
 * out is NFC: the library's (core/model.c sets it so, core/clean.c makes
 * what an older library kept so), and so the stick's databases, its paths
 * (dj stores every stick path in NFC, export.py _clean; HFS+ hands out NFD
 * names) and what Link serves. A decomposed kana and its dakuten reach a
 * CDJ-2000 as two characters, the mark drawn apart (an emulated one,
 * 2026-10-15); tags written on macOS are often so. */

#include "rxbox/common.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "src/common/nfc_tables.h"

/* ------------------------------------------------------------------ UTF-8 */

uint32_t rx_utf8_next(const char **p)
{
    const uint8_t *s = (const uint8_t *)*p;
    uint32_t c = s[0];
    int n = 0;
    uint32_t min = 0;
    if (c < 0x80) {
        *p += 1;
        return c;
    } else if ((c & 0xE0) == 0xC0) {
        n = 1;
        c &= 0x1F;
        min = 0x80;
    } else if ((c & 0xF0) == 0xE0) {
        n = 2;
        c &= 0x0F;
        min = 0x800;
    } else if ((c & 0xF8) == 0xF0) {
        n = 3;
        c &= 0x07;
        min = 0x10000;
    } else {
        *p += 1;
        return 0xFFFD;
    }
    for (int i = 1; i <= n; i++) {
        if ((s[i] & 0xC0) != 0x80) {
            *p += 1;
            return 0xFFFD;
        }
        c = c << 6 | (s[i] & 0x3F);
    }
    /* Overlong forms, surrogates and values past U+10FFFF are invalid
     * UTF-8; Python's decoder replaces their first byte. */
    if (c < min || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) {
        *p += 1;
        return 0xFFFD;
    }
    *p += n + 1;
    return c;
}

int rx_utf8_put(char *buf, uint32_t c)
{
    uint8_t *b = (uint8_t *)buf;
    if (c < 0x80) {
        b[0] = (uint8_t)c;
        return 1;
    } else if (c < 0x800) {
        b[0] = (uint8_t)(0xC0 | c >> 6);
        b[1] = (uint8_t)(0x80 | (c & 0x3F));
        return 2;
    } else if (c < 0x10000) {
        b[0] = (uint8_t)(0xE0 | c >> 12);
        b[1] = (uint8_t)(0x80 | (c >> 6 & 0x3F));
        b[2] = (uint8_t)(0x80 | (c & 0x3F));
        return 3;
    }
    b[0] = (uint8_t)(0xF0 | c >> 18);
    b[1] = (uint8_t)(0x80 | (c >> 12 & 0x3F));
    b[2] = (uint8_t)(0x80 | (c >> 6 & 0x3F));
    b[3] = (uint8_t)(0x80 | (c & 0x3F));
    return 4;
}

size_t rx_utf8_length(const char *s)
{
    size_t n = 0;
    while (*s) {
        rx_utf8_next(&s);
        n++;
    }
    return n;
}

/* -------------------------------------------------------------------- NFC */

#define HANGUL_S 0xAC00
#define HANGUL_L 0x1100
#define HANGUL_V 0x1161
#define HANGUL_T 0x11A7
#define HANGUL_LCOUNT 19
#define HANGUL_VCOUNT 21
#define HANGUL_TCOUNT 28
#define HANGUL_NCOUNT (HANGUL_VCOUNT * HANGUL_TCOUNT)
#define HANGUL_SCOUNT (HANGUL_LCOUNT * HANGUL_NCOUNT)

static const NfcTriple *find_decomposition(uint32_t c)
{
    size_t lo = 0, hi = RX_ARRAY_ELEMS(nfc_decompositions);
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (nfc_decompositions[mid].a == c)
            return &nfc_decompositions[mid];
        if (nfc_decompositions[mid].a < c)
            lo = mid + 1;
        else
            hi = mid;
    }
    return NULL;
}

static int combining_class(uint32_t c)
{
    size_t lo = 0, hi = RX_ARRAY_ELEMS(nfc_combining);
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (c < nfc_combining[mid].a)
            hi = mid;
        else if (c > nfc_combining[mid].b)
            lo = mid + 1;
        else
            return (int)nfc_combining[mid].c;
    }
    return 0;
}

static uint32_t compose(uint32_t a, uint32_t b)
{
    if (a >= HANGUL_L && a < HANGUL_L + HANGUL_LCOUNT && b >= HANGUL_V && b < HANGUL_V + HANGUL_VCOUNT)
        return HANGUL_S + ((a - HANGUL_L) * HANGUL_VCOUNT + (b - HANGUL_V)) * HANGUL_TCOUNT;
    if (a >= HANGUL_S && a < HANGUL_S + HANGUL_SCOUNT && (a - HANGUL_S) % HANGUL_TCOUNT == 0 &&
        b > HANGUL_T && b < HANGUL_T + HANGUL_TCOUNT)
        return a + (b - HANGUL_T);
    size_t lo = 0, hi = RX_ARRAY_ELEMS(nfc_compositions);
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        const NfcTriple *t = &nfc_compositions[mid];
        if (t->a == a && t->b == b)
            return t->c;
        if (t->a < a || (t->a == a && t->b < b))
            lo = mid + 1;
        else
            hi = mid;
    }
    return 0;
}

/* Full canonical decomposition of c appended to out (room for 4 more). */
static size_t decompose(uint32_t c, uint32_t *out, size_t n)
{
    if (c >= HANGUL_S && c < HANGUL_S + HANGUL_SCOUNT) {
        uint32_t s = c - HANGUL_S;
        out[n++] = HANGUL_L + s / HANGUL_NCOUNT;
        out[n++] = HANGUL_V + s % HANGUL_NCOUNT / HANGUL_TCOUNT;
        if (s % HANGUL_TCOUNT)
            out[n++] = HANGUL_T + s % HANGUL_TCOUNT;
        return n;
    }
    const NfcTriple *d = find_decomposition(c);
    if (!d) {
        out[n++] = c;
        return n;
    }
    n = decompose(d->b, out, n);
    if (d->c)
        out[n++] = d->c;
    return n;
}

char *rx_utf8_nfc(const char *s)
{
    size_t len = strlen(s);
    /* A code point decomposes into at most 4 (the longest canonical
     * decomposition in Unicode 16), and every code point takes a byte. */
    uint32_t *cp = malloc((4 * len + 4) * sizeof(*cp));
    if (!cp)
        return NULL;
    size_t n = 0;
    const char *p = s;
    while (*p)
        n = decompose(rx_utf8_next(&p), cp, n);

    /* Canonical ordering: stable sort of each run of non-starters. */
    for (size_t i = 1; i < n; i++) {
        int cc = combining_class(cp[i]);
        if (!cc)
            continue;
        size_t j = i;
        while (j > 0) {
            int prev = combining_class(cp[j - 1]);
            if (!prev || prev <= cc)
                break;
            uint32_t t = cp[j];
            cp[j] = cp[j - 1];
            cp[j - 1] = t;
            j--;
        }
    }

    /* Canonical composition (UAX #15): a character composes with the last
     * starter unless something between them blocks it. */
    size_t o = 0;
    ptrdiff_t starter = -1;
    int last_class = -1;
    for (size_t i = 0; i < n; i++) {
        uint32_t c = cp[i];
        int cc = combining_class(c);
        if (starter >= 0 && (last_class < cc || last_class == 0)) {
            uint32_t comp = compose(cp[starter], c);
            if (comp) {
                cp[starter] = comp;
                continue;
            }
        }
        if (!cc)
            starter = (ptrdiff_t)o;
        last_class = cc;
        cp[o++] = c;
    }

    char *out = malloc(4 * o + 1);
    if (out) {
        size_t b = 0;
        for (size_t i = 0; i < o; i++)
            b += (size_t)rx_utf8_put(out + b, cp[i]);
        out[b] = '\0';
    }
    free(cp);
    return out;
}


bool rx_utf8_is_nfc(const char *s)
{
    /* ASCII is NFC as it is. */
    const unsigned char *p = (const unsigned char *)s;
    while (*p && *p < 0x80)
        p++;
    if (!*p)
        return true;
    char *n = rx_utf8_nfc(s);
    bool same = n && !strcmp(n, s);
    free(n);
    return same;
}

bool rx_utf8_valid(const char *s)
{
    while (*s) {
        const char *at = s;
        if (rx_utf8_next(&s) == 0xFFFD && memcmp(at, "\xef\xbf\xbd", 3))
            return false;
    }
    return true;
}
