/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* Beat grid arithmetic (rxbox/model.h).
 *
 * A grid is a list of constant-tempo sections. Section i has its beats at
 * start + k * 60/bpm for k = 0 .. n-1, where n counts the beats before the
 * next section's start exactly as dj's Grid.beats() does
 * (ceil((end - start) / period - 1e-9)); the first section also has every
 * k < 0, back to the start of the track and beyond, and the last section
 * has every k >= 0. Every beat time anywhere in this file is computed as
 * `start + (double)k * period` and nothing else, so the beat list, the
 * nearest beat, quantize and an integer beat index all agree to the bit.
 *
 * Numbering. Beats are counted along the grid with one running index; the
 * running index of a section's first beat is the previous section's plus
 * its beat count. The beat number in the bar (1..beats_per_bar) continues
 * across a section boundary unless the section sets `beat`, which restarts
 * the count there. A bar starts at every beat whose number does not exceed
 * the previous beat's (normally number 1; a restart can also make a short
 * bar). Index 0 and bar 1 are the first downbeat at or after time 0.
 *
 * dj's sections always restarted at beat 1 and its "bar N" was plain
 * arithmetic over the beat list (first downbeat + 4 * (N - 1)). Here a bar
 * is a real bar of the numbering, so after a restart "bar N" is still on a
 * beat 1 and the bar counter on the deck agrees with the drawn downbeats.
 * The two agree whenever sections begin on bar lines.
 *
 * Between the last beat of a section and the next section's start (a gap
 * shorter than a beat) the fractional beat index is interpolated linearly,
 * as dj's beat_time() did. Every query walks the sections from the first:
 * there are rarely more than a handful, and the walk keeps the struct plain
 * data with no cache to invalidate. */

#include "rxbox/model.h"

#include <math.h>

#include "core.h"

/* Index clamp for absurd inputs: beat counts stay exact in a double. */
#define K_LIMIT ((int64_t)1 << 50)

static int64_t floor_div(int64_t a, int64_t b)
{
    int64_t q = a / b;
    if (a % b && (a < 0) != (b < 0))
        q--;
    return q;
}

static int64_t floor_mod(int64_t a, int64_t b)
{
    int64_t m = a % b;
    return m < 0 ? m + b : m;
}

static int64_t clamp_k(double x)
{
    if (!(x > -(double)K_LIMIT))
        return -K_LIMIT;
    if (x > (double)K_LIMIT)
        return K_LIMIT;
    return (int64_t)x;
}

static int bpb_of(const RxGrid *g)
{
    return g->beats_per_bar > 0 ? g->beats_per_bar : 4;
}

/* ------------------------------------------------------------- the walk */

/* One section during a walk, with its running beat index and numbering. */
typedef struct Sec {
    int     index;
    int     bpb;
    double  start, period;
    double  end;            /* the next section's start; INFINITY for the last */
    int64_t n;              /* beats in [start, end); K_LIMIT for the last */
    int64_t g;              /* running index of beat k = 0 */
    int     ph;             /* number of beat k = 0, 1..bpb */
    int64_t bar;            /* running bar of beat k = 0 */
    bool    last;
    /* The last beat before this section (valid from the second section). */
    double  prev_time;
    int64_t prev_g, prev_bar;
    int     prev_number;
} Sec;

typedef struct Beat {
    double  time;
    int64_t g;              /* running index */
    int64_t bar;            /* running bar */
    int     number;
} Beat;

static double beat_time(const Sec *s, int64_t k)
{
    return s->start + (double)k * s->period;
}

static Beat beat_at(const Sec *s, int64_t k)
{
    return (Beat) {
        .time   = beat_time(s, k),
        .g      = s->g + k,
        .bar    = s->bar + floor_div(s->ph - 1 + k, s->bpb),
        .number = (int)floor_mod(s->ph - 1 + k, s->bpb) + 1,
    };
}

/* True when the section has its own last beat (k = n - 1); an inner
 * section shorter than a billionth of a beat has none. */
