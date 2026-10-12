/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* rxbox-gpu-demo: the gpu module's acceptance test and demo.
 *
 * Opens the window and draws, every frame, ImGui's demo window and a panel
 * of synthetic waveforms: 600 columns of a made-up kick/snare pattern as a
 * 3-band detail (whole, and zoomed around a scrolling playhead so that the
 * columns are wider than the pixels), the same stacked and overlaid as half
 * waveforms, a blue one and an RGB one, plus a texture. After N frames it
 * writes a screenshot of the last frame.
 *
 *   --check       also draw a reference strip of known columns and check
 *                 pixels of the last frame against the colour rules
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

#include "rxbox/common.h"
#include "rxbox/gpu.h"

#include "src/gpu/gpu_demo.h"

#define NB_COLUMNS 600
#define NB_REF     8

typedef struct Demo {
    RxWindow  *w;
    uint8_t    blue_cols[NB_COLUMNS];
    RxWaveGpu *band, *blue, *rgb;
    RxWaveGpu *ref[NB_REF];
    uint64_t   tex;
    bool       check;
    double     playhead;
} Demo;

/* ---------------------------------------------------------------- synthesis */

static uint8_t level(double v)
{
    return (uint8_t)lround(RX_CLAMP(v, 0.0, 127.0));
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
        blue[i] = (uint8_t)(RX_CLAMP(white, 0, 7) << 5 | RX_CLAMP(h, 0, 31));

        /* PWV5-style big-endian rrrgggbbbhhhhh00. */
        unsigned r = (unsigned)lround(7.0 * low / 127.0);
        unsigned g = (unsigned)lround(7.0 * mid / 127.0);
        unsigned b = (unsigned)lround(7.0 * high / 127.0);
        unsigned word = r << 13 | g << 10 | b << 7 | (unsigned)RX_CLAMP(h, 0, 31) << 2;
        rgb[2 * i + 0] = (uint8_t)(word >> 8);
        rgb[2 * i + 1] = (uint8_t)word;
    }
}

/* ------------------------------------------------------------ reference */

/* Strips of columns whose colours are known exactly, drawn side by side
 * at a fixed place over everything else, each column REF_COL wide. */
static const uint8_t ref_band[] = {
    127, 0, 0,      /* low: blue */
    0, 127, 0,      /* mid: amber */
    0, 0, 127,      /* high: white */
    127, 64, 0,     /* low+mid at the core, low outside */
    127, 127, 127,  /* all */
    0, 0, 0,        /* silence: a line */
    64, 0, 127,     /* low+high at the core, high outside */
    0, 127, 127,    /* mid+high */
};
static const uint8_t ref_blue[]   = { 0 << 5 | 31, 7 << 5 | 31 };
static const uint8_t ref_rgb[]    = { 0xE0, 31 << 2, 0xEC, 31 << 2 };  /* r=7; r=7 g=3 */
static const uint8_t ref_pwv4[]   = { 127, 0, 0, 100, 50, 0 };
static const uint8_t ref_stack[]  = { 127, 0, 0, 64, 127, 0 };          /* low; low under mid */
static const uint8_t ref_low[]    = { 127, 0, 0 };
static const uint8_t ref_lowmid[] = { 127, 64, 0 };
/* Detail interpolation: 40 -> 60 ramps, 0 -> 100 is a sharp attack. */
static const uint8_t ref_interp[] = { 40, 0, 0, 60, 0, 0, 0, 0, 0, 100, 0, 0 };

typedef struct RefStrip {
    enum RxWaveKind kind;
    const uint8_t *cols;
    int nb;
    enum RxWaveMode mode;
    float dim;
    bool detail;
} RefStrip;

#define REF(k, a, m, dim, detail) \
    { k, a, (int)(sizeof(a) / (k == RX_WAVE_3BAND ? 3 : k == RX_WAVE_BLUE ? 1 : \
                               k == RX_WAVE_RGB_DETAIL ? 2 : 6)), m, dim, detail }

