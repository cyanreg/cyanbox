/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* The window: SDL3 on Wayland, the Wayland frame callback that paces it,
 * the event loop, and the public entry points of cyanbox/gpu.h that are not
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
 * then never draws, still processes every event, and after 150 ms without
 * an answer sleeps at least 100 ms between looks (cb_window_poll): it
 * costs nothing, and it never blocks in the driver, which a FIFO present
 * on such a window would. When the window is shown again the compositor
 * answers the outstanding request, the socket wakes the sleep, and drawing
 * resumes. */

#include "cyanbox/gpu.h"

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
#define CB_SETTLE_FRAMES 3

void cb_window_options_default(CBWindowOptions *o)
{
    *o = (CBWindowOptions) {
        .title     = "cyanbox",
        .width     = 1600,
        .height    = 900,
        .font_size = 15.0f,
    };
}

/* -------------------------------------------------------- frame callbacks */

static void forget_cb(CBWindow *w, struct wl_callback *cb)
{
    for (size_t i = 0; i < w->frame_cbs.nb; i++) {
        struct wl_callback **p = cb_array_at(&w->frame_cbs, i);
        if (*p == cb) {
            cb_array_remove(&w->frame_cbs, i);
            break;
        }
    }
}

static void frame_done(void *opaque, struct wl_callback *cb, uint32_t time)
{
    CBWindow *w = opaque;

    /* Only the callback asked for with the last present says the
     * compositor wants the next frame; an older one (from a present that
     * failed and whose request rode on a later commit) only goes away. */
    if (cb == w->frame_cb) {
        int64_t now = cb_time_us();

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

void cb_window_want_frame_cb(CBWindow *w)
{
    struct wl_callback *cb = wl_surface_frame(w->wl_wrap);
    if (!cb || !cb_array_push(&w->frame_cbs, &cb)) {
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
    w->req_wall = cb_time_us();
}

void cb_window_frame_skipped(CBWindow *w)
{
    w->frame_cb = NULL;
    w->frame_done = true;
}

/* ------------------------------------------------------------------ events */

static void update_size(CBWindow *w)
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

static void handle_event(CBWindow *w, const SDL_Event *ev)
{
    if (w->imgui)
        cb_imgui_process_event(w, ev);

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
            cb_imgui_set_scale(w);
        w->swap_dirty = true;
        w->need_redraw = true;
        w->stalled = false;
        break;
    }
    case SDL_EVENT_WINDOW_EXPOSED:
    case SDL_EVENT_WINDOW_SHOWN:
    case SDL_EVENT_WINDOW_RESTORED:
        if (w->hidden)
            cb_debug("gpu: window shown");
        w->hidden = false;
        w->need_redraw = true;
        w->stalled = false;
        break;
    case SDL_EVENT_WINDOW_OCCLUDED:
    case SDL_EVENT_WINDOW_HIDDEN:
    case SDL_EVENT_WINDOW_MINIMIZED:
        if (!w->hidden)
            cb_debug("gpu: window hidden");
        w->hidden = true;
        break;
    default:
        w->settle = CB_SETTLE_FRAMES;
        break;
    }
}

static void dispatch_queue(CBWindow *w)
{
    if (wl_display_dispatch_queue_pending(w->wl_dpy, w->wl_queue) < 0) {
        int err = wl_display_get_error(w->wl_dpy);
        cb_err("gpu: the Wayland connection failed: %s", strerror(err ? err : EPIPE));
        w->should_close = true;
    }
}

/* SDL's events (SDL_PollEvent reads the socket), then ours. */
static void pump(CBWindow *w)
{
    SDL_Event ev;
    while (SDL_PollEvent(&ev))
        handle_event(w, &ev);
    dispatch_queue(w);
}

static void wait_event(CBWindow *w, int timeout_ms)
{
    SDL_Event ev;
    if (SDL_WaitEventTimeout(&ev, timeout_ms))
        handle_event(w, &ev);
}

/* Sleeps until something arrives on the Wayland socket or the timeout
 * passes (see the top of the file for why not SDL's wait). */
static void wait_frame(CBWindow *w, int timeout_ms)
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

static bool wanted(const CBWindow *w, bool want_redraw)
{
    return want_redraw || w->need_redraw || w->settle > 0;
}

static bool ready(const CBWindow *w)
{
    return w->frame_done && !w->stalled;
}

bool cb_window_poll(CBWindow *w, bool want_redraw, int timeout_ms)
{
    if (w->opts.max_frames > 0 && w->frames_drawn >= w->opts.max_frames)
        w->should_close = true;

    pump(w);
    if (w->should_close)
        return false;
    if (wanted(w, want_redraw) && ready(w))
        return true;

    if (wanted(w, want_redraw) && !w->frame_done) {
        /* A window nobody can see gets no answer. After 150 ms without one
         * the wait stretches to 100 ms: the socket wakes it the moment the
         * answer comes, so a visible window loses nothing, and a hidden one
         * stops waking every few milliseconds for a callback that is not
         * coming (the caller's short timeout is for its own bookkeeping). */
        int t = timeout_ms;
        if (cb_time_us() - w->req_wall > 150000)
            t = CB_MAX(t, 100);
        wait_frame(w, t);
    } else {
        wait_event(w, timeout_ms);
    }

    /* A frame that could not be drawn (no area, swapchain out of date) is
     * retried after one wait rather than at once. */
    w->stalled = false;

    pump(w);
    return !w->should_close && wanted(w, want_redraw) && ready(w);
}

bool cb_window_hidden(const CBWindow *w)
{
    return w && w->hidden;
}

bool cb_window_should_close(const CBWindow *w)
{
    if (w->opts.max_frames > 0 && w->frames_drawn >= w->opts.max_frames)
        return true;
    return w->should_close;
}

void cb_window_request_close(CBWindow *w)
{
    w->should_close = true;
}

/* ------------------------------------------------------------------ frames */

void cb_window_begin_frame(CBWindow *w)
{
    if (w->in_frame)
        return;
    cb_imgui_new_frame(w);
    w->in_frame = true;
}

void cb_window_end_frame(CBWindow *w)
{
    if (!w->in_frame)
        return;
    w->in_frame = false;

    void *dd = cb_imgui_render(w);

    /* The last frame of a test run is always captured for the screenshot. */
    bool capture = w->capture_next ||
                   (w->opts.max_frames > 0 && w->frames_drawn + 1 >= w->opts.max_frames);

    int ret = cb_swapchain_frame(w, dd, capture);
    if (ret < 0) {
        cb_err("gpu: drawing a frame failed (%d)", ret);
        if (ret == CB_ERROR(EIO) || ret == CB_ERROR(ENODEV))
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
        /* cb_window_screenshot_next: this frame was captured and is now
         * presented. The readback waits for its submission, once. */
        if (w->shot_path) {
            int err = cb_swapchain_save_png(w, w->shot_path);
            if (err < 0)
                cb_err("gpu: screenshot to %s failed (%d)", w->shot_path, err);
            free(w->shot_path);
            w->shot_path = NULL;
        }
    }

    wl_display_flush(w->wl_dpy);
}

/* ----------------------------------------------------------------- getters */

void cb_window_size(const CBWindow *w, int *width, int *height)
{
    if (width)
        *width = w->width;
    if (height)
        *height = w->height;
}

float cb_window_scale(const CBWindow *w)
{
    return w->scale;
}

int cb_window_frames_drawn(const CBWindow *w)
{
    return w->frames_drawn;
}

double cb_window_frame_interval_ms(const CBWindow *w)
{
    return w->interval_ms;
}

double cb_window_gpu_ms(const CBWindow *w)
{
    return w->gpu_ms;
}

int cb_window_validation_errors(const CBWindow *w)
{
    return cb_vkdev_validation_errors(&w->vk);
}

bool cb_window_frame_pending(const CBWindow *w)
{
    return !w->frame_done;
}

void cb_window_capture_next(CBWindow *w)
{
    w->capture_next = true;
}

int cb_window_readback(CBWindow *w, uint8_t **rgba, int *width, int *height)
{
    if (!w->cap_serial || w->cap_serial != w->last_present_serial)
        return CB_ERROR(EAGAIN);
    return cb_swapchain_readback(w, rgba, width, height);
}

/* ----------------------------------------------------------- create/destroy */

static int open_wayland(CBWindow *w)
{
    SDL_PropertiesID props = SDL_GetWindowProperties(w->win);
    w->wl_dpy  = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, NULL);
    w->wl_surf = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, NULL);
    if (!w->wl_dpy || !w->wl_surf) {
        cb_err("gpu: not a Wayland window; the interface runs on Wayland only");
        return CB_ERROR(ENODEV);
    }

