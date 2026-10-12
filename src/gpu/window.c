/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* The window: SDL3 on Wayland, the Wayland frame callback that paces it,
 * the event loop, and the public entry points of rxbox/gpu.h that are not
 * about textures or waveforms.
 *
 * Frame pacing. A frame is drawn only when two things hold: the
 * compositor has answered the wl_surface.frame request that went out with
 * the last present (or nothing was presented yet), and there is a reason
 * to draw (the caller wants one, input arrived, or the window was resized,
 * exposed or rescaled). The request is made on a private wl_event_queue
 * through a proxy wrapper of SDL's wl_surface, the way libcraw's
 * tools/gui/colour.c wraps the display: SDL, Mesa's WSI and this file all
 * read the one socket, but each dispatches only its own queue, so none of
 * them sees the others' events and SDL's own frame callback is untouched.
 *
 * Waiting. With nothing to draw, the loop sleeps in SDL_WaitEventTimeout()
 * for the caller's timeout. With something to draw but the compositor's
 * answer still out, SDL's wait would be the wrong one: it would read the
 * answer off the socket into this file's queue, see no SDL event, and
 * sleep on. So that wait is a poll() on the display's socket through
 * wl_display_prepare_read_queue() on the private queue, which returns as
 * soon as anything arrives - the callback, input for SDL (read into SDL's
 * queue and dispatched by the SDL_PollEvent() that follows), or an
 * SDL_PushEvent() from another thread, which SDL turns into a wl_display
 * sync and so into bytes on the same socket.
 *
 * A window nobody can see (another workspace, the scratchpad, a
 * minimised or fully covered window) gets no frame callbacks. The loop
 * then never draws, still processes every event, and sleeps for the
 * caller's timeout between them: it costs nothing, and it never blocks in
 * the driver, which a FIFO present on such a window would. When the window
 * is shown again the compositor answers the outstanding request and
 * drawing resumes. */

#include "rxbox/gpu.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>
#include <wayland-client.h>

#include "config.h"
#include "gpu_demo.h"
#include "gpu_priv.h"

/* Frames drawn after input even without a redraw request: ImGui trickles
 * a press and a release that arrive together over two frames, and a
 * widget that changes the layout shows it one frame later still. */
#define RX_SETTLE_FRAMES 3

void rx_window_options_default(RxWindowOptions *o)
{
    *o = (RxWindowOptions) {
        .title     = "rxbox",
        .width     = 1600,
        .height    = 900,
        .font_size = 15.0f,
    };
}

/* -------------------------------------------------------- frame callbacks */

static void forget_cb(RxWindow *w, struct wl_callback *cb)
{
    for (size_t i = 0; i < w->frame_cbs.nb; i++) {
        struct wl_callback **p = rx_array_at(&w->frame_cbs, i);
        if (*p == cb) {
            rx_array_remove(&w->frame_cbs, i);
            break;
        }
    }
}

static void frame_done(void *opaque, struct wl_callback *cb, uint32_t time)
{
    RxWindow *w = opaque;

    /* Only the callback asked for with the last present says the
     * compositor wants the next frame; an older one (from a present that
     * failed and whose request rode on a later commit) only goes away. */
    if (cb == w->frame_cb) {
        int64_t now = rx_time_us();

        /* The interval between two answers means something only while
         * frames follow each other without a pause: the request that this
         * answers went out soon after the previous answer arrived, so the
         * compositor answered at its next refresh rather than whenever the
         * interface next had something to draw. */
        uint32_t delta = time - w->cb_time;
        bool continuous = w->cb_wall && w->req_wall >= w->cb_wall &&
                          w->req_wall - w->cb_wall < (int64_t)(w->interval_ms * 500.0);
        if (continuous && delta >= 1 && delta <= 100)
            w->interval_ms = 0.875 * w->interval_ms + 0.125 * delta;

        w->cb_time = time;
        w->cb_wall = now;
        w->frame_done = true;
        w->frame_cb = NULL;
    }

    forget_cb(w, cb);
    wl_callback_destroy(cb);
}

static const struct wl_callback_listener frame_listener = {
    .done = frame_done,
};

void rx_window_want_frame_cb(RxWindow *w)
{
    struct wl_callback *cb = wl_surface_frame(w->wl_wrap);
    if (!cb || !rx_array_push(&w->frame_cbs, &cb)) {
        /* Better unpaced than never drawn again. */
        if (cb)
            wl_callback_destroy(cb);
        w->frame_cb = NULL;
        w->frame_done = true;
        return;
    }

    wl_callback_add_listener(cb, &frame_listener, w);
    w->frame_cb = cb;
    w->frame_done = false;
    w->req_wall = rx_time_us();
}