static const RefStrip ref_strips[NB_REF] = {
    REF(RX_WAVE_3BAND,       ref_band,   RX_WAVE_CENTRED,       0.0f, false),  /* cols 0-7 */
    REF(RX_WAVE_BLUE,        ref_blue,   RX_WAVE_CENTRED,       0.0f, false),  /* 8-9 */
    REF(RX_WAVE_RGB_DETAIL,  ref_rgb,    RX_WAVE_CENTRED,       0.0f, false),  /* 10-11 */
    REF(RX_WAVE_RGB_PREVIEW, ref_pwv4,   RX_WAVE_CENTRED,       0.0f, false),  /* 12 */
    REF(RX_WAVE_3BAND,       ref_stack,  RX_WAVE_HALF_STACKED,  0.0f, false),  /* 13-14 */
    REF(RX_WAVE_3BAND,       ref_lowmid, RX_WAVE_HALF_OVERLAID, 0.0f, false),  /* 15 */
    REF(RX_WAVE_3BAND,       ref_low,    RX_WAVE_CENTRED,       0.5f, false),  /* 16 */
    REF(RX_WAVE_3BAND,       ref_interp, RX_WAVE_CENTRED,       0.0f, true),   /* 17-20 */
};

#define REF_X      20.0f
#define REF_COL    24.0f
#define REF_H      100.0f
#define REF_GAP    12.0f

static float ref_y(RxWindow *w)
{
    int ww, wh;
    rx_window_size(w, &ww, &wh);
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
        d->ref[i] = rx_wave_upload(d->w, ref_strips[i].kind, ref_strips[i].cols,
                                   ref_strips[i].nb);
        if (!d->ref[i])
            return RX_ERROR(ENOMEM);
    }
    return 0;
}

