/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* The swapchain and the one path that draws a frame: wait for the frame
 * slot, acquire, record (staged uploads, the layout transition, a dynamic
 * rendering pass with ImGui's draw data in it), submit, ask for the frame
 * callback, present.
 *
 * Synchronisation, per frame:
 *   - The slot's fence covers its command buffer, its acquire semaphore
 *     and (since ImGui rotates over the same count) ImGui's vertex
 *     buffers for that slot. It is waited on before any of them is reused,
 *     and reset only once an image has been acquired, so a frame that
 *     bails out before submitting never leaves an unsignalled fence behind.
 *   - The acquire semaphore is waited on at COLOR_ATTACHMENT_OUTPUT; the
 *     UNDEFINED -> COLOR_ATTACHMENT_OPTIMAL barrier starts at that same
 *     stage, so it chains after the wait and the transition cannot happen
 *     while the presentation engine still reads the image.
 *   - The image leaves the pass for PRESENT_SRC with destination stage
 *     NONE: the semaphore signalled at the end of the submission is what
 *     makes the writes visible to the presentation engine.
 *   - The render-finished semaphore is one per swapchain image, not per
 *     slot. A present has no fence, so the only point at which its wait is
 *     known to have happened is when the same image is acquired again.
 *   - A recreation waits for the device to go idle before the old images,
 *     views and semaphores are destroyed.
 *
 * The format is 8-bit UNORM (BGRA or RGBA) in the sRGB colour space: ImGui
 * and the waveform colours are sRGB-encoded values and are written as
 * they are, which an _SRGB format would instead treat as linear. The
 * window is opaque (OPAQUE composite alpha when offered).
 *
 * MAILBOX when the device offers it, FIFO otherwise. Pacing does not come
 * from the present mode but from the Wayland frame callback (window.c),
 * which is asked for right before vkQueuePresentKHR so that the commit the
 * WSI makes for this present carries it. With MAILBOX the present never
 * blocks; FIFO on a compositor without fifo-v1 blocks inside the driver
 * until the previous frame was shown, which the callback pacing makes
 * rare (it was shown: that is when the callback came). */

#include "gpu_priv.h"

#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>

/* --------------------------------------------------------------- swapchain */

static void destroy_images(RXWindow *w)
{
    for (uint32_t i = 0; i < w->nb_images; i++) {
        if (w->views && w->views[i])
            vkDestroyImageView(w->dev, w->views[i], NULL);
        if (w->rendered && w->rendered[i])
            vkDestroySemaphore(w->dev, w->rendered[i], NULL);
    }
    free(w->images);
    free(w->views);
    free(w->rendered);
    w->images = NULL;
    w->views = NULL;
    w->rendered = NULL;
    w->nb_images = 0;
}

static int pick_format(RXWindow *w, VkSurfaceFormatKHR *out)
{
    uint32_t nb = 0;
    VkResult res = vkGetPhysicalDeviceSurfaceFormatsKHR(w->phys, w->surface, &nb, NULL);
    if (res != VK_SUCCESS || !nb)
        return res != VK_SUCCESS ? rx_vk_ret2err(res) : RX_ERROR(ENODEV);

    VkSurfaceFormatKHR *fmts = calloc(nb, sizeof(*fmts));
    if (!fmts)
        return RX_ERROR(ENOMEM);
    vkGetPhysicalDeviceSurfaceFormatsKHR(w->phys, w->surface, &nb, fmts);

    static const VkFormat prefer[] = {
        VK_FORMAT_B8G8R8A8_UNORM,
        VK_FORMAT_R8G8B8A8_UNORM,
    };

    /* Once chosen the format never changes: ImGui's pipeline and the
     * waveform pipeline are built for it. */
    bool found = false;
    if (w->format.format != VK_FORMAT_UNDEFINED) {
        for (uint32_t i = 0; i < nb && !found; i++) {
            if (fmts[i].format == w->format.format &&
                fmts[i].colorSpace == w->format.colorSpace) {
                *out = fmts[i];
                found = true;
            }
        }
        free(fmts);
        if (!found)
            rx_err("gpu: the surface no longer offers the swapchain format");
        return found ? 0 : RX_ERROR(ENODEV);
    }

    for (size_t p = 0; p < RX_ARRAY_ELEMS(prefer) && !found; p++) {
        for (uint32_t i = 0; i < nb && !found; i++) {
            if (fmts[i].format == prefer[p] &&
                fmts[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                *out = fmts[i];
                found = true;
            }
        }
    }
    if (!found) {
        *out = fmts[0];
        rx_warn("gpu: no 8-bit UNORM swapchain format, using format %d; colours may be off",
                (int)fmts[0].format);
    }

    free(fmts);
    return 0;
}

static VkPresentModeKHR pick_present_mode(RXWindow *w)
{
    uint32_t nb = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(w->phys, w->surface, &nb, NULL);
    VkPresentModeKHR *modes = calloc(RX_MAX(nb, 1), sizeof(*modes));
    if (!modes)
        return VK_PRESENT_MODE_FIFO_KHR;
    vkGetPhysicalDeviceSurfacePresentModesKHR(w->phys, w->surface, &nb, modes);

    bool mailbox = false;
    for (uint32_t i = 0; i < nb; i++)
        mailbox |= modes[i] == VK_PRESENT_MODE_MAILBOX_KHR;

    free(modes);
    return mailbox ? VK_PRESENT_MODE_MAILBOX_KHR : VK_PRESENT_MODE_FIFO_KHR;
}

/* Returns 1 when the window has no area (nothing to create yet). */
int rx_swapchain_create(RXWindow *w)
{
    VkSurfaceCapabilitiesKHR caps;
    VkResult res = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(w->phys, w->surface, &caps);
    if (res != VK_SUCCESS) {
        rx_err("gpu: surface capabilities: %s", rx_vk_strerror(res));
        return rx_vk_ret2err(res);
    }

    /* Wayland leaves the size to the client (0xFFFFFFFF): the window's
     * pixel size, which is what SDL's viewport or buffer scale expects. */
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX) {
        int pw = 0, ph = 0;
        SDL_GetWindowSizeInPixels(w->win, &pw, &ph);
        extent.width  = RX_CLAMP((uint32_t)RX_MAX(pw, 0), caps.minImageExtent.width,
                                 caps.maxImageExtent.width);
        extent.height = RX_CLAMP((uint32_t)RX_MAX(ph, 0), caps.minImageExtent.height,
                                 caps.maxImageExtent.height);
    }

    /* The format is settled even when there is nothing to create yet:
     * the pipelines are built for it at startup. */
    VkSurfaceFormatKHR format;
    int ret = pick_format(w, &format);
    if (ret < 0)
        return ret;
    if (w->format.format == VK_FORMAT_UNDEFINED)
        w->format = format;

    if (!extent.width || !extent.height) {
        w->swap_dirty = true;
        return 1;
    }
    VkPresentModeKHR mode = pick_present_mode(w);

    uint32_t nb_images = RX_MAX(caps.minImageCount, 3u);
    if (caps.maxImageCount)
        nb_images = RX_MIN(nb_images, caps.maxImageCount);

    VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    w->can_capture = caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (w->can_capture)
        usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & alpha)) {
        alpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
        if (!(caps.supportedCompositeAlpha & alpha))
            alpha = (VkCompositeAlphaFlagBitsKHR)(caps.supportedCompositeAlpha &
                                                  -caps.supportedCompositeAlpha);
    }

    VkSwapchainKHR old = w->swapchain;
    VkSwapchainCreateInfoKHR info = {
        .sType            = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface          = w->surface,
        .minImageCount    = nb_images,
        .imageFormat      = format.format,
        .imageColorSpace  = format.colorSpace,
        .imageExtent      = extent,
        .imageArrayLayers = 1,
        .imageUsage       = usage,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform     = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)
                            ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR : caps.currentTransform,
        .compositeAlpha   = alpha,
        .presentMode      = mode,
        .clipped          = VK_TRUE,
        .oldSwapchain     = old,
    };
    VkSwapchainKHR sc = VK_NULL_HANDLE;
    res = vkCreateSwapchainKHR(w->dev, &info, NULL, &sc);

    /* Whatever happened, the old swapchain is retired now. Its images may
     * still be read by frames in flight and its semaphores waited on by
     * presents, so everything of it goes only once the device is idle. */
    if (old) {
        vkDeviceWaitIdle(w->dev);
        rx_vk_poll_completed(w);
        destroy_images(w);
        vkDestroySwapchainKHR(w->dev, old, NULL);
        w->swapchain = VK_NULL_HANDLE;
    }
    if (res != VK_SUCCESS) {
        rx_err("gpu: unable to create the swapchain: %s", rx_vk_strerror(res));
        return rx_vk_ret2err(res);
    }
    w->swapchain = sc;

    uint32_t nb = 0;
    vkGetSwapchainImagesKHR(w->dev, sc, &nb, NULL);
    w->images   = calloc(nb, sizeof(*w->images));
    w->views    = calloc(nb, sizeof(*w->views));
    w->rendered = calloc(nb, sizeof(*w->rendered));
    if (!w->images || !w->views || !w->rendered) {
        destroy_images(w);
        return RX_ERROR(ENOMEM);
    }
    vkGetSwapchainImagesKHR(w->dev, sc, &nb, w->images);
    w->nb_images = nb;

    for (uint32_t i = 0; i < nb; i++) {
        VkImageViewCreateInfo vinfo = {
            .sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image    = w->images[i],
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format   = format.format,
            .subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .levelCount = 1,
                .layerCount = 1,
            },
        };
        res = vkCreateImageView(w->dev, &vinfo, NULL, &w->views[i]);
        if (res != VK_SUCCESS)
            return rx_vk_ret2err(res);

        VkSemaphoreCreateInfo sinfo = {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        };
        res = vkCreateSemaphore(w->dev, &sinfo, NULL, &w->rendered[i]);
        if (res != VK_SUCCESS)
            return rx_vk_ret2err(res);
    }

    /* w->extent is only ever set here, so zero means the first swapchain. */
    if (!w->extent.width)
        rx_info("gpu: presenting %s, format %d", mode == VK_PRESENT_MODE_MAILBOX_KHR ?
                "MAILBOX" : "FIFO", (int)format.format);
    rx_debug("gpu: swapchain %ux%u, %u images", extent.width, extent.height, nb);

    w->format = format;
    w->present_mode = mode;
    w->extent = extent;
    w->swap_dirty = false;

    return 0;
}

