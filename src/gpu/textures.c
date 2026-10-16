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

#include "cyanbox/gpu.h"

#include <string.h>

#include "gpu_priv.h"

/* ImGui's descriptor pool holds 4096 (imgui_glue.cpp); destroyed textures
 * keep theirs for a frame or two, the font atlas needs a few. */
#define CB_MAX_TEXTURES 3584

uint64_t cb_texture_create(CBWindow *w, const uint8_t *rgba, int width, int height)
{
    if (!w || !rgba || width <= 0 || height <= 0 ||
        (uint32_t)width > w->vk.props.limits.maxImageDimension2D ||
        (uint32_t)height > w->vk.props.limits.maxImageDimension2D)
        return 0;
    if (w->textures.nb >= CB_MAX_TEXTURES) {
        cb_err("gpu: too many textures (%d)", CB_MAX_TEXTURES);
        return 0;
    }

    /* Room to defer the texture and its staging buffer (vk.c). */
    if (cb_vk_defer_reserve(w, 2) < 0) {
        cb_err("gpu: out of memory for a %dx%d texture", width, height);
        return 0;
    }

    CBTexture tex = { 0 };
    CBUpload up = { 0 };
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
    VkResult res = vmaCreateImage(w->vk.vma, &iinfo, &ainfo, &tex.img, &tex.alloc, NULL);
    if (res != VK_SUCCESS) {
        cb_err("gpu: unable to create a %dx%d texture: %s", width, height,
               cb_vk_strerror(res));
        cb_vk_defer_unreserve(w, 2);
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
    res = vkCreateImageView(w->vk.dev, &vinfo, NULL, &tex.view);
    if (res != VK_SUCCESS)
        goto fail;

    if (cb_vk_buffer_create(w, &up.staging, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                            CB_MEM_UPLOAD) < 0)
        goto fail;
    memcpy(up.staging.map, rgba, size);
    vmaFlushAllocation(w->vk.vma, up.staging.alloc, 0, VK_WHOLE_SIZE);
    up.dst_img = tex.img;
    up.width   = (uint32_t)width;
    up.height  = (uint32_t)height;

    tex.id = cb_imgui_add_texture(w, tex.view);
    if (!tex.id)
        goto fail;

    if (!cb_array_push(&w->textures, &tex))
        goto fail;
    if (cb_vk_upload(w, &up) < 0) {
        w->textures.nb--;
        goto fail;
    }

    return tex.id;

fail:
    /* Nothing of it was ever submitted: it can all go at once. */
    cb_err("gpu: unable to create a %dx%d texture", width, height);
    cb_vk_defer_unreserve(w, 2);
    if (tex.id)
        cb_imgui_remove_texture(w, tex.id);
    cb_vk_buffer_free(w, &up.staging);
    if (tex.view)
        vkDestroyImageView(w->vk.dev, tex.view, NULL);
    vmaDestroyImage(w->vk.vma, tex.img, tex.alloc);
    return 0;
}

void cb_texture_destroy(CBWindow *w, uint64_t id)
{
    if (!w || !id)
        return;

    for (size_t i = 0; i < w->textures.nb; i++) {
        CBTexture *t = cb_array_at(&w->textures, i);
        if (t->id != id)
            continue;

        CBDeferred d = {
            .img    = t->img,
            .view   = t->view,
            .alloc  = t->alloc,
            .tex_id = t->id,
        };
        cb_vk_defer(w, &d);
        cb_array_remove(&w->textures, i);
        return;
    }

    cb_warn("gpu: destroying a texture that does not exist");
}

void cb_textures_destroy_all(CBWindow *w)
{
    while (w->textures.nb) {
        CBTexture *t = cb_array_at(&w->textures, w->textures.nb - 1);
        cb_texture_destroy(w, t->id);
    }
}