static void draw_reference(Demo *d)
{
    void *dl = rx_demo_foreground_draw_list();
    float y = ref_y(d->w);
    float end = ref_col_x(1000) - REF_GAP;

    rx_demo_rect(dl, REF_X - 4, y - 4, end + 4, y + REF_H + 4, 0x000000FF);

    float x = REF_X;
    for (int i = 0; i < NB_REF; i++) {
        const RefStrip *r = &ref_strips[i];
        RxWaveDraw wd = {
            .x0 = x,  .y0 = y,
            .x1 = x + r->nb * REF_COL,  .y1 = y + REF_H,
            .col0 = 0, .col1 = r->nb,
            .mode      = r->mode,
            .detail    = r->detail,
            .alpha     = 1.0f,
            .dim       = r->dim,
            .dim_until = r->nb,
        };
        rx_wave_draw(d->w, dl, d->ref[i], &wd);
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
    { "3-band low",                0, 12.0f,   0.5f, 0x0055E1 },
    { "3-band mid",                1, 12.0f,   0.5f, 0xFFA600 },
    { "3-band high",               2, 12.0f,   0.5f, 0xFFFFFF },
    { "3-band low+mid core",       3, 12.0f,   0.5f, 0xB4690A },
    { "3-band low+mid envelope",   3, 12.0f, -35.0f, 0x0055E1 },
    { "3-band all",                4, 12.0f,   0.5f, 0xF5EBD7 },
    { "silence line",              5, 12.0f,   0.0f, 0xD2DCFA },
    { "silence above the line",    5, 12.0f,  -5.0f, 0x000000 },
    { "3-band low+high core",      6, 12.0f,   0.5f, 0xD2DCFA },
    { "3-band low+high envelope",  6, 12.0f, -35.0f, 0xFFFFFF },
    { "3-band mid+high",           7, 12.0f,   0.5f, 0xFFF0D7 },
    { "blue, whiteness 0",         8, 12.0f,   0.5f, 0x0055E1 },
    { "blue, whiteness 7",         9, 12.0f,   0.5f, 0xF5EBD7 },
    { "rgb detail red",           10, 12.0f,   0.5f, 0xFF0000 },
    { "rgb detail red+green",     11, 12.0f,   0.5f, 0xFF6D00 },
    { "rgb preview 100/50/0",     12, 12.0f,   0.5f, 0xFF7F00 },
    { "stacked low",              13, 12.0f,  45.0f, 0x0055E1 },
    { "stacked low slab",         14, 12.0f,  40.0f, 0x0055E1 },
    { "stacked mid slab",         14, 12.0f, -10.0f, 0xFFA600 },
    { "overlaid low+mid core",    15, 12.0f,  40.0f, 0xB4690A },
    { "overlaid low envelope",    15, 12.0f, -30.0f, 0x0055E1 },
    { "dimmed low",               16, 12.0f,   0.5f, 0x002B71 },
    /* Halfway through column 0 the level is 50, a reach of 19.7 px;
     * without the ramp it would be 40, 15.7 px. */
    { "detail ramp, inside",      17, 12.0f, -18.0f, 0x0055E1 },
    { "detail ramp, outside",     17, 12.0f, -21.0f, 0x000000 },
    { "sharp attack stays silent", 19, 12.0f, -10.0f, 0x000000 },
    { "sharp attack's silence",   19, 12.0f,   0.0f, 0xD2DCFA },
    { "attack column",            20, 12.0f, -30.0f, 0x0055E1 },
};

static int run_checks(Demo *d)
{
    uint8_t *rgba = NULL;
    int width = 0, height = 0;
    int ret = rx_window_readback(d->w, &rgba, &width, &height);
    if (ret < 0) {
        rx_err("demo: no captured frame to check (%d)", ret);
        return ret;
    }

    float scale = rx_window_scale(d->w);
    float centre = ref_y(d->w) + REF_H / 2;
    int failed = 0;
    for (size_t i = 0; i < RX_ARRAY_ELEMS(probes); i++) {
        const Probe *p = &probes[i];
        float lx = ref_col_x(p->col) + p->dx;
        float ly = centre + p->dy;
        int x = (int)floorf(lx * scale), y = (int)floorf(ly * scale);
        if (x < 0 || y < 0 || x >= width || y >= height) {
            rx_err("demo: probe %s is outside the frame", p->what);
            failed++;
            continue;
        }

        const uint8_t *px = rgba + ((size_t)y * width + x) * 4;
        int er = p->rgb >> 16 & 0xFF, eg = p->rgb >> 8 & 0xFF, eb = p->rgb & 0xFF;
        bool ok = abs(px[0] - er) <= 2 && abs(px[1] - eg) <= 2 && abs(px[2] - eb) <= 2;
        if (!ok)
            failed++;
        rx_log(ok ? RX_LOG_INFO : RX_LOG_ERROR,
               "demo: %-26s at %4d,%4d: %02X%02X%02X, want %06X %s", p->what, x, y,
               px[0], px[1], px[2], p->rgb, ok ? "ok" : "WRONG");
    }

    free(rgba);
    if (failed)
        rx_err("demo: %d of %zu pixel checks failed", failed, RX_ARRAY_ELEMS(probes));
    else
        rx_info("demo: all %zu pixel checks pass", RX_ARRAY_ELEMS(probes));
    return failed ? RX_ERROR(EINVAL) : 0;
}

/* ------------------------------------------------------------------ frame */

static uint64_t make_texture(RxWindow *w, int phase)
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
    return rx_texture_create(w, pixels, 64, 64);
}

/* Replaces the texture and the blue waveform after they have been drawn in
 * this frame: the worst case for deferred destruction, which must keep
 * both alive until the frame's submission has completed. */
static void churn(Demo *d)
{
    int n = rx_window_frames_drawn(d->w);
    if (n % 20 != 10)
        return;

    uint64_t tex = make_texture(d->w, n / 20);
    if (tex) {
        rx_texture_destroy(d->w, d->tex);
        d->tex = tex;
    }

    RxWaveGpu *blue = rx_wave_upload(d->w, RX_WAVE_BLUE, d->blue_cols, NB_COLUMNS);
    if (blue) {
        rx_wave_release(d->w, d->blue);
        d->blue = blue;
    }
}

