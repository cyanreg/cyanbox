/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* Textures for the interface (cover art, thumbnails): RGBA8 images in
 * device-local memory, filled through a staging buffer, and registered
 * with ImGui's Vulkan backend, whose descriptor set is the ImTextureID.
 *
 * The images are R8G8B8A8_UNORM, not _SRGB: the swapchain is UNORM and
 * the bytes are sRGB-encoded already, so they go to the screen as they
 * are. The backend samples with its own linear sampler (bound once per
 * draw list since imgui 1.92.8); there is no sampler here.
 *
 * Creating returns at once: the copy is recorded at the start of the next
 * frame (vk.c), in the same submission as the first draw that uses it.
 * Destroying is deferred until every submission that may have sampled the
 * texture, including the frame being built now, has completed. */

#include "rxbox/gpu.h"

#include <string.h>

#include "gpu_priv.h"

/* ImGui's descriptor pool holds 4096 (imgui_glue.cpp); destroyed textures
 * keep theirs for a frame or two, the font atlas needs a few. */
#define RX_MAX_TEXTURES 3584

uint64_t rx_texture_create(RXWindow *w, const uint8_t *rgba, int width, int height)
{
    if (!w || !rgba || width <= 0 || height <= 0 ||
        (uint32_t)width > w->props.limits.maxImageDimension2D ||
        (uint32_t)height > w->props.limits.maxImageDimension2D)
        return 0;
    if (w->textures.nb >= RX_MAX_TEXTURES) {
        rx_err("gpu: too many textures (%d)", RX_MAX_TEXTURES);
        return 0;
    }

    RXTexture tex = { 0 };
    RXUpload up = { 0 };
    const VkDeviceSize size = (VkDeviceSize)width * height * 4;

    VkImageCreateInfo iinfo = {
        .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType     = VK_IMAGE_TYPE_2D,
        .format        = VK_FORMAT_R8G8B8A8_UNORM,
        .extent        = { (uint32_t)width, (uint32_t)height, 1 },
        .mipLevels     = 1,
        .arrayLayers   = 1,
        .samples       = VK_SAMPLE_COUNT_1_BIT,
        .tiling        = VK_IMAGE_TILING_OPTIMAL,
        .usage         = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode   = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VmaAllocationCreateInfo ainfo = {
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
    };
    VkResult res = vmaCreateImage(w->vma, &iinfo, &ainfo, &tex.img, &tex.alloc, NULL);
    if (res != VK_SUCCESS) {
        rx_err("gpu: unable to create a %dx%d texture: %s", width, height,
               rx_vk_strerror(res));
        return 0;
    }

    VkImageViewCreateInfo vinfo = {
        .sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image    = tex.img,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format   = VK_FORMAT_R8G8B8A8_UNORM,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .levelCount = 1,
            .layerCount = 1,
        },
    };
    res = vkCreateImageView(w->dev, &vinfo, NULL, &tex.view);
    if (res != VK_SUCCESS)
        goto fail;

    if (rx_vk_buffer_create(w, &up.staging, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                            RX_MEM_UPLOAD) < 0)
        goto fail;
    memcpy(up.staging.map, rgba, size);
    vmaFlushAllocation(w->vma, up.staging.alloc, 0, VK_WHOLE_SIZE);
    up.dst_img = tex.img;
    up.width   = (uint32_t)width;
    up.height  = (uint32_t)height;

    tex.id = rx_imgui_add_texture(w, tex.view);
    if (!tex.id)
        goto fail;

    if (!rx_array_push(&w->textures, &tex))
        goto fail;
    if (rx_vk_upload(w, &up) < 0) {
        w->textures.nb--;
        goto fail;
    }

    return tex.id;

fail:
    /* Nothing of it was ever submitted: it can all go at once. */
    rx_err("gpu: unable to create a %dx%d texture", width, height);
    if (tex.id)
        rx_imgui_remove_texture(w, tex.id);
    rx_vk_buffer_free(w, &up.staging);
    if (tex.view)
        vkDestroyImageView(w->dev, tex.view, NULL);
    vmaDestroyImage(w->vma, tex.img, tex.alloc);
    return 0;
}

void rx_texture_destroy(RXWindow *w, uint64_t id)
{
    if (!w || !id)
        return;

    for (size_t i = 0; i < w->textures.nb; i++) {
        RXTexture *t = rx_array_at(&w->textures, i);
        if (t->id != id)
            continue;

        RXDeferred d = {
            .img    = t->img,
            .view   = t->view,
            .alloc  = t->alloc,
            .tex_id = t->id,
        };
        rx_vk_defer(w, &d);
        rx_array_remove(&w->textures, i);
        return;
    }

    rx_warn("gpu: destroying a texture that does not exist");
}

void rx_textures_destroy_all(RXWindow *w)
{
    while (w->textures.nb) {
        RXTexture *t = rx_array_at(&w->textures, w->textures.nb - 1);
        rx_texture_destroy(w, t->id);
    }
}
