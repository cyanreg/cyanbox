/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* Private to src/gpu: the window's state and the calls the files of the
 * module make into each other. window.c owns SDL, the Wayland frame
 * callback and the public entry points; vk.c the instance, the device,
 * VMA, buffers, deferred destruction and staged uploads; swapchain.c the
 * swapchain, the frames in flight, recording, presenting and the
 * screenshot; textures.c and wave.c the two kinds of user resources;
 * imgui_glue.cpp everything that has to be C++.
 *
 * C++ includes this inside extern "C" { } after <vulkan/vulkan.h> and
 * vk_mem_alloc.h, which it must include itself first: neither header may
 * be opened inside a C linkage block. SDL and Wayland types are only
 * forward-declared here for the same reason. */

#ifndef RXBOX_GPU_PRIV_H
#define RXBOX_GPU_PRIV_H

#include "rxbox/gpu.h"

#include <vulkan/vulkan.h>

/* VMA tests extension macros that only exist on Windows with #if. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wundef"
#include "vk_mem_alloc.h"
#pragma GCC diagnostic pop

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
#define RX_FRAMES_IN_FLIGHT 2

/* Background the frame is cleared to before ImGui draws (the UI paints its
 * own windows over it). */
#define RX_CLEAR_R (0x20 / 255.0f)
#define RX_CLEAR_G (0x20 / 255.0f)
#define RX_CLEAR_B (0x20 / 255.0f)

enum RxMemUsage {
    RX_MEM_DEVICE = 0,  /* device local, never mapped */
    RX_MEM_UPLOAD,      /* host visible, mapped, written sequentially */
    RX_MEM_READBACK,    /* host visible and cached, mapped, read by the CPU */
};

typedef struct RxVkBuffer {
    VkBuffer        buf;
    VmaAllocation   alloc;
    VkDeviceSize    size;
    VkDeviceAddress addr;   /* 0 unless created with SHADER_DEVICE_ADDRESS usage */
    void           *map;    /* NULL for RX_MEM_DEVICE */
} RxVkBuffer;

/* Something the GPU may still be reading: freed once the submission
 * numbered `serial` has completed. Any field may be null. */
typedef struct RxDeferred {
    uint64_t      serial;
    VkBuffer      buf;
    VkImage       img;
    VkImageView   view;
    VmaAllocation alloc;
    uint64_t      tex_id;   /* an ImGui texture descriptor set */
} RxDeferred;

/* A copy from a staging buffer into a buffer or an image, recorded at the
 * start of the next frame's command buffer (vk.c). */
typedef struct RxUpload {
    RxVkBuffer staging;
    VkBuffer   dst_buf;
    VkImage    dst_img;
    uint32_t   width, height;
} RxUpload;

/* A texture handed out as an ImTextureID. */
typedef struct RxTexture {
    uint64_t      id;
    VkImage       img;
    VkImageView   view;
    VmaAllocation alloc;
} RxTexture;

/* One frame in flight. The fence says the slot's command buffer and its
 * acquire semaphore are free again. */
typedef struct RxFrame {
    VkCommandPool   pool;
    VkCommandBuffer cmd;
    VkFence         fence;
    VkSemaphore     acquired;
    uint64_t        serial;     /* the submission the slot last carried, 0 = none */
} RxFrame;

struct RxWindow {
    RxWindowOptions opts;
    char *title, *font, *device;    /* the options' strings, owned */

    /* SDL, and SDL's Wayland objects (borrowed, never destroyed here). */
    struct SDL_Window *win;
    bool sdl_init;
    struct wl_display *wl_dpy;
    struct wl_surface *wl_surf;

    /* The frame callback (window.c). A private queue and a proxy wrapper of
     * SDL's surface on it, so the callbacks are dispatched only by
     * rx_window_poll() and SDL's own dispatch never sees them. */
    struct wl_event_queue *wl_queue;
    struct wl_surface *wl_wrap;
    RxArray frame_cbs;              /* struct wl_callback *, every outstanding one */
    struct wl_callback *frame_cb;   /* the one asked for with the last present */
    bool     frame_done;            /* ... and it has arrived */
    uint32_t cb_time;               /* compositor time of the last callback, ms */
    int64_t  cb_wall;               /* rx_time_us() when it was dispatched */
    double   interval_ms;
    int64_t  req_wall;              /* rx_time_us() when the last one was asked for */

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
    VkInstance inst;
    VkDebugUtilsMessengerEXT messenger;
    int validation_errors;
    VkSurfaceKHR surface;
    VkPhysicalDevice phys;
    VkPhysicalDeviceProperties props;
    VkDevice dev;
    uint32_t qfam;
    VkQueue queue;
    VmaAllocator vma;

    RxFrame  frames[RX_FRAMES_IN_FLIGHT];
    uint32_t frame_idx;
    uint64_t serial;                /* submissions made; the next one is serial + 1 */
    uint64_t completed;             /* every submission up to this one has finished */
    int frames_drawn;

