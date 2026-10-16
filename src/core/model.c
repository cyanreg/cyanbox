/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* The plain-data parts of cyanbox/model.h: cues, tag metadata and tracks.
 * The grid arithmetic is in grid.c.
 *
 * Copies (cb_meta_copy, cb_grid_copy) replace what the destination held, so
 * a destination must be zeroed or valid. Empty strings are stored as NULL:
 * the library compares a track's tags with the file's to find the user's
 * overrides, and "" and absent mean the same there. The
 * cue list is kept sorted by time, stable for equal times, because the
 * memory cue calls (CALL previous/next) and the stick writer both read it
 * in order. */

#include "cyanbox/model.h"

#include <math.h>
#include <stdio.h>

/* ------------------------------------------------------------------- cues */

void cb_cue_init(CBCue *c, double time)
{
    *c = (CBCue) {
        .time       = time,
        .grid_beat  = NAN,
        .hot        = -1,
        .loop_end   = -1.0,
        .loop_beats = NAN,
    };
}

/* A value within a millionth of a step of a multiple of it is that
 * multiple: time_to_beat() of a quantised time lands a few ulps off. */
static double snap(double x, double step)
{
    double r = round(x / step) * step;
    return fabs(x - r) < 1e-6 ? r : x;
}

void cb_cue_attach_to_grid(CBCue *c, const CBGrid *grid)
{
    if (!cb_grid_valid(grid)) {
        c->grid_beat = NAN;
        c->loop_beats = NAN;
        return;
    }

    c->grid_beat = snap(cb_grid_time_to_beat(grid, c->time), 1.0);
    c->time = cb_grid_beat_to_time(grid, c->grid_beat);
    if (cb_cue_is_loop(c)) {
        double end = cb_grid_time_to_beat(grid, c->loop_end);
        /* Loop lengths are whole beats or 1/2 .. 1/32 of one. */
        c->loop_beats = snap(end - c->grid_beat, 1.0 / 64);
        c->loop_end = cb_grid_beat_to_time(grid, c->grid_beat + c->loop_beats);
    } else {
        c->loop_beats = NAN;
    }
}

/* --------------------------------------------------------------- metadata */

/* The string fields, by offset, so they can be walked in one loop. */
static const size_t meta_string_offsets[] = {
    offsetof(CBMeta, title),    offsetof(CBMeta, artist),   offsetof(CBMeta, album),
    offsetof(CBMeta, album_artist), offsetof(CBMeta, genre), offsetof(CBMeta, label),
    offsetof(CBMeta, remixer),  offsetof(CBMeta, composer), offsetof(CBMeta, original_artist),
    offsetof(CBMeta, comment),
};

static char **meta_string(CBMeta *m, int i)
{
    return (char **)((char *)m + meta_string_offsets[i]);
}

void cb_meta_free(CBMeta *m)
{
    if (!m)
        return;
    for (size_t i = 0; i < CB_ARRAY_ELEMS(meta_string_offsets); i++)
        free(*meta_string(m, (int)i));
    for (int i = 0; i < m->nb_tags; i++)
        free(m->tags[i]);
    free(m->tags);
    *m = (CBMeta) { .hue = CB_HUE_NONE };
}

/* Text is kept in NFC (src/common/utf8.c says why); bytes that are not
 * UTF-8 (a title from a file's name, which Linux keeps as bytes) as
 * they are. */
static char *nfc_copy(const char *s)
{
    return cb_utf8_valid(s) ? cb_utf8_nfc(s) : cb_strdup(s);
}

static bool needs_nfc(const char *s)
{
    return s && cb_utf8_valid(s) && !cb_utf8_is_nfc(s);
}

int cb_meta_set(char **field, const char *value)
{
    char *copy = NULL;
    if (value && *value) {
        copy = nfc_copy(value);
        if (!copy)
            return CB_ERROR(ENOMEM);
    }
    free(*field);
    *field = copy;
    return 0;
}

int cb_meta_normalise(CBMeta *m)
{
    int changed = 0;
    for (size_t i = 0; i < CB_ARRAY_ELEMS(meta_string_offsets); i++) {
        char **f = meta_string(m, (int)i);
        if (!needs_nfc(*f))
            continue;
        char *n = cb_utf8_nfc(*f);
        if (!n)
            return CB_ERROR(ENOMEM);
        free(*f);
        *f = n;
        changed++;
    }
    for (int i = 0; i < m->nb_tags; i++) {
        if (!needs_nfc(m->tags[i]))
            continue;
        char *n = cb_utf8_nfc(m->tags[i]);
        if (!n)
            return CB_ERROR(ENOMEM);
        free(m->tags[i]);
        m->tags[i] = n;
        changed++;
    }
    return changed;
}

