/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* Private to src/gpu: the window's state and the calls the files of the
 * module make into each other. window.c owns SDL, the Wayland frame
 * callback and the public entry points; vk.c the device (set up by
 * vkdev.c, as the compute context's and the model's are, but its own),
 * buffers, deferred destruction and staged uploads; swapchain.c the
 * swapchain, the frames in flight, recording, presenting and the
 * screenshot; textures.c and wave.c the two kinds of user resources;
 * imgui_glue.cpp everything that has to be C++.
 *
 * C++ includes this inside extern "C" { } after <vulkan/vulkan.h> and
 * vk_mem_alloc.h, which it must include itself first: neither header may
 * be opened inside a C linkage block. SDL and Wayland types are only
 * forward-declared here for the same reason. */

#ifndef CYANBOX_GPU_PRIV_H
#define CYANBOX_GPU_PRIV_H

#include "cyanbox/gpu.h"

#include <vulkan/vulkan.h>

/* VMA tests extension macros that only exist on Windows with #if. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wundef"
#include "vk_mem_alloc.h"
#pragma GCC diagnostic pop

#include "vkdev.h"

struct SDL_Window;
union SDL_Event;
struct wl_display;
struct wl_surface;
struct wl_event_queue;
struct wl_callback;

/* Frames the CPU may record ahead of the GPU. Two is enough for an
 * interface that draws only when the compositor asks: the third would only
 * add a frame of latency. ImGui's vertex buffers rotate over the same
 * count (ImGui_ImplVulkan_InitInfo.ImageCount). */
#define CB_FRAMES_IN_FLIGHT 2

/* Background the frame is cleared to before ImGui draws (the UI paints its
 * own windows over it). */
#define CB_CLEAR_R (0x20 / 255.0f)
#define CB_CLEAR_G (0x20 / 255.0f)
#define CB_CLEAR_B (0x20 / 255.0f)

enum CBMemUsage {
    CB_MEM_DEVICE = 0,  /* device local, never mapped */
    CB_MEM_UPLOAD,      /* host visible, mapped, written sequentially */
    CB_MEM_READBACK,    /* host visible and cached, mapped, read by the CPU */
};

typedef struct CBVkBuffer {
    VkBuffer        buf;
    VmaAllocation   alloc;
    VkDeviceSize    size;
    VkDeviceAddress addr;   /* 0 unless created with SHADER_DEVICE_ADDRESS usage */
    void           *map;    /* NULL for CB_MEM_DEVICE */
} CBVkBuffer;

/* Something the GPU may still be reading: freed once the submission
 * numbered `serial` has completed. Any field may be null. */
typedef struct CBDeferred {
    uint64_t      serial;
    VkBuffer      buf;
    VkImage       img;
    VkImageView   view;
    VmaAllocation alloc;
    uint64_t      tex_id;   /* an ImGui texture descriptor set */
} CBDeferred;

/* A copy from a staging buffer into a buffer or an image, recorded at the
 * start of the next frame's command buffer (vk.c). */
typedef struct CBUpload {
    CBVkBuffer staging;
    VkBuffer   dst_buf;
    VkImage    dst_img;
    uint32_t   width, height;
} CBUpload;

/* A texture handed out as an ImTextureID. */
typedef struct CBTexture {
    uint64_t      id;
    VkImage       img;
    VkImageView   view;
    VmaAllocation alloc;
} CBTexture;

/* One frame in flight. The fence says the slot's command buffer, its
 * acquire semaphore and its timestamps are free again. */
typedef struct CBFrame {
    VkCommandPool   pool;
    VkCommandBuffer cmd;
    VkFence         fence;
    VkSemaphore     acquired;
    VkQueryPool     times;      /* two timestamps around the rendering, if the queue has them */
    bool            timed;      /* the last submission wrote them */
    uint64_t        serial;     /* the submission the slot last carried, 0 = none */
} CBFrame;

struct CBWindow {
    CBWindowOptions opts;
    char *title, *font, *device;    /* the options' strings, owned */

    /* SDL, and SDL's Wayland objects (borrowed, never destroyed here). */
    struct SDL_Window *win;
    bool sdl_init;
    struct wl_display *wl_dpy;
    struct wl_surface *wl_surf;

