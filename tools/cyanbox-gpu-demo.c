/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* cyanbox-gpu-demo: the gpu module's acceptance test and demo.
 *
 * Opens the window and draws, every frame, ImGui's demo window and a panel
 * of synthetic waveforms: 600 columns of a made-up kick/snare pattern as a
 * 3-band detail (whole, and zoomed around a scrolling playhead so that the
 * columns are wider than the pixels), a longer pattern with one-column
 * spikes at 1.6 columns a pixel scrolling as the player's detail view does
 * (12 bars at 128-175 BPM), the 600 columns stacked and overlaid as half
 * waveforms, a blue one and an RGB one, plus a texture. After N frames it
 * writes a screenshot of the last frame. The last line it prints has the
 * mean GPU time of a frame's rendering (timestamps).
 *
 *   --check       also draw a reference strip of known columns and check
 *                 pixels of the last frame against the colour rules, and
 *                 draws that must leave their box black (no columns,
 *                 reversed or NaN columns, an infinite rectangle) or stay
 *                 inside it (insets larger than the rectangle), and the
 *                 gain (a quiet column drawn up to full height, or not),
 *                 and the horizontal rules: a spike and an attack drawn a
 *                 quarter of a pixel off the grid, box-filtered or split
 *                 as light, and the maximum kept where it should be; and
 *                 1 px lines drawn with cb_vbar_draw an eighth of a pixel
 *                 apart, each with a whole pixel's light, where it was put
 *   --scroll      draw the zoomed detail rows (about 120 columns,
 *                 interpolated; 1.6 columns a pixel) of the spike pattern
 *                 over black and over a dark grey, moving 0.07 columns a
 *                 frame for 40 frames, read every frame back, and check
 *                 that the picture moves smoothly: the row's total
 *                 coverage stays constant, consecutive frames differ
 *                 little and steadily, and every one-column spike of the
 *                 1.6 row gives the same light (see scroll_check)
 *   --bench COLS  draw four full-window detail waveforms at COLS columns
 *                 a pixel behind everything, for the shader's cost
 *   --idle        stop asking for redraws after the first frames (the
 *                 loop should then sleep: watch it with top)
 *   --validation  Vulkan validation layer; any error fails the run
 *   --stats       once a second: frames drawn, loop iterations, and
 *                 whether the compositor still owes a frame callback
 *
 * Open windows only on a private compositor (docs/HACKING.md). */

#include <getopt.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cyanbox/common.h"
#include "cyanbox/gpu.h"

#include "src/gpu/gpu_demo.h"

#define NB_COLUMNS 600
#define NB_REF     8

/* The spike pattern repeats every SPIKE_PERIOD columns; the long waveform
 * has enough of them for a full window at 40 columns a pixel. */
#define SPIKE_PERIOD  41
#define SPIKE_COLUMNS (1 << 17)

/* --scroll: rows of SCROLL_W logical pixels, each drawn twice (over black
 * and over a dark grey, which together give the alpha; over white, a pixel
 * whose colour, blended as light, is brighter than its alpha would clip),
 * stepped SCROLL_STEP columns a frame for SCROLL_FRAMES frames from frame
 * SCROLL_START.
 * 1.6 columns a pixel over SCROLL_W is 40 whole periods, and a period is
 * 25.625 pixels, so the pixels meet the spikes at every phase rather than
 * all at the same one: what the row shows is the average over phases. */
#define NB_SCROLL     2
#define SCROLL_FRAMES 40
#define SCROLL_START  5
#define SCROLL_STEP   0.07
#define SCROLL_X      20.0f
#define SCROLL_Y      40.0f
#define SCROLL_W      1025.0f
#define SCROLL_H      90.0f
#define SCROLL_GAP    10.0f
/* A colour of alpha a blended as light is at most a encoded, below
 * 1 - 0.44 * (1 - a): over a grey under 0.44 nothing clips. */
#define SCROLL_GREY   0x40

/* --bench: full-window layers a frame. */
#define BENCH_LAYERS  4
/* Frames left out of the GPU time: the first ones compile, upload and
 * resize. */
#define GPU_WARMUP    10

typedef struct ScrollRow {
    double   cov[SCROLL_FRAMES];    /* sum of alpha over the row */
    double   mad[SCROLL_FRAMES];    /* mean |difference| of light from the frame before */
    uint8_t *prev;                  /* the frame before, over black, RGB */
    int      nb_frames;
    double   light_lo, light_hi;    /* the one-column spikes' light, every frame (1.6 row) */
    double   light_sum;
    int      nb_lights;
} ScrollRow;

typedef struct Demo {
    CBWindow  *w;
    uint8_t    blue_cols[NB_COLUMNS];
    CBWaveGpu *band, *blue, *rgb;
    CBWaveGpu *spikes;              /* SPIKE_COLUMNS of the spike pattern */
    CBWaveGpu *ref[NB_REF];
    CBWaveGpu *empty;               /* uploaded with no columns */
    CBWaveGpu *quiet;               /* one column, low band at 32 */
    CBWaveGpu *spike, *attack;      /* ref_spike, ref_attack */
    uint64_t   tex;
    bool       check;
    bool       scroll;
    double     bench;               /* columns a pixel, 0 = off */
    double     playhead;
    double     spike_head;
    ScrollRow  rows[NB_SCROLL];
    bool       scroll_lost;         /* a frame of the scroll could not be read back */
    double     gpu_ms;              /* sum over gpu_frames frames */
    int        gpu_frames;
} Demo;

/* ---------------------------------------------------------------- synthesis */

static uint8_t level(double v)
{
    return (uint8_t)lround(CB_CLAMP(v, 0.0, 127.0));
}

/* The light of an 8-bit encoded value read back, 0 to 1. */
static double srgb_light(int v)
{
    static double lut[256];
    static bool init;
    if (!init) {
        for (int i = 0; i < 256; i++) {
            double c = i / 255.0;
            lut[i] = c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
        }
        init = true;
    }
    return lut[v & 0xFF];
}

/* 120 BPM at 150 columns a second: a beat every 75 columns, kick on the
 * odd beats, snare on the even ones, hats on the eighths, a bass line
 * under it, then a fade and silence at the end. */