static bool has_last(const Sec *s)
{
    return s->index == 0 || s->n > 0;
}

/* The last beat at or before the end of this section: its own, or the one
 * carried from before. Not for the last section. */
static Beat last_beat(const Sec *s)
{
    if (has_last(s))
        return beat_at(s, s->n - 1);
    return (Beat) {
        .time   = s->prev_time,
        .g      = s->prev_g,
        .bar    = s->prev_bar,
        .number = s->prev_number,
    };
}

static void set_extent(const RxGrid *g, Sec *s)
{
    s->last = s->index + 1 >= g->nb_sections;
    if (s->last) {
        s->end = INFINITY;
        s->n = K_LIMIT;
        return;
    }
    s->end = g->sections[s->index + 1].start;
    /* dj: k1 = ceil((end - start) / period - 1e-9), then t < end. */
    int64_t n = clamp_k(ceil((s->end - s->start) / s->period - 1e-9));
    if (n < 0)
        n = 0;
    while (n > 0 && beat_time(s, n - 1) >= s->end)
        n--;
    s->n = n;
}

static void walk_init(const RxGrid *g, Sec *s)
{
    const RxTempoSection *t = &g->sections[0];
    int bpb = bpb_of(g);
    *s = (Sec) {
        .index  = 0,
        .bpb    = bpb,
        .start  = t->start,
        .period = 60.0 / t->bpm,
        .g      = 0,
        .ph     = t->beat >= 1 && t->beat <= bpb ? t->beat : 1,
        .bar    = 0,
    };
    set_extent(g, s);
}

static bool walk_next(const RxGrid *g, Sec *s)
{
    if (s->last)
        return false;

    Beat prev = last_beat(s);
    int64_t next_g = s->g + s->n;
    const RxTempoSection *t = &g->sections[s->index + 1];

    s->index++;
    s->start = t->start;
    s->period = 60.0 / t->bpm;
    s->g = next_g;
    s->ph = t->beat >= 1 && t->beat <= s->bpb ? t->beat : prev.number % s->bpb + 1;
    s->bar = s->ph <= prev.number ? prev.bar + 1 : prev.bar;
    s->prev_time = prev.time;
    s->prev_g = prev.g;
    s->prev_bar = prev.bar;
    s->prev_number = prev.number;
    set_extent(g, s);
    return true;
}

/* To the section whose time range holds t (the first one for t before it). */
static void walk_to_time(const RxGrid *g, double t, Sec *s)
{
    walk_init(g, s);
    while (!s->last && g->sections[s->index + 1].start <= t)
        walk_next(g, s);
}

/* To the section holding running index x (fractional allowed); empty
 * sections are skipped since the next one has the same index. */
static void walk_to_index(const RxGrid *g, double x, Sec *s)
{
    walk_init(g, s);
    while (!s->last && (double)(s->g + s->n) <= x)
        walk_next(g, s);
}

/* Largest k with beat_time(k) <= t / smallest with beat_time(k) >= t. */
static int64_t floor_k(const Sec *s, double t)
{
    int64_t k = clamp_k(floor((t - s->start) / s->period));
    while (k < K_LIMIT && beat_time(s, k + 1) <= t)
        k++;
    while (k > -K_LIMIT && beat_time(s, k) > t)
        k--;
    return k;
}

static int64_t ceil_k(const Sec *s, double t)
{
    int64_t k = clamp_k(ceil((t - s->start) / s->period));
    while (k > -K_LIMIT && beat_time(s, k - 1) >= t)
        k--;
    while (k < K_LIMIT && beat_time(s, k) < t)
        k++;
    return k;
}

/* The first beat of the sections after s (walks s forward). */
static Beat next_section_beat(const RxGrid *g, Sec *s)
{
    while (walk_next(g, s)) {
        if (s->n > 0 || s->last)
            break;
    }
    return beat_at(s, 0);
}

