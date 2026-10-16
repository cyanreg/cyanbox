/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* Hooks for the gpu module's own demo and tests (tools/cyanbox-gpu-demo.c,
 * tests/gpu): what a test needs to look at that the interface never does,
 * and the handful of ImGui calls the demo makes, in C so that the demo can
 * stay a C program. Not for src/ui, which calls ImGui directly. */

#ifndef CYANBOX_GPU_DEMO_H
#define CYANBOX_GPU_DEMO_H

#include "cyanbox/gpu.h"

/* Errors the validation layer reported (opts.validation). */
int  cb_window_validation_errors(const CBWindow *w);
/* A frame was presented and the compositor has not answered it yet. */
bool cb_window_frame_pending(const CBWindow *w);
/* Captures the next presented frame for cb_window_readback(). */
void cb_window_capture_next(CBWindow *w);
/* The last presented frame as RGBA, if it was captured; -EAGAIN if not. */
int  cb_window_readback(CBWindow *w, uint8_t **rgba, int *width, int *height);
/* How long the GPU took to render the latest frame known to be finished
 * (frames in flight make it a frame or two old), in ms; 0 when the queue
 * has no timestamps or no frame has finished yet. */
double cb_window_gpu_ms(const CBWindow *w);

/* ImGui, from C. Colours are 0xRRGGBBAA. Draw lists are ImDrawList *. */
void  cb_demo_show_demo_window(void);
bool  cb_demo_begin(const char *title, float x, float y, float width, float height);
void  cb_demo_end(void);
void  cb_demo_text(const char *fmt, ...) CB_PRINTF(1, 2);
void *cb_demo_window_draw_list(void);
void *cb_demo_background_draw_list(void);
void *cb_demo_foreground_draw_list(void);
/* Screen position of the next item, and the width left on its line. */
void  cb_demo_cursor(float *x, float *y);
float cb_demo_avail_width(void);
/* Reserves space in the layout. */
void  cb_demo_dummy(float width, float height);
void  cb_demo_rect(void *draw_list, float x0, float y0, float x1, float y1, uint32_t rgba);
void  cb_demo_line(void *draw_list, float x0, float y0, float x1, float y1, uint32_t rgba,
                   float thickness);
void  cb_demo_image(uint64_t tex, float width, float height);
bool  cb_demo_checkbox(const char *label, bool *v);
bool  cb_demo_slider(const char *label, float *v, float lo, float hi);

#endif /* CYANBOX_GPU_DEMO_H */