static void synth(uint8_t *band, uint8_t *blue, uint8_t *rgb)
{
    for (int i = 0; i < NB_COLUMNS; i++) {
        int beat = i / 75, in_beat = i % 75;
        double low, mid, high;

        if (beat % 2 == 0) {
            low  = 122.0 * exp(-in_beat / 14.0);
            mid  = 40.0 * exp(-in_beat / 5.0);
            high = 6.0 * exp(-in_beat / 3.0);
        } else {
            low  = 22.0 * exp(-in_beat / 6.0);
            mid  = 100.0 * exp(-in_beat / 10.0);
            high = 70.0 * exp(-in_beat / 6.0);
        }
        double in_eighth = fmod(i, 37.5);
        high = fmax(high, 38.0 * exp(-in_eighth / 3.0));
        low  = fmax(low, 28.0 + 12.0 * sin(i * 0.07));
        mid  = fmax(mid, 12.0 + 6.0 * sin(i * 0.11));

        double fade = i < 500 ? 1.0 : i < 540 ? (540 - i) / 40.0 : 0.0;
        low *= fade;
        mid *= fade;
        high *= fade;

        band[3 * i + 0] = level(low);
        band[3 * i + 1] = level(mid);
        band[3 * i + 2] = level(high);

        /* PWV3-style: height from the loudest band, whiteness from how
         * much of it is above the bass. */
        double peak = fmax(low, fmax(mid, high));
        double sum = low + mid + high;
        int h = (int)lround(31.0 * peak / 127.0);
        int white = sum > 0.0 ? (int)lround(7.0 * (0.5 * mid + high) / sum) : 7;
        blue[i] = (uint8_t)(CB_CLAMP(white, 0, 7) << 5 | CB_CLAMP(h, 0, 31));

        /* PWV5-style big-endian rrrgggbbbhhhhh00. */
        unsigned r = (unsigned)lround(7.0 * low / 127.0);
        unsigned g = (unsigned)lround(7.0 * mid / 127.0);
        unsigned b = (unsigned)lround(7.0 * high / 127.0);
        unsigned word = r << 13 | g << 10 | b << 7 | (unsigned)CB_CLAMP(h, 0, 31) << 2;
        rgb[2 * i + 0] = (uint8_t)(word >> 8);
        rgb[2 * i + 1] = (uint8_t)word;
    }
}

/* A period of SPIKE_PERIOD columns: a kick (a sharp attack from its own
 * tail, then a decay), two hats, a one-column spike in the mid and high
 * bands, and three silent columns that the next column attacks from. The
 * spike is what shimmered when a pixel took the maximum over one column or
 * two; whole periods in a row keep the row's total coverage constant while
 * it scrolls, which is what --scroll checks. */
static void synth_spikes(uint8_t *band, int nb)
{
    for (int i = 0; i < nb; i++) {
        int p = i % SPIKE_PERIOD;
        double low  = fmax(120.0 * exp(-p / 9.0), 18.0);
        double mid  = 8.0 + 30.0 * exp(-p / 4.0);
        double high = 40.0 * exp(-(p % 20) / 2.0);
        if (p == 27) {
            low  = 30.0;
            mid  = 110.0;
            high = 127.0;
        } else if (p >= 34 && p < 37) {
            low = mid = high = 0.0;
        }
        band[3 * i + 0] = level(low);
        band[3 * i + 1] = level(mid);
        band[3 * i + 2] = level(high);
    }
}

/* ------------------------------------------------------------ reference */

/* Strips of columns whose colours are known exactly, drawn side by side
 * at a fixed place over everything else, each column REF_COL wide. */
static const uint8_t ref_band[] = {
    127, 0, 0,      /* low: red */
    0, 127, 0,      /* mid: green */
    0, 0, 127,      /* high: blue */
    127, 64, 0,     /* low+mid at the core, low outside */
    127, 127, 127,  /* all */
    0, 0, 0,        /* silence: a line */
    64, 0, 127,     /* low+high at the core, high outside */
    0, 127, 127,    /* mid+high */
};
static const uint8_t ref_blue[]   = { 0 << 5 | 31, 7 << 5 | 31 };
static const uint8_t ref_rgb[]    = { 0xE0, 31 << 2, 0xEC, 31 << 2 };  /* r=7; r=7 g=3 */
static const uint8_t ref_pwv4[]   = { 127, 0, 0, 100, 50, 0 };
static const uint8_t ref_stack[]  = { 64, 0, 64, 64, 127, 0 };          /* low, high; low, mid */
static const uint8_t ref_low[]    = { 127, 0, 0 };
static const uint8_t ref_quiet[]  = { 32, 0, 0 };
static const uint8_t ref_lowmid[] = { 127, 64, 0 };
/* Detail interpolation: 40 -> 60 ramps, 0 -> 100 is a sharp attack. */
static const uint8_t ref_interp[] = { 40, 0, 0, 60, 0, 0, 0, 0, 0, 100, 0, 0 };
/* The horizontal rules: one loud column (low at 100, a reach of 39.4 px)
 * in the middle of 41 silent ones, and silence then a sharp attack. */
static const uint8_t ref_spike[3 * 41] = { [3 * 20] = 100 };
static const uint8_t ref_attack[] = { 0, 0, 0, 0, 0, 0, 100, 0, 0, 100, 0, 0 };

typedef struct RefStrip {
    enum CBWaveKind kind;
    const uint8_t *cols;
    int nb;
    enum CBWaveMode mode;
    float dim;
    bool detail;
} RefStrip;

#define REF(k, a, m, dim, detail) \
    { k, a, (int)(sizeof(a) / (k == CB_WAVE_3BAND ? 3 : k == CB_WAVE_BLUE ? 1 : \
                               k == CB_WAVE_RGB_DETAIL ? 2 : 6)), m, dim, detail }

static const RefStrip ref_strips[NB_REF] = {
    REF(CB_WAVE_3BAND,       ref_band,   CB_WAVE_CENTRED,       0.0f, false),  /* cols 0-7 */
    REF(CB_WAVE_BLUE,        ref_blue,   CB_WAVE_CENTRED,       0.0f, false),  /* 8-9 */
    REF(CB_WAVE_RGB_DETAIL,  ref_rgb,    CB_WAVE_CENTRED,       0.0f, false),  /* 10-11 */
    REF(CB_WAVE_RGB_PREVIEW, ref_pwv4,   CB_WAVE_CENTRED,       0.0f, false),  /* 12 */
    REF(CB_WAVE_3BAND,       ref_stack,  CB_WAVE_HALF_STACKED,  0.0f, false),  /* 13-14 */
    REF(CB_WAVE_3BAND,       ref_lowmid, CB_WAVE_HALF_OVERLAID, 0.0f, false),  /* 15 */
    REF(CB_WAVE_3BAND,       ref_low,    CB_WAVE_CENTRED,       0.5f, false),  /* 16 */
    REF(CB_WAVE_3BAND,       ref_interp, CB_WAVE_CENTRED,       0.0f, true),   /* 17-20 */
};

#define REF_X      20.0f
#define REF_COL    24.0f
#define REF_H      100.0f
#define REF_GAP    12.0f

static float ref_y(CBWindow *w)
{
    int ww, wh;
    cb_window_size(w, &ww, &wh);
    return floorf(wh - REF_H - 20.0f);
}

/* Left edge of column `col`, counted across all strips. */
static float ref_col_x(int col)
{
    float x = REF_X;
    for (int i = 0; i < NB_REF; i++) {
        if (col < ref_strips[i].nb)
            return x + col * REF_COL;
        col -= ref_strips[i].nb;
        x += ref_strips[i].nb * REF_COL + REF_GAP;
    }
    return x;
}

