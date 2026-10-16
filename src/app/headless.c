/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* Stand-ins for the window (cyanbox/gpu.h) and the interface (the cb_ui_*
 * functions of cyanbox/app.h), linked only into cyanbox-cli: the command line
 * built without src/gpu and src/ui, which the CLI test runs so that it
 * neither needs a display nor waits for the interface to build. Creating a
 * window fails, so the interface is never entered; everything else is
 * unreachable and harmless. */

#include "cyanbox/app.h"
#include "cyanbox/gpu.h"

void cb_window_options_default(CBWindowOptions *o)
{
    *o = (CBWindowOptions) {
        .title  = "cyanbox",
        .width  = 1600,
        .height = 900,
    };
}

int cb_window_create(CBWindow **w, const CBWindowOptions *o)
{
    cb_err("this build of cyanbox has no interface (cyanbox-cli); use a subcommand");
    *w = NULL;
    return CB_ERROR(ENOSYS);
}

void cb_window_destroy(CBWindow *w)
{
}

bool cb_window_poll(CBWindow *w, bool want_redraw, int timeout_ms)
{
    return false;
}

bool cb_window_hidden(const CBWindow *w)
{
    return false;
}

bool cb_window_should_close(const CBWindow *w)
{
    return true;
}

void cb_window_request_close(CBWindow *w)
{
}

void cb_window_begin_frame(CBWindow *w)
{
}

void cb_window_end_frame(CBWindow *w)
{
}

void cb_window_size(const CBWindow *w, int *width, int *height)
{
    *width = 0;
    *height = 0;
}

float cb_window_scale(const CBWindow *w)
{
    return 1.0f;
}

int cb_window_frames_drawn(const CBWindow *w)
{
    return 0;
}

double cb_window_frame_interval_ms(const CBWindow *w)
{
    return 0.0;
}

int cb_window_screenshot(CBWindow *w, const char *png_path)
{
    return CB_ERROR(ENOSYS);
}

uint64_t cb_texture_create(CBWindow *w, const uint8_t *rgba, int width, int height)
{
    return 0;
}

void cb_texture_destroy(CBWindow *w, uint64_t tex)
{
}

CBWaveGpu *cb_wave_upload(CBWindow *w, enum CBWaveKind kind, const uint8_t *columns, int nb_columns)
{
    return NULL;
}

void cb_wave_release(CBWindow *w, CBWaveGpu *g)
{
}

int cb_wave_columns(const CBWaveGpu *g)
{
    return 0;
}

void cb_wave_draw(CBWindow *w, void *im_draw_list, const CBWaveGpu *g, const CBWaveDraw *d)
{
}

int cb_ui_init(CBApp *app)
{
    return CB_ERROR(ENOSYS);
}

void cb_ui_shutdown(CBApp *app)
{
}

bool cb_ui_frame(CBApp *app)
{
    return false;
}