int cb_meta_copy(CBMeta *dst, const CBMeta *src)
{
    if (dst == src)
        return 0;

    CBMeta tmp = *src;
    for (size_t i = 0; i < CB_ARRAY_ELEMS(meta_string_offsets); i++)
        *meta_string(&tmp, (int)i) = NULL;
    tmp.tags = NULL;
    tmp.nb_tags = 0;

    int ret = 0;
    for (size_t i = 0; i < CB_ARRAY_ELEMS(meta_string_offsets); i++) {
        ret = cb_meta_set(meta_string(&tmp, (int)i), *meta_string((CBMeta *)src, (int)i));
        if (ret < 0)
            goto fail;
    }
    for (int i = 0; i < src->nb_tags; i++) {
        ret = cb_meta_add_tag(&tmp, src->tags[i]);
        if (ret < 0)
            goto fail;
    }

    cb_meta_free(dst);
    *dst = tmp;
    return 0;

fail:
    cb_meta_free(&tmp);
    return ret;
}

bool cb_meta_has_tag(const CBMeta *m, const char *tag)
{
    for (int i = 0; i < m->nb_tags; i++) {
        if (!strcmp(m->tags[i], tag))
            return true;
    }
    return false;
}

int cb_meta_add_tag(CBMeta *m, const char *tag)
{
    if (!tag || !*tag)
        return CB_ERROR(EINVAL);
    char *copy = nfc_copy(tag);
    if (copy && cb_meta_has_tag(m, copy)) {
        free(copy);
        return 0;
    }
    if (!copy)
        return CB_ERROR(ENOMEM);
    char **tags = realloc(m->tags, (size_t)(m->nb_tags + 1) * sizeof(*tags));
    if (!tags) {
        free(copy);
        return CB_ERROR(ENOMEM);
    }
    tags[m->nb_tags++] = copy;
    m->tags = tags;
    return 0;
}

void cb_meta_clear_tags(CBMeta *m)
{
    for (int i = 0; i < m->nb_tags; i++)
        free(m->tags[i]);
    free(m->tags);
    m->tags = NULL;
    m->nb_tags = 0;
}

/* ------------------------------------------------------------------ track */

CBTrack *cb_track_new(const char *path)
{
    CBTrack *t = calloc(1, sizeof(*t));
    if (!t)
        return NULL;
    t->grid.beats_per_bar = 4;
    t->meta.hue = CB_HUE_NONE;
    t->file_meta.hue = CB_HUE_NONE;
    t->ml_club = t->ml_intensity = t->ml_valence = t->ml_vocals = NAN;
    for (size_t i = 0; i < CB_ARRAY_ELEMS(t->ml_colors); i++)
        t->ml_colors[i] = NAN;
    if (path) {
        t->path = cb_strdup(path);
        if (!t->path) {
            free(t);
            return NULL;
        }
    }
    return t;
}

void cb_track_free(CBTrack *t)
{
    if (!t)
        return;
    free(t->path);
    cb_meta_free(&t->meta);
    cb_meta_free(&t->file_meta);
    cb_grid_free(&t->grid);
    free(t->cues);
    free(t);
}

CBTrack *cb_track_dup(const CBTrack *src)
{
    CBTrack *t = calloc(1, sizeof(*t));
    if (!t)
        return NULL;
    *t = *src;
    t->path = NULL;
    t->meta = (CBMeta) { 0 };
    t->file_meta = (CBMeta) { 0 };
    t->grid = (CBGrid) { .beats_per_bar = src->grid.beats_per_bar };
    t->cues = NULL;
    t->nb_cues = 0;

    if ((src->path && !(t->path = cb_strdup(src->path))) ||
        cb_meta_copy(&t->meta, &src->meta) < 0 ||
        cb_meta_copy(&t->file_meta, &src->file_meta) < 0 ||
        cb_grid_copy(&t->grid, &src->grid) < 0)
        goto fail;

    if (src->nb_cues > 0) {
        t->cues = malloc((size_t)src->nb_cues * sizeof(*t->cues));
        if (!t->cues)
            goto fail;
        memcpy(t->cues, src->cues, (size_t)src->nb_cues * sizeof(*t->cues));
        t->nb_cues = src->nb_cues;
    }
    return t;

fail:
    cb_track_free(t);
    return NULL;
}

/* ------------------------------------------------------------- cue lists */