static int ref_upload(Demo *d)
{
    for (int i = 0; i < NB_REF; i++) {
        d->ref[i] = cb_wave_upload(d->w, ref_strips[i].kind, ref_strips[i].cols,
                                   ref_strips[i].nb);
        if (!d->ref[i])
            return CB_ERROR(ENOMEM);
    }
    return 0;
}

/* ------------------------------------------------------------- edge cases */

/* Boxes right of the reference strips, EDGE_W wide, each with one draw
 * that must leave it black, or (the last) keep inside it. */
#define EDGE_W   40.0f
#define EDGE_GAP 10.0f
#define NB_EDGES 8

static float edge_x(int i)
{
    return ref_col_x(1000) - REF_GAP + 20.0f + i * (EDGE_W + EDGE_GAP);
}

static void draw_edges(Demo *d)
{
    void *dl = cb_demo_foreground_draw_list();
    float y = ref_y(d->w);
    cb_demo_rect(dl, edge_x(0) - 4, y - 24, edge_x(NB_EDGES) + 4, y + REF_H + 24, 0x000000FF);

    for (int i = 0; i < NB_EDGES; i++) {
        float x = edge_x(i);
        CBWaveDraw wd = {
            .x0 = x,  .y0 = y,
            .x1 = x + EDGE_W,  .y1 = y + REF_H,
            .col0 = 0, .col1 = 1,
            .mode  = CB_WAVE_CENTRED,
            .alpha = 1.0f,
        };
        const CBWaveGpu *g = d->ref[6];             /* one loud low column */
        switch (i) {
        case 0: g = d->empty; break;                /* no columns */
        case 1: wd.col0 = 1; wd.col1 = 0; break;    /* reversed */
        case 2: wd.col0 = NAN; break;
        case 3: wd.x1 = INFINITY; break;
        case 4: wd.alpha = NAN; break;
        case 5:                                     /* insets past the rectangle */
            wd.inset_top = 1000.0f;
            wd.inset_bottom = 1000.0f;
            wd.dim = NAN;
            wd.dim_until = NAN;
            break;
        case 6:                                     /* 32 of 127 at full height */
            g = d->quiet;
            wd.gain = 127.0f / 32.0f;
            break;
        case 7:                                     /* gain 0: none */
            g = d->quiet;
            break;
        }
        cb_wave_draw(d->w, dl, g, &wd);
    }
}

static const struct {
    const char *what;
    int box;
    float dy;           /* from the box's centre line */
    uint32_t rgb;
} edge_probes[] = {
    { "no columns",                    0,   0.5f, 0x000000 },
    { "reversed columns",              1,   0.5f, 0x000000 },
    { "NaN column",                    2,   0.5f, 0x000000 },
    { "infinite rectangle",            3,   0.5f, 0x000000 },
    { "right of the infinite one",     4,  30.0f, 0x000000 },
    { "NaN alpha",                     4,   0.5f, 0x000000 },
    { "huge insets: above the box",    5, -62.0f, 0x000000 },
    { "huge insets: below the box",    5,  62.0f, 0x000000 },
    { "huge insets: the centre line",  5,   0.0f, 0x3C6ED7 },
    { "gain: near the top",            6, -45.0f, 0x3C6ED7 },
    { "no gain: near the top",         7, -45.0f, 0x000000 },
    { "no gain: within 32/127",        7,  -8.0f, 0x3C6ED7 },
};

/* ------------------------------------------------------------- horizontal */

/* Boxes right of the edge cases for the horizontal rules, sized in device
 * pixels, a quarter of a pixel off the grid where that matters:
 *   0: the spike, detail at 1 column a pixel: box-filtered, it is in two
 *      pixels at 3/4 and 1/4 of its light;
 *   1: the attack, detail at 0.1 columns a pixel: the pixel its vertical
 *      edge runs through is 1/4 silence and 3/4 attack;
 *   2: the spike at 4 columns a pixel, not detail (a row preview): the
 *      maximum, in full;
 *   3: the spike at 20.5 columns a pixel, detail: past 16, the maximum. */
#define NB_HBOX 4

static const struct {
    bool   attack;      /* ref_attack, or ref_spike */
    int    width;       /* device pixels */
    double col0, per_px;
    bool   detail;
} hboxes[NB_HBOX] = {
    { false, 40,  0.25,   1.0, true  },
    { true,  40, -0.025,  0.1, true  },
    { false, 10,  0.0,    4.0, false },
    { false,  2,  0.0,   20.5, true  },
};

static float hbox_x(Demo *d, int box)
{
    float x = edge_x(NB_EDGES) + EDGE_GAP;
    for (int i = 0; i < box; i++)
        x += hboxes[i].width / cb_window_scale(d->w) + EDGE_GAP;
    return x;
}

static void draw_hboxes(Demo *d)
{
    void *dl = cb_demo_foreground_draw_list();
    float y = ref_y(d->w);
    cb_demo_rect(dl, hbox_x(d, 0) - 4, y - 4, hbox_x(d, NB_HBOX) + 4, y + REF_H + 4, 0x000000FF);

    for (int i = 0; i < NB_HBOX; i++) {
        float x = hbox_x(d, i);
        CBWaveDraw wd = {
            .x0 = x,  .y0 = y,
            .x1 = x + hboxes[i].width / cb_window_scale(d->w),  .y1 = y + REF_H,
            .col0 = hboxes[i].col0,
            .col1 = hboxes[i].col0 + hboxes[i].per_px * hboxes[i].width,
            .mode   = CB_WAVE_CENTRED,
            .detail = hboxes[i].detail,
            .alpha  = 1.0f,
        };
        cb_wave_draw(d->w, dl, hboxes[i].attack ? d->attack : d->spike, &wd);
    }
}

static const struct {
    const char *what;
    int box, px;        /* the box, and the device pixel in it */
    float dy;           /* from the box's centre line */
    uint32_t rgb;
} hprobes[] = {
    /* 3/4 and 1/4 of #3C6ED7's light, encoded: 3/4 and 1/4 of the encoded
     * value (#2D52A1, #0F1C36) would be much darker. */
    { "box filter: 3/4 of a spike",    0, 20, -20.0f, 0x3460BD },
    { "box filter: 1/4 of a spike",    0, 19, -20.0f, 0x1C3873 },
    { "box filter: the spike's top",   0, 20, -38.0f, 0x3460BD },
    { "box filter: above the spike",   0, 20, -41.0f, 0x000000 },
    { "box filter: silence line",      0,  5,   0.0f, 0xD2DCFA },
    { "box filter: above the line",    0,  5,  -2.0f, 0x000000 },
    { "attack split: 3/4 attack",      1, 20, -20.0f, 0x3460BD },
    { "attack split: before it",       1, 19, -20.0f, 0x000000 },
    { "attack split: after it",        1, 21, -20.0f, 0x3C6ED7 },
    { "not detail: the maximum",       2,  5, -20.0f, 0x3C6ED7 },
    { "past 16 columns: the maximum",  3,  1, -20.0f, 0x3C6ED7 },
};