    /* The frame callback (window.c). A private queue and a proxy wrapper of
     * SDL's surface on it, so the callbacks are dispatched only by
     * cb_window_poll() and SDL's own dispatch never sees them. */
    struct wl_event_queue *wl_queue;
    struct wl_surface *wl_wrap;
    CBArray frame_cbs;              /* struct wl_callback *, every outstanding one */
    struct wl_callback *frame_cb;   /* the one asked for with the last present */
    bool     frame_done;            /* ... and it has arrived */
    uint32_t cb_time;               /* compositor time of the last callback, ms */
    int64_t  cb_wall;               /* cb_time_us() when it was dispatched */
    double   interval_ms;
    int64_t  req_wall;              /* cb_time_us() when the last one was asked for */

    bool should_close;
    bool need_redraw;               /* first frame, resize, expose, scale change */
    bool stalled;                   /* the last frame could not be drawn: wait once first */
    int  settle;                    /* frames still owed after input (ImGui trickles it) */
    bool hidden;                    /* occluded, minimized or hidden, as SDL said */

    int   width, height;            /* logical */
    int   px_width, px_height;
    float scale;                    /* pixels per logical unit */
    float content_scale;            /* the user's UI scale on top of that */

    /* Vulkan (vk.c) */
    CBVkDev vk;
    VkSurfaceKHR surface;

    CBFrame  frames[CB_FRAMES_IN_FLIGHT];
    uint32_t frame_idx;
    uint64_t serial;                /* submissions made; the next one is serial + 1 */
    uint64_t completed;             /* every submission up to this one has finished */
    int frames_drawn;
    double gpu_ms;                  /* the rendering of the last finished frame, 0 = unknown */

    CBArray garbage;                /* CBDeferred */
    size_t defer_room;              /* room in `garbage` taken for later deferrals (vk.c) */
    CBArray uploads;                /* CBUpload */
    CBArray textures;               /* CBTexture */

    /* Swapchain (swapchain.c) */
    VkSwapchainKHR swapchain;
    VkSurfaceFormatKHR format;
    VkPresentModeKHR present_mode;
    VkExtent2D extent;
    bool can_capture;               /* the images allow TRANSFER_SRC */
    uint32_t nb_images;
    VkImage *images;
    VkImageView *views;
    VkSemaphore *rendered;          /* one per image: what the present waits on */
    bool swap_dirty;

    /* The screenshot readback: the last captured frame and its submission. */
    CBVkBuffer cap;
    uint32_t cap_width, cap_height;
    uint64_t cap_serial;
    uint64_t last_present_serial;
    bool capture_next;
    char *shot_path;                /* cb_window_screenshot_next: where the next frame goes */

    /* Waveforms (wave.c) */
    VkPipelineLayout wave_layout;
    VkPipeline wave_pipe;

    /* ImGui (imgui_glue.cpp) */
    void *imgui;                    /* ImGuiContext * */
    float imgui_scale;              /* the content scale the style was scaled by */
    bool imgui_vk;                  /* the Vulkan backend is up */
    bool imgui_sdl;
    bool in_frame;                  /* between begin and end */
};

/* ---------------------------------------------------------------- window.c */

/* Asks for wl_surface.frame on the next commit. Called right before
 * vkQueuePresentKHR, whose commit then carries it. */
void cb_window_want_frame_cb(CBWindow *w);
/* The present did not happen: nothing will answer the request. */
void cb_window_frame_skipped(CBWindow *w);

/* -------------------------------------------------------------------- vk.c */

int  cb_vk_create(CBWindow *w);
void cb_vk_destroy(CBWindow *w);

int  cb_vk_buffer_create(CBWindow *w, CBVkBuffer *b, VkDeviceSize size,
                         VkBufferUsageFlags usage, enum CBMemUsage mem);
void cb_vk_buffer_free(CBWindow *w, CBVkBuffer *b);
int  cb_vk_buffer_invalidate(CBWindow *w, CBVkBuffer *b);

/* Takes room in the deferred destruction list for `n` things to be deferred
 * later, so that cb_vk_defer never needs memory; called when they are made
 * (-ENOMEM fails the making). cb_vk_defer_unreserve gives it back when one
 * of them is freed at once instead (it never reached the GPU). */
int  cb_vk_defer_reserve(CBWindow *w, size_t n);
void cb_vk_defer_unreserve(CBWindow *w, size_t n);
/* Hands something to the deferred destruction list, to be freed after the
 * next submission (the one that may still use it) has completed. */
