/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* The Vulkan device: what a window's device must have (the instance, the
 * choice of device, the device, its queue and VMA are set up by vkdev.c,
 * as the compute context's and the model's are), buffers, the
 * frames-in-flight slots, deferred destruction and staged uploads. Taken
 * from libcraw's lib/vk_buffer.c, cut down to what an interface needs.
 *
 * Vulkan 1.3 is the floor: dynamic rendering (no render pass objects),
 * synchronization2 barriers and submits, and buffer device addresses (the
 * waveform shader reads its column buffer through a pointer in its push
 * constants).
 *
 * Destruction is deferred by submission number rather than by frame slot:
 * every submission gets the next serial, a resource released while the
 * frame that may use it is being built is tagged with that frame's serial,
 * and it is freed once a fence says the submission has finished. A fence
 * signal covers every earlier submission on the queue, so "completed" is
 * simply the highest serial whose fence has signalled.
 *
 * Deferring never needs memory: whatever will be deferred (a texture, a
 * waveform's buffer, a staging buffer, the capture buffer) takes its room
 * in the list when it is made, where a failure is an ordinary error. The
 * frame being recorded may still use what is released, so there is no
 * freeing it at once when the list cannot grow, even after waiting for
 * the device.
 *
 * Uploads are staged and recorded at the start of the next frame's command
 * buffer, not submitted on their own: creating a texture or a waveform
 * never waits for the GPU, and the copy lands in the same submission as
 * the first draw that needs it. */

#include "gpu_priv.h"

#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

/* ------------------------------------------------------------------ device */

/* Why a device cannot run the interface, or NULL if it can (CBVkDevCheck,
 * `opaque` the window); `qfam` gets a queue family that draws and
 * presents to the window's surface. */
static const char *device_unusable(void *opaque, VkPhysicalDevice pd,
                                   const VkPhysicalDeviceProperties *props, uint32_t *qfam)
{
    CBWindow *w = opaque;
    VkPhysicalDeviceVulkan13Features f13 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
    };
    VkPhysicalDeviceVulkan12Features f12 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
        .pNext = &f13,
    };
    VkPhysicalDeviceFeatures2 f2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext = &f12,
    };
    vkGetPhysicalDeviceFeatures2(pd, &f2);
    if (!f13.dynamicRendering)
        return "no dynamicRendering";
    if (!f13.synchronization2)
        return "no synchronization2";
    if (!f12.bufferDeviceAddress)
        return "no bufferDeviceAddress";

    if (!cb_vkdev_has_ext(pd, VK_KHR_SWAPCHAIN_EXTENSION_NAME))
        return "no VK_KHR_swapchain";

    uint32_t nb = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nb, NULL);
    VkQueueFamilyProperties *qf = calloc(CB_MAX(nb, 1), sizeof(*qf));
    if (!qf)
        return "out of memory";
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nb, qf);

    /* One queue that both draws and presents: everything here is recorded
     * and submitted on the main thread into that one queue. */
    int found = -1;
    for (uint32_t i = 0; i < nb && found < 0; i++) {
        VkBool32 present = VK_FALSE;
        if (!(qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) || !qf[i].queueCount)
            continue;
        if (vkGetPhysicalDeviceSurfaceSupportKHR(pd, i, w->surface, &present) == VK_SUCCESS &&
            present)
            found = (int)i;
    }
    free(qf);
    if (found < 0)
        return "no graphics queue that can present to the window";

    /* Mesa's hardware drivers answer the support query with yes on a
     * compositor without linux-dmabuf (a pixman wlroots, say) and only
     * fail when asked what the surface can do; the software rasterizer
     * presents there through wl_shm. So ask. */
    VkSurfaceCapabilitiesKHR caps;
    uint32_t nb_fmts = 0;
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, w->surface, &caps) != VK_SUCCESS ||
        vkGetPhysicalDeviceSurfaceFormatsKHR(pd, w->surface, &nb_fmts, NULL) != VK_SUCCESS ||
        !nb_fmts)
        return "cannot present on this compositor";

    *qfam = (uint32_t)found;
    return NULL;
}