void rx_window_frame_skipped(RxWindow *w)
{
    w->frame_cb = NULL;
    w->frame_done = true;
}

/* ------------------------------------------------------------------ events */

static void update_size(RxWindow *w)
{
    SDL_GetWindowSize(w->win, &w->width, &w->height);
    SDL_GetWindowSizeInPixels(w->win, &w->px_width, &w->px_height);

    /* SDL's display scale is the pixel density times the user's content
     * scale. On Wayland the logical size already carries the output scale,
     * so the density is what turns ImGui units into pixels and what is
     * left over is a UI scale (1 unless a platform reports one). */
    float density = SDL_GetWindowPixelDensity(w->win);
    float display = SDL_GetWindowDisplayScale(w->win);
    w->scale = density > 0.0f ? density : 1.0f;
    w->content_scale = display > 0.0f ? display / w->scale : 1.0f;
}

static void handle_event(RxWindow *w, const SDL_Event *ev)
{
    if (w->imgui)
        rx_imgui_process_event(w, ev);

    switch (ev->type) {
    case SDL_EVENT_QUIT:
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        w->should_close = true;
        break;
    case SDL_EVENT_WINDOW_RESIZED:
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        update_size(w);
        w->swap_dirty = true;
        w->need_redraw = true;
        w->stalled = false;
        break;
    case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED: {
        float old = w->content_scale;
        update_size(w);
        if (w->imgui && w->content_scale != old)
            rx_imgui_set_scale(w);
        w->swap_dirty = true;
        w->need_redraw = true;
        w->stalled = false;
        break;
    }
    case SDL_EVENT_WINDOW_EXPOSED:
    case SDL_EVENT_WINDOW_SHOWN:
    case SDL_EVENT_WINDOW_RESTORED:
        if (w->hidden)
            rx_debug("gpu: window shown");
        w->hidden = false;
        w->need_redraw = true;
        w->stalled = false;
        break;
    case SDL_EVENT_WINDOW_OCCLUDED:
    case SDL_EVENT_WINDOW_HIDDEN:
    case SDL_EVENT_WINDOW_MINIMIZED:
        if (!w->hidden)
            rx_debug("gpu: window hidden");
        w->hidden = true;
        break;
    default:
        w->settle = RX_SETTLE_FRAMES;
        break;
    }
}

static void dispatch_queue(RxWindow *w)
{
    if (wl_display_dispatch_queue_pending(w->wl_dpy, w->wl_queue) < 0) {
        int err = wl_display_get_error(w->wl_dpy);
        rx_err("gpu: the Wayland connection failed: %s", strerror(err ? err : EPIPE));
        w->should_close = true;
    }
}

/* SDL's events (SDL_PollEvent reads the socket), then ours. */
static void pump(RxWindow *w)
{
    SDL_Event ev;
    while (SDL_PollEvent(&ev))
        handle_event(w, &ev);
    dispatch_queue(w);
}

static void wait_event(RxWindow *w, int timeout_ms)
{
    SDL_Event ev;
    if (SDL_WaitEventTimeout(&ev, timeout_ms))
        handle_event(w, &ev);
}

/* Sleeps until something arrives on the Wayland socket or the timeout
 * passes (see the top of the file for why not SDL's wait). */
static void wait_frame(RxWindow *w, int timeout_ms)
{
    while (wl_display_prepare_read_queue(w->wl_dpy, w->wl_queue) != 0) {
        dispatch_queue(w);
        if (w->frame_done || w->should_close)
            return;
    }

    if (wl_display_flush(w->wl_dpy) < 0 && errno != EAGAIN) {
        wl_display_cancel_read(w->wl_dpy);
        dispatch_queue(w);
        return;
    }

    struct pollfd pfd = {
        .fd     = wl_display_get_fd(w->wl_dpy),
        .events = POLLIN,
    };
    int n = poll(&pfd, 1, timeout_ms);
    if (n > 0)
        wl_display_read_events(w->wl_dpy);
    else
        wl_display_cancel_read(w->wl_dpy);

    dispatch_queue(w);
}

static bool wanted(const RxWindow *w, bool want_redraw)
{
    return want_redraw || w->need_redraw || w->settle > 0;
}