static void wave_row(Demo *d, const char *label, RxWaveGpu *g, enum RxWaveMode mode,
                     double col0, double col1, bool detail, float height, bool fixed_head)
{
    rx_demo_text("%s", label);

    float x, y;
    rx_demo_cursor(&x, &y);
    float width = rx_demo_avail_width();
    void *dl = rx_demo_window_draw_list();

    rx_demo_rect(dl, x, y, x + width, y + height, 0x101010FF);
    RxWaveDraw wd = {
        .x0 = x,  .y0 = y,
        .x1 = x + width,  .y1 = y + height,
        .col0 = col0,  .col1 = col1,
        .mode         = mode,
        .inset_top    = 8.0f,
        .inset_bottom = 2.0f,
        .detail       = detail,
        .alpha        = 1.0f,
        .dim          = 0.45f,
        .dim_until    = d->playhead,
    };
    rx_wave_draw(d->w, dl, g, &wd);

    float head = fixed_head ? x + width / 2
                            : x + (float)((d->playhead - col0) / (col1 - col0)) * width;
    rx_demo_line(dl, head, y, head, y + height, 0xFF3030FF, 2.0f);
    rx_demo_dummy(width, height);
}

static void frame(Demo *d)
{
    RxWindow *w = d->w;
    int ww, wh;
    rx_window_size(w, &ww, &wh);

    /* The playhead moves by frame, not by clock, so a run is repeatable. */
    d->playhead = fmod(rx_window_frames_drawn(w) * 1.5, NB_COLUMNS);

    rx_window_begin_frame(w);
    rx_demo_show_demo_window();

    float px = floorf(ww * 0.40f);
    if (rx_demo_begin("Waveforms", px, 10.0f, ww - px - 10.0f, wh - 20.0f)) {
        rx_demo_text("%s, frame %d, %.2f ms a frame", rx_window_device_name(w),
                     rx_window_frames_drawn(w), rx_window_frame_interval_ms(w));
        rx_demo_text("日本語のタイトル: CJK fallback");

        wave_row(d, "3-band detail, centred, whole track", d->band, RX_WAVE_CENTRED,
                 0, NB_COLUMNS, false, 90.0f, false);
        wave_row(d, "3-band detail, zoomed (120 columns), interpolated", d->band,
                 RX_WAVE_CENTRED, d->playhead - 60, d->playhead + 60, true, 90.0f, true);
        wave_row(d, "3-band, stacked half (row preview)", d->band, RX_WAVE_HALF_STACKED,
                 0, NB_COLUMNS, false, 45.0f, false);
        wave_row(d, "3-band, overlaid half (2-player detail)", d->band, RX_WAVE_HALF_OVERLAID,
                 0, NB_COLUMNS, false, 45.0f, false);
        wave_row(d, "blue", d->blue, RX_WAVE_CENTRED, 0, NB_COLUMNS, false, 70.0f, false);
        wave_row(d, "rgb", d->rgb, RX_WAVE_CENTRED, 0, NB_COLUMNS, false, 70.0f, false);

        rx_demo_text("texture");
        rx_demo_image(d->tex, 64.0f, 64.0f);
    }
    rx_demo_end();

    if (d->check)
        draw_reference(d);

    churn(d);
    rx_window_end_frame(w);
}

/* ------------------------------------------------------------------- main */

static void usage(void)
{
    fprintf(stderr,
            "usage: rxbox-gpu-demo [--frames N] [--screenshot out.png] [--validation]\n"
            "                      [--device NAME] [--check] [--idle] [--stats] [--verbose]\n");
}

