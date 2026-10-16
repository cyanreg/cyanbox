/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* The text helpers: NFC against Python's unicodedata (the vectors were
 * produced with unicodedata.normalize("NFC", ...), Unicode 16), invalid
 * UTF-8 replaced like dj's decode(errors="replace"), UTF-16 round trips,
 * and zlib's CRC-32 check value. */

#include "src/formats/formats_priv.h"

#include "tests/formats/test.h"

static void nfc(const char *in, const char *want)
{
    char *got = cb_utf8_clean_nfc(in);
    CHECK_STR(got, want);
    free(got);
}

int main(void)
{
    nfc("Ame\xCC\x81lie", "Am\xC3\xA9lie");                  /* e + U+0301 -> é */
    nfc("A\xCC\x8A", "\xC3\x85");                             /* A + ring -> Å */
    nfc("\xE2\x84\xAB", "\xC3\x85");                          /* U+212B ANGSTROM SIGN -> Å */
    nfc("e\xCC\xA3\xCC\x82", "\xE1\xBB\x87");                 /* e + dot below + circumflex -> ệ */
    nfc("o\xCC\x82\xCC\xA3", "\xE1\xBB\x99");                 /* reordered marks -> ộ */
    nfc("\xE3\x81\x8B\xE3\x82\x99", "\xE3\x81\x8C");          /* か + dakuten -> が */
    nfc("\xE3\x83\x8F\xE3\x82\x9A", "\xE3\x83\x91");          /* ハ + handakuten -> パ */
    nfc("\xE1\x84\x80\xE1\x85\xA1\xE1\x86\xA8", "\xEA\xB0\x81"); /* Hangul L V T -> 각 */
    nfc("\xE0\xA5\x98", "\xE0\xA4\x95\xE0\xA4\xBC");          /* excluded U+0958 stays decomposed */
    nfc("plain ascii", "plain ascii");
    nfc("bad \xFF byte", "bad \xEF\xBF\xBD byte");            /* invalid byte -> U+FFFD */

    CHECK_EQ(cb_crc32(0, "123456789", 9), 0xCBF43926);
    CHECK_EQ(cb_utf8_length("R\xC3\xB8""dh\xC3\xA5""d"), 6);

    uint16_t *u;
    size_t n;
    CHECK_EQ(cb_utf8_to_utf16("\xF0\x9F\x85\xB4", &u, &n), 0);   /* U+1F174 */
    CHECK_EQ(n, 2);
    CHECK_EQ(u[0], 0xD83C);
    CHECK_EQ(u[1], 0xDD74);
    uint8_t le[4] = { (uint8_t)u[0], (uint8_t)(u[0] >> 8), (uint8_t)u[1], (uint8_t)(u[1] >> 8) };
    char *back = cb_utf16le_to_utf8(le, 4);
    CHECK_STR(back, "\xF0\x9F\x85\xB4");
    free(back);
    free(u);
    return test_result("util");
}
