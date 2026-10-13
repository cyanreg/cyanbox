/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* Positions and key names as people write them (rxbox/notation.h).
 *
 * Positions follow dj's sidecar syntax, which people already type: plain
 * seconds, "m:ss.mmm" and "h:mm:ss" (dj's (?:(\d+):)?(\d+):(\d+(?:\.\d*)?)),
 * "bar N" / "bar N.B" and "beat N" (bar\s+(\d+)(?:\.(\d+))? and
 * beat\s+-?(\d+(?:\.\d*)?), case-insensitive, trimmed; the sign is new, for
 * fractions of a beat in the pickup bar before the first downbeat). Bars are real bars of
 * the grid's numbering (see grid.c), counted from the first downbeat;
 * "beat N" is the Nth beat from that downbeat, so "beat 129" is "bar 33"
 * in 4/4. A beat number the bar lacks counts on from the bar's first beat,
 * as dj's arithmetic did ("bar 2.5" is "bar 3" in 4/4).
 *
 * Keys follow dj's analysis/key.py: its spellings (C Db D Eb E F F# G Ab A
 * Bb B, minor with "m", as rekordbox displays them) and its Camelot and
 * Open Key tables. The Traffic Light's reaches are rekordbox's, as its
 * Preferences tooltip spells them out for a track in 2A (Same Key 2A;
 * Related Key 1 2A/2B; Related Key 2 2A/2B/1A/3A; Related Key 3
 * 2A/2B/1A/3A/1B/3B), the rule rbxport's src/lib/camelot.ts
 * (trafficLightCodes, trafficLightLit) applies. */

#include "rxbox/notation.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <strings.h>

#include "core.h"

/* -------------------------------------------------------------- positions */

/* Lower-cased, trimmed copy; false when it does not fit. */
static bool normalise_spec(const char *spec, char *out, size_t size)
{
    while (isspace((unsigned char)*spec))
        spec++;
    size_t n = strlen(spec);
    while (n && isspace((unsigned char)spec[n - 1]))
        n--;
    if (n >= size)
        return false;
    for (size_t i = 0; i < n; i++)
        out[i] = (char)tolower((unsigned char)spec[i]);
    out[n] = '\0';
    return true;
}

static bool all_digits(const char *s, size_t n)
{
    if (!n)
        return false;
    for (size_t i = 0; i < n; i++) {
        if (!isdigit((unsigned char)s[i]))
            return false;
    }
    return true;
}

/* \d+(\.\d*)? as a whole string. */
static bool parse_decimal(const char *s, double *out)
{
    size_t int_len = strspn(s, "0123456789");
    if (!int_len)
        return false;
    const char *p = s + int_len;
    if (*p == '.')
        p += 1 + strspn(p + 1, "0123456789");
    if (*p)
        return false;
    *out = strtod(s, NULL);
    return isfinite(*out);
}

static bool parse_clock(const char *s, double *out)
{
    const char *c1 = strchr(s, ':');
    if (!c1)
        return false;
    const char *c2 = strchr(c1 + 1, ':');
    if (c2 && strchr(c2 + 1, ':'))
        return false;

    double h = 0, m, sec;
    if (c2) {
        if (!all_digits(s, (size_t)(c1 - s)) || !all_digits(c1 + 1, (size_t)(c2 - c1 - 1)))
            return false;
        h = strtod(s, NULL);
        m = strtod(c1 + 1, NULL);
        if (!parse_decimal(c2 + 1, &sec))
            return false;
    } else {
        if (!all_digits(s, (size_t)(c1 - s)))
            return false;
        m = strtod(s, NULL);
        if (!parse_decimal(c1 + 1, &sec))
            return false;
    }
    *out = h * 3600 + m * 60 + sec;
    return true;
}

/* Skips "word" and the white space after it; NULL unless both are there. */
static const char *after_word(const char *s, const char *word)
{
    size_t n = strlen(word);
    if (strncmp(s, word, n) || !isspace((unsigned char)s[n]))
        return NULL;
    s += n;
    while (isspace((unsigned char)*s))
        s++;
    return s;
}

static bool parse_bar(const char *s, int64_t *bar, int64_t *beat)
{
    s = after_word(s, "bar");
    if (!s)
        return false;
    size_t n = strspn(s, "0123456789");
    if (!n || n > 9)
        return false;
    *bar = strtoll(s, NULL, 10);
    *beat = 1;
    s += n;
    if (*s == '.') {
        size_t m = strspn(s + 1, "0123456789");
        if (!m || m > 9)
            return false;
        *beat = strtoll(s + 1, NULL, 10);
        s += 1 + m;
    }
    return !*s;
}

/* Signed, so a fraction of a beat inside the pickup bar has a name too. */
static bool parse_beat(const char *s, double *n)
{
    s = after_word(s, "beat");
    if (!s)
        return false;
    bool neg = *s == '-';
    if (!parse_decimal(s + neg, n) || *n >= 1e9)
        return false;
    if (neg)
        *n = -*n;
    return true;
}

