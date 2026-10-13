/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* The plain-data parts of rxbox/model.h: cues, tag metadata and tracks.
 * The grid arithmetic is in grid.c.
 *
 * Copies (rx_meta_copy, rx_grid_copy) replace what the destination held, so
 * a destination must be zeroed or valid. Empty strings are stored as NULL:
 * the library compares a track's tags with the file's to find the user's
 * overrides, and "" and absent mean the same there. The
 * cue list is kept sorted by time, stable for equal times, because the
 * memory cue calls (CALL previous/next) and the stick writer both read it
 * in order. */

#include "rxbox/model.h"

#include <math.h>
#include <stdio.h>

/* ------------------------------------------------------------------- cues */

void rx_cue_init(RXCue *c, double time)
{
    *c = (RXCue) {
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

void rx_cue_attach_to_grid(RXCue *c, const RXGrid *grid)
{
    if (!rx_grid_valid(grid)) {
        c->grid_beat = NAN;
        c->loop_beats = NAN;
        return;
    }

    c->grid_beat = snap(rx_grid_time_to_beat(grid, c->time), 1.0);
    c->time = rx_grid_beat_to_time(grid, c->grid_beat);
    if (rx_cue_is_loop(c)) {
        double end = rx_grid_time_to_beat(grid, c->loop_end);
        /* Loop lengths are whole beats or 1/2 .. 1/32 of one. */
        c->loop_beats = snap(end - c->grid_beat, 1.0 / 64);
        c->loop_end = rx_grid_beat_to_time(grid, c->grid_beat + c->loop_beats);
    } else {
        c->loop_beats = NAN;
    }
}

/* --------------------------------------------------------------- metadata */

/* The string fields, by offset, so they can be walked in one loop. */
static const size_t meta_string_offsets[] = {
    offsetof(RXMeta, title),    offsetof(RXMeta, artist),   offsetof(RXMeta, album),
    offsetof(RXMeta, album_artist), offsetof(RXMeta, genre), offsetof(RXMeta, label),
    offsetof(RXMeta, remixer),  offsetof(RXMeta, composer), offsetof(RXMeta, original_artist),
    offsetof(RXMeta, comment),
};

static char **meta_string(RXMeta *m, int i)
{
    return (char **)((char *)m + meta_string_offsets[i]);
}

void rx_meta_free(RXMeta *m)
{
    if (!m)
        return;
    for (size_t i = 0; i < RX_ARRAY_ELEMS(meta_string_offsets); i++)
        free(*meta_string(m, (int)i));
    for (int i = 0; i < m->nb_tags; i++)
        free(m->tags[i]);
    free(m->tags);
    *m = (RXMeta) { .hue = RX_HUE_NONE };
}

int rx_meta_set(char **field, const char *value)
{
    char *copy = NULL;
    if (value && *value) {
        copy = rx_strdup(value);
        if (!copy)
            return RX_ERROR(ENOMEM);
    }
    free(*field);
    *field = copy;
    return 0;
}

int rx_meta_copy(RXMeta *dst, const RXMeta *src)
{
    if (dst == src)
        return 0;

    RXMeta tmp = *src;
    for (size_t i = 0; i < RX_ARRAY_ELEMS(meta_string_offsets); i++)
        *meta_string(&tmp, (int)i) = NULL;
    tmp.tags = NULL;
    tmp.nb_tags = 0;

    int ret = 0;
    for (size_t i = 0; i < RX_ARRAY_ELEMS(meta_string_offsets); i++) {
        ret = rx_meta_set(meta_string(&tmp, (int)i), *meta_string((RXMeta *)src, (int)i));
        if (ret < 0)
            goto fail;
    }
    for (int i = 0; i < src->nb_tags; i++) {
        ret = rx_meta_add_tag(&tmp, src->tags[i]);
        if (ret < 0)
            goto fail;
    }

    rx_meta_free(dst);
    *dst = tmp;
    return 0;

fail:
    rx_meta_free(&tmp);
    return ret;
}

bool rx_meta_has_tag(const RXMeta *m, const char *tag)
{
    for (int i = 0; i < m->nb_tags; i++) {
        if (!strcmp(m->tags[i], tag))
            return true;
    }
    return false;
}

int rx_meta_add_tag(RXMeta *m, const char *tag)
{
    if (!tag || !*tag)
        return RX_ERROR(EINVAL);
    if (rx_meta_has_tag(m, tag))
        return 0;
    char *copy = rx_strdup(tag);
    if (!copy)
        return RX_ERROR(ENOMEM);
    char **tags = realloc(m->tags, (size_t)(m->nb_tags + 1) * sizeof(*tags));
    if (!tags) {
        free(copy);
        return RX_ERROR(ENOMEM);
    }
    tags[m->nb_tags++] = copy;
    m->tags = tags;
    return 0;
}

void rx_meta_clear_tags(RXMeta *m)
{
    for (int i = 0; i < m->nb_tags; i++)
        free(m->tags[i]);
    free(m->tags);
    m->tags = NULL;
    m->nb_tags = 0;
}

/* ------------------------------------------------------------------ track */

RXTrack *rx_track_new(const char *path)
{
    RXTrack *t = calloc(1, sizeof(*t));
    if (!t)
        return NULL;
    t->grid.beats_per_bar = 4;
    t->meta.hue = RX_HUE_NONE;
    t->file_meta.hue = RX_HUE_NONE;
    if (path) {
        t->path = rx_strdup(path);
        if (!t->path) {
            free(t);
            return NULL;
        }
    }
    return t;
}

void rx_track_free(RXTrack *t)
{
    if (!t)
        return;
    free(t->path);
    rx_meta_free(&t->meta);
    rx_meta_free(&t->file_meta);
    rx_grid_free(&t->grid);
    free(t->cues);
    free(t);
}

RXTrack *rx_track_dup(const RXTrack *src)
{
    RXTrack *t = calloc(1, sizeof(*t));
    if (!t)
        return NULL;
    *t = *src;
    t->path = NULL;
    t->meta = (RXMeta) { 0 };
    t->file_meta = (RXMeta) { 0 };
    t->grid = (RXGrid) { .beats_per_bar = src->grid.beats_per_bar };
    t->cues = NULL;
    t->nb_cues = 0;

    if ((src->path && !(t->path = rx_strdup(src->path))) ||
        rx_meta_copy(&t->meta, &src->meta) < 0 ||
        rx_meta_copy(&t->file_meta, &src->file_meta) < 0 ||
        rx_grid_copy(&t->grid, &src->grid) < 0)
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
    rx_track_free(t);
    return NULL;
}

/* ------------------------------------------------------------- cue lists */

int rx_track_add_cue(RXTrack *t, const RXCue *cue)
{
    if (cue->hot < -1 || cue->hot >= RX_HOT_CUE_SLOTS || !isfinite(cue->time))
        return RX_ERROR(EINVAL);

    /* Grow first so a failure leaves the list as it was. */
    RXCue *cues = realloc(t->cues, (size_t)(t->nb_cues + 1) * sizeof(*cues));
    if (!cues)
        return RX_ERROR(ENOMEM);
    t->cues = cues;

    if (cue->hot >= 0) {
        int old = rx_track_hot_cue(t, cue->hot);
        if (old >= 0)
            rx_track_remove_cue(t, old);
    }

    int at = 0;
    while (at < t->nb_cues && t->cues[at].time <= cue->time)
        at++;
    memmove(&t->cues[at + 1], &t->cues[at], (size_t)(t->nb_cues - at) * sizeof(*t->cues));
    t->cues[at] = *cue;
    t->cues[at].name[RX_CUE_NAME_MAX - 1] = '\0';
    t->cues[at].color[RX_COLOR_MAX - 1] = '\0';
    t->nb_cues++;
    return at;
}

int rx_track_remove_cue(RXTrack *t, int index)
{
    if (index < 0 || index >= t->nb_cues)
        return RX_ERROR(EINVAL);
    memmove(&t->cues[index], &t->cues[index + 1],
            (size_t)(t->nb_cues - index - 1) * sizeof(*t->cues));
    t->nb_cues--;
    return 0;
}

int rx_track_hot_cue(const RXTrack *t, int slot)
{
    for (int i = 0; i < t->nb_cues; i++) {
        if (t->cues[i].hot == slot && slot >= 0)
            return i;
    }
    return -1;
}

int rx_track_memory_cue_at(const RXTrack *t, double time, double tolerance)
{
    int best = -1;
    for (int i = 0; i < t->nb_cues; i++) {
        const RXCue *c = &t->cues[i];
        if (rx_cue_is_hot(c) || fabs(c->time - time) > tolerance)
            continue;
        if (best < 0 || fabs(c->time - time) < fabs(t->cues[best].time - time))
            best = i;
    }
    return best;
}

int rx_track_memory_cue_before(const RXTrack *t, double time, double tolerance)
{
    int best = -1;
    for (int i = 0; i < t->nb_cues; i++) {
        const RXCue *c = &t->cues[i];
        if (!rx_cue_is_hot(c) && c->time < time - tolerance)
            best = i;
    }
    return best;
}

int rx_track_memory_cue_after(const RXTrack *t, double time, double tolerance)
{
    for (int i = 0; i < t->nb_cues; i++) {
        const RXCue *c = &t->cues[i];
        if (!rx_cue_is_hot(c) && c->time > time + tolerance)
            return i;
    }
    return -1;
}

int rx_track_memory_cue_nth(const RXTrack *t, int n)
{
    for (int i = 0; i < t->nb_cues; i++) {
        if (!rx_cue_is_hot(&t->cues[i]) && --n == 0)
            return i;
    }
    return -1;
}

void rx_track_resolve_cues(RXTrack *t)
{
    const RXGrid *g = &t->grid;
    if (!rx_grid_valid(g))
        return;

    for (int i = 0; i < t->nb_cues; i++) {
        RXCue *c = &t->cues[i];
        if (rx_cue_is_musical(c))
            c->time = rx_grid_beat_to_time(g, c->grid_beat);
        if (isnan(c->loop_beats))
            continue;
        if (rx_cue_is_musical(c))
            c->loop_end = rx_grid_beat_to_time(g, c->grid_beat + c->loop_beats);
        else
            c->loop_end = rx_grid_offset_beats(g, c->time, c->loop_beats);
        /* A grid edit can carry a loop on the pickup before 0, where a
         * negative end would read as "not a loop" and the next save would
         * drop it; loop_beats brings the end back with the grid. */
        c->loop_end = fmax(c->loop_end, 0.0);
    }

    /* Musical and absolute cues can trade places after a grid edit:
     * insertion sort keeps equal times in their order. */
    for (int i = 1; i < t->nb_cues; i++) {
        RXCue c = t->cues[i];
        int j = i;
        while (j > 0 && t->cues[j - 1].time > c.time) {
            t->cues[j] = t->cues[j - 1];
            j--;
        }
        t->cues[j] = c;
    }
}

const char *rx_track_display(const RXTrack *t, char *buf, size_t size)
{
    if (!size)
        return buf;
    const char *title = t->meta.title, *artist = t->meta.artist;
    if (artist && title)
        snprintf(buf, size, "%s - %s", artist, title);
    else if (title)
        snprintf(buf, size, "%s", title);
    else
        snprintf(buf, size, "%s", t->path ? rx_path_base(t->path) : "");
    return buf;
}
