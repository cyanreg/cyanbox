/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* Small helpers the format writers share: UTF-8 <-> UTF-16 (export.pdb
 * strings and the ANLZ path are UTF-16, the directory hash runs over UTF-16
 * code units), Unicode NFC (dj stores every stick path in NFC, export.py
 * _clean; HFS+ hands out NFD names), zlib's CRC-32 (dj derives the library
 * and per-track ids and the My Tag ids from it), and the per-track values
 * export.pdb and exportLibrary.db must agree on. */

#include "src/formats/formats_priv.h"

#include <stdio.h>
#include <time.h>

#include "src/formats/buf.h"
#include "src/formats/nfc_tables.h"

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

int rx_utf8_to_utf16(const char *s, uint16_t **units, size_t *nb)
{
    size_t len = strlen(s);
    uint16_t *u = malloc((2 * len + 1) * sizeof(*u));
    if (!u)
        return RX_ERROR(ENOMEM);
    size_t n = 0;
    while (*s) {
        uint32_t c = rx_utf8_next(&s);
        if (c >= 0x10000) {
            c -= 0x10000;
            u[n++] = (uint16_t)(0xD800 | c >> 10);
            u[n++] = (uint16_t)(0xDC00 | (c & 0x3FF));
        } else {
            u[n++] = (uint16_t)c;
        }
    }
    *units = u;
    *nb = n;
    return 0;
}

static char *utf16_to_utf8(const uint8_t *p, size_t bytes, bool be)
{
    size_t n = bytes / 2;
    char *out = malloc(4 * n + 1);
    if (!out)
        return NULL;
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        uint32_t c = be ? rx_rd_be16(p + 2 * i) : rx_rd_le16(p + 2 * i);
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < n) {
            uint32_t d = be ? rx_rd_be16(p + 2 * i + 2) : rx_rd_le16(p + 2 * i + 2);
            if (d >= 0xDC00 && d <= 0xDFFF) {
                c = 0x10000 + ((c - 0xD800) << 10) + (d - 0xDC00);
                i++;
            } else {
                c = 0xFFFD;
            }
        } else if (c >= 0xD800 && c <= 0xDFFF) {
            c = 0xFFFD;
        }
        o += (size_t)rx_utf8_put(out + o, c);
    }
    out[o] = '\0';
    return out;
}

char *rx_utf16le_to_utf8(const uint8_t *p, size_t bytes)
{
    return utf16_to_utf8(p, bytes, false);
}

char *rx_utf16be_to_utf8(const uint8_t *p, size_t bytes)
{
    return utf16_to_utf8(p, bytes, true);
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

char *rx_utf8_clean_nfc(const char *s)
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

/* ------------------------------------------------------------------ CRC-32 */

uint32_t rx_crc32(uint32_t crc, const void *data, size_t size)
{
    const uint8_t *p = data;
    crc = ~crc;
    for (size_t i = 0; i < size; i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++)
            crc = crc & 1 ? (crc >> 1) ^ 0xEDB88320u : crc >> 1;
    }
    return ~crc;
}

/* ------------------------------------------------------------ plan values */

const char *const rx_my_tag_columns[4] = { "Genre", "Components", "Situation", "Tags" };

uint32_t rx_plan_library_id(const RXPlan *plan)
{
    return plan->library_id;
}

uint32_t rx_plan_master_id(const RXPlanTrack *t)
{
    uint32_t id = t->usb_path ? rx_crc32(0, t->usb_path, strlen(t->usb_path)) : 0;
    return id ? id : 1;
}

void rx_plan_date_added(const RXPlanTrack *t, char buf[16])
{
    buf[0] = '\0';
    if (!t->track)
        return;
    /* When the track came into the library, as rekordbox's date added is;
     * dj used the audio file's modification time, which a retag or a copy
     * moves. The file's time only for a track without one. */
    int64_t when = t->track->added;
    if (!when)
        when = t->track->path ? rx_file_mtime(t->track->path) : 0;
    if (!when)
        when = t->track->mtime;
    if (!when)
        return;
    time_t tt = (time_t)when;
    struct tm tm;
    if (localtime_r(&tt, &tm))
        strftime(buf, 16, "%Y-%m-%d", &tm);
}

char *rx_plan_anlz_path(const RXPlanTrack *t, const char *ext)
{
    return rx_asprintf("%s/ANLZ%04d%s", t->anlz_dir, t->anlz_index, ext);
}

char *rx_plan_artwork_dir(uint32_t art_id)
{
    return rx_asprintf("/PIONEER/Artwork/%05u", art_id / 20 + 1);
}

/* A string cut to a buffer ends on a whole UTF-8 sequence: half a
 * character would reach export.pdb as U+FFFD and exportLibrary.db as an
 * invalid byte. */
static void cut_utf8(char *buf, size_t want, size_t size)
{
    size_t n = strlen(buf);
    if (want < size || !n)
        return;
    size_t k = n;
    while (k > 0 && n - k < 3 && ((uint8_t)buf[k - 1] & 0xC0) == 0x80)
        k--;
    uint8_t lead = k ? (uint8_t)buf[k - 1] : 0;
    size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
    if (lead >= 0xC0 && n - (k - 1) < need)
        buf[k - 1] = '\0';
}

const char *rx_plan_title(const RXPlanTrack *t, char *buf, size_t size)
{
    buf[0] = '\0';
    if (!t->track)
        return buf;
    const char *title = t->track->meta.title;
    if (title && *title) {
        snprintf(buf, size, "%s", title);
        cut_utf8(buf, strlen(title), size);
        return buf;
    }
    if (!t->track->path)
        return buf;
    /* pathlib's stem: the name without its last suffix, a leading dot
     * not counting as one. */
    const char *base = rx_path_base(t->track->path);
    const char *dot = strrchr(base, '.');
    size_t n = dot && dot != base ? (size_t)(dot - base) : strlen(base);
    snprintf(buf, size, "%.*s", (int)n, base);
    cut_utf8(buf, n, size);
    return buf;
}

void rx_plan_ascii_label(const char *name, char *buf, size_t size)
{
    size_t o = 0;
    if (!size)
        return;
    while (name && *name && o + 1 < size && o < 32) {
        uint32_t c = rx_utf8_next(&name);
        buf[o++] = c >= 0x20 && c < 0x7F ? (char)c : '_';
    }
    buf[o] = '\0';
}
