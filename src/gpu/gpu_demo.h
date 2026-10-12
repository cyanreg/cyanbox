/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* Hooks for the gpu module's own demo and tests (tools/rxbox-gpu-demo.c,
 * tests/gpu): what a test needs to look at that the interface never does,
 * and the handful of ImGui calls the demo makes, in C so that the demo can
 * stay a C program. Not for src/ui, which calls ImGui directly. */

#ifndef RXBOX_GPU_DEMO_H
#define RXBOX_GPU_DEMO_H

#include "rxbox/gpu.h"

/* Errors the validation layer reported (opts.validation). */
int  rx_window_validation_errors(const RXWindow *w);
/* A frame was presented and the compositor has not answered it yet. */
bool rx_window_frame_pending(const RXWindow *w);
/* Captures the next presented frame for rx_window_readback(). */
void rx_window_capture_next(RXWindow *w);
/* The last presented frame as RGBA, if it was captured; -EAGAIN if not. */
int  rx_window_readback(RXWindow *w, uint8_t **rgba, int *width, int *height);

/* ImGui, from C. Colours are 0xRRGGBBAA. Draw lists are ImDrawList *. */
void  rx_demo_show_demo_window(void);
bool  rx_demo_begin(const char *title, float x, float y, float width, float height);
void  rx_demo_end(void);
void  rx_demo_text(const char *fmt, ...) RX_PRINTF(1, 2);
void *rx_demo_window_draw_list(void);
void *rx_demo_background_draw_list(void);
void *rx_demo_foreground_draw_list(void);
/* Screen position of the next item, and the width left on its line. */
void  rx_demo_cursor(float *x, float *y);
float rx_demo_avail_width(void);
/* Reserves space in the layout. */
void  rx_demo_dummy(float width, float height);
void  rx_demo_rect(void *draw_list, float x0, float y0, float x1, float y1, uint32_t rgba);
void  rx_demo_line(void *draw_list, float x0, float y0, float x1, float y1, uint32_t rgba,
                   float thickness);
void  rx_demo_image(uint64_t tex, float width, float height);
bool  rx_demo_checkbox(const char *label, bool *v);
bool  rx_demo_slider(const char *label, float *v, float lo, float hi);

#endif /* RXBOX_GPU_DEMO_H */