    RxArray garbage;                /* RxDeferred */
    RxArray uploads;                /* RxUpload */
    RxArray textures;               /* RxTexture */

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
    RxVkBuffer cap;
    uint32_t cap_width, cap_height;
    uint64_t cap_serial;
    uint64_t last_present_serial;
    bool capture_next;
    char *shot_path;                /* rx_window_screenshot_next: where the next frame goes */

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
void rx_window_want_frame_cb(RxWindow *w);
/* The present did not happen: nothing will answer the request. */
void rx_window_frame_skipped(RxWindow *w);

/* -------------------------------------------------------------------- vk.c */

int  rx_vk_create(RxWindow *w);
void rx_vk_destroy(RxWindow *w);
int  rx_vk_ret2err(VkResult res);
const char *rx_vk_strerror(VkResult res);

int  rx_vk_buffer_create(RxWindow *w, RxVkBuffer *b, VkDeviceSize size,
                         VkBufferUsageFlags usage, enum RxMemUsage mem);
void rx_vk_buffer_free(RxWindow *w, RxVkBuffer *b);
int  rx_vk_buffer_invalidate(RxWindow *w, RxVkBuffer *b);

/* Hands something to the deferred destruction list, to be freed after the
 * next submission (the one that may still use it) has completed. */
void rx_vk_defer(RxWindow *w, const RxDeferred *d);
/* Frees what the GPU is done with; everything when `all` (device idle). */
void rx_vk_collect(RxWindow *w, bool all);
/* Learns which submissions have finished without waiting. */
void rx_vk_poll_completed(RxWindow *w);

/* Queues a staged copy; takes ownership of up->staging. */
int  rx_vk_upload(RxWindow *w, const RxUpload *up);
/* Records every queued copy into `cmd` (the frame about to be submitted
 * as `serial`), with the barriers that make the results visible to the
 * fragment shader, and defers the staging buffers. */
void rx_vk_record_uploads(RxWindow *w, VkCommandBuffer cmd, uint64_t serial);

/* ------------------------------------------------------------- swapchain.c */

int  rx_swapchain_create(RxWindow *w);
void rx_swapchain_destroy(RxWindow *w);
/* Records the draw data (an ImDrawData *), submits and presents. With
 * `capture`, the image is also copied into w->cap before it is presented.
 * Returns 0 when presented, 1 when the frame was skipped (the swapchain is
 * out of date or the window has no area), <0 on error. */
int  rx_swapchain_frame(RxWindow *w, void *draw_data, bool capture);
/* Reads the last captured frame back as tightly packed RGBA. */
int  rx_swapchain_readback(RxWindow *w, uint8_t **rgba, int *width, int *height);
/* Writes the last presented frame as PNG; -EAGAIN when it was not captured. */
int  rx_swapchain_save_png(RxWindow *w, const char *png_path);

/* ------------------------------------------------------------- textures.c */

void rx_textures_destroy_all(RxWindow *w);

/* ----------------------------------------------------------------- wave.c */

int  rx_wave_pipeline_create(RxWindow *w);
void rx_wave_pipeline_destroy(RxWindow *w);

/* What a waveform callback carries in the draw list. */
typedef struct RxWaveCmd {
    RxWindow       *w;
    VkDeviceAddress addr;
    int32_t         nb_columns;
    int32_t         kind;
    RxWaveDraw      d;
} RxWaveCmd;

/* How the draw data maps ImGui coordinates to the framebuffer, and the
 * callback's clip rectangle already in framebuffer pixels. */
typedef struct RxWaveView {
    float off_x, off_y;         /* ImDrawData::DisplayPos */
    float scale_x, scale_y;     /* ImDrawData::FramebufferScale */
    float fb_width, fb_height;
    float clip[4];              /* x0, y0, x1, y1 */
} RxWaveView;

void rx_wave_record(const RxWaveCmd *c, VkCommandBuffer cmd, const RxWaveView *v);

/* -------------------------------------------------------- imgui_glue.cpp */

int  rx_imgui_init(RxWindow *w);
void rx_imgui_shutdown(RxWindow *w);
/* Feeds one SDL event to ImGui. */
void rx_imgui_process_event(RxWindow *w, const union SDL_Event *ev);
void rx_imgui_set_scale(RxWindow *w);
void rx_imgui_new_frame(RxWindow *w);
/* ImGui::Render(); returns the ImDrawData *, NULL when there is nothing. */
void *rx_imgui_render(RxWindow *w);
/* The draw data of the last rendered frame while it is still valid
 * (until the next new frame), else NULL. */
void *rx_imgui_last_draw_data(RxWindow *w);
/* Records the draw data inside the current dynamic rendering pass. */
void rx_imgui_record(RxWindow *w, void *draw_data, VkCommandBuffer cmd);
uint64_t rx_imgui_add_texture(RxWindow *w, VkImageView view);
void rx_imgui_remove_texture(RxWindow *w, uint64_t id);
/* Adds the waveform callback, then ImGui's reset-render-state callback. */
void rx_imgui_add_wave(void *draw_list, const RxWaveCmd *c);

#endif /* RXBOX_GPU_PRIV_H */