int rx_parse_position(const char *spec, const RXGrid *grid, double *time, double *grid_beat)
{
    char s[64];
    if (!spec || !normalise_spec(spec, s, sizeof(s)) || !s[0])
        return RX_ERROR(EINVAL);

    double v, gb = NAN;
    int64_t bar, beat;
    if (parse_clock(s, &v)) {
        /* v is the time */
    } else if (parse_bar(s, &bar, &beat)) {
        if (!rx_grid_valid(grid))
            return RX_ERROR(EINVAL);
        gb = rx_core_grid_bar_index(grid, bar, beat);
        v = rx_grid_beat_to_time(grid, gb);
    } else if (parse_beat(s, &v)) {
        if (!rx_grid_valid(grid))
            return RX_ERROR(EINVAL);
        gb = v - 1;
        v = rx_grid_beat_to_time(grid, gb);
    } else {
        /* Plain seconds, as Python's float() would take them. */
        char *end;
        v = strtod(s, &end);
        if (end == s || *end || !isfinite(v))
            return RX_ERROR(EINVAL);
    }

    *time = v;
    if (grid_beat)
        *grid_beat = gb;
    return 0;
}

const char *rx_format_grid_position(const RXGrid *grid, double grid_beat, char *buf, size_t size)
{
    if (!size)
        return NULL;
    buf[0] = '\0';
    if (!isfinite(grid_beat) || !rx_grid_valid(grid))
        return NULL;

    /* Within a millionth of a beat is on the beat: grid_beat of a quantised
     * time comes out a few ulps off. Beyond a trillion beats (a damaged row;
     * that is 15000 years at 120 BPM) the bar arithmetic would overflow, so
     * such an index is only written as a number. */
    double whole = round(grid_beat);
    if (fabs(whole) >= 1e12) {
        snprintf(buf, size, "beat %g", grid_beat + 1);
        return buf;
    }
    if (fabs(grid_beat - whole) < 1e-6) {
        int64_t bar;
        int beat;
        rx_core_grid_describe(grid, (int64_t)whole, &bar, &beat);
        if (bar >= 0) {
            if (beat == 1)
                snprintf(buf, size, "bar %lld", (long long)bar);
            else
                snprintf(buf, size, "bar %lld.%d", (long long)bar, beat);
            return buf;
        }
    }

    /* Six decimals: a millionth of a beat is well under a sample. */
    double n = grid_beat + 1;
    char num[48];
    snprintf(num, sizeof(num), "%.6f", n);
    char *p = num + strlen(num);
    while (p > num && p[-1] == '0')
        *--p = '\0';
    if (p > num && p[-1] == '.')
        *--p = '\0';
    snprintf(buf, size, "beat %s", num);
    return buf;
}

/* ------------------------------------------------------------------- keys */

static const char *const key_names[12] = {
    "C", "Db", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B",
};
/* Camelot number of each tonic pitch class (dj's _CAMELOT_MAJOR/MINOR). */
static const int camelot_major[12] = { 8, 3, 10, 5, 12, 7, 2, 9, 4, 11, 6, 1 };
static const int camelot_minor[12] = { 5, 12, 7, 2, 9, 4, 11, 6, 1, 8, 3, 10 };

static int camelot_of(int tonic, bool minor)
{
    return (minor ? camelot_minor : camelot_major)[tonic];
}

/* Open Key is Camelot turned by four: 8A (A minor) is 1m, 8B (C major) 1d. */
static int open_of(int tonic, bool minor)
{
    return (camelot_of(tonic, minor) + 4) % 12 + 1;
}

static bool word_is(const char *s, size_t n, const char *word)
{
    return strlen(word) == n && !strncasecmp(s, word, n);
}

/* Classic ("F#m", "Bb", "A minor", "E♭ min"), Camelot ("8A") or Open Key
 * ("1m"), as dj's Key.parse. The mode words are matched in any case, but
 * a lone "m" is minor and a lone "M" major, as there. */
