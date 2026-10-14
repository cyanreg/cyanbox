/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* rekordbox's colours.
 *
 * Hot cues carry a colour code (1..64) and, after it in the PCP2 entry, the
 * RGB the players light the pad with. dj stored rekordbox's on-screen
 * colours (beat-link's findRekordboxColor) and derived the pad RGB by
 * stretching each to full saturation, which matched rekordbox for the ten
 * codes it had seen and got 48 of the other 52 wrong. The table below is
 * the one rbxport extracted from rekordbox 7 (djplay::getDevicePadColor)
 * and from CDJ-3000 firmware 3.20, and cross-checked against the RGB
 * rekordbox writes into exported PCO2 entries
 * (rbxport crates/rbl-anlz/src/cues.rs DEVICE_PALETTE and
 * crates/rbl-anlz/src/lib.rs MEASURED_CUE_COLOURS; facts only, rbxport is
 * GPL). Every value dj had measured (docs/anlz.md 4.6.1) agrees with it.
 *
 * The sixteen names and the codes they select are dj's (formats/colors.py
 * HOT_CUE_NAMES), ordered by code, which is rekordbox's hue order; only
 * their RGB comes from the measured table now.
 *
 * Track and memory cue colours are the eight rows of the colour table
 * (ids 1..8, docs/pdb.md colors); their RGB is how rekordbox 7.2.19 draws
 * them (rekordbox::cPink ... ColorTheme, as rbxport's tokens.css records).
 * In the library a track's colour is a hue instead of one of the eight;
 * the export writes the entry whose hue, computed from that RGB, is
 * nearest around the circle, so each of the eight hues comes back as its
 * own entry and anything between goes to the closer neighbour. The
 * interface's swatches and the tagging model set one of the eight hues;
 * rx_track_color_of_hue tells those from a hue between them, which an
 * older library may hold. */

#include "rxbox/colors.h"

#include <math.h>
#include <stdio.h>
#include <strings.h>

#include "rxbox/model.h"

/* RGB rekordbox stores in PCP2 per colour code, and lights the pads with.
 * 0 is the "no colour" sentinel, 63 and 64 are black and white. */
static const uint32_t device_palette[65] = {
    0x000000, 0x0000FF, 0x001CFF, 0x0038FF, 0x0054FF, 0x0070FF, 0x008CFF, 0x00A8FF,
    0x00C4FF, 0x00E0FF, 0x00FFFF, 0x00FFE8, 0x00FFD1, 0x00FFBA, 0x00FFA3, 0x00FF8C,
    0x00FF75, 0x00FF5E, 0x00FF47, 0x00FF30, 0x00FF1A, 0x00FF00, 0x1AFF00, 0x33FF00,
    0x4DFF00, 0x66FF00, 0x80FF00, 0x99FF00, 0xB3FF00, 0xCCFF00, 0xE6FF00, 0xFFFF00,
    0xFFE800, 0xFFD100, 0xFFBA00, 0xFFA300, 0xFF8C00, 0xFF7500, 0xFF5E00, 0xFF4700,
    0xFF3000, 0xFF1A00, 0xFF0000, 0xFF0017, 0xFF002E, 0xFF0045, 0xFF005C, 0xFF0073,
    0xFF008A, 0xFF00A1, 0xFF00B8, 0xFF00CF, 0xFF00E6, 0xFF00FF, 0xE600FF, 0xCC00FF,
    0xB300FF, 0x9900FF, 0x8000FF, 0x6600FF, 0x4D00FF, 0x3300FF, 0x1A00FF, 0x000000,
    0xFFFFFF,
};

/* Codes 1..62 are the colours a user can pick; nearest-colour lookups stay
 * among them. */
#define PALETTE_FIRST 1
#define PALETTE_LAST  62

const char *const rx_hot_cue_color_names[RX_HOT_CUE_COLORS] = {
    "blue", "sky", "aqua", "teal", "mint", "green", "lime", "yellow",
    "orange", "red", "rose", "pink", "magenta", "violet", "purple", "lavender",
};

static const int hot_cue_name_codes[RX_HOT_CUE_COLORS] = {
    0x01, 0x08, 0x09, 0x0D, 0x13, 0x15, 0x17, 0x20,
    0x24, 0x2A, 0x2B, 0x2D, 0x35, 0x38, 0x3C, 0x3D,
};

const char *const rx_track_color_names[RX_TRACK_COLORS] = {
    "pink", "red", "orange", "yellow", "green", "aqua", "blue", "purple",
};

static const uint32_t track_color_rgb[RX_TRACK_COLORS] = {
    RX_TRACK_RGB_PINK, RX_TRACK_RGB_RED,   RX_TRACK_RGB_ORANGE, RX_TRACK_RGB_YELLOW,
    RX_TRACK_RGB_GREEN, RX_TRACK_RGB_AQUA, RX_TRACK_RGB_BLUE,   RX_TRACK_RGB_PURPLE,
};

/* Pad colour of a hot cue without a colour. A to D: the RGB rekordbox 7
 * writes with code 0 for those slots (rbxport cues.rs export_colour: codes
 * 43, 8, 23 and 60 of the table above, read from rekordbox exports). E to
 * H: rbxport has no reading; dj's 00FF00, the most common code-0 RGB in a
 * rekordbox 7 stick (docs/anlz.md 4.6, 75 entries). */
static const uint32_t default_pad_rgb[RX_HOT_CUE_SLOTS] = {
    0xFF0017, 0x00C4FF, 0x33FF00, 0x4D00FF, 0x00FF00, 0x00FF00, 0x00FF00, 0x00FF00,
};

static bool name_eq(const char *a, const char *b)
{
    while (*a == ' ' || *a == '\t')
        a++;
    size_t n = strlen(a);
    while (n && (a[n - 1] == ' ' || a[n - 1] == '\t'))
        n--;
    return strlen(b) == n && !strncasecmp(a, b, n);
}

static uint32_t rgb_distance(uint32_t a, uint32_t b)
{
    uint32_t d = 0;
    for (int s = 0; s < 24; s += 8) {
        int x = (int)(a >> s & 0xFF) - (int)(b >> s & 0xFF);
        d += (uint32_t)(x * x);
    }
    return d;
}

int rx_parse_hex_rgb(const char *s, uint32_t *rgb)
{
    if (!s)
        return RX_ERROR(EINVAL);
    while (*s == ' ')
        s++;
    if (*s == '#')
        s++;
    uint32_t v = 0;
    int n = 0;
    for (; s[n] && s[n] != ' '; n++) {
        int c = s[n], d;
        if (c >= '0' && c <= '9')
            d = c - '0';
        else if (c >= 'a' && c <= 'f')
            d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F')
            d = c - 'A' + 10;
        else
            return RX_ERROR(EINVAL);
        v = v << 4 | (uint32_t)d;
    }
    if (n != 6)
        return RX_ERROR(EINVAL);
    *rgb = v;
    return 0;
}

/* ----------------------------------------------------------- track colours */

int rx_track_color_id(const char *name)
{
    if (!name)
        return 0;
    for (int i = 0; i < RX_TRACK_COLORS; i++)
        if (name_eq(name, rx_track_color_names[i]))
            return i + 1;
    return 0;
}

const char *rx_track_color_name(int id)
{
    return id >= 1 && id <= RX_TRACK_COLORS ? rx_track_color_names[id - 1] : NULL;
}

uint32_t rx_track_color_rgb(int id)
{
    return id >= 1 && id <= RX_TRACK_COLORS ? track_color_rgb[id - 1] : 0;
}

/* --------------------------------------------------------------------- hue */

static uint32_t channel(double v)
{
    long c = lround(v * 255.0);
    return (uint32_t)RX_CLAMP(c, 0, 255);
}

uint32_t rx_hue_rgb(double hue, double saturation, double value)
{
    double h = isfinite(hue) ? fmod(hue, 360.0) : 0.0;
    if (h < 0)
        h += 360.0;
    /* A hue a hair below 0 wraps to 360.0 itself. */
    if (h >= 360.0)
        h = 0.0;
    double v = RX_CLAMP(value, 0.0, 1.0);
    double c = v * RX_CLAMP(saturation, 0.0, 1.0);
    double x = c * (1.0 - fabs(fmod(h / 60.0, 2.0) - 1.0));
    double r = 0, g = 0, b = 0;
    switch ((int)(h / 60.0)) {
    case 0:  r = c; g = x; break;
    case 1:  r = x; g = c; break;
    case 2:  g = c; b = x; break;
    case 3:  g = x; b = c; break;
    case 4:  r = x; b = c; break;
    default: r = c; b = x; break;
    }
    double m = v - c;
    return channel(r + m) << 16 | channel(g + m) << 8 | channel(b + m);
}

double rx_rgb_hue(uint32_t rgb)
{
    int r = (int)(rgb >> 16 & 0xFF), g = (int)(rgb >> 8 & 0xFF), b = (int)(rgb & 0xFF);
    int max = RX_MAX(r, RX_MAX(g, b)), min = RX_MIN(r, RX_MIN(g, b));
    if (max == min)
        return -1.0;
    double d = max - min, h;
    if (max == r)
        h = (g - b) / d;
    else if (max == g)
        h = (b - r) / d + 2.0;
    else
        h = (r - g) / d + 4.0;
    h *= 60.0;
    return h < 0 ? h + 360.0 : h;
}

double rx_track_color_hue(int id)
{
    return id >= 1 && id <= RX_TRACK_COLORS ? rx_rgb_hue(track_color_rgb[id - 1]) : -1.0;
}

int rx_track_color_nearest_hue(double hue)
{
    if (!isfinite(hue) || hue < 0)
        return 0;
    hue = fmod(hue, 360.0);
    /* Ties (330 sits halfway between pink and red) go to the lower id. */
    int best = 0;
    double best_d = INFINITY;
    for (int i = 0; i < RX_TRACK_COLORS; i++) {
        double d = fabs(hue - rx_rgb_hue(track_color_rgb[i]));
        d = RX_MIN(d, 360.0 - d);
        if (d < best_d) {
            best_d = d;
            best = i + 1;
        }
    }
    return best;
}

int rx_track_color_of_hue(double hue)
{
    int id = rx_track_color_nearest_hue(hue);
    if (!id)
        return 0;
    double d = fabs(fmod(hue, 360.0) - rx_track_color_hue(id));
    return RX_MIN(d, 360.0 - d) < 0.01 ? id : 0;
}

uint32_t rx_track_hue_rgb(double hue)
{
    if (!isfinite(hue) || hue < 0)
        return 0;
    return rx_hue_rgb(hue, 1.0, 0xF8 / 255.0);
}

int rx_parse_hue(const char *s, double *hue)
{
    if (!s)
        return RX_ERROR(EINVAL);
    while (*s == ' ' || *s == '\t')
        s++;
    if (!*s || name_eq(s, "none")) {
        *hue = -1.0;
        return 0;
    }
    int id = rx_track_color_id(s);
    if (id) {
        *hue = rx_track_color_hue(id);
        return 0;
    }
    char *end;
    double v = strtod(s, &end);
    if (end == s || !isfinite(v) || v < 0 || v > 360)
        return RX_ERROR(EINVAL);
    if (!strncmp(end, "\xC2\xB0", 2))
        end += 2;
    while (*end == ' ' || *end == '\t')
        end++;
    if (*end)
        return RX_ERROR(EINVAL);
    /* Degrees only where they name one of the eight: a colour between
     * them is not one the library keeps (src/core/clean.c). */
    int named = rx_track_color_of_hue(v == 360 ? 0.0 : v);
    if (!named)
        return RX_ERROR(EINVAL);
    *hue = rx_track_color_hue(named);
    return 0;
}

const char *rx_hue_format(double hue, char *buf, size_t size)
{
    int id = rx_track_color_nearest_hue(hue);
    if (!id) {
        snprintf(buf, size, "none");
        return buf;
    }
    char deg[16];
    snprintf(deg, sizeof(deg), "%.1f", fmod(hue, 360.0));
    size_t n = strlen(deg);
    if (n > 2 && !strcmp(deg + n - 2, ".0"))
        deg[n - 2] = '\0';
    snprintf(buf, size, "%s\xC2\xB0 (%s)", deg, rx_track_color_names[id - 1]);
    return buf;
}

/* --------------------------------------------------------------- hot cues */

uint32_t rx_cue_palette_rgb(int code)
{
    return code >= 1 && code <= 64 ? device_palette[code] : 0;
}

int rx_hot_cue_led(int code)
{
    /* The PCP2 entry stores the code followed by this RGB (docs/anlz.md
     * 4.6: "The RGB bytes are an LED colour"); it is the same table. */
    return (int)rx_cue_palette_rgb(code);
}

uint32_t rx_hot_cue_default_rgb(int slot)
{
    return slot >= 0 && slot < RX_HOT_CUE_SLOTS ? default_pad_rgb[slot] : 0x00FF00;
}

static int nearest_code(uint32_t rgb)
{
    int best = PALETTE_FIRST;
    uint32_t best_d = UINT32_MAX;
    for (int c = PALETTE_FIRST; c <= PALETTE_LAST; c++) {
        uint32_t d = rgb_distance(rgb, device_palette[c]);
        if (d < best_d) {
            best_d = d;
            best = c;
        }
    }
    return best;
}

int rx_hot_cue_color(const char *name_or_hex, int *code, uint32_t *rgb)
{
    *code = 0;
    *rgb = 0;
    if (!name_or_hex)
        return 0;
    const char *s = name_or_hex;
    while (*s == ' ')
        s++;
    if (!*s)
        return 0;

    for (int i = 0; i < RX_HOT_CUE_COLORS; i++) {
        if (name_eq(s, rx_hot_cue_color_names[i])) {
            *code = hot_cue_name_codes[i];
            *rgb = device_palette[*code];
            return 0;
        }
    }
    /* dj also took "cyan" for aqua. */
    if (name_eq(s, "cyan")) {
        *code = 0x09;
        *rgb = device_palette[*code];
        return 0;
    }

    uint32_t want;
    if (s[0] == '#' && !rx_parse_hex_rgb(s, &want)) {
        *code = nearest_code(want);
        *rgb = device_palette[*code];
        return 0;
    }

    /* A palette code, "0x2a" or "42", as dj accepted. */
    char *end;
    long v = strtol(s, &end, 0);
    while (*end == ' ')
        end++;
    if (end != s && !*end && v >= 1 && v <= 64) {
        *code = (int)v;
        *rgb = device_palette[*code];
        return 0;
    }
    if (!rx_parse_hex_rgb(s, &want)) {
        *code = nearest_code(want);
        *rgb = device_palette[*code];
        return 0;
    }
    return RX_ERROR(EINVAL);
}

const char *rx_cue_palette_name(int code)
{
    if (code < 1 || code > 64)
        return NULL;
    int best = 0;
    uint32_t best_d = UINT32_MAX;
    for (int i = 0; i < RX_HOT_CUE_COLORS; i++) {
        if (hot_cue_name_codes[i] == code)
            return rx_hot_cue_color_names[i];
        uint32_t d = rgb_distance(device_palette[code], device_palette[hot_cue_name_codes[i]]);
        if (d < best_d) {
            best_d = d;
            best = i;
        }
    }
    return rx_hot_cue_color_names[best];
}

/* ------------------------------------------------------------ memory cues */

int rx_memory_cue_color_id(const char *name)
{
    if (!name)
        return 0;
    const char *s = name;
    while (*s == ' ')
        s++;
    if (!*s)
        return 0;
    int id = rx_track_color_id(s);
    if (id)
        return id;
    /* dj's aliases: the hot cue names outside the eight map to the nearest. */
    static const struct { const char *alias, *name; } aliases[] = {
        { "cyan", "aqua" }, { "sky", "aqua" }, { "teal", "aqua" }, { "mint", "green" },
        { "lime", "green" }, { "magenta", "pink" }, { "rose", "pink" }, { "violet", "purple" },
        { "lavender", "purple" },
    };
    for (size_t i = 0; i < RX_ARRAY_ELEMS(aliases); i++)
        if (name_eq(s, aliases[i].alias))
            return rx_track_color_id(aliases[i].name);
    uint32_t want;
    if (!rx_parse_hex_rgb(s, &want)) {
        int best = 1;
        uint32_t best_d = UINT32_MAX;
        for (int i = 0; i < RX_TRACK_COLORS; i++) {
            uint32_t d = rgb_distance(want, track_color_rgb[i]);
            if (d < best_d) {
                best_d = d;
                best = i + 1;
            }
        }
        return best;
    }
    return RX_ERROR(EINVAL);
}

uint32_t rx_memory_cue_color_rgb(int id)
{
    return rx_track_color_rgb(id);
}