static int create_device(CBWindow *w)
{
    VkPhysicalDeviceVulkan13Features f13 = {
        .sType            = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
        .synchronization2 = VK_TRUE,
        .dynamicRendering = VK_TRUE,
    };
    VkPhysicalDeviceVulkan12Features f12 = {
        .sType               = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
        .pNext               = &f13,
        .bufferDeviceAddress = VK_TRUE,
    };
    VkPhysicalDeviceFeatures2 f2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext = &f12,
    };

    const char *exts[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    return cb_vkdev_device(&w->vk, &f2, exts, CB_ARRAY_ELEMS(exts));
}

static int create_frames(CBWindow *w)
{
    /* Timestamps only measure (the demo's frame times): a queue without
     * them (vk.ts_mask 0) just goes without. */
    for (int i = 0; i < CB_FRAMES_IN_FLIGHT; i++) {
        CBFrame *f = &w->frames[i];

        /* The pool is reset as a whole each time the slot comes round. */
        VkCommandPoolCreateInfo pinfo = {
            .sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
            .queueFamilyIndex = w->vk.qfam,
        };
        VkResult res = vkCreateCommandPool(w->vk.dev, &pinfo, NULL, &f->pool);
        if (res != VK_SUCCESS)
            return cb_vk_ret2err(res);

        VkCommandBufferAllocateInfo cinfo = {
            .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool        = f->pool,
            .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1,
        };
        res = vkAllocateCommandBuffers(w->vk.dev, &cinfo, &f->cmd);
        if (res != VK_SUCCESS)
            return cb_vk_ret2err(res);

        /* Signalled, so the first wait on a slot that never ran returns. */
        VkFenceCreateInfo finfo = {
            .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
            .flags = VK_FENCE_CREATE_SIGNALED_BIT,
        };
        res = vkCreateFence(w->vk.dev, &finfo, NULL, &f->fence);
        if (res != VK_SUCCESS)
            return cb_vk_ret2err(res);

        VkSemaphoreCreateInfo sinfo = {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        };
        res = vkCreateSemaphore(w->vk.dev, &sinfo, NULL, &f->acquired);
        if (res != VK_SUCCESS)
            return cb_vk_ret2err(res);

        if (w->vk.ts_mask) {
            VkQueryPoolCreateInfo qinfo = {
                .sType      = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                .queryType  = VK_QUERY_TYPE_TIMESTAMP,
                .queryCount = 2,
            };
            res = vkCreateQueryPool(w->vk.dev, &qinfo, NULL, &f->times);
            if (res != VK_SUCCESS)
                return cb_vk_ret2err(res);
        }
    }

    return 0;
}

int cb_vk_create(CBWindow *w)
{
    cb_array_init(&w->garbage, sizeof(CBDeferred));
    cb_array_init(&w->uploads, sizeof(CBUpload));
    cb_array_init(&w->textures, sizeof(CBTexture));

    /* SDL's instance extensions: surface and wayland_surface. */
    Uint32 nb_sdl = 0;
    const char *const *sdl_exts = SDL_Vulkan_GetInstanceExtensions(&nb_sdl);
    if (!sdl_exts) {
        cb_err("gpu: SDL_Vulkan_GetInstanceExtensions: %s", SDL_GetError());
        return CB_ERROR(ENODEV);
    }
    CBVkDevOptions vo = {
        .tag         = "gpu",
        .validation  = w->opts.validation,
        .performance = true,
        .exts        = sdl_exts,
        .nb_exts     = nb_sdl,
        .device      = w->device,
        .cpu         = true,
        .check       = device_unusable,
        .opaque      = w,
    };
    int ret = cb_vkdev_instance(&w->vk, &vo);
    if (ret < 0) {
        cb_err("gpu: unable to create a Vulkan 1.3 instance: %s", strerror(-ret));
        return ret;
    }

    if (!SDL_Vulkan_CreateSurface(w->win, w->vk.inst, NULL, &w->surface)) {
        cb_err("gpu: SDL_Vulkan_CreateSurface: %s", SDL_GetError());
        return CB_ERROR(ENODEV);
    }

    ret = cb_vkdev_pick(&w->vk, &vo);
    if (ret < 0) {
        cb_err("gpu: no Vulkan 1.3 device can draw to this window");
        return ret;
    }

    ret = create_device(w);
    if (ret < 0)
        return ret;

    ret = create_frames(w);
    if (ret < 0) {
        cb_err("gpu: unable to create the frame resources");
        return ret;
    }

    cb_info("gpu: %s, Vulkan %u.%u, queue family %u", w->vk.props.deviceName,
            VK_API_VERSION_MAJOR(w->vk.props.apiVersion),
            VK_API_VERSION_MINOR(w->vk.props.apiVersion), w->vk.qfam);

    return 0;
}