static int key_parse(const char *in, int *tonic, bool *minor)
{
    char s[32];
    if (!normalise_spec(in, s, sizeof(s)) || !s[0])
        return RX_ERROR(EINVAL);
    while (isspace((unsigned char)*in))
        in++;

    size_t digits = strspn(s, "0123456789");
    if (digits) {
        int num = atoi(s);
        const char *suffix = s + digits;
        if (digits > 2 || num < 1 || num > 12 || strlen(suffix) != 1)
            return RX_ERROR(EINVAL);
        for (int k = 0; k < 12; k++) {
            for (int m = 0; m < 2; m++) {
                if ((*suffix == (m ? 'a' : 'b') && camelot_of(k, m) == num) ||
                    (*suffix == (m ? 'm' : 'd') && open_of(k, m) == num)) {
                    *tonic = k;
                    *minor = m;
                    return 0;
                }
            }
        }
        return RX_ERROR(EINVAL);
    }

    /* Pitch classes of A..G. */
    static const int letter_pc[7] = { 9, 11, 0, 2, 4, 5, 7 };
    char letter = (char)toupper((unsigned char)in[0]);
    if (letter < 'A' || letter > 'G')
        return RX_ERROR(EINVAL);
    int pc = letter_pc[letter - 'A'];
    const char *p = in + 1;
    if (*p == '#') {
        pc++;
        p++;
    } else if (*p == 'b') {
        pc--;
        p++;
    } else if (!strncmp(p, "\xe2\x99\xaf", 3)) {     /* U+266F sharp */
        pc++;
        p += 3;
    } else if (!strncmp(p, "\xe2\x99\xad", 3)) {     /* U+266D flat */
        pc--;
        p += 3;
    }
    while (isspace((unsigned char)*p))
        p++;
    size_t n = strlen(p);
    while (n && isspace((unsigned char)p[n - 1]))
        n--;

    bool is_minor;
    if (!n || (n == 1 && *p == 'M') || word_is(p, n, "maj") || word_is(p, n, "major"))
        is_minor = false;
    else if ((n == 1 && *p == 'm') || word_is(p, n, "min") || word_is(p, n, "minor"))
        is_minor = true;
    else
        return RX_ERROR(EINVAL);

    *tonic = (pc + 12) % 12;
    *minor = is_minor;
    return 0;
}

int rx_key_normalise(const char *in, char *classic, size_t size)
{
    if (!size)
        return RX_ERROR(EINVAL);
    classic[0] = '\0';
    /* Empty means unknown, which is a valid answer. */
    if (!in || !in[strspn(in, " \t\r\n")])
        return 0;

    int tonic;
    bool minor;
    int ret = key_parse(in, &tonic, &minor);
    if (ret < 0)
        return ret;
    if (snprintf(classic, size, "%s%s", key_names[tonic], minor ? "m" : "") >= (int)size) {
        classic[0] = '\0';
        return RX_ERROR(ERANGE);
    }
    return 0;
}

const char *rx_key_to_camelot(const char *classic, char *buf, size_t size)
{
    int tonic;
    bool minor;
    if (!size)
        return buf;
    buf[0] = '\0';
    if (classic && key_parse(classic, &tonic, &minor) >= 0)
        snprintf(buf, size, "%d%c", camelot_of(tonic, minor), minor ? 'A' : 'B');
    return buf;
}

const char *rx_key_to_open(const char *classic, char *buf, size_t size)
{
    int tonic;
    bool minor;
    if (!size)
        return buf;
    buf[0] = '\0';
    if (classic && key_parse(classic, &tonic, &minor) >= 0)
        snprintf(buf, size, "%d%c", open_of(tonic, minor), minor ? 'm' : 'd');
    return buf;
}

const char *rx_key_display(const char *classic, const char *notation, char *buf, size_t size)
{
    if (notation && !strcmp(notation, "camelot"))
        return rx_key_to_camelot(classic, buf, size);
    if (notation && !strcmp(notation, "open"))
        return rx_key_to_open(classic, buf, size);
    if (size && rx_key_normalise(classic, buf, size) < 0)
        buf[0] = '\0';
    return buf;
}

/* ---------------------------------------------------------- traffic light */

static const char *const reach_names[] = { "off", "same", "related1", "related2", "related3" };

const char *rx_key_reach_name(RXKeyReach reach)
{
    if ((int)reach < 0 || (int)reach >= (int)RX_ARRAY_ELEMS(reach_names))
        return NULL;
    return reach_names[reach];
}

int rx_key_reach_parse(const char *name)
{
    for (int i = 0; name && i < (int)RX_ARRAY_ELEMS(reach_names); i++) {
        if (!strcmp(name, reach_names[i]))
            return i;
    }
    return -1;
}

/* A key's place on the Camelot wheel: its number and its letter (A minor,
 * B major). */
static bool wheel_of(const char *key, int *num, bool *minor)
{
    char code[8];
    if (!key || !rx_key_to_camelot(key, code, sizeof(code))[0])
        return false;
    char *letter;
    *num = (int)strtol(code, &letter, 10);
    *minor = *letter == 'A';
    return true;
}

bool rx_key_traffic_light(const char *key, const char *reference, RXKeyReach reach)
{
    int n, k;
    bool ref_minor, key_minor;
    if (reach <= RX_KEY_REACH_OFF || !wheel_of(reference, &n, &ref_minor) ||
        !wheel_of(key, &k, &key_minor))
        return false;

    /* Steps clockwise from the reference, 0..11: 1 and 11 are the
     * neighbours either side, 12 and 1 included. */
    int step = ((k - n) % 12 + 12) % 12;
    bool same_letter = key_minor == ref_minor;
    if (step == 0)
        return same_letter || reach >= RX_KEY_REACH_RELATED1;
    if (step == 1 || step == 11)
        return reach >= (same_letter ? RX_KEY_REACH_RELATED2 : RX_KEY_REACH_RELATED3);
    return false;
}
