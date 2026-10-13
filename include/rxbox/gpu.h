/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

#ifndef RXBOX_GPU_H
#define RXBOX_GPU_H

/* The window and everything drawn in it: an SDL3 window on Wayland, a
 * Vulkan device with VMA, a swapchain paced by the compositor's frame
 * callback, Dear ImGui through its SDL3 and Vulkan backends, textures for
 * the interface, and the waveform renderer (a fragment shader reading
 * ANLZ column bytes from a storage buffer).
 *
 * Frame pacing. A Wayland client must not draw faster than the compositor
 * asks: wl_surface.frame says when the next frame is wanted, and a surface
 * nobody can see gets none. rx_window_poll() therefore returns true only
 * when the last frame's callback has arrived (or no frame was ever drawn)
 * and the caller wants a redraw. Between frames it waits on events with a
 * timeout, so a paused interface costs nothing. The callback is registered
 * on a private wl_event_queue through a proxy wrapper of SDL's wl_surface
 * (see libcraw tools/gui/colour.c for the pattern), so SDL's own event
 * dispatch is untouched. MAILBOX presentation when the device offers it,
 * FIFO otherwise; the swapchain is recreated on resize and on
 * VK_ERROR_OUT_OF_DATE_KHR.
 *
 * Every function is main-thread only. The C++ side (src/ui) builds the
 * ImGui frame between begin and end. */

#include "rxbox/common.h"

typedef struct RXWindow RXWindow;

typedef struct RXWindowOptions {
    const char *title;
    int   width, height;        /* logical; 1600x900 default */
    bool  validation;           /* enable VK_LAYER_KHRONOS_validation */
    const char *device;         /* substring of the GPU name to prefer, NULL = first suitable;
                                   the analyses' compute context picks it too (rxbox/jobs.h) */
    bool  cpu_analysis;         /* the analyses' spectral front end on the CPU only (--no-gpu) */
    int   max_frames;           /* exit after this many drawn frames (tests), 0 = never */
    const char *font;           /* a font file, NULL = system sans-serif via fontconfig
                                   plus a CJK fallback (Noto Sans CJK) when found */
    float font_size;            /* 0 = 15 */
} RXWindowOptions;
void rx_window_options_default(RXWindowOptions *o);

int  rx_window_create(RXWindow **w, const RXWindowOptions *o);
void rx_window_destroy(RXWindow *w);

/* Processes events (feeding ImGui), then returns true when a frame should
 * be built now: the compositor wants one and (`want_redraw` or input
 * arrived or the window needs its first/resized frame). Otherwise it has
 * waited up to `timeout_ms` for events and returns false. */
bool rx_window_poll(RXWindow *w, bool want_redraw, int timeout_ms);
bool rx_window_should_close(const RXWindow *w);
void rx_window_request_close(RXWindow *w);
/* Begins an ImGui frame (NewFrame). */
void rx_window_begin_frame(RXWindow *w);
/* Renders the ImGui draw data, submits, presents, and asks the compositor
 * for the next frame callback. */
void rx_window_end_frame(RXWindow *w);
/* Logical size and the pixel scale (HiDPI). */
void  rx_window_size(const RXWindow *w, int *width, int *height);
float rx_window_scale(const RXWindow *w);
int   rx_window_frames_drawn(const RXWindow *w);
/* Milliseconds the compositor's last frame interval suggests (16.7 at 60 Hz). */
double rx_window_frame_interval_ms(const RXWindow *w);
/* Saves the last presented frame as PNG (reads the swapchain image back;
 * for the headless tests). Not from inside a frame. */
int   rx_window_screenshot(RXWindow *w, const char *png_path);
/* Saves the next presented frame as PNG: rx_window_end_frame captures it,
 * presents it, then reads it back and writes the file. Callable from inside
 * a frame (that frame is the one saved); a second call before then
 * replaces the path. */
void  rx_window_screenshot_next(RXWindow *w, const char *png_path);
/* The GPU's name (VkPhysicalDeviceProperties.deviceName). */
const char *rx_window_gpu_name(const RXWindow *w);