const char *cb_window_gpu_name(const CBWindow *w)
{
    return w->vk.props.deviceName;
}

static void free_upload(CBWindow *w, CBUpload *up)
{
    cb_vk_buffer_free(w, &up->staging);
}

void cb_vk_destroy(CBWindow *w)
{
    if (w->vk.dev) {
        vkDeviceWaitIdle(w->vk.dev);

        cb_vk_collect(w, true);
        for (size_t i = 0; i < w->uploads.nb; i++)
            free_upload(w, cb_array_at(&w->uploads, i));
        cb_array_clear(&w->uploads);

        for (int i = 0; i < CB_FRAMES_IN_FLIGHT; i++) {
            CBFrame *f = &w->frames[i];
            if (f->times)
                vkDestroyQueryPool(w->vk.dev, f->times, NULL);
            if (f->acquired)
                vkDestroySemaphore(w->vk.dev, f->acquired, NULL);
            if (f->fence)
                vkDestroyFence(w->vk.dev, f->fence, NULL);
            if (f->pool)
                vkDestroyCommandPool(w->vk.dev, f->pool, NULL);
            *f = (CBFrame) { 0 };
        }

    }

    /* SDL made the surface, so SDL unmakes it: it knows which loader entry
     * point it used. After the swapchain (window.c), before the instance,
     * while the window still exists. */
    if (w->surface && w->vk.inst)
        SDL_Vulkan_DestroySurface(w->vk.inst, w->surface, NULL);
    w->surface = VK_NULL_HANDLE;
    cb_vkdev_destroy(&w->vk);

    cb_array_free(&w->garbage);
    cb_array_free(&w->uploads);
    cb_array_free(&w->textures);
}

/* ----------------------------------------------------------------- buffers */

int cb_vk_buffer_create(CBWindow *w, CBVkBuffer *b, VkDeviceSize size,
                        VkBufferUsageFlags usage, enum CBMemUsage mem)
{
    *b = (CBVkBuffer) { 0 };
    if (!size)
        return CB_ERROR(EINVAL);

    VkBufferCreateInfo info = {
        .sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size        = size,
        .usage       = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };

    /* RANDOM also asks VMA for HOST_CACHED, which is what makes reading a
     * frame back on the CPU fast. */
    VmaAllocationCreateInfo ci = { 0 };
    switch (mem) {
    case CB_MEM_UPLOAD:
        ci.usage = VMA_MEMORY_USAGE_AUTO;
        ci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                   VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
        break;
    case CB_MEM_READBACK:
        ci.usage = VMA_MEMORY_USAGE_AUTO;
        ci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                   VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
        break;
    case CB_MEM_DEVICE:
    default:
        ci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        break;
    }

    VmaAllocationInfo ai = { 0 };
    VkResult res = vmaCreateBuffer(w->vk.vma, &info, &ci, &b->buf, &b->alloc, &ai);
    if (res != VK_SUCCESS) {
        cb_err("gpu: unable to allocate a %llu byte buffer: %s", (unsigned long long)size,
               cb_vk_strerror(res));
        *b = (CBVkBuffer) { 0 };
        return cb_vk_ret2err(res);
    }

    b->size = size;
    b->map  = ai.pMappedData;

    if (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) {
        VkBufferDeviceAddressInfo ainfo = {
            .sType  = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
            .buffer = b->buf,
        };
        b->addr = vkGetBufferDeviceAddress(w->vk.dev, &ainfo);
    }

    return 0;
}