/* ------------------------------------------------------------------- bars */

/* Lines one device pixel wide drawn with cb_vbar_draw right of the
 * horizontal boxes, over black, in the detail's downbeat colour (#9A9A9A
 * at 0.85), the first centred on a pixel and each next one BAR_GAP pixels
 * and an eighth of a pixel further. Each must give the light of one whole
 * pixel of the line, centred where it was put: snapped to whole pixels a
 * line is up to half a pixel off, and with its coverage as alpha one
 * between two pixels gives half the light. The first must be its one
 * pixel alone, as AddLine drew it. */
#define NB_BARS       8
#define BAR_GAP       12
#define BAR_COL       0xD99A9A9Au   /* IM_COL32(0x9A, 0x9A, 0x9A, 217) */
#define BAR_FULL      0x83          /* 0x9A at 217/255 over black */
#define BAR_MAX_LIGHT 0.03          /* (max - min) / mean of the lines' light */
#define BAR_MAX_OFF   0.05          /* device pixels from where a line was put */

/* Device pixels: the left edge of the box, and the centre of line i. */
static int bars_x(Demo *d)
{
    return (int)lroundf((hbox_x(d, NB_HBOX) + EDGE_GAP) * cb_window_scale(d->w));
}

static double bar_centre(Demo *d, int i)
{
    return bars_x(d) + 4 + i * BAR_GAP + 0.5 + i / 8.0;
}

static void draw_bars(Demo *d)
{
    void *dl = cb_demo_foreground_draw_list();
    float scale = cb_window_scale(d->w);
    float y = ref_y(d->w), x = bars_x(d) / scale;
    cb_demo_rect(dl, x, y, x + (8 + NB_BARS * BAR_GAP) / scale, y + REF_H, 0x000000FF);
    for (int i = 0; i < NB_BARS; i++) {
        double c = bar_centre(d, i);
        cb_vbar_draw(dl, (float)((c - 0.5) / scale), (float)((c + 0.5) / scale), y, y + REF_H,
                     BAR_COL);
    }
}

static int check_bars(Demo *d, const uint8_t *rgba, int width, int height)
{
    int y = (int)floorf((ref_y(d->w) + REF_H / 2) * cb_window_scale(d->w));
    double full = srgb_light(BAR_FULL), lo = 1e9, hi = 0.0, sum = 0.0, off = 0.0;
    bool alone = true;
    for (int i = 0; i < NB_BARS; i++) {
        double c = bar_centre(d, i), light = 0.0, at = 0.0;
        int p0 = (int)floor(c) - 3;
        if (p0 < 0 || p0 + 7 > width || y >= height)
            return 1;
        for (int p = p0; p < p0 + 7; p++) {
            const uint8_t *px = rgba + ((size_t)y * width + p) * 4;
            double l = srgb_light(px[1]);
            light += l;
            at += l * (p + 0.5);
            if (i == 0)
                alone = alone && (p == (int)floor(c) ? abs(px[1] - BAR_FULL) <= 1 : px[1] == 0);
        }
        lo = fmin(lo, light);
        hi = fmax(hi, light);
        sum += light;
        if (light > 0.0)
            off = fmax(off, fabs(at / light - c));
        else
            off = 1.0;
    }
    double mean = sum / NB_BARS, var = mean > 0.0 ? (hi - lo) / mean : 1.0;
    bool ok = alone && var <= BAR_MAX_LIGHT && fabs(mean / full - 1.0) <= BAR_MAX_LIGHT &&
              off <= BAR_MAX_OFF;
    cb_log(ok ? CB_LOG_INFO : CB_LOG_ERROR,
           "demo: lines at eighths of a pixel: light %.4f of a whole pixel's %.4f, varies by "
           "%.2f %%; %.3f px at most from where they were put; the first %s %s", mean, full,
           100.0 * var, off, alone ? "one pixel" : "NOT one pixel", ok ? "ok" : "WRONG");
    return ok ? 0 : 1;
}

static void draw_reference(Demo *d)
{
    void *dl = cb_demo_foreground_draw_list();
    float y = ref_y(d->w);
    float end = ref_col_x(1000) - REF_GAP;

    cb_demo_rect(dl, REF_X - 4, y - 4, end + 4, y + REF_H + 4, 0x000000FF);

    float x = REF_X;
    for (int i = 0; i < NB_REF; i++) {
        const RefStrip *r = &ref_strips[i];
        CBWaveDraw wd = {
            .x0 = x,  .y0 = y,
            .x1 = x + r->nb * REF_COL,  .y1 = y + REF_H,
            .col0 = 0, .col1 = r->nb,
            .mode      = r->mode,
            .detail    = r->detail,
            .alpha     = 1.0f,
            .dim       = r->dim,
            .dim_until = r->nb,
        };
        cb_wave_draw(d->w, dl, d->ref[i], &wd);
        x += r->nb * REF_COL + REF_GAP;
    }
}

typedef struct Probe {
    const char *what;
    int col;            /* counted across all strips */
    float dx;           /* from the column's left edge, logical pixels */
    float dy;           /* from the strip's centre line */
    uint32_t rgb;
} Probe;

static const Probe probes[] = {
    { "3-band low",                0, 12.0f,   0.5f, 0x3C6ED7 },
    { "3-band mid",                1, 12.0f,   0.5f, 0x5CBCF3 },
    { "3-band high",               2, 12.0f,   0.5f, 0xDEF2FC },
    { "3-band low+mid core",       3, 12.0f,   0.5f, 0x4495EA },
    { "3-band low+mid envelope",   3, 12.0f, -35.0f, 0x3C6ED7 },
    { "3-band all",                4, 12.0f,   0.5f, 0xF1FAFF },
    { "silence line",              5, 12.0f,   0.0f, 0xD2DCFA },
    { "silence above the line",    5, 12.0f,  -5.0f, 0x000000 },
    { "3-band low+high core",      6, 12.0f,   0.5f, 0xAA9DC5 },
    { "3-band low+high envelope",  6, 12.0f, -35.0f, 0xDEF2FC },
    { "3-band mid+high",           7, 12.0f,   0.5f, 0xA1DBF5 },
    { "blue, whiteness 0",         8, 12.0f,   0.5f, 0x0055E1 },
    { "blue, whiteness 7",         9, 12.0f,   0.5f, 0xF5EBD7 },
    { "rgb detail red",           10, 12.0f,   0.5f, 0xFF0000 },
    { "rgb detail red+green",     11, 12.0f,   0.5f, 0xFF6D00 },
    { "rgb preview 100/50/0",     12, 12.0f,   0.5f, 0xFF7F00 },
    { "stacked low",              13, 12.0f,  45.0f, 0x3C6ED7 },
    { "stacked high slab",        13, 12.0f, -45.0f, 0xDEF2FC },
    { "stacked low slab",         14, 12.0f,  40.0f, 0x3C6ED7 },
    { "stacked mid slab",         14, 12.0f, -10.0f, 0x5CBCF3 },
    { "overlaid low+mid core",    15, 12.0f,  40.0f, 0x4495EA },
    { "overlaid low envelope",    15, 12.0f, -30.0f, 0x3C6ED7 },
    { "dimmed low",               16, 12.0f,   0.5f, 0x1E376B },
    /* At the probe's pixel centre, 12.5 px into column 0, the level is
     * 50.4, a reach of 19.8 px; without the ramp it would be 40, 15.7 px. */
    { "detail ramp, inside",      17, 12.0f, -18.0f, 0x3C6ED7 },
    { "detail ramp, outside",     17, 12.0f, -21.0f, 0x000000 },
    { "sharp attack stays silent", 19, 12.0f, -10.0f, 0x000000 },
    { "sharp attack's silence",   19, 12.0f,   0.0f, 0xD2DCFA },
    { "attack column",            20, 12.0f, -30.0f, 0x3C6ED7 },
};

