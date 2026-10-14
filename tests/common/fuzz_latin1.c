/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* A deterministic fuzzer for rx_utf8_latin1 (src/common/latin1.c), run
 * under AddressSanitizer and UndefinedBehaviorSanitizer
 * (src/common/meson.build): strings of the code points it treats apart
 * (kana small and large, the marks, kanji, halfwidth forms, punctuation,
 * other scripts, ASCII) with invalid bytes among them, through MeCab and
 * ICU when rxbox has them. What comes out must be UTF-8.
 *
 * RX_FUZZ_SEED sets the seed, RX_FUZZ_ITERS the number of strings. A
 * finding aborts (the sanitizers, or a check here); 0 when clean. */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rxbox/common.h"

#define DEFAULT_SEED  UINT64_C(0x6c6174696e316b61)
#define DEFAULT_ITERS 300000

static uint64_t state;

static uint32_t rnd(void)
{
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return (uint32_t)(state >> 11);
}

static const uint32_t pool[] = {
    0x3041, 0x3042, 0x3063, 0x3083, 0x3093, 0x3094, 0x3096, 0x309B, 0x309C, 0x309D, 0x309E,
    0x30A1, 0x30A6, 0x30C3, 0x30C6, 0x30D5, 0x30F4, 0x30F6, 0x30F7, 0x30FA, 0x30FB, 0x30FC, 0x30FD,
    0x4E00, 0x5927, 0x6F22, 0x9053, 0x3005, 0x3006, 0x3007, 0x20B9F, 0xF900,
    0xFF01, 0xFF08, 0xFF09, 0xFF10, 0xFF21, 0xFF5E, 0xFF61, 0xFF64, 0xFF65, 0xFF66, 0xFF6F, 0xFF70,
    0xFF76, 0xFF8A, 0xFF9D, 0xFF9E, 0xFF9F, 0x3000, 0x3001, 0x3002, 0x3008, 0x300A, 0x300C, 0x300D,
    0x3010, 0x3011, 0x301C, 0x2019, 0x201C, 0x2014, 0x2026, 0x2500,
    0x0391, 0x03AC, 0x0416, 0xAC00, 0xD7A3, 0x0142, 0x0E01, 0x2606, 0x2665, 0x221E,
    'a', 'Z', '1', ' ', '(', '[', '-', 0xAB, 0xE9, 0xFFFD,
};

int main(void)
{
    const char *e = getenv("RX_FUZZ_SEED");
    state = e ? strtoull(e, NULL, 0) : DEFAULT_SEED;
    if (!state)
        state = DEFAULT_SEED;
    e = getenv("RX_FUZZ_ITERS");
    long iters = e ? strtol(e, NULL, 0) : DEFAULT_ITERS;
    fprintf(stderr, "fuzz_latin1: seed 0x%" PRIx64 ", %ld strings, kanji %s\n", state, iters,
            rx_utf8_latin1_reads_kanji() ? "read" : "as they are");

    char s[512];
    for (long it = 0; it < iters; it++) {
        size_t n = 0;
        int len = (int)(rnd() % 32);
        for (int i = 0; i < len && n < sizeof(s) - 8; i++) {
            if (rnd() % 16 == 0)
                s[n++] = (char)(rnd() | 0x80);          /* not UTF-8 */
            else
                n += (size_t)rx_utf8_put(s + n, pool[rnd() % RX_ARRAY_ELEMS(pool)]);
        }
        s[n] = 0;
        char *l = rx_utf8_latin1(s);
        if (l && !rx_utf8_valid(l)) {
            fprintf(stderr, "fuzz_latin1: not UTF-8 out of string %ld\n", it);
            abort();
        }
        free(l);
    }
    return 0;
}