void cb_vk_buffer_free(CBWindow *w, CBVkBuffer *b)
{
    if (!b || !b->buf)
        return;
    vmaDestroyBuffer(w->vk.vma, b->buf, b->alloc);
    *b = (CBVkBuffer) { 0 };
}

int cb_vk_buffer_invalidate(CBWindow *w, CBVkBuffer *b)
{
    if (!b->map)
        return 0;
    return cb_vk_ret2err(vmaInvalidateAllocation(w->vk.vma, b->alloc, 0, VK_WHOLE_SIZE));
}

/* ------------------------------------------------------ deferred destruction */

static void free_deferred(CBWindow *w, const CBDeferred *d)
{
    if (d->tex_id)
        cb_imgui_remove_texture(w, d->tex_id);
    if (d->view)
        vkDestroyImageView(w->vk.dev, d->view, NULL);
    if (d->img)
        vmaDestroyImage(w->vk.vma, d->img, d->alloc);
    else if (d->buf)
        vmaDestroyBuffer(w->vk.vma, d->buf, d->alloc);
    else if (d->alloc)
        vmaFreeMemory(w->vk.vma, d->alloc);
}

int cb_vk_defer_reserve(CBWindow *w, size_t n)
{
    size_t need = w->garbage.nb + w->defer_room + n;
    if (need > w->garbage.cap &&
        cb_array_reserve(&w->garbage, CB_MAX(need, w->garbage.cap * 2)) < 0)
        return CB_ERROR(ENOMEM);
    w->defer_room += n;
    return 0;
}

void cb_vk_defer_unreserve(CBWindow *w, size_t n)
{
    w->defer_room -= CB_MIN(n, w->defer_room);
}

static void defer_at(CBWindow *w, const CBDeferred *d, uint64_t serial)
{
    /* The room taken when it was made: the push below does not allocate. */
    if (w->defer_room)
        w->defer_room--;
    CBDeferred *item = cb_array_push(&w->garbage, d);
    if (!item) {
        /* Deferred without room taken for it, and no memory to grow:
         * everything submitted is done once the device is idle, and its
         * garbage makes room. */
        vkDeviceWaitIdle(w->vk.dev);
        w->completed = w->serial;
        cb_vk_collect(w, false);
        item = cb_array_push(&w->garbage, d);
    }
    if (!item) {
        /* The frame being built may still use it: leaked, not freed. */
        cb_err("gpu: out of memory to defer the destruction of a resource; it is leaked");
        return;
    }
    item->serial = serial;
}

void cb_vk_defer(CBWindow *w, const CBDeferred *d)
{
    defer_at(w, d, w->serial + 1);
}

void cb_vk_defer_hold(CBWindow *w, uint64_t from, uint64_t until)
{
    for (size_t i = 0; i < w->garbage.nb; i++) {
        CBDeferred *d = cb_array_at(&w->garbage, i);
        if (d->serial >= from && d->serial < until)
            d->serial = until;
    }
}

void cb_vk_poll_completed(CBWindow *w)
{
    for (int i = 0; i < CB_FRAMES_IN_FLIGHT; i++) {
        CBFrame *f = &w->frames[i];
        if (f->serial > w->completed && vkGetFenceStatus(w->vk.dev, f->fence) == VK_SUCCESS)
            w->completed = f->serial;
    }
}