int main(int argc, char **argv)
{
    static const struct option longopts[] = {
        { "frames",     required_argument, NULL, 'n' },
        { "screenshot", required_argument, NULL, 's' },
        { "validation", no_argument,       NULL, 'V' },
        { "device",     required_argument, NULL, 'd' },
        { "check",      no_argument,       NULL, 'c' },
        { "idle",       no_argument,       NULL, 'i' },
        { "stats",      no_argument,       NULL, 'S' },
        { "verbose",    no_argument,       NULL, 'v' },
        { "help",       no_argument,       NULL, 'h' },
        { NULL, 0, NULL, 0 },
    };

    RxWindowOptions opts;
    rx_window_options_default(&opts);
    opts.title = "rxbox gpu demo";
    opts.max_frames = 120;
    const char *shot = NULL;
    bool idle = false, stats = false;
    Demo d = { 0 };

    int c;
    while ((c = getopt_long(argc, argv, "n:s:Vd:ciSvh", longopts, NULL)) != -1) {
        switch (c) {
        case 'n': opts.max_frames = atoi(optarg); break;
        case 's': shot = optarg; break;
        case 'V': opts.validation = true; break;
        case 'd': opts.device = optarg; break;
        case 'c': d.check = true; break;
        case 'i': idle = true; break;
        case 'S': stats = true; break;
        case 'v': rx_log_set_level(RX_LOG_DEBUG); break;
        case 'h': usage(); return 0;
        default:  usage(); return 2;
        }
    }

    int ret = rx_window_create(&d.w, &opts);
    if (ret < 0) {
        rx_err("demo: no window (%d)", ret);
        return 1;
    }

    uint8_t band[3 * NB_COLUMNS], rgb[2 * NB_COLUMNS];
    synth(band, d.blue_cols, rgb);
    d.band      = rx_wave_upload(d.w, RX_WAVE_3BAND, band, NB_COLUMNS);
    d.blue      = rx_wave_upload(d.w, RX_WAVE_BLUE, d.blue_cols, NB_COLUMNS);
    d.rgb       = rx_wave_upload(d.w, RX_WAVE_RGB_DETAIL, rgb, NB_COLUMNS);

    d.tex = make_texture(d.w, 0);

    if (!d.band || !d.blue || !d.rgb || !d.tex || ref_upload(&d) < 0) {
        rx_err("demo: could not create the resources");
        ret = RX_ERROR(ENOMEM);
        goto end;
    }

    /* With --idle nothing asks for a frame after the first few: the loop
     * should sleep in rx_window_poll(), only waking for input. */
    int64_t last = rx_time_us();
    long iterations = 0;
    int last_frames = 0;
    while (!rx_window_should_close(d.w)) {
        bool want = !idle || rx_window_frames_drawn(d.w) < 3;
        if (rx_window_poll(d.w, want, 250))
            frame(&d);

        iterations++;
        int64_t now = rx_time_us();
        if (stats && now - last >= 1000000) {
            int frames = rx_window_frames_drawn(d.w);
            rx_info("demo: %d frames drawn (+%d), %ld loop iterations, callback %s", frames,
                    frames - last_frames, iterations,
                    rx_window_frame_pending(d.w) ? "pending" : "in");
            last = now;
            last_frames = frames;
            iterations = 0;
        }
    }

    if (shot) {
        ret = rx_window_screenshot(d.w, shot);
        if (ret < 0)
            rx_err("demo: screenshot failed (%d)", ret);
    }
    if (ret >= 0 && d.check)
        ret = run_checks(&d);

end:;
    int errors = rx_window_validation_errors(d.w);
    rx_info("demo: %d frames drawn, %.2f ms frame interval, %d validation errors",
            rx_window_frames_drawn(d.w), rx_window_frame_interval_ms(d.w), errors);

    rx_texture_destroy(d.w, d.tex);
    rx_wave_release(d.w, d.band);
    rx_wave_release(d.w, d.blue);
    rx_wave_release(d.w, d.rgb);
    for (int i = 0; i < NB_REF; i++)
        rx_wave_release(d.w, d.ref[i]);
    rx_window_destroy(d.w);

    return ret < 0 || errors ? 1 : 0;
}