/* One pixel of the frame against its colour, within 2 a channel. */
static bool probe(const uint8_t *rgba, int width, int height, const char *what, int x, int y,
                  uint32_t want)
{
    if (x < 0 || y < 0 || x >= width || y >= height) {
        cb_err("demo: probe %s is outside the frame", what);
        return false;
    }

    const uint8_t *px = rgba + ((size_t)y * width + x) * 4;
    bool ok = abs(px[0] - (int)(want >> 16 & 0xFF)) <= 2 &&
              abs(px[1] - (int)(want >> 8 & 0xFF)) <= 2 && abs(px[2] - (int)(want & 0xFF)) <= 2;
    cb_log(ok ? CB_LOG_INFO : CB_LOG_ERROR, "demo: %-28s at %4d,%4d: %02X%02X%02X, want %06X %s",
           what, x, y, px[0], px[1], px[2], want, ok ? "ok" : "WRONG");
    return ok;
}

static int run_checks(Demo *d)
{
    uint8_t *rgba = NULL;
    int width = 0, height = 0;
    int ret = cb_window_readback(d->w, &rgba, &width, &height);
    if (ret < 0) {
        cb_err("demo: no captured frame to check (%d)", ret);
        return ret;
    }

    float scale = cb_window_scale(d->w);
    float centre = ref_y(d->w) + REF_H / 2;
    int failed = 0;
    for (size_t i = 0; i < CB_ARRAY_ELEMS(probes); i++) {
        const Probe *p = &probes[i];
        int x = (int)floorf((ref_col_x(p->col) + p->dx) * scale);
        int y = (int)floorf((centre + p->dy) * scale);
        failed += !probe(rgba, width, height, p->what, x, y, p->rgb);
    }

    for (size_t i = 0; i < CB_ARRAY_ELEMS(edge_probes); i++) {
        int x = (int)floorf((edge_x(edge_probes[i].box) + EDGE_W * 0.5f) * scale);
        int y = (int)floorf((centre + edge_probes[i].dy) * scale);
        failed += !probe(rgba, width, height, edge_probes[i].what, x, y, edge_probes[i].rgb);
    }

    for (size_t i = 0; i < CB_ARRAY_ELEMS(hprobes); i++) {
        int x = (int)lroundf(hbox_x(d, hprobes[i].box) * scale) + hprobes[i].px;
        int y = (int)floorf((centre + hprobes[i].dy) * scale);
        failed += !probe(rgba, width, height, hprobes[i].what, x, y, hprobes[i].rgb);
    }

    failed += check_bars(d, rgba, width, height);

    free(rgba);
    size_t total = CB_ARRAY_ELEMS(probes) + CB_ARRAY_ELEMS(edge_probes) +
                   CB_ARRAY_ELEMS(hprobes) + 1;
    if (failed)
        cb_err("demo: %d of %zu pixel checks failed", failed, total);
    else
        cb_info("demo: all %zu pixel checks pass", total);
    return failed ? CB_ERROR(EINVAL) : 0;
}

/* ----------------------------------------------------------------- scroll */

/* What a smooth scroll leaves: the row's total coverage moves only by
 * rounding (the picture over whole periods is the same picture shifted),
 * consecutive frames differ by about as much light every time, and a
 * one-column spike gives the same light wherever it is. Box-filtered as
 * light, the rows measure 0.006 and 0.06 % (over the grey, an alpha is
 * read to a quarter of the precision white gave) and 0 and 1 %, and the
 * spikes 0.9 %; blended as encoded values, the spikes' light went from
 * 12.6 to 27.5 (69 %) as they crossed the pixels. Taking the maximum over
 * whole columns at the left edge, the rows moved 0.52 and 0.00 % and their
 * frame differences 11 and 141 % (measured over white and in encoded
 * values; the 1.6 row unchanged in two frames of three, then a jump). */
#define SCROLL_MAX_COV    0.001     /* (max - min) / mean of the coverage */
#define SCROLL_MAX_UNEVEN 0.05      /* standard deviation / mean of the frame differences */
#define SCROLL_MAX_LIGHT  0.03      /* (max - min) / mean of the spikes' light */

static const char *const scroll_names[NB_SCROLL] = {
    "123 columns, interpolated",
    "1.6 columns a pixel",
};

static float scroll_y(int row, bool grey)
{
    return SCROLL_Y + row * 2 * (SCROLL_H + SCROLL_GAP) + (grey ? SCROLL_H + SCROLL_GAP : 0.0f);
}

/* The two zoomed detail rows at step k, on the spike pattern: the panel's
 * interpolated zoom (its 120 columns made three whole periods, 123) and the
 * player's usual one (1.6 columns a pixel, 40 periods). */
static void scroll_draw(Demo *d, int k)
{
    void *dl = cb_demo_foreground_draw_list();
    float scale = cb_window_scale(d->w);
    for (int r = 0; r < NB_SCROLL; r++) {
        double span = r == 0 ? 3 * SPIKE_PERIOD : 1.6 * SCROLL_W * scale;
        double col0 = 1000.0 + k * SCROLL_STEP;
        for (int grey = 0; grey < 2; grey++) {
            float y = scroll_y(r, grey);
            cb_demo_rect(dl, SCROLL_X - 2, y - 2, SCROLL_X + SCROLL_W + 2, y + SCROLL_H + 2,
                         grey ? SCROLL_GREY * 0x01010100u | 0xFF : 0x000000FF);
            CBWaveDraw wd = {
                .x0 = SCROLL_X,  .y0 = y,
                .x1 = SCROLL_X + SCROLL_W,  .y1 = y + SCROLL_H,
                .col0 = col0,  .col1 = col0 + span,
                .mode         = CB_WAVE_CENTRED,
                .inset_top    = 8.0f,
                .inset_bottom = 2.0f,
                .detail       = true,
                .alpha        = 1.0f,
            };
            cb_wave_draw(d->w, dl, d->spikes, &wd);
        }
    }
}