/* The last beat at or before t and the first one after it. */
static void locate(const RxGrid *g, double t, Beat *before, Beat *after)
{
    Sec s;
    walk_to_time(g, t, &s);

    int64_t k = floor_k(&s, t);
    if (k >= s.n || !has_last(&s)) {
        /* In the gap after this section's last beat. */
        *before = last_beat(&s);
        if (after)
            *after = next_section_beat(g, &s);
        return;
    }

    *before = beat_at(&s, k);
    if (!after)
        return;
    if (k + 1 < s.n)
        *after = beat_at(&s, k + 1);
    else
        *after = next_section_beat(g, &s);
}

/* The reference point of the numbering, in running units: the first
 * downbeat at or after time 0 is beat index 0 and bar 1. */
typedef struct Ref {
    int64_t g_down, bar_down;
} Ref;

static Ref grid_ref(const RxGrid *g)
{
    Ref r = { 0 };
    Sec s;
    walk_init(g, &s);
    while (1) {
        int64_t lo = ceil_k(&s, 0.0);
        if (s.index > 0 && lo < 0)
            lo = 0;
        if (lo < s.n) {
            int64_t kd = lo + floor_mod(-(s.ph - 1 + lo), s.bpb);
            if (kd < s.n) {
                Beat b = beat_at(&s, kd);
                r.g_down = b.g;
                r.bar_down = b.bar;
                return r;
            }
        }
        if (!walk_next(g, &s))
            break;
    }
    return r;
}

/* The fraction is measured from the beat at or before t, so a beat's own
 * time gives exactly its integer index. */
static double walk_time_to_index(const RxGrid *g, double t)
{
    Sec s;
    walk_to_time(g, t, &s);
    if (!s.last) {
        Beat lb = last_beat(&s);
        if (t > lb.time)
            return (double)lb.g + (t - lb.time) / (s.end - lb.time);
    }
    int64_t k = floor_k(&s, t);
    double tk = beat_time(&s, k);
    return (double)(s.g + k) + (t - tk) / (beat_time(&s, k + 1) - tk);
}

static double walk_index_to_time(const RxGrid *g, double x)
{
    Sec s;
    walk_to_index(g, x, &s);
    if (!s.last) {
        Beat lb = last_beat(&s);
        if (x > (double)lb.g)
            return lb.time + (x - (double)lb.g) * (s.end - lb.time);
    }
    return s.start + (x - (double)s.g) * s.period;
}

/* Running index of the beat of (bar, number), bars counted from the first
 * downbeat. A number the bar lacks (a short bar, or > beats_per_bar) counts
 * on from the bar's first beat, as dj's arithmetic did. */
static int64_t bar_beat_index(const RxGrid *g, const Ref *r, int64_t bar, int64_t beat)
{
    int64_t target = bar - 1 + r->bar_down;
    bool have_first = false;
    int64_t first_g = 0;
    int first_number = 1;

    Sec s;
    walk_init(g, &s);
    while (1) {
        if (has_last(&s)) {
            int64_t klo = s.index == 0 ? -K_LIMIT : 0;
            int64_t khi = s.n - 1;
            /* The k of this section in the target bar. */
            int64_t a = (target - s.bar) * s.bpb - (s.ph - 1);
            int64_t b = a + s.bpb - 1;
            a = RX_MAX(a, klo);
            b = RX_MIN(b, khi);
            if (a <= b) {
                if (!have_first) {
                    first_g = s.g + a;
                    first_number = beat_at(&s, a).number;
                    have_first = true;
                }
                int64_t k = (target - s.bar) * s.bpb + beat - s.ph;
                if (beat >= 1 && beat <= s.bpb && k >= a && k <= b)
                    return s.g + k;
            } else if (have_first) {
                break;
            }
        }
        if (!walk_next(g, &s))
            break;
    }

    if (have_first)
        return first_g + (beat - first_number);
    return r->g_down + (bar - 1) * bpb_of(g) + beat - 1;
}