int cb_track_add_cue(CBTrack *t, const CBCue *cue)
{
    if (cue->hot < -1 || cue->hot >= CB_HOT_CUE_SLOTS || !isfinite(cue->time))
        return CB_ERROR(EINVAL);

    /* Grow first so a failure leaves the list as it was. */
    CBCue *cues = realloc(t->cues, (size_t)(t->nb_cues + 1) * sizeof(*cues));
    if (!cues)
        return CB_ERROR(ENOMEM);
    t->cues = cues;

    if (cue->hot >= 0) {
        int old = cb_track_hot_cue(t, cue->hot);
        if (old >= 0)
            cb_track_remove_cue(t, old);
    }

    int at = 0;
    while (at < t->nb_cues && t->cues[at].time <= cue->time)
        at++;
    memmove(&t->cues[at + 1], &t->cues[at], (size_t)(t->nb_cues - at) * sizeof(*t->cues));
    t->cues[at] = *cue;
    t->cues[at].name[CB_CUE_NAME_MAX - 1] = '\0';
    t->cues[at].color[CB_COLOR_MAX - 1] = '\0';
    t->nb_cues++;
    return at;
}

int cb_track_remove_cue(CBTrack *t, int index)
{
    if (index < 0 || index >= t->nb_cues)
        return CB_ERROR(EINVAL);
    memmove(&t->cues[index], &t->cues[index + 1],
            (size_t)(t->nb_cues - index - 1) * sizeof(*t->cues));
    t->nb_cues--;
    return 0;
}

int cb_track_hot_cue(const CBTrack *t, int slot)
{
    for (int i = 0; i < t->nb_cues; i++) {
        if (t->cues[i].hot == slot && slot >= 0)
            return i;
    }
    return -1;
}

int cb_track_memory_cue_at(const CBTrack *t, double time, double tolerance)
{
    int best = -1;
    for (int i = 0; i < t->nb_cues; i++) {
        const CBCue *c = &t->cues[i];
        if (cb_cue_is_hot(c) || fabs(c->time - time) > tolerance)
            continue;
        if (best < 0 || fabs(c->time - time) < fabs(t->cues[best].time - time))
            best = i;
    }
    return best;
}

int cb_track_memory_cue_before(const CBTrack *t, double time, double tolerance)
{
    int best = -1;
    for (int i = 0; i < t->nb_cues; i++) {
        const CBCue *c = &t->cues[i];
        if (!cb_cue_is_hot(c) && c->time < time - tolerance)
            best = i;
    }
    return best;
}

int cb_track_memory_cue_after(const CBTrack *t, double time, double tolerance)
{
    for (int i = 0; i < t->nb_cues; i++) {
        const CBCue *c = &t->cues[i];
        if (!cb_cue_is_hot(c) && c->time > time + tolerance)
            return i;
    }
    return -1;
}

int cb_track_memory_cue_nth(const CBTrack *t, int n)
{
    for (int i = 0; i < t->nb_cues; i++) {
        if (!cb_cue_is_hot(&t->cues[i]) && --n == 0)
            return i;
    }
    return -1;
}

void cb_track_resolve_cues(CBTrack *t)
{
    const CBGrid *g = &t->grid;
    if (!cb_grid_valid(g))
        return;

    for (int i = 0; i < t->nb_cues; i++) {
        CBCue *c = &t->cues[i];
        if (cb_cue_is_musical(c))
            c->time = cb_grid_beat_to_time(g, c->grid_beat);
        if (isnan(c->loop_beats))
            continue;
        if (cb_cue_is_musical(c))
            c->loop_end = cb_grid_beat_to_time(g, c->grid_beat + c->loop_beats);
        else
            c->loop_end = cb_grid_offset_beats(g, c->time, c->loop_beats);
        /* A grid edit can carry a loop on the pickup before 0, where a
         * negative end would read as "not a loop" and the next save would
         * drop it; loop_beats brings the end back with the grid. */
        c->loop_end = fmax(c->loop_end, 0.0);
    }

    /* Musical and absolute cues can trade places after a grid edit:
     * insertion sort keeps equal times in their order. */
    for (int i = 1; i < t->nb_cues; i++) {
        CBCue c = t->cues[i];
        int j = i;
        while (j > 0 && t->cues[j - 1].time > c.time) {
            t->cues[j] = t->cues[j - 1];
            j--;
        }
        t->cues[j] = c;
    }
}

const char *cb_track_display(const CBTrack *t, char *buf, size_t size)
{
    if (!size)
        return buf;
    const char *title = t->meta.title, *artist = t->meta.artist;
    if (artist && title)
        snprintf(buf, size, "%s - %s", artist, title);
    else if (title)
        snprintf(buf, size, "%s", title);
    else
        snprintf(buf, size, "%s", t->path ? cb_path_base(t->path) : "");
    return buf;
}