/* The light of every one-column spike (column 27 of a period) of the 1.6
 * row in the frame of step k, over black: in the rows from the top of its
 * reach down to 10 px above the centre line, which no column near it
 * reaches, and the 5 pixels around it. The coverage cannot tell, but the
 * eye sees light: blended as encoded values, a spike 0.625 pixels wide gave
 * 0.16 to 0.35 of a white pixel's light as it crossed the pixels, and
 * twinkled as it scrolled. */
static void spike_light(ScrollRow *s, const uint8_t *rgba, int width, int x0, int x1, int yb,
                        int k, float scale)
{
    double col0 = 1000.0 + k * SCROLL_STEP;
    int y0 = yb + (int)lroundf(8.0f * scale), y1 = yb + (int)lroundf(38.0f * scale);
    for (int n = (int)(col0 / SPIKE_PERIOD); ; n++) {
        double xs = x0 + (n * SPIKE_PERIOD + 27 - col0) / 1.6;
        int p0 = (int)floor(xs) - 2, p1 = (int)floor(xs) + 3;
        if (p0 < x0)
            continue;
        if (p1 > x1)
            break;
        double light = 0.0;
        for (int y = y0; y < y1; y++) {
            const uint8_t *px = rgba + ((size_t)y * width + p0) * 4;
            for (int p = p0; p < p1; p++, px += 4)
                light += srgb_light(px[0]) + srgb_light(px[1]) + srgb_light(px[2]);
        }
        s->light_lo = s->nb_lights ? fmin(s->light_lo, light) : light;
        s->light_hi = s->nb_lights ? fmax(s->light_hi, light) : light;
        s->light_sum += light;
        s->nb_lights++;
    }
}

/* Reads the frame of step k back: each row's total coverage, with the
 * alpha recovered from its two copies (a premultiplied draw leaves rgb
 * over black and rgb + (1 - alpha) * grey over the grey), and how much the
 * light of its pixels over black changed since step k - 1. */
static void scroll_measure(Demo *d, int k)
{
    uint8_t *rgba = NULL;
    int width = 0, height = 0;
    if (cb_window_readback(d->w, &rgba, &width, &height) < 0) {
        d->scroll_lost = true;
        return;
    }

    float scale = cb_window_scale(d->w);
    int x0 = (int)lroundf(SCROLL_X * scale), x1 = (int)lroundf((SCROLL_X + SCROLL_W) * scale);
    int h = (int)lroundf(SCROLL_H * scale);
    for (int r = 0; r < NB_SCROLL; r++) {
        ScrollRow *s = &d->rows[r];
        int yb = (int)lroundf(scroll_y(r, false) * scale);
        int yg = (int)lroundf(scroll_y(r, true) * scale);
        size_t n = (size_t)(x1 - x0) * h * 3;
        if (x1 > width || yg + h > height || (!s->prev && !(s->prev = calloc(n, 1)))) {
            d->scroll_lost = true;
            break;
        }

        double cov = 0.0, diff = 0.0;
        uint8_t *prev = s->prev;
        for (int y = 0; y < h; y++) {
            const uint8_t *b = rgba + ((size_t)(yb + y) * width + x0) * 4;
            const uint8_t *g = rgba + ((size_t)(yg + y) * width + x0) * 4;
            for (int x = x0; x < x1; x++, b += 4, g += 4) {
                for (int c = 0; c < 3; c++) {
                    cov += 1.0 - (g[c] - b[c]) / (double)SCROLL_GREY;
                    diff += fabs(srgb_light(b[c]) - srgb_light(*prev));
                    *prev++ = b[c];
                }
            }
        }
        s->cov[k] = cov / 3.0;
        s->mad[k] = k > 0 ? diff / (double)n : 0.0;
        s->nb_frames = k + 1;
        if (r == 1)
            spike_light(s, rgba, width, x0, x1, yb, k, scale);
    }
    free(rgba);
}

static int scroll_check(Demo *d)
{
    int failed = 0;
    for (int r = 0; r < NB_SCROLL; r++) {
        const ScrollRow *s = &d->rows[r];
        if (d->scroll_lost || s->nb_frames < SCROLL_FRAMES) {
            cb_err("demo: scroll, %s: only %d of %d frames read back", scroll_names[r],
                   s->nb_frames, SCROLL_FRAMES);
            failed++;
            continue;
        }

        double lo = s->cov[0], hi = s->cov[0], sum = 0.0;
        for (int k = 0; k < SCROLL_FRAMES; k++) {
            lo = fmin(lo, s->cov[k]);
            hi = fmax(hi, s->cov[k]);
            sum += s->cov[k];
        }
        double mean = sum / SCROLL_FRAMES;
        double cov_var = mean > 0.0 ? (hi - lo) / mean : 1.0;

        double dlo = s->mad[1], dhi = s->mad[1], dsum = 0.0, dsq = 0.0;
        for (int k = 1; k < SCROLL_FRAMES; k++) {
            dlo = fmin(dlo, s->mad[k]);
            dhi = fmax(dhi, s->mad[k]);
            dsum += s->mad[k];
        }
        double dmean = dsum / (SCROLL_FRAMES - 1);
        for (int k = 1; k < SCROLL_FRAMES; k++)
            dsq += (s->mad[k] - dmean) * (s->mad[k] - dmean);
        double dsd = sqrt(dsq / (SCROLL_FRAMES - 1));
        double uneven = dmean > 0.0 ? dsd / dmean : 0.0;

        bool ok = cov_var <= SCROLL_MAX_COV && uneven <= SCROLL_MAX_UNEVEN;
        if (!ok)
            failed++;
        cb_log(ok ? CB_LOG_INFO : CB_LOG_ERROR,
               "demo: scroll, %s: coverage %.1f px, varies by %.3f %%; frame difference "
               "%.5f of light (%.5f to %.5f, sd %.5f, %.0f %% of the mean) %s", scroll_names[r],
               mean, 100.0 * cov_var, dmean, dlo, dhi, dsd, 100.0 * uneven, ok ? "ok" : "WRONG");

        if (r != 1)
            continue;
        double lmean = s->nb_lights ? s->light_sum / s->nb_lights : 0.0;
        double light_var = lmean > 0.0 ? (s->light_hi - s->light_lo) / lmean : 1.0;
        ok = s->nb_lights >= SCROLL_FRAMES && light_var <= SCROLL_MAX_LIGHT;
        if (!ok)
            failed++;
        cb_log(ok ? CB_LOG_INFO : CB_LOG_ERROR,
               "demo: scroll, %s: %d one-column spikes, light %.3f (%.3f to %.3f), varies by "
               "%.2f %% %s", scroll_names[r], s->nb_lights, lmean, s->light_lo, s->light_hi,
               100.0 * light_var, ok ? "ok" : "WRONG");
    }
    return failed ? CB_ERROR(EINVAL) : 0;
}