void cb_vk_defer(CBWindow *w, const CBDeferred *d);
/* What was deferred to submissions from `from` on is kept until `until`
 * has completed: the draw data of frame `from` is drawn again in it. */
void cb_vk_defer_hold(CBWindow *w, uint64_t from, uint64_t until);
/* Frees what the GPU is done with; everything when `all` (device idle). */
void cb_vk_collect(CBWindow *w, bool all);
/* Learns which submissions have finished without waiting. */
void cb_vk_poll_completed(CBWindow *w);

/* Queues a staged copy; takes ownership of up->staging. */
int  cb_vk_upload(CBWindow *w, const CBUpload *up);
/* Records every queued copy into `cmd` (the frame about to be submitted
 * as `serial`), with the barriers that make the results visible to the
 * fragment shader, and defers the staging buffers. */
void cb_vk_record_uploads(CBWindow *w, VkCommandBuffer cmd, uint64_t serial);

/* ------------------------------------------------------------- swapchain.c */

int  cb_swapchain_create(CBWindow *w);
void cb_swapchain_destroy(CBWindow *w);
/* Records the draw data (an ImDrawData *), submits and presents. With
 * `capture`, the image is also copied into w->cap before it is presented.
 * Returns 0 when presented, 1 when the frame was skipped (the swapchain is
 * out of date, or the window or the draw data has no area), <0 on error. */
int  cb_swapchain_frame(CBWindow *w, void *draw_data, bool capture);
/* Reads the last captured frame back as tightly packed RGBA. */
int  cb_swapchain_readback(CBWindow *w, uint8_t **rgba, int *width, int *height);
/* Writes the last presented frame as PNG; -EAGAIN when it was not captured. */
int  cb_swapchain_save_png(CBWindow *w, const char *png_path);

/* ------------------------------------------------------------- textures.c */

void cb_textures_destroy_all(CBWindow *w);

/* ----------------------------------------------------------------- wave.c */

int  cb_wave_pipeline_create(CBWindow *w);
void cb_wave_pipeline_destroy(CBWindow *w);

/* What a waveform callback carries in the draw list. */
typedef struct CBWaveCmd {
    CBWindow       *w;
    VkDeviceAddress addr;
    int32_t         nb_columns;
    int32_t         kind;
    CBWaveDraw      d;
} CBWaveCmd;

/* How the draw data maps ImGui coordinates to the framebuffer, and the
 * callback's clip rectangle already in framebuffer pixels. */
typedef struct CBWaveView {
    float off_x, off_y;         /* ImDrawData::DisplayPos */
    float scale_x, scale_y;     /* ImDrawData::FramebufferScale */
    float fb_width, fb_height;
    float clip[4];              /* x0, y0, x1, y1 */
} CBWaveView;

void cb_wave_record(const CBWaveCmd *c, VkCommandBuffer cmd, const CBWaveView *v);

/* -------------------------------------------------------- imgui_glue.cpp */

int  cb_imgui_init(CBWindow *w);
void cb_imgui_shutdown(CBWindow *w);
/* Feeds one SDL event to ImGui. */
void cb_imgui_process_event(CBWindow *w, const union SDL_Event *ev);
void cb_imgui_set_scale(CBWindow *w);
void cb_imgui_new_frame(CBWindow *w);
/* ImGui::Render(); returns the ImDrawData *, NULL when there is nothing. */
void *cb_imgui_render(CBWindow *w);
/* The draw data of the last rendered frame while it is still valid
 * (until the next new frame), else NULL. */
void *cb_imgui_last_draw_data(CBWindow *w);
/* Whether ImGui would draw the draw data at all: false when it has no
 * area (a minimised window), and then ImGui does not move on to its next
 * vertex buffers either. */
bool cb_imgui_drawable(const void *draw_data);
/* Records the draw data inside the current dynamic rendering pass. */
void cb_imgui_record(CBWindow *w, void *draw_data, VkCommandBuffer cmd);
uint64_t cb_imgui_add_texture(CBWindow *w, VkImageView view);
void cb_imgui_remove_texture(CBWindow *w, uint64_t id);
/* Adds the waveform callback, then ImGui's reset-render-state callback. */
void cb_imgui_add_wave(void *draw_list, const CBWaveCmd *c);

#endif /* CYANBOX_GPU_PRIV_H */