static bool ready(const RxWindow *w)
{
    return w->frame_done && !w->stalled;
}

bool rx_window_poll(RxWindow *w, bool want_redraw, int timeout_ms)
{
    if (w->opts.max_frames > 0 && w->frames_drawn >= w->opts.max_frames)
        w->should_close = true;

    pump(w);
    if (w->should_close)
        return false;
    if (wanted(w, want_redraw) && ready(w))
        return true;

    if (wanted(w, want_redraw) && !w->frame_done)
        wait_frame(w, timeout_ms);
    else
        wait_event(w, timeout_ms);

    /* A frame that could not be drawn (no area, swapchain out of date) is
     * retried after one wait rather than at once. */
    w->stalled = false;

    pump(w);
    return !w->should_close && wanted(w, want_redraw) && ready(w);
}

bool rx_window_should_close(const RxWindow *w)
{
    if (w->opts.max_frames > 0 && w->frames_drawn >= w->opts.max_frames)
        return true;
    return w->should_close;
}

void rx_window_request_close(RxWindow *w)
{
    w->should_close = true;
}

/* ------------------------------------------------------------------ frames */

void rx_window_begin_frame(RxWindow *w)
{
    if (w->in_frame)
        return;
    rx_imgui_new_frame(w);
    w->in_frame = true;
}

void rx_window_end_frame(RxWindow *w)
{
    if (!w->in_frame)
        return;
    w->in_frame = false;

    void *dd = rx_imgui_render(w);

    /* The last frame of a test run is always captured for the screenshot. */
    bool capture = w->capture_next ||
                   (w->opts.max_frames > 0 && w->frames_drawn + 1 >= w->opts.max_frames);

    int ret = rx_swapchain_frame(w, dd, capture);
    if (ret < 0) {
        rx_err("gpu: drawing a frame failed (%d)", ret);
        if (ret == RX_ERROR(EIO) || ret == RX_ERROR(ENODEV))
            w->should_close = true;
        w->stalled = true;
    } else if (ret > 0) {
        w->stalled = true;
    } else {
        w->frames_drawn++;
        w->need_redraw = false;
        w->capture_next = false;
        if (w->settle > 0)
            w->settle--;
        /* rx_window_screenshot_next: this frame was captured and is now
         * presented. The readback waits for its submission, once. */
        if (w->shot_path) {
            int err = rx_swapchain_save_png(w, w->shot_path);
            if (err < 0)
                rx_err("gpu: screenshot to %s failed (%d)", w->shot_path, err);
            free(w->shot_path);
            w->shot_path = NULL;
        }
    }

    wl_display_flush(w->wl_dpy);
}

/* ----------------------------------------------------------------- getters */

void rx_window_size(const RxWindow *w, int *width, int *height)
{
    if (width)
        *width = w->width;
    if (height)
        *height = w->height;
}

float rx_window_scale(const RxWindow *w)
{
    return w->scale;
}

int rx_window_frames_drawn(const RxWindow *w)
{
    return w->frames_drawn;
}

double rx_window_frame_interval_ms(const RxWindow *w)
{
    return w->interval_ms;
}

int rx_window_validation_errors(const RxWindow *w)
{
    return w->validation_errors;
}

bool rx_window_frame_pending(const RxWindow *w)
{
    return !w->frame_done;
}

void rx_window_capture_next(RxWindow *w)
{
    w->capture_next = true;
}

int rx_window_readback(RxWindow *w, uint8_t **rgba, int *width, int *height)
{
    if (!w->cap_serial || w->cap_serial != w->last_present_serial)
        return RX_ERROR(EAGAIN);
    return rx_swapchain_readback(w, rgba, width, height);
}

/* ----------------------------------------------------------- create/destroy */

static int open_wayland(RxWindow *w)
{
    SDL_PropertiesID props = SDL_GetWindowProperties(w->win);
    w->wl_dpy  = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, NULL);
    w->wl_surf = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, NULL);
    if (!w->wl_dpy || !w->wl_surf) {
        rx_err("gpu: not a Wayland window; the interface runs on Wayland only");
        return RX_ERROR(ENODEV);
    }

#if WAYLAND_VERSION_MAJOR > 1 || WAYLAND_VERSION_MINOR >= 23
    w->wl_queue = wl_display_create_queue_with_name(w->wl_dpy, "rxbox frame callbacks");
#else
    w->wl_queue = wl_display_create_queue(w->wl_dpy);