void cb_vk_collect(CBWindow *w, bool all)
{
    size_t kept = 0;

    for (size_t i = 0; i < w->garbage.nb; i++) {
        CBDeferred *d = cb_array_at(&w->garbage, i);
        if (all || d->serial <= w->completed) {
            free_deferred(w, d);
            continue;
        }
        if (kept != i)
            memcpy(cb_array_at(&w->garbage, kept), d, sizeof(*d));
        kept++;
    }

    w->garbage.nb = kept;
}

/* ----------------------------------------------------------------- uploads */

int cb_vk_upload(CBWindow *w, const CBUpload *up)
{
    if (!cb_array_push(&w->uploads, up))
        return CB_ERROR(ENOMEM);
    return 0;
}

void cb_vk_record_uploads(CBWindow *w, VkCommandBuffer cmd, uint64_t serial)
{
    if (!w->uploads.nb)
        return;

    for (size_t i = 0; i < w->uploads.nb; i++) {
        CBUpload *up = cb_array_at(&w->uploads, i);

        /* The staging memory was written and flushed on the host before
         * this submission; vkQueueSubmit makes host writes visible to the
         * device, so the copy needs no barrier on the source. */
        if (up->dst_img) {
            const VkImageSubresourceRange range = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .levelCount = 1,
                .layerCount = 1,
            };

            /* UNDEFINED: the image is new and its contents are replaced. */
            VkImageMemoryBarrier2 pre = {
                .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                .srcStageMask        = VK_PIPELINE_STAGE_2_NONE,
                .srcAccessMask       = VK_ACCESS_2_NONE,
                .dstStageMask        = VK_PIPELINE_STAGE_2_COPY_BIT,
                .dstAccessMask       = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                .oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED,
                .newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image               = up->dst_img,
                .subresourceRange    = range,
            };
            VkDependencyInfo dep = {
                .sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                .imageMemoryBarrierCount = 1,
                .pImageMemoryBarriers    = &pre,
            };
            vkCmdPipelineBarrier2(cmd, &dep);

            VkBufferImageCopy region = {
                .imageSubresource = {
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .layerCount = 1,
                },
                .imageExtent = { up->width, up->height, 1 },
            };
            vkCmdCopyBufferToImage(cmd, up->staging.buf, up->dst_img,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

            /* Into the layout ImGui samples it in, visible to the fragment
             * shader of the draws later in this command buffer. */
            VkImageMemoryBarrier2 post = pre;
            post.srcStageMask  = VK_PIPELINE_STAGE_2_COPY_BIT;
            post.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            post.dstStageMask  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
            post.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
            post.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            post.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            dep.pImageMemoryBarriers = &post;
            vkCmdPipelineBarrier2(cmd, &dep);
        } else if (up->dst_buf) {
            VkBufferCopy region = { .size = up->staging.size };
            vkCmdCopyBuffer(cmd, up->staging.buf, up->dst_buf, 1, &region);

            /* The waveform shader reads the columns through a buffer
             * device address, which is a storage read. */
            VkBufferMemoryBarrier2 post = {
                .sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
                .srcStageMask        = VK_PIPELINE_STAGE_2_COPY_BIT,
                .srcAccessMask       = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                .dstStageMask        = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                .dstAccessMask       = VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .buffer              = up->dst_buf,
                .offset              = 0,
                .size                = VK_WHOLE_SIZE,
            };
            VkDependencyInfo dep = {
                .sType                    = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                .bufferMemoryBarrierCount = 1,
                .pBufferMemoryBarriers    = &post,
            };
            vkCmdPipelineBarrier2(cmd, &dep);
        }

        /* The staging buffer goes once this submission is done with it. */
        CBDeferred d = {
            .buf   = up->staging.buf,
            .alloc = up->staging.alloc,
        };
        defer_at(w, &d, serial);
    }

    cb_array_clear(&w->uploads);
}