/* ------------------------------------------------------------------ bench */

/* BENCH_LAYERS full-window detail draws of the spike pattern behind
 * everything at d->bench columns a pixel, scrolling as the player does, so
 * that the frame's GPU time is mostly the waveform shader's. */
static void draw_bench(Demo *d)
{
    int ww, wh;
    cb_window_size(d->w, &ww, &wh);
    void *dl = cb_demo_background_draw_list();
    double span = d->bench * ww * cb_window_scale(d->w);
    for (int i = 0; i < BENCH_LAYERS; i++) {
        double col0 = 1000.0 + cb_window_frames_drawn(d->w) * 2.5 + i * 0.37;
        CBWaveDraw wd = {
            .x0 = 0.0f,  .y0 = 0.0f,
            .x1 = (float)ww,  .y1 = (float)wh,
            .col0 = col0,  .col1 = col0 + span,
            .mode         = CB_WAVE_CENTRED,
            .inset_top    = 8.0f,
            .inset_bottom = 2.0f,
            .detail       = true,
            .alpha        = 1.0f,
        };
        cb_wave_draw(d->w, dl, d->spikes, &wd);
    }
}

/* ------------------------------------------------------------------ frame */

static uint64_t make_texture(CBWindow *w, int phase)
{
    uint8_t pixels[64 * 64 * 4];
    for (int y = 0; y < 64; y++) {
        for (int x = 0; x < 64; x++) {
            uint8_t *p = &pixels[(y * 64 + x) * 4];
            p[0] = (uint8_t)(x * 4);
            p[1] = (uint8_t)(y * 4);
            p[2] = (uint8_t)((((x + phase) / 8 + y / 8) & 1) ? 255 : 64);
            p[3] = 255;
        }
    }
    return cb_texture_create(w, pixels, 64, 64);
}

/* Replaces the texture and the blue waveform after they have been drawn in
 * this frame: the worst case for deferred destruction, which must keep
 * both alive until the frame's submission has completed. */
static void churn(Demo *d)
{
    int n = cb_window_frames_drawn(d->w);
    if (n % 20 != 10)
        return;

    uint64_t tex = make_texture(d->w, n / 20);
    if (tex) {
        cb_texture_destroy(d->w, d->tex);
        d->tex = tex;
    }

    CBWaveGpu *blue = cb_wave_upload(d->w, CB_WAVE_BLUE, d->blue_cols, NB_COLUMNS);
    if (blue) {
        cb_wave_release(d->w, d->blue);
        d->blue = blue;
    }
}

static void wave_row(Demo *d, const char *label, CBWaveGpu *g, enum CBWaveMode mode,
                     double col0, double col1, double head_col, bool detail, float height,
                     bool fixed_head)
{
    cb_demo_text("%s", label);

    float x, y;
    cb_demo_cursor(&x, &y);
    float width = cb_demo_avail_width();
    void *dl = cb_demo_window_draw_list();

    cb_demo_rect(dl, x, y, x + width, y + height, 0x101010FF);
    CBWaveDraw wd = {
        .x0 = x,  .y0 = y,
        .x1 = x + width,  .y1 = y + height,
        .col0 = col0,  .col1 = col1,
        .mode         = mode,
        .inset_top    = 8.0f,
        .inset_bottom = 2.0f,
        .detail       = detail,
        .alpha        = 1.0f,
        .dim          = 0.45f,
        .dim_until    = head_col,
    };
    cb_wave_draw(d->w, dl, g, &wd);

    float head = fixed_head ? x + width / 2
                            : x + (float)((head_col - col0) / (col1 - col0)) * width;
    cb_demo_line(dl, head, y, head, y + height, 0xFF3030FF, 2.0f);
    cb_demo_dummy(width, height);
}

static void frame(Demo *d)
{
    CBWindow *w = d->w;
    int ww, wh;
    cb_window_size(w, &ww, &wh);

    /* The playheads move by frame, not by clock, so a run is repeatable.
     * The spike row's moves as the player's does at 60 frames a second
     * (150 columns a second). */
    d->playhead = fmod(cb_window_frames_drawn(w) * 1.5, NB_COLUMNS);
    d->spike_head = 2000.0 + fmod(cb_window_frames_drawn(w) * 2.5, 100000.0);

    cb_window_begin_frame(w);
    cb_demo_show_demo_window();

    float px = floorf(ww * 0.40f);
    if (cb_demo_begin("Waveforms", px, 10.0f, ww - px - 10.0f, wh - 20.0f)) {
        cb_demo_text("%s, frame %d, %.2f ms a frame", cb_window_gpu_name(w),
                     cb_window_frames_drawn(w), cb_window_frame_interval_ms(w));
        cb_demo_text("日本語のタイトル: CJK fallback");

        wave_row(d, "3-band detail, centred, whole track", d->band, CB_WAVE_CENTRED,
                 0, NB_COLUMNS, d->playhead, false, 90.0f, false);
        wave_row(d, "3-band detail, zoomed (120 columns), interpolated", d->band,
                 CB_WAVE_CENTRED, d->playhead - 60, d->playhead + 60, d->playhead, true, 90.0f,
                 true);
        double span = 1.6 * cb_demo_avail_width() * cb_window_scale(w);
        wave_row(d, "3-band detail, 1.6 columns a pixel, one-column spikes", d->spikes,
                 CB_WAVE_CENTRED, d->spike_head - span / 2, d->spike_head + span / 2,
                 d->spike_head, true, 90.0f, true);
        wave_row(d, "3-band, stacked half (row preview)", d->band, CB_WAVE_HALF_STACKED,
                 0, NB_COLUMNS, d->playhead, false, 45.0f, false);
        wave_row(d, "3-band, overlaid half (2-player detail)", d->band, CB_WAVE_HALF_OVERLAID,
                 0, NB_COLUMNS, d->playhead, false, 45.0f, false);
        wave_row(d, "blue", d->blue, CB_WAVE_CENTRED, 0, NB_COLUMNS, d->playhead, false, 70.0f,
                 false);
        wave_row(d, "rgb", d->rgb, CB_WAVE_CENTRED, 0, NB_COLUMNS, d->playhead, false, 70.0f,
                 false);

        cb_demo_text("texture");
        cb_demo_image(d->tex, 64.0f, 64.0f);
    }
    cb_demo_end();

    if (d->check) {
        draw_reference(d);
        draw_edges(d);
        draw_hboxes(d);
        draw_bars(d);
    }
    if (d->bench > 0.0)
        draw_bench(d);

    /* A frame that is not drawn (the swapchain was out of date) is built
     * again with the same step. */
    int drawn = cb_window_frames_drawn(w);
    int step = drawn - SCROLL_START;
    bool scrolling = d->scroll && step >= 0 && step < SCROLL_FRAMES;
    if (scrolling) {
        scroll_draw(d, step);
        cb_window_capture_next(w);
    }

    churn(d);
    cb_window_end_frame(w);

    if (cb_window_frames_drawn(w) == drawn + 1) {
        if (scrolling)
            scroll_measure(d, step);
        if (drawn >= GPU_WARMUP && cb_window_gpu_ms(w) > 0.0) {
            d->gpu_ms += cb_window_gpu_ms(w);
            d->gpu_frames++;
        }
    }
}