/* ------------------------------------------------------------ lifecycle */

int rx_grid_init(RxGrid *g, double start, double bpm)
{
    *g = (RxGrid) { .beats_per_bar = 4 };
    if (!isfinite(start) || !isfinite(bpm) || bpm <= 0)
        return RX_ERROR(EINVAL);
    g->sections = malloc(sizeof(*g->sections));
    if (!g->sections)
        return RX_ERROR(ENOMEM);
    g->sections[0] = (RxTempoSection) {
        .start = start,
        .bpm   = bpm,
        .beat  = 1,
    };
    g->nb_sections = 1;
    return 0;
}

/* dst may hold a grid (it is freed) or be zeroed; dst == src is fine. */
int rx_grid_copy(RxGrid *dst, const RxGrid *src)
{
    if (dst == src)
        return 0;
    RxTempoSection *secs = NULL;
    if (src->nb_sections > 0) {
        secs = malloc((size_t)src->nb_sections * sizeof(*secs));
        if (!secs)
            return RX_ERROR(ENOMEM);
        memcpy(secs, src->sections, (size_t)src->nb_sections * sizeof(*secs));
    }
    free(dst->sections);
    dst->sections = secs;
    dst->nb_sections = secs ? src->nb_sections : 0;
    dst->beats_per_bar = src->beats_per_bar > 0 ? src->beats_per_bar : 4;
    return 0;
}

void rx_grid_free(RxGrid *g)
{
    if (!g)
        return;
    free(g->sections);
    g->sections = NULL;
    g->nb_sections = 0;
}

bool rx_grid_valid(const RxGrid *g)
{
    if (!g || g->nb_sections < 1 || !g->sections)
        return false;
    if (g->beats_per_bar < 0 || g->beats_per_bar > 64)
        return false;
    for (int i = 0; i < g->nb_sections; i++) {
        const RxTempoSection *s = &g->sections[i];
        if (!isfinite(s->start) || !isfinite(s->bpm) || s->bpm <= 0)
            return false;
        if (i > 0 && !(s->start > g->sections[i - 1].start))
            return false;
    }
    return true;
}

double rx_grid_bpm(const RxGrid *g)
{
    return g && g->nb_sections > 0 ? g->sections[0].bpm : 0.0;
}

int rx_grid_section_at(const RxGrid *g, double t)
{
    if (!g || g->nb_sections < 1)
        return -1;
    int i = 0;
    while (i + 1 < g->nb_sections && g->sections[i + 1].start <= t)
        i++;
    return i;
}

double rx_grid_bpm_at(const RxGrid *g, double t)
{
    int i = rx_grid_section_at(g, t);
    return i < 0 ? 0.0 : g->sections[i].bpm;
}

/* Section starts closer than this are the same start. */
#define SAME_START 1e-4

int rx_grid_add_section(RxGrid *g, double start, double bpm, int beat)
{
    int bpb = bpb_of(g);
    if (!isfinite(start) || !isfinite(bpm) || bpm <= 0 || beat < 0 || beat > bpb)
        return RX_ERROR(EINVAL);

    RxTempoSection sec = {
        .start = start,
        .bpm   = bpm,
        .beat  = beat,
    };

    int at = 0;
    while (at < g->nb_sections && g->sections[at].start < start - SAME_START)
        at++;
    if (at < g->nb_sections && fabs(g->sections[at].start - start) <= SAME_START) {
        g->sections[at] = sec;
        return at;
    }

    RxTempoSection *secs = realloc(g->sections, (size_t)(g->nb_sections + 1) * sizeof(*secs));
    if (!secs)
        return RX_ERROR(ENOMEM);
    memmove(&secs[at + 1], &secs[at], (size_t)(g->nb_sections - at) * sizeof(*secs));
    secs[at] = sec;
    g->sections = secs;
    g->nb_sections++;
    if (g->beats_per_bar <= 0)
        g->beats_per_bar = 4;
    return at;
}