/* ----------------------------------------------------------------- textures */

/* RGBA8 texture usable as ImTextureID (ImGui::Image). 0 on failure.
 * Destruction is deferred until frames in flight are done. */
uint64_t rx_texture_create(RXWindow *w, const uint8_t *rgba, int width, int height);
void     rx_texture_destroy(RXWindow *w, uint64_t tex);

/* ---------------------------------------------------------------- waveforms */

enum RXWaveKind {
    RX_WAVE_3BAND,      /* 3 bytes per column: low, mid, high 0..127 (PWV6, PWV7) */
    RX_WAVE_BLUE,       /* 1 byte: height 5 bits | whiteness 3 bits << 5 (PWAV, PWV3) */
    RX_WAVE_RGB_DETAIL, /* 2 bytes big-endian rrrgggbbbhhhhh00 (PWV5) */
    RX_WAVE_RGB_PREVIEW,/* 6 bytes: height, ?, ?, r, g, b (PWV4) */
};

enum RXWaveMode {
    RX_WAVE_CENTRED = 0,    /* each band from the centre line out, outer envelope first, core last */
    RX_WAVE_HALF_STACKED,   /* from the bottom: low, then mid on it, then high (row preview) */
    RX_WAVE_HALF_OVERLAID,  /* from the bottom, each band over the last (2-player detail) */
};

/* Column bytes uploaded to a storage buffer. */
typedef struct RXWaveGpu RXWaveGpu;
RXWaveGpu *rx_wave_upload(RXWindow *w, enum RXWaveKind kind, const uint8_t *columns, int nb_columns);
void       rx_wave_release(RXWindow *w, RXWaveGpu *g);
int        rx_wave_columns(const RXWaveGpu *g);

typedef struct RXWaveDraw {
    float  x0, y0, x1, y1;      /* destination rectangle in ImGui screen pixels */
    double col0, col1;          /* columns mapped onto x0..x1 (fractional; may run past the data: drawn empty) */
    enum RXWaveMode mode;
    float  inset_top, inset_bottom;  /* pixels left clear (rekordbox: 8 above, 2 below) */
    bool   detail;              /* a view that scrolls: box-filtered across, and the detail
                                   interpolation rule when columns are wider than a pixel */
    float  alpha;               /* 1 */
    float  dim;                 /* 0..1: darkens the part before col_dim_until (played part), 0 = off */
    double dim_until;           /* column */
    float  gain;                /* every band's level is multiplied by it before the full
                                   scale clamps it (an overview of PWV6 at rekordbox's
                                   levels needs about 4 to fill its height); 0 = 1 */
} RXWaveDraw;

/* Adds the draw to an ImDrawList (passed as void* from C++) as a render
 * callback. Colours are rekordbox's as measured by rbxport:
 *   3-band: low #0055E1, mid #FFA600, high #FFFFFF; low+mid #B4690A,
 *           low+high #D2DCFA, mid+high #FFF0D7, all #F5EBD7; stacked scales
 *           128/256/128; silence a 1 px line in #D2DCFA.
 *   blue:   height 0..31 and whiteness 0..7 ramped from #0055E1 to #F5EBD7.
 *   rgb:    the strongest channel at full, the others in proportion.
 * The 3-band centred draw uses the reach rule: each pixel in the colour of
 * exactly the bands that reach it.
 * A detail draw is box-filtered horizontally, so that it scrolls by
 * fractions of a pixel without shimmering: up to 16 columns a pixel, each
 * column the pixel covers is drawn on its own by the rules above and the
 * results are blended by the share of the pixel each covers; columns wider
 * than a pixel are interpolated at the pixel's centre, and the pixel a
 * sharp attack's vertical edge runs through is split by area between the
 * two sides. Other draws (the overview, the row previews), and a detail
 * draw past 16 columns a pixel, take each band's maximum over the columns
 * a pixel covers, so that a transient survives. */
void rx_wave_draw(RXWindow *w, void *im_draw_list, const RXWaveGpu *g, const RXWaveDraw *d);

#endif /* RXBOX_GPU_H */
