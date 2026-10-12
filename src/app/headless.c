/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* Stand-ins for the window (rxbox/gpu.h) and the interface (the rx_ui_*
 * functions of rxbox/app.h), linked only into rxbox-cli: the command line
 * built without src/gpu and src/ui, which the CLI test runs so that it
 * neither needs a display nor waits for the interface to build. Creating a
 * window fails, so the interface is never entered; everything else is
 * unreachable and harmless. */

#include "rxbox/app.h"
#include "rxbox/gpu.h"

void rx_window_options_default(RXWindowOptions *o)
{
    *o = (RXWindowOptions) {
        .title  = "rxbox",
        .width  = 1600,
        .height = 900,
    };
}

int rx_window_create(RXWindow **w, const RXWindowOptions *o)
{
    rx_err("this build of rxbox has no interface (rxbox-cli); use a subcommand");
    *w = NULL;
    return RX_ERROR(ENOSYS);
}

void rx_window_destroy(RXWindow *w)
{
}

bool rx_window_poll(RXWindow *w, bool want_redraw, int timeout_ms)
{
    return false;
}

bool rx_window_should_close(const RXWindow *w)
{
    return true;
}

void rx_window_request_close(RXWindow *w)
{
}

void rx_window_begin_frame(RXWindow *w)
{
}

void rx_window_end_frame(RXWindow *w)
{
}

void rx_window_size(const RXWindow *w, int *width, int *height)
{
    *width = 0;
    *height = 0;
}

float rx_window_scale(const RXWindow *w)
{
    return 1.0f;
}

int rx_window_frames_drawn(const RXWindow *w)
{
    return 0;
}

double rx_window_frame_interval_ms(const RXWindow *w)
{
    return 0.0;
}

int rx_window_screenshot(RXWindow *w, const char *png_path)
{
    return RX_ERROR(ENOSYS);
}

uint64_t rx_texture_create(RXWindow *w, const uint8_t *rgba, int width, int height)
{
    return 0;
}

void rx_texture_destroy(RXWindow *w, uint64_t tex)
{
}

RXWaveGpu *rx_wave_upload(RXWindow *w, enum RXWaveKind kind, const uint8_t *columns, int nb_columns)
{
    return NULL;
}

void rx_wave_release(RXWindow *w, RXWaveGpu *g)
{
}

int rx_wave_columns(const RXWaveGpu *g)
{
    return 0;
}

void rx_wave_draw(RXWindow *w, void *im_draw_list, const RXWaveGpu *g, const RXWaveDraw *d)
{
}

int rx_ui_init(RXApp *app)
{
    return RX_ERROR(ENOSYS);
}

void rx_ui_shutdown(RXApp *app)
{
}

bool rx_ui_frame(RXApp *app)
{
    return false;
}
