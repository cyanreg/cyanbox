/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* The stick plan: what goes where, with which ids, resolved once so that
 * export.pdb, exportLibrary.db and the ANLZ files describe exactly the same
 * library. A port of dj's plan.py and export.py plan_stick, with cyanbox's
 * changes:
 *
 *  - track ids are the library ids, so players keep their cached data
 *    across exports (docs/OVERVIEW.md 3.4); playlist and folder ids are the
 *    library's too;
 *  - name-table ids, artwork ids and ANLZ collision numbers are assigned as
 *    dj did: in first-use order over the tracks sorted by path; remixers,
 *    composers and original artists are artists (one table in both
 *    databases), so CBPlan's separate lists for them stay empty;
 *  - cue colours come from the measured palette (cyanbox/colors.h); a
 *    track's colour is a hue in the library, written as the nearest of
 *    rekordbox's eight by circular hue distance;
 *  - the cover is the embedded picture or the album folder's image
 *    (cb_artwork_raw), deduplicated by a SHA-1 of its bytes so an album's
 *    tracks share one artwork row (dj hashed the image the same way);
 *  - a plan for Link (CB_PLAN_STORED) reads no audio: the files are
 *    described from what the library stored when it probed them and the
 *    covers come from the library's thumbnails, one row per stored cover.
 *
 * Stick paths follow rekordbox's layout (docs/pdb.md 1.2):
 * /Contents/<Artist>/<Album>/<file>, the stem cut to 43 characters, FAT-
 * illegal characters replaced, collisions numbered -1, -2 ...; names are
 * NFC (dj's _clean). */

#include "cyanbox/formats.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <libavutil/mem.h>
#include <libavutil/sha.h>

#include "cyanbox/colors.h"
#include "cyanbox/notation.h"
#include "src/export/export_priv.h"
#include "src/formats/formats_priv.h"

/* ------------------------------------------------------------- string map */

/* String -> u32, open addressing; keys are copies. */
typedef struct StrMap {
    char    **keys;
    uint32_t *vals;
    size_t    cap, nb;
} StrMap;

static uint64_t hash_str(const char *s)
{
    uint64_t h = 1469598103934665603ull;
    for (; *s; s++)
        h = (h ^ (uint8_t)*s) * 1099511628211ull;
    return h;
}

static uint32_t *strmap_get(const StrMap *m, const char *key)
{
    if (!m->cap)
        return NULL;
    for (size_t i = hash_str(key) & (m->cap - 1);; i = (i + 1) & (m->cap - 1)) {
        if (!m->keys[i])
            return NULL;
        if (!strcmp(m->keys[i], key))
            return &m->vals[i];
    }
}

static int strmap_put(StrMap *m, const char *key, uint32_t val)
{
    if ((m->nb + 1) * 10 > m->cap * 7) {
        size_t cap = m->cap ? m->cap * 2 : 64;
        char **keys = calloc(cap, sizeof(*keys));
        uint32_t *vals = calloc(cap, sizeof(*vals));
        if (!keys || !vals) {
            free(keys);
            free(vals);
            return CB_ERROR(ENOMEM);
        }
        for (size_t i = 0; i < m->cap; i++) {
            if (!m->keys[i])
                continue;
            size_t k = hash_str(m->keys[i]) & (cap - 1);
            while (keys[k])
                k = (k + 1) & (cap - 1);
            keys[k] = m->keys[i];
            vals[k] = m->vals[i];
        }
        free(m->keys);
        free(m->vals);
        m->keys = keys;
        m->vals = vals;
        m->cap = cap;
    }
    size_t i = hash_str(key) & (m->cap - 1);
    while (m->keys[i])
        i = (i + 1) & (m->cap - 1);
    if (!(m->keys[i] = cb_strdup(key)))
        return CB_ERROR(ENOMEM);
    m->vals[i] = val;
    m->nb++;
    return 0;
}

static void strmap_free(StrMap *m)
{
    for (size_t i = 0; i < m->cap; i++)
        free(m->keys[i]);
    free(m->keys);
    free(m->vals);
    *m = (StrMap) { 0 };
}

/* -------------------------------------------------------------- names */

/* FAT and exFAT compare names without case, and exFAT up-cases both with
 * the volume's up-case table, code unit by code unit, characters beyond
 * the BMP compared as they are (the Linux driver, fs/exfat/nls.c, and
 * Windows alike). Every formatter writes the table the exFAT specification
 * recommends (Microsoft, 2019, section 7.2.5.1; checksum 0xE619D30D): two
 * names it up-cases alike are one file, so two tracks named Șerban and
 * șerban must not share a path. Its 874 mappings as runs, read out of the
 * table mkfs.exfat (exfatprogs 1.4.3) wrote to a new volume and checked
 * against it code point by code point: every code point from `first` to
 * `last` in steps of `step` up-cases to itself plus `delta`. dj folded
 * with str.lower(), which differs in a few places (ǅ, the Kelvin sign);
 * the stick decides. Linux's vfat folds less (ASCII), so there a name this
 * folds together is at worst kept apart needlessly. */
typedef struct UpcaseRun {
    uint16_t first, last;
    int16_t  delta;
    uint8_t  step;
} UpcaseRun;

static const UpcaseRun upcase_runs[] = {
    { 0x0061, 0x007A, -32, 1 }, { 0x00E0, 0x00F6, -32, 1 }, { 0x00F8, 0x00FE, -32, 1 },
    { 0x00FF, 0x00FF, 121, 1 }, { 0x0101, 0x012F, -1, 2 }, { 0x0133, 0x0137, -1, 2 },
    { 0x013A, 0x0148, -1, 2 }, { 0x014B, 0x0177, -1, 2 }, { 0x017A, 0x017E, -1, 2 },
    { 0x0180, 0x0180, 195, 1 }, { 0x0183, 0x0185, -1, 2 }, { 0x0188, 0x0188, -1, 1 },
    { 0x018C, 0x018C, -1, 1 }, { 0x0192, 0x0192, -1, 1 }, { 0x0195, 0x0195, 97, 1 },
    { 0x0199, 0x0199, -1, 1 }, { 0x019A, 0x019A, 163, 1 }, { 0x019E, 0x019E, 130, 1 },
    { 0x01A1, 0x01A5, -1, 2 }, { 0x01A8, 0x01A8, -1, 1 }, { 0x01AD, 0x01AD, -1, 1 },
    { 0x01B0, 0x01B0, -1, 1 }, { 0x01B4, 0x01B6, -1, 2 }, { 0x01B9, 0x01B9, -1, 1 },
    { 0x01BD, 0x01BD, -1, 1 }, { 0x01BF, 0x01BF, 56, 1 }, { 0x01C6, 0x01C6, -2, 1 },
    { 0x01C9, 0x01C9, -2, 1 }, { 0x01CC, 0x01CC, -2, 1 }, { 0x01CE, 0x01DC, -1, 2 },
    { 0x01DD, 0x01DD, -79, 1 }, { 0x01DF, 0x01EF, -1, 2 }, { 0x01F3, 0x01F3, -2, 1 },
    { 0x01F5, 0x01F5, -1, 1 }, { 0x01F9, 0x021F, -1, 2 }, { 0x0223, 0x0233, -1, 2 },
    { 0x023A, 0x023A, 10795, 1 }, { 0x023C, 0x023C, -1, 1 }, { 0x023E, 0x023E, 10792, 1 },
    { 0x0242, 0x0242, -1, 1 }, { 0x0247, 0x024F, -1, 2 }, { 0x0253, 0x0253, -210, 1 },
    { 0x0254, 0x0254, -206, 1 }, { 0x0256, 0x0257, -205, 1 }, { 0x0259, 0x0259, -202, 1 },
    { 0x025B, 0x025B, -203, 1 }, { 0x0260, 0x0260, -205, 1 }, { 0x0263, 0x0263, -207, 1 },
    { 0x0268, 0x0268, -209, 1 }, { 0x0269, 0x0269, -211, 1 }, { 0x026B, 0x026B, 10743, 1 },
    { 0x026F, 0x026F, -211, 1 }, { 0x0272, 0x0272, -213, 1 }, { 0x0275, 0x0275, -214, 1 },
    { 0x027D, 0x027D, 10727, 1 }, { 0x0280, 0x0280, -218, 1 }, { 0x0283, 0x0283, -218, 1 },
    { 0x0288, 0x0288, -218, 1 }, { 0x0289, 0x0289, -69, 1 }, { 0x028A, 0x028B, -217, 1 },
    { 0x028C, 0x028C, -71, 1 }, { 0x0292, 0x0292, -219, 1 }, { 0x037B, 0x037D, 130, 1 },
    { 0x03AC, 0x03AC, -38, 1 }, { 0x03AD, 0x03AF, -37, 1 }, { 0x03B1, 0x03C1, -32, 1 },
    { 0x03C2, 0x03C2, -31, 1 }, { 0x03C3, 0x03CB, -32, 1 }, { 0x03CC, 0x03CC, -64, 1 },
    { 0x03CD, 0x03CE, -63, 1 }, { 0x03D9, 0x03EF, -1, 2 }, { 0x03F2, 0x03F2, 7, 1 },
    { 0x03F8, 0x03F8, -1, 1 }, { 0x03FB, 0x03FB, -1, 1 }, { 0x0430, 0x044F, -32, 1 },
    { 0x0450, 0x045F, -80, 1 }, { 0x0461, 0x0481, -1, 2 }, { 0x048B, 0x04BF, -1, 2 },
    { 0x04C2, 0x04CE, -1, 2 }, { 0x04CF, 0x04CF, -15, 1 }, { 0x04D1, 0x0513, -1, 2 },
    { 0x0561, 0x0586, -48, 1 }, { 0x1D7D, 0x1D7D, 3814, 1 }, { 0x1E01, 0x1E95, -1, 2 },
    { 0x1EA1, 0x1EF9, -1, 2 }, { 0x1F00, 0x1F07, 8, 1 }, { 0x1F10, 0x1F15, 8, 1 },
    { 0x1F20, 0x1F27, 8, 1 }, { 0x1F30, 0x1F37, 8, 1 }, { 0x1F40, 0x1F45, 8, 1 },
    { 0x1F51, 0x1F57, 8, 2 }, { 0x1F60, 0x1F67, 8, 1 }, { 0x1F70, 0x1F71, 74, 1 },
    { 0x1F72, 0x1F75, 86, 1 }, { 0x1F76, 0x1F77, 100, 1 }, { 0x1F78, 0x1F79, 128, 1 },
    { 0x1F7A, 0x1F7B, 112, 1 }, { 0x1F7C, 0x1F7D, 126, 1 }, { 0x1F80, 0x1F87, 8, 1 },
    { 0x1F90, 0x1F97, 8, 1 }, { 0x1FA0, 0x1FA7, 8, 1 }, { 0x1FB0, 0x1FB1, 8, 1 },
    { 0x1FB3, 0x1FB3, 9, 1 }, { 0x1FCC, 0x1FCC, -9, 1 }, { 0x1FD0, 0x1FD1, 8, 1 },
    { 0x1FE0, 0x1FE1, 8, 1 }, { 0x1FE5, 0x1FE5, 7, 1 }, { 0x1FFC, 0x1FFC, -9, 1 },
    { 0x214E, 0x214E, -28, 1 }, { 0x2170, 0x217F, -16, 1 }, { 0x2184, 0x2184, -1, 1 },
    { 0x24D0, 0x24E9, -26, 1 }, { 0x2C30, 0x2C5E, -48, 1 }, { 0x2C61, 0x2C61, -1, 1 },
    { 0x2C68, 0x2C6C, -1, 2 }, { 0x2C76, 0x2C76, -1, 1 }, { 0x2C81, 0x2CE3, -1, 2 },
    { 0x2D00, 0x2D25, -7264, 1 }, { 0xFF41, 0xFF5A, -32, 1 },
};

static uint32_t upcase(uint32_t c)
{
    if (c > 0xFFFF)
        return c;
    int lo = 0, hi = (int)CB_ARRAY_ELEMS(upcase_runs) - 1, k = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (upcase_runs[mid].first <= c) {
            k = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    if (k < 0)
        return c;
    const UpcaseRun *r = &upcase_runs[k];
    return c <= r->last && (c - r->first) % r->step == 0 ? (uint32_t)((int32_t)c + r->delta) : c;
}

char *cb_plan_fold_case(const char *s)
{
    char *out = malloc(4 * strlen(s) + 1);
    if (!out)
        return NULL;
    size_t o = 0;
    while (*s)
        o += (size_t)cb_utf8_put(out + o, upcase(cb_utf8_next(&s)));
    out[o] = '\0';
    return out;
}

/* Python's str.isspace() beyond the control characters, which are already
 * replaced by then. */
static bool is_space(uint32_t c)
{
    return c == ' ' || c == 0x85 || c == 0xA0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200A) ||
           c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000;
}

char *cb_plan_fat_name(const char *s, size_t limit)
{
    char *clean = cb_utf8_clean_nfc(s ? s : "");
    if (!clean)
        return NULL;
    size_t n = cb_utf8_length(clean);
    uint32_t *cp = malloc((n + 1) * sizeof(*cp));
    char *out = malloc(4 * n + 2);
    if (!cp || !out) {
        free(cp);
        free(out);
        free(clean);
        return NULL;
    }
    const char *p = clean;
    for (size_t i = 0; i < n; i++) {
        uint32_t c = cb_utf8_next(&p);
        /* dj's _ILLEGAL: control characters, DEL and " * / : < > ? \ | */
        if (c < 0x20 || c == 0x7F || (c < 0x80 && strchr("\"*/:<>?\\|", (int)c)))
            c = '_';
        cp[i] = c;
    }
    free(clean);
    size_t a = 0, b = n;
    while (a < b && is_space(cp[a]))
        a++;
    while (b > a && is_space(cp[b - 1]))
        b--;
    if (b - a > limit)
        b = a + limit;
    while (b > a && (cp[b - 1] == '.' || cp[b - 1] == ' '))
        b--;
    size_t o = 0;
    for (size_t i = a; i < b; i++)
        o += (size_t)cb_utf8_put(out + o, cp[i]);
    if (!o)
        out[o++] = '_';
    out[o] = '\0';
    free(cp);
    return out;
}

/* pathlib's stem and suffix of the base name (Python 3.14: no suffix when
 * only dots precede the last one, so "....mp3" is all stem). */
static void split_name(const char *path, size_t *stem_len, const char **suffix)
{
    const char *base = cb_path_base(path);
    const char *dot = strrchr(base, '.');
    bool dots_only = true;
    for (const char *p = base; dot && p < dot; p++)
        dots_only &= *p == '.';
    if (dot && dot != base && dot[1] && !dots_only) {
        *stem_len = (size_t)(dot - base);
        *suffix = dot;
    } else {
        *stem_len = strlen(base);
        *suffix = "";
    }
}

char *cb_plan_usb_stem(const CBTrack *t)
{
    size_t n;
    const char *suffix;
    split_name(t->path, &n, &suffix);
    char *stem = cb_strndup(cb_path_base(t->path), n);
    if (!stem)
        return NULL;
    char *fat = cb_plan_fat_name(stem, 43);
    free(stem);
    return fat;
}

/* The first `n` code points of s, allocating. */
static char *utf8_prefix(const char *s, size_t n)
{
    const char *p = s;
    for (size_t i = 0; i < n && *p; i++)
        cb_utf8_next(&p);
    return cb_strndup(s, (size_t)(p - s));
}

bool cb_plan_usb_name_matches(const CBTrack *t, const char *stick_path)
{
    char *stem = cb_plan_usb_stem(t);
    if (!stem)
        return false;
    size_t n;
    const char *suffix;
    split_name(stick_path, &n, &suffix);
    const char *base = cb_path_base(stick_path);
    bool ok = strlen(stem) == n && !memcmp(stem, base, n);
    if (!ok) {
        char *head = utf8_prefix(stem, 40);
        size_t hl = head ? strlen(head) : 0;
        if (head && n > hl + 1 && !memcmp(base, head, hl) && base[hl] == '-') {
            ok = true;
            for (size_t i = hl + 1; i < n; i++)
                ok &= base[i] >= '0' && base[i] <= '9';
        }
        free(head);
    }
    free(stem);
    return ok;
}

static const char *nonempty(const char *s)
{
    return s && *s ? s : NULL;
}

typedef struct UsedPaths {
    StrMap map;
} UsedPaths;

/* dj's usb_path(): the name from the library file, the extension from what
 * is copied (they differ for transcoded tracks). */
static char *usb_path(const CBTrack *t, const char *source, bool (*taken)(void *, const char *), void *opaque)
{
    const char *artist = nonempty(t->meta.album_artist) ? t->meta.album_artist : nonempty(t->meta.artist);
    char *a = artist ? cb_plan_fat_name(artist, 64) : cb_strdup("UnknownArtist");
    char *al = nonempty(t->meta.album) ? cb_plan_fat_name(t->meta.album, 64) : cb_strdup("UnknownAlbum");
    char *stem = cb_plan_usb_stem(t);
    char *head = stem ? utf8_prefix(stem, 40) : NULL;
    char *out = NULL;
    size_t n;
    const char *suffix;
    split_name(source, &n, &suffix);
    /* Lower case, and only ASCII letters and digits after the dot (every
     * audio extension is): FAT refuses " * : < > ? \ | and trailing spaces,
     * and a cut multi-byte character would not be UTF-8. */
    char ext[16];
    snprintf(ext, sizeof(ext), "%s", suffix);
    for (char *e = ext; *e; e++)
        *e = e == ext ? *e : isalnum((unsigned char)*e) ? (char)tolower((unsigned char)*e) : '_';
    if (a && al && stem && head) {
        for (int k = 0; k < 10000 && !out; k++) {
            char *p = k ? cb_asprintf("/Contents/%s/%s/%s-%d%s", a, al, head, k, ext)
                        : cb_asprintf("/Contents/%s/%s/%s%s", a, al, stem, ext);
            if (!p)
                break;
            if (!taken(opaque, p))
                out = p;
            else
                free(p);
        }
    }
    free(a);
    free(al);
    free(stem);
    free(head);
    return out;
}

typedef struct ListUsed {
    const char *const *used;
    int nb;
} ListUsed;

static bool list_taken(void *opaque, const char *p)
{
    ListUsed *l = opaque;
    char *fp = cb_plan_fold_case(p);
    bool taken = false;
    for (int i = 0; fp && i < l->nb && !taken; i++) {
        char *fu = cb_plan_fold_case(l->used[i]);
        taken = fu && !strcmp(fu, fp);
        free(fu);
    }
    free(fp);
    return taken;
}

char *cb_plan_usb_path(const CBTrack *t, const char *source, const char *const *used, int nb_used)
{
    ListUsed l = { used, nb_used };
    return usb_path(t, source ? source : t->path, list_taken, &l);
}

static bool map_taken(void *opaque, const char *p)
{
    StrMap *m = opaque;
    char *fp = cb_plan_fold_case(p);
    bool taken = !fp || strmap_get(m, fp);
    if (fp && !taken)
        strmap_put(m, fp, 1);
    free(fp);
    return taken;
}

/* ------------------------------------------------------------ transcodes */

bool cb_plan_codec_playable(const char *codec)
{
    static const char *const ok[] = {
        "mp3", "aac", "alac", "flac", "pcm_s16le", "pcm_s24le", "pcm_s32le", "pcm_s16be",
        "pcm_s24be", "pcm_s32be", "pcm_u8", "pcm_f32le",
    };
    for (size_t i = 0; i < CB_ARRAY_ELEMS(ok); i++)
        if (!strcmp(codec, ok[i]))
            return true;
    return false;
}

/* Whether the cache file is Link's WAV, by its name. */
static bool link_wav(const char *path)
{
    size_t n = strlen(path);
    return n > 9 && !strcmp(path + n - 9, ".link.wav");
}

int cb_plan_transcode_path(const CBLibrary *lib, const CBTrack *t, int compat, bool create,
                           char **cache_path, int *max_rate)
{
    *cache_path = NULL;
    bool need = !cb_plan_codec_playable(t->codec);
    bool hires = !strcmp(t->codec, "flac") && (t->bit_depth > 16 || t->sample_rate > 48000);
    bool for_compat = compat == CB_TRANSCODE_COMPAT && (!strcmp(t->codec, "aac") || hires);
    bool pcm16 = (!strcmp(t->codec, "pcm_s16le") || !strcmp(t->codec, "pcm_s16be")) &&
                 t->sample_rate <= 48000;
    bool for_wav = (compat == CB_TRANSCODE_LINK && strcmp(t->codec, "mp3") && !pcm16) ||
                   compat == CB_TRANSCODE_LINK_ANY;
    if (!need && !for_compat && !for_wav)
        return 0;
    /* dj kept up to 96 kHz for formats no player reads; compatibility mode
     * and Link go to 16 bit at 44.1/48 kHz. */
    *max_rate = for_compat || for_wav ? 48000 : 96000;
    const char *ext = for_wav ? "link.wav" : for_compat ? "compat.flac" : "flac";
    /* cb_library_cache_path's name, without creating its directory. */
    *cache_path = create ? cb_library_cache_path(lib, "transcode", t->id, ext)
                         : cb_asprintf("%s/cache/transcode/%llu.%s", lib->root, (unsigned long long)t->id, ext);
    return *cache_path ? 1 : CB_ERROR(ENOMEM);
}

bool cb_plan_transcode_fresh(const CBTrack *t, const char *cache)
{
    return cb_file_exists(cache) && cb_file_mtime(cache) >= cb_file_mtime(t->path);
}

int cb_plan_transcode(const CBLibrary *lib, const CBTrack *t, int compat, char **source)
{
    char *cache;
    int max_rate = 0;
    int ret = cb_plan_transcode_path(lib, t, compat, true, &cache, &max_rate);
    if (ret <= 0) {
        *source = ret < 0 ? NULL : cb_strdup(t->path);
        return ret < 0 ? ret : *source ? 0 : CB_ERROR(ENOMEM);
    }
    if (cb_plan_transcode_fresh(t, cache)) {
        *source = cache;
        return 0;
    }
    bool wav = link_wav(cache);
    char *tmp = cb_asprintf("%s.tmp.%s", cache, wav ? "wav" : "flac");
    if (!tmp) {
        free(cache);
        return CB_ERROR(ENOMEM);
    }
    ret = wav ? cb_transcode_wav(t->path, tmp, max_rate) : cb_transcode_flac(t->path, tmp, max_rate);
    if (ret >= 0 && rename(tmp, cache) < 0)
        ret = CB_ERROR(errno);
    if (ret < 0) {
        unlink(tmp);
        free(cache);
        cache = NULL;
    }
    free(tmp);
    *source = cache;
    return ret;
}

/* -------------------------------------------------------------- the plan */

typedef struct Names {
    CBArray arr;                 /* CBPlanName */
    StrMap  map;
} Names;

static int intern(Names *n, const char *name, uint32_t artist_id, bool album, uint32_t *id)
{
    *id = 0;
    if (!name || !*name)
        return 0;
    char *key = album ? cb_asprintf("%s\x1f%u", name, artist_id) : cb_strdup(name);
    if (!key)
        return CB_ERROR(ENOMEM);
    uint32_t *v = strmap_get(&n->map, key);
    if (v) {
        *id = *v;
        free(key);
        return 0;
    }
    CBPlanName pn = {
        .id        = (uint32_t)n->arr.nb + 1,
        .name      = cb_strdup(name),
        .artist_id = artist_id,
    };
    int ret = pn.name ? strmap_put(&n->map, key, pn.id) : CB_ERROR(ENOMEM);
    free(key);
    if (ret < 0 || !cb_array_push(&n->arr, &pn)) {
        free(pn.name);
        return ret < 0 ? ret : CB_ERROR(ENOMEM);
    }
    *id = pn.id;
    return 0;
}

static void names_take(Names *n, CBPlanName **out, int *nb)
{
    *out = n->arr.data;
    *nb = (int)n->arr.nb;
    n->arr.data = NULL;
    cb_array_free(&n->arr);
    strmap_free(&n->map);
}

static void names_free(Names *n)
{
    for (size_t i = 0; i < n->arr.nb; i++)
        free(((CBPlanName *)cb_array_at(&n->arr, i))->name);
    cb_array_free(&n->arr);
    strmap_free(&n->map);
}

static void report(cb_progress_cb cb, void *opaque, const char *fmt, ...) CB_PRINTF(3, 4);
static void report(cb_progress_cb cb, void *opaque, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char *msg = cb_vasprintf(fmt, ap);
    va_end(ap);
    if (msg) {
        if (cb)
            cb(opaque, "prepare", 0, 0, msg);
        else
            cb_warn("%s", msg);
    }
    free(msg);
}

/* dj's _tag(): "Situation/Peak Time" is the tag "Peak Time" in the My Tag
 * column "Situation"; a bare tag goes in the fourth column. */
static int tag_column(const char *tag, char **name)
{
    const char *slash = strchr(tag, '/');
    if (slash) {
        char *col = cb_strndup(tag, (size_t)(slash - tag));
        char *c = col;
        while (c && isspace((unsigned char)*c))
            c++;
        size_t n = c ? strlen(c) : 0;
        while (n && isspace((unsigned char)c[n - 1]))
            c[--n] = '\0';
        for (int i = 0; c && i < 4; i++) {
            if (!strcasecmp(c, cb_my_tag_columns[i])) {
                free(col);
                const char *s = slash + 1;
                while (isspace((unsigned char)*s))
                    s++;
                *name = cb_strdup(s);
                size_t l = *name ? strlen(*name) : 0;
                while (l && isspace((unsigned char)(*name)[l - 1]))
                    (*name)[--l] = '\0';
                return i + 1;
            }
        }
        free(col);
    }
    while (isspace((unsigned char)*tag))
        tag++;
    *name = cb_strdup(tag);
    size_t l = *name ? strlen(*name) : 0;
    while (l && isspace((unsigned char)(*name)[l - 1]))
        (*name)[--l] = '\0';
    return 4;
}

/* An MP3's rate without its frames read (a stored probe): the container's
 * average, which counts the tags too, taken as the standard rate it is
 * within 3 % of (a CBR file's); else as it is (a VBR file's average). */
static int mp3_kbps(int bit_rate)
{
    static const int rates[] = { 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160,
                                 192, 224, 256, 320 };
    int kbps = (int)lrint(bit_rate / 1000.0);
    for (size_t i = 0; i < CB_ARRAY_ELEMS(rates); i++)
        if (abs(kbps - rates[i]) <= rates[i] * 3 / 100)
            return rates[i];
    return kbps;
}

/* dj's _bitrate(): kbps and bit depth the way rekordbox fills them
 * (docs/pdb.md track row 0x30). */
static void bitrate(CBPlanTrack *pt)
{
    const CBProbe *p = &pt->probe;
    if (!strcmp(p->codec, "mp3")) {
        pt->bit_rate = p->mp3.valid && p->mp3.bitrate_kbps ? p->mp3.bitrate_kbps
                                                           : mp3_kbps(p->bit_rate);
        pt->bit_depth = 16;
    } else if (!strcmp(p->codec, "aac")) {
        pt->bit_rate = (int)((double)pt->file_size * 8 / CB_MAX(p->duration, 1e-3) / 1000);
        pt->bit_depth = 16;
    } else {
        int depth = p->bit_depth ? p->bit_depth : 16;
        pt->bit_rate = (int)((int64_t)p->sample_rate * depth * p->channels / 1000);
        pt->bit_depth = depth;
    }
}

/* DeviceSQL file types (docs/pdb.md track row 0x5A; must match the codec,
 * G7). */
static int file_type(const CBProbe *p)
{
    if (!strcmp(p->codec, "mp3"))
        return 1;
    if (!strcmp(p->codec, "aac"))
        return 4;
    if (!strcmp(p->codec, "alac"))
        return 6;
    if (!strcmp(p->codec, "flac"))
        return 5;
    if (cb_str_starts(p->codec, "pcm_"))
        return strstr(p->container, "aiff") ? 12 : 11;
    return 0;
}

void cb_stick_cue_from_plan(const CBPlanCue *c, CBStickCue *s)
{
    *s = (CBStickCue) {
        .hot    = c->hot,
        .in     = cb_anlz_ms(c->time),
        .out    = c->loop_end >= 0 ? cb_anlz_ms(c->loop_end) : 0xFFFFFFFFu,
        .code   = c->color_code,
        .rgb    = c->hot >= 0 ? c->rgb : 0,
        .active = c->active,
    };
    snprintf(s->name, sizeof(s->name), "%s", c->name);
}

static void plan_cue(const CBCue *c, CBPlanCue *p, bool *bad_color)
{
    *p = (CBPlanCue) {
        .time     = c->time,
        .loop_end = c->loop_end >= 0 ? c->loop_end : -1.0,
        .hot      = c->hot >= 0 && c->hot < CB_HOT_CUE_SLOTS ? c->hot : -1,
    };
    /* Only a memory loop can be the active loop (dj's resolve_cues). */
    p->active = c->active && p->hot < 0 && p->loop_end >= 0;
    snprintf(p->name, sizeof(p->name), "%s", c->name);
    *bad_color = false;
    if (p->hot >= 0) {
        if (cb_hot_cue_color(c->color, &p->color_code, &p->rgb) < 0)
            *bad_color = true;
        /* No colour: code 0 with the slot's default pad colour. */
        if (!p->color_code)
            p->rgb = cb_hot_cue_default_rgb(p->hot);
    } else {
        int id = cb_memory_cue_color_id(c->color);
        if (id < 0) {
            *bad_color = true;
            id = 0;
        }
        p->color_code = id;
        p->rgb = cb_memory_cue_color_rgb(id);
    }
}

int cb_plan_track_cues(CBPlanTrack *pt, const CBTrack *t)
{
    CBPlanCue *cues = NULL;
    if (t->nb_cues > 0 && !(cues = calloc((size_t)t->nb_cues, sizeof(*cues))))
        return CB_ERROR(ENOMEM);
    for (int k = 0; k < t->nb_cues; k++) {
        bool bad;
        plan_cue(&t->cues[k], &cues[k], &bad);
    }
    free(pt->cues);
    pt->cues = cues;
    pt->nb_cues = CB_MAX(t->nb_cues, 0);
    return 0;
}

void cb_stick_cue_from_track(const CBCue *c, CBStickCue *s)
{
    CBPlanCue p;
    bool bad;
    plan_cue(c, &p, &bad);
    cb_stick_cue_from_plan(&p, s);
}

void cb_plan_free(CBPlan *p)
{
    for (int i = 0; i < p->nb_tracks; i++) {
        CBPlanTrack *t = &p->tracks[i];
        free(t->source);
        free(t->usb_path);
        free(t->anlz_dir);
        cb_grid_free(&t->grid);
        free(t->cues);
        free(t->tag_ids);
        cb_probe_free(&t->probe);
    }
    free(p->tracks);
    CBPlanName *tables[] = { p->artists, p->albums, p->genres, p->labels, p->keys, p->remixers,
                             p->composers, p->original_artists };
    int counts[] = { p->nb_artists, p->nb_albums, p->nb_genres, p->nb_labels, p->nb_keys,
                     p->nb_remixers, p->nb_composers, p->nb_original_artists };
    for (size_t k = 0; k < CB_ARRAY_ELEMS(tables); k++) {
        for (int i = 0; i < counts[k]; i++)
            free(tables[k][i].name);
        free(tables[k]);
    }
    for (int i = 0; i < p->nb_artworks; i++) {
        free(p->artworks[i].path80);
        free(p->artworks[i].path240);
        free(p->artworks[i].jpeg80);
        free(p->artworks[i].jpeg240);
    }
    free(p->artworks);
    for (int i = 0; i < p->nb_playlists; i++) {
        free(p->playlists[i].name);
        free(p->playlists[i].track_ids);
    }
    free(p->playlists);
    for (int i = 0; i < p->nb_tags; i++)
        free(p->tags[i].name);
    free(p->tags);
    free(p->tag_tracks);
    free(p->device_name);
    *p = (CBPlan) { 0 };
}

static int cmp_track_path(const void *a, const void *b)
{
    const CBTrack *x = *(CBTrack *const *)a, *y = *(CBTrack *const *)b;
    return strcmp(x->path, y->path);
}

static int cmp_plan_id(const void *a, const void *b)
{
    uint32_t x = ((const CBPlanTrack *)a)->id, y = ((const CBPlanTrack *)b)->id;
    return x < y ? -1 : x > y;
}

static bool plan_has(const CBPlan *p, uint64_t id)
{
    CBPlanTrack key = { .id = (uint32_t)id };
    return id && id <= UINT32_MAX &&
           bsearch(&key, p->tracks, (size_t)p->nb_tracks, sizeof(key), cmp_plan_id);
}

typedef struct Artwork {
    StrMap   ids;                /* SHA-1 hex, or "library <id>" -> artwork id */
    CBArray  arr;                /* CBPlanArtwork */
} Artwork;

/* The artwork row of the cover `raw` (an image file's bytes), known by
 * `key`: made the first time the key is seen, 0 when it does not decode
 * (once). Takes `raw`. */
static uint32_t artwork_add(Artwork *aw, const char *key, uint8_t *raw, size_t size,
                            const CBTrack *t, cb_progress_cb cb, void *opaque)
{
    uint32_t *known = strmap_get(&aw->ids, key);
    if (known) {
        free(raw);
        return *known;
    }

    uint8_t *rgba;
    int w, h;
    int ret = cb_image_decode(raw, size, &rgba, &w, &h);
    free(raw);
    CBPlanArtwork a = { .id = (uint32_t)aw->arr.nb + 1 };
    if (ret >= 0) {
        /* 80x80 a<id>.jpg and 240x240 a<id>_m.jpg (docs/pdb.md 1.4), JPEG
         * quality 90 as dj. */
        ret = cb_image_jpeg_square(rgba, w, h, 80, 90, &a.jpeg80, &a.size80);
        if (ret >= 0)
            ret = cb_image_jpeg_square(rgba, w, h, 240, 90, &a.jpeg240, &a.size240);
        free(rgba);
    }
    if (ret < 0) {
        char name[256];
        report(cb, opaque, "%s: artwork unreadable (%s)", cb_track_display(t, name, sizeof(name)),
               strerror(-ret));
        free(a.jpeg80);
        free(a.jpeg240);
        strmap_put(&aw->ids, key, 0);
        return 0;
    }
    char *dir = cb_plan_artwork_dir(a.id);
    a.path80 = dir ? cb_asprintf("%s/a%u.jpg", dir, a.id) : NULL;
    a.path240 = dir ? cb_asprintf("%s/a%u_m.jpg", dir, a.id) : NULL;
    free(dir);
    if (!a.path80 || !a.path240 || strmap_put(&aw->ids, key, a.id) < 0 || !cb_array_push(&aw->arr, &a)) {
        free(a.path80);
        free(a.path240);
        free(a.jpeg80);
        free(a.jpeg240);
        return 0;
    }
    return a.id;
}

/* The track's cover read from its file, by a SHA-1 of its bytes. */
static uint32_t artwork_for(Artwork *aw, const CBTrack *t, cb_progress_cb cb, void *opaque)
{
    uint8_t *raw;
    size_t size;
    char codec[16];
    if (cb_artwork_raw(t->path, &raw, &size, codec) < 0)
        return 0;
    uint8_t md[20];
    char hex[2 * sizeof(md) + 1];
    struct AVSHA *sha = av_sha_alloc();
    if (!sha) {
        free(raw);
        return 0;
    }
    av_sha_init(sha, 160);
    av_sha_update(sha, raw, size);
    av_sha_final(sha, md);
    av_free(sha);
    for (size_t i = 0; i < sizeof(md); i++)
        snprintf(hex + 2 * i, 3, "%02x", md[i]);
    return artwork_add(aw, hex, raw, size, t, cb, opaque);
}

/* The track's cover as the library stores it (its 240 px thumbnail, one
 * per cover however many tracks share it), by the library's cover id; a
 * track whose cover has no thumbnail yet (the interface makes one when it
 * first shows the track) has it read from its file. */
static uint32_t artwork_stored(Artwork *aw, const CBLibrary *lib, const CBTrack *t,
                               cb_progress_cb cb, void *opaque)
{
    int64_t cover = cb_library_artwork_id(lib, t->id);
    if (cover <= 0)
        return t->has_artwork ? artwork_for(aw, t, cb, opaque) : 0;
    char key[40];
    snprintf(key, sizeof(key), "library %lld", (long long)cover);
    uint32_t *known = strmap_get(&aw->ids, key);
    if (known)
        return *known;
    uint8_t *jpeg;
    size_t size;
    if (cb_library_artwork_load(lib, t->id, &jpeg, &size, NULL, NULL) < 0)
        return artwork_for(aw, t, cb, opaque);
    return artwork_add(aw, key, jpeg, size, t, cb, opaque);
}

/* Folders first, then playlists, as CBPlaylistFolder holds them (the
 * library keeps their interleaving among siblings private). */
static bool folder_selected(const CBPlaylistFolder *f, const CBPlaylist *const *sel, int nb_sel)
{
    if (!sel)
        return true;
    for (int i = 0; i < f->nb_playlists; i++)
        for (int k = 0; k < nb_sel; k++)
            if (sel[k] == f->playlists[i] || sel[k]->id == f->playlists[i]->id)
                return true;
    for (int i = 0; i < f->nb_folders; i++)
        if (folder_selected(f->folders[i], sel, nb_sel))
            return true;
    return false;
}

static bool playlist_selected(const CBPlaylist *p, const CBPlaylist *const *sel, int nb_sel)
{
    if (!sel)
        return true;
    for (int k = 0; k < nb_sel; k++)
        if (sel[k] == p || sel[k]->id == p->id)
            return true;
    return false;
}

static int walk_playlists(const CBPlaylistFolder *f, uint32_t parent, CBPlan *plan, CBArray *out,
                          const CBPlaylist *const *sel, int nb_sel, cb_progress_cb cb, void *opaque)
{
    int order = 0, ret;
    for (int i = 0; i < f->nb_folders; i++) {
        const CBPlaylistFolder *sub = f->folders[i];
        if (!folder_selected(sub, sel, nb_sel))
            continue;
        CBPlanPlaylist p = {
            .id         = (uint32_t)sub->id,
            .parent_id  = parent,
            .sort_order = order++,
            .is_folder  = true,
            .name       = cb_strdup(sub->name ? sub->name : ""),
        };
        if (!p.name || !cb_array_push(out, &p)) {
            free(p.name);
            return CB_ERROR(ENOMEM);
        }
        if ((ret = walk_playlists(sub, p.id, plan, out, sel, nb_sel, cb, opaque)) < 0)
            return ret;
    }
    for (int i = 0; i < f->nb_playlists; i++) {
        const CBPlaylist *pl = f->playlists[i];
        if (!playlist_selected(pl, sel, nb_sel))
            continue;
        CBPlanPlaylist p = {
            .id         = (uint32_t)pl->id,
            .parent_id  = parent,
            .sort_order = order++,
            .name       = cb_strdup(pl->name ? pl->name : ""),
            .track_ids  = malloc((size_t)CB_MAX(pl->nb, 1) * sizeof(uint32_t)),
        };
        if (!p.name || !p.track_ids) {
            free(p.name);
            free(p.track_ids);
            return CB_ERROR(ENOMEM);
        }
        int skipped = 0;
        for (int k = 0; k < pl->nb; k++) {
            if (plan_has(plan, pl->track_ids[k]))
                p.track_ids[p.nb++] = (uint32_t)pl->track_ids[k];
            else
                skipped++;
        }
        if (skipped)
            report(cb, opaque, "playlist '%s': %d entr%s not exported", p.name, skipped,
                   skipped == 1 ? "y" : "ies");
        if (!cb_array_push(out, &p)) {
            free(p.name);
            free(p.track_ids);
            return CB_ERROR(ENOMEM);
        }
    }
    return 0;
}

/* What the library stored when it probed the track's file (CBTrack's
 * stream properties), when the file is still the one probed: the same
 * size and modification time. No tags, frame tables or artwork: what a
 * plan that reads no audio needs, the type, rate, depth, length and size.
 * The container is not stored; a PCM file's is told by its name. */
static bool stored_probe(const CBTrack *t, CBProbe *p, int64_t *file_size)
{
    struct stat st;
    if (!t->codec[0] || t->sample_rate <= 0 || t->file_size <= 0 || !t->mtime ||
        stat(t->path, &st) < 0 || st.st_size != t->file_size || (int64_t)st.st_mtime != t->mtime)
        return false;
    *p = (CBProbe) {
        .duration      = t->duration,
        .total_samples = t->total_samples,
        .sample_rate   = t->sample_rate,
        .channels      = t->channels,
        .bit_rate      = t->bit_rate,
        .bit_depth     = t->bit_depth,
        .has_artwork   = t->has_artwork,
    };
    snprintf(p->codec, sizeof(p->codec), "%s", t->codec);
    const char *ext = strrchr(cb_path_base(t->path), '.');
    bool aiff = ext && (!strcasecmp(ext, ".aif") || !strcasecmp(ext, ".aiff") ||
                        !strcasecmp(ext, ".aifc"));
    snprintf(p->container, sizeof(p->container), "%s",
             aiff ? "aiff" : cb_str_starts(t->codec, "pcm_") ? "wav" : t->codec);
    *file_size = st.st_size;
    return true;
}

/* A dry run's source for a track whose transcode is not made yet: the
 * cache file it would be, described from the original as the transcode
 * would be (16-bit FLAC at most at max_rate, cb_transcode_flac, or Link's
 * 16-bit WAV); its size a typical FLAC's 60 % of PCM, for the free-space
 * check, or the WAV's own. Nothing is written. `stored`: the original as the library stored it, unless it
 * changed since (stored_probe); a transcode made already is then
 * described the same way, its size its own. */
/* What Link serves as a WAV made as it is read (src/link/wav.c): a file
 * of one or two channels at 48 kHz at most, which every player reads as
 * 16-bit stereo, within NFS version 2's 4 GiB. */
static bool link_pcm(const CBTrack *t)
{
    return t->codec[0] && t->sample_rate > 0 && t->sample_rate <= 48000 &&
           t->channels >= 1 && t->channels <= 2 && t->total_samples > 0 &&
           t->total_samples < (INT64_C(0xffffffff) - 44) / 4;
}

/* ---------------------------------------------------------- the stick */

static bool lossless_codec(const char *c)
{
    return !strcmp(c, "flac") || !strcmp(c, "alac") || !strcmp(c, "wavpack") || !strcmp(c, "ape") ||
           !strcmp(c, "tta") || cb_str_starts(c, "pcm_");
}

/* How an export writes a file described by `p` (cyanbox/export.h,
 * CBStickFormat): copied (on false) or encoded so. */
static CBPlanEncode stick_encode(const CBProbe *p, int kind)
{
    bool compat = kind == CB_TRANSCODE_STICK_COPY_COMPAT || kind == CB_TRANSCODE_STICK_FLAC_COMPAT;
    bool mp3 = !strcmp(p->codec, "mp3"), aac = !strcmp(p->codec, "aac");
    bool lossless = lossless_codec(p->codec), fpcm = cb_str_starts(p->codec, "pcm_f");
    CBPlanEncode e = { 0 };
    switch (kind) {
    case CB_TRANSCODE_STICK_COPY:
    case CB_TRANSCODE_STICK_COPY_COMPAT:
        if (!cb_plan_codec_playable(p->codec))
            e = (CBPlanEncode) { true, CB_ENCODE_FLAC, 16, false, compat ? 48000 : 96000 };
        else if (compat && (aac || (!strcmp(p->codec, "flac") &&
                                    (p->bit_depth > 16 || p->sample_rate > 48000))))
            e = (CBPlanEncode) { true, CB_ENCODE_FLAC, 16, false, 48000 };
        break;
    case CB_TRANSCODE_STICK_FLAC:
    case CB_TRANSCODE_STICK_FLAC_COMPAT:
        if (mp3 || (aac && !compat))
            break;
        e = (CBPlanEncode) { true, CB_ENCODE_FLAC,
                             !compat && lossless && !fpcm && p->bit_depth > 16 ? 24 : 16, false,
                             compat ? 48000 : 96000 };
        break;
    case CB_TRANSCODE_STICK_WAV:
    case CB_TRANSCODE_STICK_AIFF:
        if (mp3)
            break;
        e = (CBPlanEncode) { true, kind == CB_TRANSCODE_STICK_WAV ? CB_ENCODE_WAV : CB_ENCODE_AIFF,
                             16, false, 48000 };
        break;
    }
    if (e.on) {
        /* Players play stereo: more channels are mixed down. */
        e.channels = p->channels > 2 ? 2 : 0;
        e.exact = lossless && !fpcm && p->bit_depth > 0 && p->bit_depth <= e.bits &&
                  p->sample_rate <= e.max_rate && p->channels <= 2;
    }
    return e;
}

/* An export's source: the track's own file, copied or encoded; nothing
 * made before it is written. An encoded one is described as what it will
 * be (its size the WAV's or AIFF's own, a FLAC's guessed: the source's
 * when it is a FLAC of the same samples, else 60 % of the samples). */
static int stick_source(const CBTrack *t, int kind, bool stored, CBPlanTrack *pt)
{
    int64_t size = 0;
    if (!(stored && stored_probe(t, &pt->probe, &size))) {
        cb_probe_free(&pt->probe);
        int ret = cb_probe(t->path, &pt->probe);
        if (ret < 0)
            return ret;
        size = cb_file_size(t->path);
    }
    if (!(pt->source = cb_strdup(t->path)))
        return CB_ERROR(ENOMEM);
    CBProbe *p = &pt->probe;
    CBPlanEncode e = stick_encode(p, kind);
    if (!e.on) {
        pt->file_size = size;
        return 0;
    }
    pt->encode = e;
    bool same = !strcmp(p->codec, "flac") && p->bit_depth == e.bits && p->channels <= 2 &&
                p->sample_rate <= e.max_rate;
    if (p->sample_rate > e.max_rate) {
        p->total_samples = (int64_t)((double)p->total_samples * e.max_rate / p->sample_rate);
        p->sample_rate = e.max_rate;
    }
    p->channels = e.channels ? e.channels : p->channels;
    p->bit_depth = e.bits;
    cb_flac_info_free(&p->flac);
    p->mp3 = (CBMp3Info) { 0 };
    static const char *const codec[] = { [CB_ENCODE_FLAC] = "flac", [CB_ENCODE_WAV] = "pcm_s16le",
                                         [CB_ENCODE_AIFF] = "pcm_s16be" };
    static const char *const box[] = { [CB_ENCODE_FLAC] = "flac", [CB_ENCODE_WAV] = "wav",
                                       [CB_ENCODE_AIFF] = "aiff" };
    snprintf(p->codec, sizeof(p->codec), "%s", e.bits == 24 && e.format == CB_ENCODE_WAV ? "pcm_s24le"
             : e.bits == 24 && e.format == CB_ENCODE_AIFF ? "pcm_s24be" : codec[e.format]);
    snprintf(p->container, sizeof(p->container), "%s", box[e.format]);
    int64_t pcm = p->total_samples * CB_MAX(p->channels, 1) * (e.bits / 8);
    pt->file_size = e.format == CB_ENCODE_WAV ? 44 + pcm : e.format == CB_ENCODE_AIFF ? 54 + pcm
                  : same ? size : (int64_t)(0.6 * (double)pcm);
    return 0;
}

static int dry_source(const CBLibrary *lib, const CBTrack *t, int compat, bool stored,
                      CBPlanTrack *pt)
{
    if (compat >= CB_TRANSCODE_STICK_COPY)
        return stick_source(t, compat, stored, pt);

    /* Link's WAV of the track's own file, described as what a player
     * gets: 16-bit stereo at the file's rate, the frames the library
     * counted (the WAV keeps to them), the header and the samples. */
    if (compat == CB_TRANSCODE_LINK && link_pcm(t)) {
        int64_t size = 0;
        if (!(stored && stored_probe(t, &pt->probe, &size))) {
            cb_probe_free(&pt->probe);
            int ret = cb_probe(t->path, &pt->probe);
            if (ret < 0)
                return ret;
        }
        CBProbe *p = &pt->probe;
        if (p->sample_rate <= 0 || p->sample_rate > 48000 || p->total_samples <= 0 ||
            p->total_samples >= (INT64_C(0xffffffff) - 44) / 4)
            return CB_ERROR(EINVAL);
        cb_flac_info_free(&p->flac);
        p->mp3 = (CBMp3Info) { 0 };
        snprintf(p->codec, sizeof(p->codec), "pcm_s16le");
        snprintf(p->container, sizeof(p->container), "wav");
        p->channels = 2;
        p->bit_depth = 16;
        p->bit_rate = p->sample_rate * 32;
        pt->source = cb_strdup(t->path);
        pt->pcm = true;
        pt->file_size = 44 + p->total_samples * 4;
        return pt->source ? 0 : CB_ERROR(ENOMEM);
    }

    char *cache;
    int max_rate = 0;
    int ret = cb_plan_transcode_path(lib, t, compat, false, &cache, &max_rate);
    if (ret < 0)
        return ret;
    int64_t size = 0;
    bool fresh = ret > 0 && cb_plan_transcode_fresh(t, cache);
    bool known = stored && stored_probe(t, &pt->probe, &size);
    if (ret == 0 && known) {
        pt->source = cb_strdup(t->path);
        pt->file_size = size;
        return pt->source ? 0 : CB_ERROR(ENOMEM);
    }
    if (ret == 0 || (fresh && !known)) {
        free(cache);
        cb_probe_free(&pt->probe);
        ret = cb_plan_transcode(lib, t, compat, &pt->source);
        return ret < 0 ? ret : cb_probe(pt->source, &pt->probe);
    }
    pt->source = cache;
    if (!known && (ret = cb_probe(t->path, &pt->probe)) < 0)
        return ret;
    CBProbe *p = &pt->probe;
    bool wav = link_wav(cache);
    cb_flac_info_free(&p->flac);
    p->mp3 = (CBMp3Info) { 0 };
    snprintf(p->codec, sizeof(p->codec), wav ? "pcm_s16le" : "flac");
    snprintf(p->container, sizeof(p->container), wav ? "wav" : "flac");
    if (max_rate > 0 && p->sample_rate > max_rate) {
        p->total_samples = (int64_t)((double)p->total_samples * max_rate / p->sample_rate);
        p->sample_rate = max_rate;
    }
    p->bit_depth = 16;
    /* A WAV's size is its header's 44 bytes and the samples. */
    double pcm = (double)p->duration * p->sample_rate * CB_MAX(p->channels, 1) * 2;
    pt->file_size = fresh ? cb_file_size(cache)
                  : wav ? 44 + p->total_samples * CB_MAX(p->channels, 1) * 2
                  : (int64_t)(0.6 * pcm);
    return 0;
}

int cb_plan_build_cb(const CBLibrary *lib, CBTrack *const *tracks, int nb_tracks,
                     const CBPlaylist *const *playlists, int nb_playlists,
                     const char *device_name, int compat_transcode, enum CBPlanMode mode,
                     CBPlan *out, cb_progress_cb cb, void *opaque)
{
    bool dry = mode != CB_PLAN_EXPORT;
    *out = (CBPlan) { 0 };
    int ret = 0;
    CBTrack **sorted = malloc((size_t)CB_MAX(nb_tracks, 1) * sizeof(*sorted));
    Names artists = { 0 }, albums = { 0 }, genres = { 0 }, labels = { 0 }, keys = { 0 };
    StrMap used = { 0 }, anlz_dirs = { 0 }, tag_ids = { 0 };
    Artwork aw = { 0 };
    CBArray tags, tag_tracks, pls;
    cb_array_init(&artists.arr, sizeof(CBPlanName));
    cb_array_init(&albums.arr, sizeof(CBPlanName));
    cb_array_init(&genres.arr, sizeof(CBPlanName));
    cb_array_init(&labels.arr, sizeof(CBPlanName));
    cb_array_init(&keys.arr, sizeof(CBPlanName));
    cb_array_init(&aw.arr, sizeof(CBPlanArtwork));
    cb_array_init(&tags, sizeof(CBPlanTag));
    cb_array_init(&tag_tracks, sizeof(CBPlanTagTrack));
    cb_array_init(&pls, sizeof(CBPlanPlaylist));
    out->tracks = calloc((size_t)CB_MAX(nb_tracks, 1), sizeof(*out->tracks));
    if (!sorted || !out->tracks) {
        ret = CB_ERROR(ENOMEM);
        goto fail;
    }
    memcpy(sorted, tracks, (size_t)nb_tracks * sizeof(*sorted));
    qsort(sorted, (size_t)nb_tracks, sizeof(*sorted), cmp_track_path);

    const CBConfig *cfg = &lib->cfg;
    /* The four My Tag columns (ids 1..4) come first; tags follow. */
    for (int i = 0; i < 4; i++) {
        CBPlanTag c = {
            .id          = (uint32_t)i + 1,
            .name        = cb_strdup(cb_my_tag_columns[i]),
            .position    = i,
            .is_category = true,
        };
        if (!c.name || !cb_array_push(&tags, &c)) {
            free(c.name);
            ret = CB_ERROR(ENOMEM);
            goto fail;
        }
    }
    int col_count[5] = { 0 };

    for (int i = 0; i < nb_tracks; i++) {
        const CBTrack *t = sorted[i];
        CBPlanTrack *pt = &out->tracks[out->nb_tracks];
        char disp[512];
        cb_track_display(t, disp, sizeof(disp));
        *pt = (CBPlanTrack) { .id = (uint32_t)t->id, .track = t };
        out->nb_tracks++;
        /* The library's hue goes to the stick as the nearest of
         * rekordbox's eight colours (cyanbox/colors.h); the progress line
         * (all of them in a dry run) says which. */
        pt->color_id = (uint32_t)cb_track_color_nearest_hue(t->meta.hue);
        char item[600], hue[32];
        if (pt->color_id)
            snprintf(item, sizeof(item), "%s [colour %s]", disp,
                     cb_hue_format(t->meta.hue, hue, sizeof(hue)));
        if (cb && !cb(opaque, "prepare", i + 1, nb_tracks, pt->color_id ? item : disp)) {
            ret = CB_ERROR(ECANCELED);
            goto fail;
        }

        if (dry) {
            if ((ret = dry_source(lib, t, compat_transcode, mode == CB_PLAN_STORED, pt)) < 0) {
                report(cb, opaque, "%s: cannot read %s (%s)", disp, t->path, strerror(-ret));
                goto fail;
            }
        } else {
            if ((ret = cb_plan_transcode(lib, t, compat_transcode, &pt->source)) < 0) {
                report(cb, opaque, "%s: transcoding failed (%s)", disp, strerror(-ret));
                goto fail;
            }
            if ((ret = cb_probe(pt->source, &pt->probe)) < 0) {
                report(cb, opaque, "%s: cannot read %s (%s)", disp, pt->source, strerror(-ret));
                goto fail;
            }
        }
        pt->file_type = file_type(&pt->probe);
        if (!pt->file_type) {
            report(cb, opaque, "%s: unsupported codec %s", disp, pt->probe.codec);
            ret = CB_ERROR(EINVAL);
            goto fail;
        }
        if (!dry || !pt->file_size)
            pt->file_size = cb_file_size(pt->source);
        pt->sample_rate = pt->probe.sample_rate;
        pt->duration = pt->probe.duration;
        pt->total_samples = pt->probe.total_samples;
        bitrate(pt);
        if (!strcmp(pt->probe.codec, "mp3") && pt->probe.mp3.valid && pt->probe.mp3.vbr)
            report(cb, opaque, "%s: VBR MP3; seeking and cue timing on players may be off "
                   "(rekordbox's VBR index is unknown)", disp);

        /* The extension is what is served's: a WAV's for Link's own. */
        static const char *const as[] = { [CB_ENCODE_FLAC] = "x.flac", [CB_ENCODE_WAV] = "x.wav",
                                          [CB_ENCODE_AIFF] = "x.aiff" };
        const char *named = pt->pcm ? "pcm.wav" : pt->encode.on ? as[pt->encode.format] : pt->source;
        if (!(pt->usb_path = usb_path(t, named, map_taken, &used))) {
            ret = CB_ERROR(ENOMEM);
            goto fail;
        }
        char dir[64];
        if ((ret = cb_anlz_dir(pt->usb_path, dir, sizeof(dir))) < 0 || !(pt->anlz_dir = cb_strdup(dir))) {
            ret = ret < 0 ? ret : CB_ERROR(ENOMEM);
            goto fail;
        }
        /* Two paths hashing to one directory: ANLZ0000, ANLZ0001 ... in
         * export order (docs/anlz.md 1.1). */
        uint32_t *seen = strmap_get(&anlz_dirs, dir);
        pt->anlz_index = seen ? (int)(*seen)++ : 0;
        if (!seen && (ret = strmap_put(&anlz_dirs, dir, 1)) < 0)
            goto fail;

        /* dj's interning order: artist, remixer, original artist, composer,
         * album (after its album artist), genre, label, key. */
        const CBMeta *m = &t->meta;
        if ((ret = intern(&artists, m->artist, 0, false, &pt->artist_id)) < 0 ||
            (ret = intern(&artists, m->remixer, 0, false, &pt->remixer_id)) < 0 ||
            (ret = intern(&artists, m->original_artist, 0, false, &pt->original_artist_id)) < 0 ||
            (ret = intern(&artists, m->composer, 0, false, &pt->composer_id)) < 0)
            goto fail;
        if (nonempty(m->album)) {
            uint32_t aa = 0;
            if ((ret = intern(&artists, m->album_artist, 0, false, &aa)) < 0 ||
                (ret = intern(&albums, m->album, aa, true, &pt->album_id)) < 0)
                goto fail;
        }
        if ((ret = intern(&genres, m->genre, 0, false, &pt->genre_id)) < 0 ||
            (ret = intern(&labels, m->label, 0, false, &pt->label_id)) < 0)
            goto fail;
        if (t->key[0]) {
            /* The player prints the key name verbatim (docs/pdb.md G10). */
            cb_key_display(t->key, cfg->key_notation[0] ? cfg->key_notation : "classic",
                           pt->key_name, sizeof(pt->key_name));
            if (!pt->key_name[0])
                snprintf(pt->key_name, sizeof(pt->key_name), "%s", t->key);
            if ((ret = intern(&keys, pt->key_name, 0, false, &pt->key_id)) < 0)
                goto fail;
        }
        for (int k = 0; k < m->nb_tags; k++) {
            char *name;
            int col = tag_column(m->tags[k], &name);
            if (!name) {
                ret = CB_ERROR(ENOMEM);
                goto fail;
            }
            /* Stable ids from the name (dj): crc32 of "column/name". */
            char *key = cb_asprintf("%d/%s", col, name);
            if (!key) {
                free(name);
                ret = CB_ERROR(ENOMEM);
                goto fail;
            }
            uint32_t *idp = strmap_get(&tag_ids, key);
            uint32_t id;
            if (idp) {
                id = *idp;
                free(name);
            } else {
                id = (cb_crc32(0, key, strlen(key)) | 0x100) & 0x7FFFFFFF;
                CBPlanTag tg = {
                    .id       = id,
                    .name     = name,
                    .category = col,
                    .position = col_count[col]++,
                };
                if (strmap_put(&tag_ids, key, id) < 0 || !cb_array_push(&tags, &tg)) {
                    free(name);
                    free(key);
                    ret = CB_ERROR(ENOMEM);
                    goto fail;
                }
            }
            free(key);
            uint32_t *ids = realloc(pt->tag_ids, (size_t)(pt->nb_tag_ids + 1) * sizeof(*ids));
            if (!ids) {
                ret = CB_ERROR(ENOMEM);
                goto fail;
            }
            pt->tag_ids = ids;
            pt->tag_ids[pt->nb_tag_ids++] = id;
        }

        pt->artwork_id = mode == CB_PLAN_STORED ? artwork_stored(&aw, lib, t, cb, opaque)
                                                : artwork_for(&aw, t, cb, opaque);

        if ((ret = cb_grid_copy(&pt->grid, &t->grid)) < 0)
            goto fail;
        if (t->nb_cues) {
            if (!(pt->cues = calloc((size_t)t->nb_cues, sizeof(*pt->cues)))) {
                ret = CB_ERROR(ENOMEM);
                goto fail;
            }
            for (int k = 0; k < t->nb_cues; k++) {
                bool bad;
                plan_cue(&t->cues[k], &pt->cues[k], &bad);
                if (bad)
                    report(cb, opaque, "%s: unknown cue colour '%s'", disp, t->cues[k].color);
            }
            pt->nb_cues = t->nb_cues;
        }
    }
    qsort(out->tracks, (size_t)out->nb_tracks, sizeof(*out->tracks), cmp_plan_id);
    for (int i = 0; i < out->nb_tracks; i++) {
        for (int k = 0; k < out->tracks[i].nb_tag_ids; k++) {
            CBPlanTagTrack tt = { .track_id = out->tracks[i].id, .tag_id = out->tracks[i].tag_ids[k] };
            if (!cb_array_push(&tag_tracks, &tt)) {
                ret = CB_ERROR(ENOMEM);
                goto fail;
            }
        }
    }

    if (lib->root_folder &&
        (ret = walk_playlists(lib->root_folder, 0, out, &pls, playlists, nb_playlists, cb, opaque)) < 0)
        goto fail;

    names_take(&artists, &out->artists, &out->nb_artists);
    names_take(&albums, &out->albums, &out->nb_albums);
    names_take(&genres, &out->genres, &out->nb_genres);
    names_take(&labels, &out->labels, &out->nb_labels);
    names_take(&keys, &out->keys, &out->nb_keys);
    out->artworks = aw.arr.data;
    out->nb_artworks = (int)aw.arr.nb;
    aw.arr.data = NULL;
    aw.arr.nb = aw.arr.cap = 0;
    out->tags = tags.data;
    out->nb_tags = (int)tags.nb;
    tags.data = NULL;
    tags.nb = tags.cap = 0;
    out->tag_tracks = tag_tracks.data;
    out->nb_tag_tracks = (int)tag_tracks.nb;
    tag_tracks.data = NULL;
    tag_tracks.nb = tag_tracks.cap = 0;
    out->playlists = pls.data;
    out->nb_playlists = (int)pls.nb;
    pls.data = NULL;
    pls.nb = pls.cap = 0;

    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(out->date, sizeof(out->date), "%Y-%m-%d", &tm);
    const char *dev = device_name ? device_name : cfg->device_name;
    out->device_name = cb_strdup(dev ? dev : "");
    snprintf(out->key_notation, sizeof(out->key_notation), "%s",
             cfg->key_notation[0] ? cfg->key_notation : "classic");
    out->hot_cue_auto_load = true;
    out->background_color = CB_CLAMP(cfg->background_color, 0, CB_TRACK_COLORS);
    /* dj's library constant: zlib.crc32 of the library root. */
    out->library_id = lib->root ? cb_crc32(0, lib->root, strlen(lib->root)) : 0;
    out->cfg = cfg;
    if (!out->device_name)
        ret = CB_ERROR(ENOMEM);

fail:
    free(sorted);
    names_free(&artists);
    names_free(&albums);
    names_free(&genres);
    names_free(&labels);
    names_free(&keys);
    strmap_free(&used);
    strmap_free(&anlz_dirs);
    strmap_free(&tag_ids);
    strmap_free(&aw.ids);
    for (size_t i = 0; i < aw.arr.nb; i++) {
        CBPlanArtwork *a = cb_array_at(&aw.arr, i);
        free(a->path80);
        free(a->path240);
        free(a->jpeg80);
        free(a->jpeg240);
    }
    cb_array_free(&aw.arr);
    for (size_t i = 0; i < tags.nb; i++)
        free(((CBPlanTag *)cb_array_at(&tags, i))->name);
    cb_array_free(&tags);
    cb_array_free(&tag_tracks);
    for (size_t i = 0; i < pls.nb; i++) {
        free(((CBPlanPlaylist *)cb_array_at(&pls, i))->name);
        free(((CBPlanPlaylist *)cb_array_at(&pls, i))->track_ids);
    }
    cb_array_free(&pls);
    if (ret < 0)
        cb_plan_free(out);
    return ret;
}

int cb_plan_build(const CBLibrary *lib, CBTrack *const *tracks, int nb_tracks,
                  const CBPlaylist *const *playlists, int nb_playlists,
                  const char *device_name, bool compat_transcode, CBPlan *out)
{
    return cb_plan_build_cb(lib, tracks, nb_tracks, playlists, nb_playlists, device_name,
                            compat_transcode, CB_PLAN_EXPORT, out, NULL, NULL);
}