int rx_grid_remove_section(RxGrid *g, int index)
{
    if (!g || index < 0 || index >= g->nb_sections)
        return RX_ERROR(EINVAL);
    memmove(&g->sections[index], &g->sections[index + 1],
            (size_t)(g->nb_sections - index - 1) * sizeof(*g->sections));
    g->nb_sections--;
    if (!g->nb_sections) {
        free(g->sections);
        g->sections = NULL;
    }
    return 0;
}

/* ----------------------------------------------------------------- beats */

int rx_grid_beats(const RxGrid *g, double duration, RxBeat **beats, int *nb)
{
    *beats = NULL;
    *nb = 0;
    if (!rx_grid_valid(g) || !(duration > 0))
        return 0;

    /* Two passes: count, then fill. */
    RxBeat *out = NULL;
    int64_t total = 0;
    for (int pass = 0; pass < 2; pass++) {
        int64_t i = 0;
        Sec s;
        walk_init(g, &s);
        do {
            if (!has_last(&s))
                continue;
            int64_t lo = ceil_k(&s, 0.0);
            if (s.index > 0 && lo < 0)
                lo = 0;
            int64_t hi = RX_MIN(s.n, ceil_k(&s, duration));
            for (int64_t k = lo; k < hi; k++) {
                if (pass == 1) {
                    Beat b = beat_at(&s, k);
                    out[i] = (RxBeat) {
                        .time   = b.time,
                        .number = b.number,
                        .bpm    = g->sections[s.index].bpm,
                    };
                }
                i++;
            }
        } while (walk_next(g, &s));

        if (pass == 0) {
            if (i == 0)
                return 0;
            if (i > INT32_MAX)
                return RX_ERROR(ERANGE);
            total = i;
            out = malloc((size_t)total * sizeof(*out));
            if (!out)
                return RX_ERROR(ENOMEM);
        }
    }

    *beats = out;
    *nb = (int)total;
    return 0;
}

double rx_grid_nearest_beat(const RxGrid *g, double t, int *number)
{
    if (!rx_grid_valid(g)) {
        if (number)
            *number = 0;
        return t;
    }
    Beat before, after;
    locate(g, t, &before, &after);
    Beat *b = after.time - t < t - before.time ? &after : &before;
    if (number)
        *number = b->number;
    return b->time;
}

double rx_grid_quantize(const RxGrid *g, double t)
{
    return rx_grid_nearest_beat(g, t, NULL);
}

/* The 1 ms of the header: a beat that close to t counts as "at" t. */
#define AT_TOLERANCE 0.001

double rx_grid_beat_before(const RxGrid *g, double t)
{
    if (!rx_grid_valid(g))
        return t;
    Beat before;
    locate(g, t + AT_TOLERANCE, &before, NULL);
    return before.time;
}

double rx_grid_beat_after(const RxGrid *g, double t)
{
    if (!rx_grid_valid(g))
        return t;
    Beat before, after;
    locate(g, t + AT_TOLERANCE, &before, &after);
    return after.time;
}

double rx_grid_time_to_beat(const RxGrid *g, double t)
{
    if (!rx_grid_valid(g))
        return NAN;
    Ref r = grid_ref(g);
    return walk_time_to_index(g, t) - (double)r.g_down;
}

double rx_grid_beat_to_time(const RxGrid *g, double beat)
{
    if (!rx_grid_valid(g))
        return NAN;
    Ref r = grid_ref(g);
    return walk_index_to_time(g, beat + (double)r.g_down);
}

double rx_grid_offset_beats(const RxGrid *g, double t, double beats)
{
    if (!rx_grid_valid(g))
        return t;
    return walk_index_to_time(g, walk_time_to_index(g, t) + beats);
}

double rx_grid_bar_time(const RxGrid *g, int bar, int beat)
{
    if (!rx_grid_valid(g))
        return NAN;
    Ref r = grid_ref(g);
    return walk_index_to_time(g, (double)bar_beat_index(g, &r, bar, beat));
}