/* ------------------------------------------------------------------- main */

static void usage(void)
{
    fprintf(stderr,
            "usage: cyanbox-gpu-demo [--frames N] [--screenshot out.png] [--validation]\n"
            "                      [--device NAME] [--check] [--scroll] [--bench COLS]\n"
            "                      [--idle] [--stats] [--verbose]\n");
}

int main(int argc, char **argv)
{
    static const struct option longopts[] = {
        { "frames",     required_argument, NULL, 'n' },
        { "screenshot", required_argument, NULL, 's' },
        { "validation", no_argument,       NULL, 'V' },
        { "device",     required_argument, NULL, 'd' },
        { "check",      no_argument,       NULL, 'c' },
        { "scroll",     no_argument,       NULL, 'r' },
        { "bench",      required_argument, NULL, 'b' },
        { "idle",       no_argument,       NULL, 'i' },
        { "stats",      no_argument,       NULL, 'S' },
        { "verbose",    no_argument,       NULL, 'v' },
        { "help",       no_argument,       NULL, 'h' },
        { NULL, 0, NULL, 0 },
    };

    CBWindowOptions opts;
    cb_window_options_default(&opts);
    opts.title = "cyanbox gpu demo";
    opts.max_frames = 120;
    const char *shot = NULL;
    bool idle = false, stats = false;
    Demo d = { 0 };

    int c;
    while ((c = getopt_long(argc, argv, "n:s:Vd:crb:iSvh", longopts, NULL)) != -1) {
        switch (c) {
        case 'n': opts.max_frames = atoi(optarg); break;
        case 's': shot = optarg; break;
        case 'V': opts.validation = true; break;
        case 'd': opts.device = optarg; break;
        case 'c': d.check = true; break;
        case 'r': d.scroll = true; break;
        case 'b': d.bench = atof(optarg); break;
        case 'i': idle = true; break;
        case 'S': stats = true; break;
        case 'v': cb_log_set_level(CB_LOG_DEBUG); break;
        case 'h': usage(); return 0;
        default:  usage(); return 2;
        }
    }

    if (d.scroll)
        opts.max_frames = CB_MAX(opts.max_frames, SCROLL_START + SCROLL_FRAMES + 2);

    int ret = cb_window_create(&d.w, &opts);
    if (ret < 0) {
        cb_err("demo: no window (%d)", ret);
        return 1;
    }

    uint8_t band[3 * NB_COLUMNS], rgb[2 * NB_COLUMNS];
    synth(band, d.blue_cols, rgb);
    d.band      = cb_wave_upload(d.w, CB_WAVE_3BAND, band, NB_COLUMNS);
    d.blue      = cb_wave_upload(d.w, CB_WAVE_BLUE, d.blue_cols, NB_COLUMNS);
    d.rgb       = cb_wave_upload(d.w, CB_WAVE_RGB_DETAIL, rgb, NB_COLUMNS);

    uint8_t *spikes = malloc(3 * SPIKE_COLUMNS);
    if (spikes) {
        synth_spikes(spikes, SPIKE_COLUMNS);
        d.spikes = cb_wave_upload(d.w, CB_WAVE_3BAND, spikes, SPIKE_COLUMNS);
        free(spikes);
    }

    d.tex = make_texture(d.w, 0);
    d.empty = cb_wave_upload(d.w, CB_WAVE_3BAND, NULL, 0);
    d.quiet = cb_wave_upload(d.w, CB_WAVE_3BAND, ref_quiet, 1);
    d.spike = cb_wave_upload(d.w, CB_WAVE_3BAND, ref_spike, sizeof(ref_spike) / 3);
    d.attack = cb_wave_upload(d.w, CB_WAVE_3BAND, ref_attack, sizeof(ref_attack) / 3);

    if (!d.band || !d.blue || !d.rgb || !d.spikes || !d.tex || ref_upload(&d) < 0 || !d.empty ||
        cb_wave_columns(d.empty) != 0 || !d.quiet || !d.spike || !d.attack) {
        cb_err("demo: could not create the resources");
        ret = CB_ERROR(ENOMEM);
        goto end;
    }

    /* With --idle nothing asks for a frame after the first few: the loop
     * should sleep in cb_window_poll(), only waking for input. */
    int64_t last = cb_time_us();
    long iterations = 0;
    int last_frames = 0;
    while (!cb_window_should_close(d.w)) {
        bool want = !idle || cb_window_frames_drawn(d.w) < 3;
        if (cb_window_poll(d.w, want, 250))
            frame(&d);

        iterations++;
        int64_t now = cb_time_us();
        if (stats && now - last >= 1000000) {
            int frames = cb_window_frames_drawn(d.w);
            cb_info("demo: %d frames drawn (+%d), %ld loop iterations, callback %s", frames,
                    frames - last_frames, iterations,
                    cb_window_frame_pending(d.w) ? "pending" : "in");
            last = now;
            last_frames = frames;
            iterations = 0;
        }
    }

    if (shot) {
        ret = cb_window_screenshot(d.w, shot);
        if (ret < 0)
            cb_err("demo: screenshot failed (%d)", ret);
    }
    if (ret >= 0 && (d.check || d.scroll)) {
        /* Both, whatever the first says: the scroll numbers are worth
         * seeing either way. */
        int err = d.check ? run_checks(&d) : 0;
        if (d.scroll && scroll_check(&d) < 0)
            err = CB_ERROR(EINVAL);
        ret = err;
    }

end:;
    int errors = cb_window_validation_errors(d.w);
    if (d.gpu_frames)
        cb_info("demo: GPU %.3f ms a frame (mean of %d frames, after the first %d)",
                d.gpu_ms / d.gpu_frames, d.gpu_frames, GPU_WARMUP);
    cb_info("demo: %d frames drawn, %.2f ms frame interval, %d validation errors",
            cb_window_frames_drawn(d.w), cb_window_frame_interval_ms(d.w), errors);

    cb_texture_destroy(d.w, d.tex);
    cb_wave_release(d.w, d.band);
    cb_wave_release(d.w, d.blue);
    cb_wave_release(d.w, d.rgb);
    cb_wave_release(d.w, d.spikes);
    for (int i = 0; i < NB_REF; i++)
        cb_wave_release(d.w, d.ref[i]);
    cb_wave_release(d.w, d.empty);
    cb_wave_release(d.w, d.quiet);
    cb_wave_release(d.w, d.spike);
    cb_wave_release(d.w, d.attack);
    cb_window_destroy(d.w);
    for (int i = 0; i < NB_SCROLL; i++)
        free(d.rows[i].prev);

    return ret < 0 || errors ? 1 : 0;
}