void rx_swapchain_destroy(RXWindow *w)
{
    if (!w->dev)
        return;

    vkDeviceWaitIdle(w->dev);
    destroy_images(w);
    if (w->swapchain)
        vkDestroySwapchainKHR(w->dev, w->swapchain, NULL);
    w->swapchain = VK_NULL_HANDLE;
    rx_vk_buffer_free(w, &w->cap);
}

/* ------------------------------------------------------------------- frame */

static void image_barrier(VkCommandBuffer cmd, VkImage img,
                          VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
                          VkImageLayout old_layout,
                          VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access,
                          VkImageLayout new_layout)
{
    VkImageMemoryBarrier2 bar = {
        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask        = src_stage,
        .srcAccessMask       = src_access,
        .dstStageMask        = dst_stage,
        .dstAccessMask       = dst_access,
        .oldLayout           = old_layout,
        .newLayout           = new_layout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image               = img,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .levelCount = 1,
            .layerCount = 1,
        },
    };
    VkDependencyInfo dep = {
        .sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers    = &bar,
    };
    vkCmdPipelineBarrier2(cmd, &dep);
}

/* The readback buffer for a capture, sized for the current extent. */
static int ensure_capture(RXWindow *w)
{
    if (w->cap.buf && w->cap_width == w->extent.width && w->cap_height == w->extent.height)
        return 0;

    if (w->cap.buf) {
        RXDeferred d = { .buf = w->cap.buf, .alloc = w->cap.alloc };
        rx_vk_defer(w, &d);
        w->cap = (RXVkBuffer) { 0 };
    }

    VkDeviceSize size = (VkDeviceSize)w->extent.width * w->extent.height * 4;
    int ret = rx_vk_buffer_create(w, &w->cap, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                  RX_MEM_READBACK);
    if (ret < 0)
        return ret;

    w->cap_width = w->extent.width;
    w->cap_height = w->extent.height;
    w->cap_serial = 0;
    return 0;
}