void rx_grid_bar_of(const RxGrid *g, double t, int *bar, int *beat)
{
    if (!rx_grid_valid(g)) {
        *bar = 0;
        *beat = 0;
        return;
    }
    Ref r = grid_ref(g);
    Beat before;
    /* A nanosecond of slack so a time computed onto a beat by another
     * route than beat_time() is not counted in the beat before it. */
    locate(g, t + 1e-9, &before, NULL);
    *bar = (int)(before.bar - r.bar_down + 1);
    *beat = before.number;
}

/* ----------------------------------------------------------------- edits */

void rx_grid_shift(RxGrid *g, double delta)
{
    for (int i = 0; i < g->nb_sections; i++)
        g->sections[i].start += delta;
}

void rx_grid_set_bpm_at(RxGrid *g, double t, double bpm)
{
    int i = rx_grid_section_at(g, t);
    if (i < 0 || !isfinite(bpm) || bpm <= 0)
        return;
    g->sections[i].bpm = bpm;
}

/* The section and k of the beat nearest to t. */
static Beat nearest(const RxGrid *g, double t, Sec *s, int64_t *k)
{
    Beat before, after;
    locate(g, t, &before, &after);
    Beat b = after.time - t < t - before.time ? after : before;
    walk_to_index(g, (double)b.g, s);
    *k = b.g - s->g;
    return b;
}

void rx_grid_set_downbeat(RxGrid *g, double t)
{
    if (!rx_grid_valid(g))
        return;
    Sec s;
    int64_t k;
    Beat b = nearest(g, t, &s, &k);
    RxTempoSection *sec = &g->sections[s.index];
    if (s.index == 0) {
        /* The first section extends both ways, so its start may be any of
         * its beats: moving it onto the new downbeat keeps the usual form
         * (start is a downbeat, beat 1) that the stick writers expect. */
        sec->start = b.time;
        sec->beat = 1;
    } else {
        sec->beat = (int)floor_mod(-k, s.bpb) + 1;
    }
}

int rx_grid_split_at(RxGrid *g, double t)
{
    if (!rx_grid_valid(g))
        return RX_ERROR(EINVAL);
    Sec s;
    int64_t k;
    Beat b = nearest(g, t, &s, &k);
    if (k == 0)
        return s.index;
    /* Same tempo, the count continues: nothing moves until one side is
     * edited. A split before the first section's start becomes the new
     * first section, which must name its beat. */
    int beat = b.time < g->sections[0].start ? b.number : 0;
    return rx_grid_add_section(g, b.time, g->sections[s.index].bpm, beat);
}

int rx_grid_merge_at(RxGrid *g, double t)
{
    if (!g || g->nb_sections < 2)
        return RX_ERROR(ENOENT);
    int best = 1;
    for (int i = 2; i < g->nb_sections; i++) {
        if (fabs(g->sections[i].start - t) < fabs(g->sections[best].start - t))
            best = i;
    }
    int ret = rx_grid_remove_section(g, best);
    return ret < 0 ? ret : best - 1;
}

/* ------------------------------------------------- positions (core only) */

/* Beat index (rx_grid_time_to_beat units) of "bar N.B". */
double rx_core_grid_bar_index(const RxGrid *g, int64_t bar, int64_t beat)
{
    Ref r = grid_ref(g);
    return (double)(bar_beat_index(g, &r, bar, beat) - r.g_down);
}

/* Bar and number of an integer beat index: the inverse of the above. */
void rx_core_grid_describe(const RxGrid *g, int64_t grid_beat, int64_t *bar, int *beat)
{
    Ref r = grid_ref(g);
    Sec s;
    walk_to_index(g, (double)(grid_beat + r.g_down), &s);
    Beat b = beat_at(&s, grid_beat + r.g_down - s.g);
    *bar = b.bar - r.bar_down + 1;
    *beat = b.number;
}