#endif
    w->wl_wrap = wl_proxy_create_wrapper(w->wl_surf);
    if (!w->wl_queue || !w->wl_wrap)
        return RX_ERROR(ENOMEM);
    wl_proxy_set_queue((struct wl_proxy *)w->wl_wrap, w->wl_queue);

    return 0;
}

static char *dup_opt(const char *s)
{
    return s ? rx_strdup(s) : NULL;
}

int rx_window_create(RxWindow **out, const RxWindowOptions *o)
{
    RxWindowOptions def;
    if (!o) {
        rx_window_options_default(&def);
        o = &def;
    }

    RxWindow *w = calloc(1, sizeof(*w));
    if (!w)
        return RX_ERROR(ENOMEM);

    int ret = 0;
    w->opts = *o;
    w->title  = dup_opt(o->title ? o->title : "rxbox");
    w->font   = dup_opt(o->font);
    w->device = dup_opt(o->device);
    if (!w->title || (o->font && !w->font) || (o->device && !w->device)) {
        ret = RX_ERROR(ENOMEM);
        goto fail;
    }
    w->opts.title  = w->title;
    w->opts.font   = w->font;
    w->opts.device = w->device;
    if (w->opts.width <= 0 || w->opts.height <= 0) {
        w->opts.width = 1600;
        w->opts.height = 900;
    }
    if (w->opts.font_size <= 0.0f)
        w->opts.font_size = 15.0f;

    rx_array_init(&w->frame_cbs, sizeof(struct wl_callback *));
    w->frame_done = true;
    w->need_redraw = true;
    w->interval_ms = 1000.0 / 60.0;

    /* Wayland only: the pacing is built on its frame callback. The app id
     * is what compositors match window rules on (tests/sway.conf). */
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "wayland");
    SDL_SetAppMetadata("rxbox", RXBOX_VERSION, "rxbox");
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        rx_err("gpu: SDL could not open a Wayland display: %s", SDL_GetError());
        ret = RX_ERROR(ENODEV);
        goto fail;
    }
    w->sdl_init = true;

    w->win = SDL_CreateWindow(w->title, w->opts.width, w->opts.height,
                              SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE |
                              SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!w->win) {
        rx_err("gpu: SDL_CreateWindow: %s", SDL_GetError());
        ret = RX_ERROR(ENODEV);
        goto fail;
    }

    ret = open_wayland(w);
    if (ret < 0)
        goto fail;

    update_size(w);

    ret = rx_vk_create(w);
    if (ret < 0)
        goto fail;

    ret = rx_swapchain_create(w);
    if (ret < 0)
        goto fail;

    ret = rx_imgui_init(w);
    if (ret < 0)
        goto fail;

    ret = rx_wave_pipeline_create(w);
    if (ret < 0)
        goto fail;

    rx_info("gpu: window %dx%d (%dx%d pixels, scale %.2f)", w->width, w->height,
            w->px_width, w->px_height, w->scale);

    *out = w;
    return 0;

fail:
    rx_window_destroy(w);
    return ret;
}

void rx_window_destroy(RxWindow *w)
{
    if (!w)
        return;

    /* GPU first: nothing may still be reading what goes next. Textures
     * unregister from ImGui, so they go before its backend does; the
     * swapchain before the surface (in rx_vk_destroy), and all of it
     * before SDL takes the wl_surface away. */
    if (w->dev) {
        vkDeviceWaitIdle(w->dev);
        rx_textures_destroy_all(w);
        rx_vk_collect(w, true);
    }
    rx_imgui_shutdown(w);
    if (w->dev) {
        rx_wave_pipeline_destroy(w);
        rx_swapchain_destroy(w);
    }
    rx_vk_destroy(w);

    /* Our proxies on SDL's display, before SDL closes it. */
    for (size_t i = 0; i < w->frame_cbs.nb; i++)
        wl_callback_destroy(*(struct wl_callback **)rx_array_at(&w->frame_cbs, i));
    rx_array_free(&w->frame_cbs);
    if (w->wl_wrap)
        wl_proxy_wrapper_destroy(w->wl_wrap);
    if (w->wl_queue)
        wl_event_queue_destroy(w->wl_queue);

    if (w->win)
        SDL_DestroyWindow(w->win);
    if (w->sdl_init)
        SDL_QuitSubSystem(SDL_INIT_VIDEO);

    free(w->title);
    free(w->font);
    free(w->device);
    free(w->shot_path);
    free(w);
}