/* Records the copy of the rendered image into the readback buffer. The
 * image comes from the pass in COLOR_ATTACHMENT_OPTIMAL and leaves in
 * PRESENT_SRC. */
static void record_capture(RXWindow *w, VkCommandBuffer cmd, VkImage img)
{
    image_barrier(cmd, img,
                  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                  VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                  VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

    VkBufferImageCopy region = {
        .imageSubresource = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .layerCount = 1,
        },
        .imageExtent = { w->extent.width, w->extent.height, 1 },
    };
    vkCmdCopyImageToBuffer(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, w->cap.buf, 1,
                           &region);

    /* The copy's writes made available to the host, which reads the buffer
     * after waiting for this submission's fence. */
    VkBufferMemoryBarrier2 bbar = {
        .sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
        .srcStageMask        = VK_PIPELINE_STAGE_2_COPY_BIT,
        .srcAccessMask       = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .dstStageMask        = VK_PIPELINE_STAGE_2_HOST_BIT,
        .dstAccessMask       = VK_ACCESS_2_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer              = w->cap.buf,
        .size                = VK_WHOLE_SIZE,
    };
    VkDependencyInfo dep = {
        .sType                    = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .bufferMemoryBarrierCount = 1,
        .pBufferMemoryBarriers    = &bbar,
    };
    vkCmdPipelineBarrier2(cmd, &dep);

    /* A read needs no availability, only the execution dependency that
     * keeps the transition after the copy. */
    image_barrier(cmd, img,
                  VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_NONE,
                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                  VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
}

static VkResult acquire(RXWindow *w, RXFrame *f, uint32_t *idx)
{
    /* A finite timeout: a compositor holding every buffer must not hang
     * the loop. The semaphore is left unsignalled on a timeout. */
    return vkAcquireNextImageKHR(w->dev, w->swapchain, 1000000000ull, f->acquired,
                                 VK_NULL_HANDLE, idx);
}

int rx_swapchain_frame(RXWindow *w, void *draw_data, bool capture)
{
    int pw = 0, ph = 0;
    SDL_GetWindowSizeInPixels(w->win, &pw, &ph);
    if (pw <= 0 || ph <= 0)
        return 1;

    int ret;
    if (!w->swapchain || w->swap_dirty || (uint32_t)pw != w->extent.width ||
        (uint32_t)ph != w->extent.height) {
        ret = rx_swapchain_create(w);
        if (ret != 0)
            return ret;
    }

    RXFrame *f = &w->frames[w->frame_idx];

    /* The slot's last submission must be done before its command buffer,
     * its acquire semaphore and ImGui's vertex buffers for it are reused. */
    VkResult res = vkWaitForFences(w->dev, 1, &f->fence, VK_TRUE, UINT64_MAX);
    if (res != VK_SUCCESS) {
        rx_err("gpu: waiting for a frame: %s", rx_vk_strerror(res));
        return rx_vk_ret2err(res);
    }
    if (f->serial > w->completed)
        w->completed = f->serial;
    rx_vk_poll_completed(w);
    rx_vk_collect(w, false);

    /* The slot's last frame is done, so its timestamps are there. */
    if (f->timed) {
        uint64_t ts[2];
        if (vkGetQueryPoolResults(w->dev, f->times, 0, 2, sizeof(ts), ts, sizeof(ts[0]),
                                  VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
            w->gpu_ms = (double)((ts[1] - ts[0]) & w->ts_mask) *
                        w->props.limits.timestampPeriod * 1e-6;
        f->timed = false;
    }

    uint32_t idx = 0;
    res = acquire(w, f, &idx);
    if (res == VK_ERROR_OUT_OF_DATE_KHR) {
        w->swap_dirty = true;
        ret = rx_swapchain_create(w);
        if (ret != 0)
            return ret;
        res = acquire(w, f, &idx);
    }
    if (res == VK_ERROR_OUT_OF_DATE_KHR || res == VK_TIMEOUT || res == VK_NOT_READY) {
        w->swap_dirty |= res == VK_ERROR_OUT_OF_DATE_KHR;
        return 1;
    }
    if (res == VK_SUBOPTIMAL_KHR) {
        /* The image is ours and the semaphore will signal: draw this one,
         * recreate before the next. */
        w->swap_dirty = true;
    } else if (res != VK_SUCCESS) {
        rx_err("gpu: acquiring an image: %s", rx_vk_strerror(res));
        return rx_vk_ret2err(res);
    }

    /* From here on the frame is always submitted: the acquire semaphore is
     * pending a signal and has to be waited on. */
    if (capture && w->can_capture && ensure_capture(w) < 0)
        capture = false;
    capture &= w->can_capture;

    vkResetFences(w->dev, 1, &f->fence);
    vkResetCommandPool(w->dev, f->pool, 0);

    VkCommandBufferBeginInfo binfo = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    vkBeginCommandBuffer(f->cmd, &binfo);
    if (f->times)
        vkCmdResetQueryPool(f->cmd, f->times, 0, 2);

    const uint64_t serial = w->serial + 1;
    rx_vk_record_uploads(w, f->cmd, serial);

    VkImage img = w->images[idx];
    image_barrier(f->cmd, img,
                  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_NONE,
                  VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                  VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

    VkRenderingAttachmentInfo att = {
        .sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView   = w->views[idx],
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp     = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue  = { .color = { .float32 = { RX_CLEAR_R, RX_CLEAR_G, RX_CLEAR_B, 1.0f } } },
    };
    VkRenderingInfo rinfo = {
        .sType                = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea           = { { 0, 0 }, w->extent },
        .layerCount           = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments    = &att,
    };
    /* The frame's GPU time is the rendering alone: after the uploads, before
     * the capture's copy. */
    if (f->times)
        vkCmdWriteTimestamp2(f->cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, f->times, 0);
    vkCmdBeginRendering(f->cmd, &rinfo);
    if (draw_data)
        rx_imgui_record(w, draw_data, f->cmd);
    vkCmdEndRendering(f->cmd);
    if (f->times)
        vkCmdWriteTimestamp2(f->cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, f->times, 1);

    if (capture)
        record_capture(w, f->cmd, img);
    else
        image_barrier(f->cmd, img,
                      VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                      VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                      VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                      VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

    /* A failure from here on leaves the slot's fence reset and never
     * signalled, so it is fatal to the window (EIO), not retried. */
    res = vkEndCommandBuffer(f->cmd);
    if (res != VK_SUCCESS) {
        rx_err("gpu: recording a frame: %s", rx_vk_strerror(res));
        return RX_ERROR(EIO);
    }

    VkSemaphoreSubmitInfo wait = {
        .sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .semaphore = f->acquired,
        .stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
    };
    VkSemaphoreSubmitInfo signal = {
        .sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .semaphore = w->rendered[idx],
        .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
    };
    VkCommandBufferSubmitInfo cinfo = {
        .sType         = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
        .commandBuffer = f->cmd,
    };
    VkSubmitInfo2 submit = {
        .sType                    = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .waitSemaphoreInfoCount   = 1,
        .pWaitSemaphoreInfos      = &wait,
        .commandBufferInfoCount   = 1,
        .pCommandBufferInfos      = &cinfo,
        .signalSemaphoreInfoCount = 1,
        .pSignalSemaphoreInfos    = &signal,
    };
    res = vkQueueSubmit2(w->queue, 1, &submit, f->fence);
    if (res != VK_SUCCESS) {
        rx_err("gpu: submitting a frame: %s", rx_vk_strerror(res));
        return RX_ERROR(EIO);
    }
    f->serial = serial;
    f->timed = f->times != VK_NULL_HANDLE;
    w->serial = serial;
    w->frame_idx = (w->frame_idx + 1) % RX_FRAMES_IN_FLIGHT;

    /* Before the present: wl_surface.frame is double-buffered state that
     * applies with the next commit, and the commit is the one the WSI makes
     * inside vkQueuePresentKHR. Asked for after it, the request would wait
     * for a commit that only the next frame - which waits for it - makes. */
    rx_window_want_frame_cb(w);

    VkPresentInfoKHR pinfo = {
        .sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores    = &w->rendered[idx],
        .swapchainCount     = 1,
        .pSwapchains        = &w->swapchain,
        .pImageIndices      = &idx,
    };
    res = vkQueuePresentKHR(w->queue, &pinfo);
    if (capture)
        w->cap_serial = serial;
    w->last_present_serial = serial;

    if (res == VK_SUBOPTIMAL_KHR) {
        w->swap_dirty = true;
    } else if (res == VK_ERROR_OUT_OF_DATE_KHR) {
        /* Not shown, so nothing will answer the frame request until some
         * later commit; the next frame must not wait for it. */
        w->swap_dirty = true;
        rx_window_frame_skipped(w);
        return 1;
    } else if (res != VK_SUCCESS) {
        rx_window_frame_skipped(w);
        rx_err("gpu: presenting: %s", rx_vk_strerror(res));
        return rx_vk_ret2err(res);
    }

    return 0;
}

/* -------------------------------------------------------------- screenshot */

int rx_swapchain_readback(RXWindow *w, uint8_t **rgba, int *width, int *height)
{
    if (!w->cap.buf || !w->cap_serial)
        return RX_ERROR(EAGAIN);

    /* The capture's submission, if a slot still holds it; otherwise it is
     * long done. */
    for (int i = 0; i < RX_FRAMES_IN_FLIGHT; i++) {
        RXFrame *f = &w->frames[i];
        if (f->serial == w->cap_serial)
            vkWaitForFences(w->dev, 1, &f->fence, VK_TRUE, UINT64_MAX);
    }
    rx_vk_poll_completed(w);

    int ret = rx_vk_buffer_invalidate(w, &w->cap);
    if (ret < 0)
        return ret;

    size_t nb = (size_t)w->cap_width * w->cap_height;
    uint8_t *out = malloc(nb * 4);
    if (!out)
        return RX_ERROR(ENOMEM);

    /* BGRA or RGBA in, RGBA out; the window is opaque, whatever the alpha
     * channel ended up holding. */
    const uint8_t *src = w->cap.map;
    bool bgra = w->format.format == VK_FORMAT_B8G8R8A8_UNORM ||
                w->format.format == VK_FORMAT_B8G8R8A8_SRGB;
    for (size_t i = 0; i < nb; i++) {
        out[4 * i + 0] = src[4 * i + (bgra ? 2 : 0)];
        out[4 * i + 1] = src[4 * i + 1];
        out[4 * i + 2] = src[4 * i + (bgra ? 0 : 2)];
        out[4 * i + 3] = 255;
    }

    *rgba = out;
    *width = (int)w->cap_width;
    *height = (int)w->cap_height;
    return 0;
}

/* PNG through libavcodec's encoder, as RGB: the window is opaque. */
static int write_png(const char *path, const uint8_t *rgba, int width, int height)
{
    const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_PNG);
    if (!codec) {
        rx_err("gpu: libavcodec has no PNG encoder");
        return RX_ERROR(ENOSYS);
    }

    int ret = 0;
    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    AVFrame *frame = av_frame_alloc();
    AVPacket *pkt = av_packet_alloc();
    if (!ctx || !frame || !pkt) {
        ret = RX_ERROR(ENOMEM);
        goto fail;
    }

    ctx->width     = width;
    ctx->height    = height;
    ctx->pix_fmt   = AV_PIX_FMT_RGB24;
    ctx->time_base = (AVRational) { 1, 1 };
    ret = avcodec_open2(ctx, codec, NULL);
    if (ret < 0)
        goto fail;

    frame->format = AV_PIX_FMT_RGB24;
    frame->width  = width;
    frame->height = height;
    ret = av_frame_get_buffer(frame, 0);
    if (ret < 0)
        goto fail;

    for (int y = 0; y < height; y++) {
        const uint8_t *src = rgba + (size_t)y * width * 4;
        uint8_t *dst = frame->data[0] + (size_t)y * frame->linesize[0];
        for (int x = 0; x < width; x++) {
            dst[3 * x + 0] = src[4 * x + 0];
            dst[3 * x + 1] = src[4 * x + 1];
            dst[3 * x + 2] = src[4 * x + 2];
        }
    }

    ret = avcodec_send_frame(ctx, frame);
    if (ret < 0)
        goto fail;
    ret = avcodec_receive_packet(ctx, pkt);
    if (ret < 0)
        goto fail;

    ret = rx_file_write_atomic(path, pkt->data, (size_t)pkt->size);

fail:
    if (ret < 0)
        rx_err("gpu: unable to write %s (%d)", path, ret);
    av_packet_free(&pkt);
    av_frame_free(&frame);
    avcodec_free_context(&ctx);
    return ret;
}

int rx_window_screenshot(RXWindow *w, const char *png_path)
{
    if (!w || !png_path)
        return RX_ERROR(EINVAL);
    if (!w->can_capture) {
        rx_err("gpu: the swapchain images cannot be read back");
        return RX_ERROR(ENOTSUP);
    }

    /* The last presented frame was captured only if it was asked for in
     * time (the last of opts.max_frames always is). Otherwise ImGui's draw
     * data of that frame is still there: present it once more, captured. */
    if (!w->cap_serial || w->cap_serial != w->last_present_serial) {
        void *dd = rx_imgui_last_draw_data(w);
        if (!dd)
            return RX_ERROR(EAGAIN);
        int ret = rx_swapchain_frame(w, dd, true);
        if (ret < 0)
            return ret;
        if (ret > 0 || w->cap_serial != w->last_present_serial)
            return RX_ERROR(EAGAIN);
    }

    return rx_swapchain_save_png(w, png_path);
}

void rx_window_screenshot_next(RXWindow *w, const char *png_path)
{
    if (!w || !png_path)
        return;
    if (!w->can_capture) {
        rx_err("gpu: the swapchain images cannot be read back");
        return;
    }
    char *path = rx_strdup(png_path);
    if (!path)
        return;
    free(w->shot_path);
    w->shot_path = path;
    w->capture_next = true;
}

int rx_swapchain_save_png(RXWindow *w, const char *png_path)
{
    if (!w->cap_serial || w->cap_serial != w->last_present_serial)
        return RX_ERROR(EAGAIN);

    uint8_t *rgba = NULL;
    int width = 0, height = 0;
    int ret = rx_swapchain_readback(w, &rgba, &width, &height);
    if (ret < 0)
        return ret;

    ret = write_png(png_path, rgba, width, height);
    free(rgba);
    if (ret >= 0)
        rx_info("gpu: wrote %s (%dx%d)", png_path, width, height);
    return ret < 0 ? ret : 0;
}
