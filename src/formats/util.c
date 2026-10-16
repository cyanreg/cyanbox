/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* Small helpers the format writers share: UTF-8 <-> UTF-16 (export.pdb
 * strings and the ANLZ path are UTF-16, the directory hash runs over UTF-16
 * code units; UTF-8 itself and NFC are src/common/utf8.c's), zlib's CRC-32 (dj derives the library
 * and per-track ids and the My Tag ids from it), and the per-track values
 * export.pdb and exportLibrary.db must agree on. */

#include "src/formats/formats_priv.h"

#include <stdio.h>
#include <time.h>

#include "src/formats/buf.h"

/* ------------------------------------------------------------------ UTF-16 */

int cb_utf8_to_utf16(const char *s, uint16_t **units, size_t *nb)
{
    size_t len = strlen(s);
    uint16_t *u = malloc((2 * len + 1) * sizeof(*u));
    if (!u)
        return CB_ERROR(ENOMEM);
    size_t n = 0;
    while (*s) {
        uint32_t c = cb_utf8_next(&s);
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
        uint32_t c = be ? cb_rd_be16(p + 2 * i) : cb_rd_le16(p + 2 * i);
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < n) {
            uint32_t d = be ? cb_rd_be16(p + 2 * i + 2) : cb_rd_le16(p + 2 * i + 2);
            if (d >= 0xDC00 && d <= 0xDFFF) {
                c = 0x10000 + ((c - 0xD800) << 10) + (d - 0xDC00);
                i++;
            } else {
                c = 0xFFFD;
            }
        } else if (c >= 0xD800 && c <= 0xDFFF) {
            c = 0xFFFD;
        }
        o += (size_t)cb_utf8_put(out + o, c);
    }
    out[o] = '\0';
    return out;
}

char *cb_utf16le_to_utf8(const uint8_t *p, size_t bytes)
{
    return utf16_to_utf8(p, bytes, false);
}

char *cb_utf16be_to_utf8(const uint8_t *p, size_t bytes)
{
    return utf16_to_utf8(p, bytes, true);
}

/* ------------------------------------------------------------------ CRC-32 */

uint32_t cb_crc32(uint32_t crc, const void *data, size_t size)
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

const char *const cb_my_tag_columns[4] = { "Genre", "Components", "Situation", "Tags" };

uint32_t cb_plan_library_id(const CBPlan *plan)
{
    return plan->library_id;
}

uint32_t cb_plan_master_id(const CBPlanTrack *t)
{
    uint32_t id = t->usb_path ? cb_crc32(0, t->usb_path, strlen(t->usb_path)) : 0;
    return id ? id : 1;
}

void cb_plan_date_added(const CBPlanTrack *t, char buf[16])
{
    buf[0] = '\0';
    if (!t->track)
        return;
    /* When the track came into the library, as rekordbox's date added is;
     * dj used the audio file's modification time, which a retag or a copy
     * moves. The file's time only for a track without one. */
    int64_t when = t->track->added;
    if (!when)
        when = t->track->path ? cb_file_mtime(t->track->path) : 0;
    if (!when)
        when = t->track->mtime;
    if (!when)
        return;
    time_t tt = (time_t)when;
    struct tm tm;
    if (localtime_r(&tt, &tm))
        strftime(buf, 16, "%Y-%m-%d", &tm);
}

char *cb_plan_anlz_path(const CBPlanTrack *t, const char *ext)
{
    return cb_asprintf("%s/ANLZ%04d%s", t->anlz_dir, t->anlz_index, ext);
}

char *cb_plan_artwork_dir(uint32_t art_id)
{
    return cb_asprintf("/PIONEER/Artwork/%05u", art_id / 20 + 1);
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

const char *cb_plan_title(const CBPlanTrack *t, char *buf, size_t size)
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
    const char *base = cb_path_base(t->track->path);
    const char *dot = strrchr(base, '.');
    size_t n = dot && dot != base ? (size_t)(dot - base) : strlen(base);
    snprintf(buf, size, "%.*s", (int)n, base);
    cut_utf8(buf, n, size);
    return buf;
}

void cb_plan_ascii_label(const char *name, char *buf, size_t size)
{
    size_t o = 0;
    if (!size)
        return;
    while (name && *name && o + 1 < size && o < 32) {
        uint32_t c = cb_utf8_next(&name);
        buf[o++] = c >= 0x20 && c < 0x7F ? (char)c : '_';
    }
    buf[o] = '\0';
}