#if WAYLAND_VERSION_MAJOR > 1 || WAYLAND_VERSION_MINOR >= 23
    w->wl_queue = wl_display_create_queue_with_name(w->wl_dpy, "cyanbox frame callbacks");
#else
    w->wl_queue = wl_display_create_queue(w->wl_dpy);
#endif
    w->wl_wrap = wl_proxy_create_wrapper(w->wl_surf);
    if (!w->wl_queue || !w->wl_wrap)
        return CB_ERROR(ENOMEM);
    wl_proxy_set_queue((struct wl_proxy *)w->wl_wrap, w->wl_queue);

    return 0;
}

static char *dup_opt(const char *s)
{
    return s ? cb_strdup(s) : NULL;
}

int cb_window_create(CBWindow **out, const CBWindowOptions *o)
{
    CBWindowOptions def;
    if (!o) {
        cb_window_options_default(&def);
        o = &def;
    }

    CBWindow *w = calloc(1, sizeof(*w));
    if (!w)
        return CB_ERROR(ENOMEM);

    int ret = 0;
    w->opts = *o;
    w->title  = dup_opt(o->title ? o->title : "cyanbox");
    w->font   = dup_opt(o->font);
    w->device = dup_opt(o->device);
    if (!w->title || (o->font && !w->font) || (o->device && !w->device)) {
        ret = CB_ERROR(ENOMEM);
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

    cb_array_init(&w->frame_cbs, sizeof(struct wl_callback *));
    w->frame_done = true;
    w->need_redraw = true;
    w->interval_ms = 1000.0 / 60.0;

    /* Wayland only: the pacing is built on its frame callback. The app id
     * is what compositors match window rules on (tests/sway.conf). */
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "wayland");
    SDL_SetAppMetadata("cyanbox", CYANBOX_VERSION, "cyanbox");
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        cb_err("gpu: SDL could not open a Wayland display: %s", SDL_GetError());
        ret = CB_ERROR(ENODEV);
        goto fail;
    }
    w->sdl_init = true;

    /* Borderless: no title bar. On Wayland SDL then asks the compositor
     * for client-side decoration (xdg-decoration) and draws none, so a
     * tiling compositor such as sway marks the window csd and draws no
     * title bar of its own either; the whole surface is the interface. */
    w->win = SDL_CreateWindow(w->title, w->opts.width, w->opts.height,
                              SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE |
                              SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_BORDERLESS);
    if (!w->win) {
        cb_err("gpu: SDL_CreateWindow: %s", SDL_GetError());
        ret = CB_ERROR(ENODEV);
        goto fail;
    }

    ret = open_wayland(w);
    if (ret < 0)
        goto fail;

    update_size(w);

    ret = cb_vk_create(w);
    if (ret < 0)
        goto fail;

    ret = cb_swapchain_create(w);
    if (ret < 0)
        goto fail;

    ret = cb_imgui_init(w);
    if (ret < 0)
        goto fail;

    ret = cb_wave_pipeline_create(w);
    if (ret < 0)
        goto fail;

    cb_info("gpu: window %dx%d (%dx%d pixels, scale %.2f)", w->width, w->height,
            w->px_width, w->px_height, w->scale);

    *out = w;
    return 0;

fail:
    cb_window_destroy(w);
    return ret;
}

void cb_window_destroy(CBWindow *w)
{
    if (!w)
        return;

    /* GPU first: nothing may still be reading what goes next. Textures
     * unregister from ImGui, so they go before its backend does; the
     * swapchain before the surface (in cb_vk_destroy), and all of it
     * before SDL takes the wl_surface away. */
    if (w->vk.dev) {
        vkDeviceWaitIdle(w->vk.dev);
        cb_textures_destroy_all(w);
        cb_vk_collect(w, true);
    }
    cb_imgui_shutdown(w);
    if (w->vk.dev) {
        cb_wave_pipeline_destroy(w);
        cb_swapchain_destroy(w);
    }
    cb_vk_destroy(w);

    /* Our proxies on SDL's display, before SDL closes it. */
    for (size_t i = 0; i < w->frame_cbs.nb; i++)
        wl_callback_destroy(*(struct wl_callback **)cb_array_at(&w->frame_cbs, i));
    cb_array_free(&w->frame_cbs);
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
