/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* Waveforms: the column bytes of an ANLZ waveform tag in a device-local
 * buffer, and a draw that a fragment shader turns into rekordbox's
 * picture (shaders/wave.frag has the rules).
 *
 * The buffer holds the bytes exactly as they are in the tag, at the tag's
 * own stride (3, 1, 2 or 6 bytes a column), padded to a whole word: the
 * shader reads 32-bit words through the buffer's device address and picks
 * the bytes out, which needs no 8-bit storage feature and no descriptor.
 *
 * A draw is an ImDrawList callback carrying a copy of everything it needs
 * (the address, the column count, the rectangle), so the waveform can be
 * released right after it is drawn: the buffer itself is freed only when
 * the frame that read it is done. The callback binds the pipeline, sets
 * the scissor from the draw command's clip rectangle, pushes the
 * constants and draws four vertices; ImGui's reset-render-state callback,
 * added after it, gives ImGui its own pipeline back. */

#include "rxbox/gpu.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

#include "gpu_priv.h"
/* Generated into the build directory (meson.build). */
#include "src/gpu/wave_frag.spv.h"
#include "src/gpu/wave_vert.spv.h"

struct RXWaveGpu {
    enum RXWaveKind kind;
    int        nb_columns;
    int        stride;
    RXVkBuffer buf;
};

/* The push constants of shaders/wave.vert and wave.frag (std430). */
typedef struct RXWavePush {
    VkDeviceAddress addr;
    uint32_t pad[2];
    float    rect[4];
    float    fb_size[2];
    float    col_off;
    float    col_per_px;
    int32_t  col_base;
    int32_t  nb_columns;
    int32_t  kind;
    int32_t  mode;
    float    inset_top;
    float    inset_bottom;
    float    unit;
    float    alpha;
    float    dim;
    float    dim_off;
    uint32_t flags;
    float    gain;
} RXWavePush;

static_assert(offsetof(RXWavePush, rect) == 16, "push constant layout");
static_assert(offsetof(RXWavePush, fb_size) == 32, "push constant layout");
static_assert(offsetof(RXWavePush, col_base) == 48, "push constant layout");
static_assert(offsetof(RXWavePush, inset_top) == 64, "push constant layout");
static_assert(offsetof(RXWavePush, flags) == 88, "push constant layout");
static_assert(offsetof(RXWavePush, gain) == 92, "push constant layout");
static_assert(sizeof(RXWavePush) <= 128, "push constants beyond the guaranteed 128 bytes");

#define RX_WAVE_FLAG_DETAIL 1u

static int kind_stride(enum RXWaveKind kind)
{
    switch (kind) {
    case RX_WAVE_3BAND:       return 3;
    case RX_WAVE_BLUE:        return 1;
    case RX_WAVE_RGB_DETAIL:  return 2;
    case RX_WAVE_RGB_PREVIEW: return 6;
    default:                  return 0;
    }
}

/* ---------------------------------------------------------------- pipeline */

static int shader_module(RXWindow *w, const uint8_t *code, size_t size, VkShaderModule *out)
{
    VkShaderModuleCreateInfo info = {
        .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = size,
        .pCode    = (const uint32_t *)code,
    };
    return rx_vk_ret2err(vkCreateShaderModule(w->dev, &info, NULL, out));
}

int rx_wave_pipeline_create(RXWindow *w)
{
    VkShaderModule vert = VK_NULL_HANDLE, frag = VK_NULL_HANDLE;
    int ret = shader_module(w, rx_wave_vert_spv, rx_wave_vert_spv_size, &vert);
    if (ret < 0)
        goto fail;
    ret = shader_module(w, rx_wave_frag_spv, rx_wave_frag_spv_size, &frag);
    if (ret < 0)
        goto fail;

    VkPushConstantRange range = {
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        .size       = sizeof(RXWavePush),
    };
    VkPipelineLayoutCreateInfo linfo = {
        .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges    = &range,
    };
    ret = rx_vk_ret2err(vkCreatePipelineLayout(w->dev, &linfo, NULL, &w->wave_layout));
    if (ret < 0)
        goto fail;

    VkPipelineShaderStageCreateInfo stages[] = {
        {
            .sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage  = VK_SHADER_STAGE_VERTEX_BIT,
            .module = vert,
            .pName  = "main",
        }, {
            .sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage  = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = frag,
            .pName  = "main",
        },
    };
    VkPipelineVertexInputStateCreateInfo vertex = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
    };
    VkPipelineInputAssemblyStateCreateInfo assembly = {
        .sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,
    };
    VkPipelineViewportStateCreateInfo viewport = {
        .sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount  = 1,
    };
    VkPipelineRasterizationStateCreateInfo raster = {
        .sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode    = VK_CULL_MODE_NONE,
        .frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .lineWidth   = 1.0f,
    };
    VkPipelineMultisampleStateCreateInfo multisample = {
        .sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };

    /* The shader writes premultiplied colour; the alpha channel blends the
     * way ImGui's does. */
    VkPipelineColorBlendAttachmentState blend_att = {
        .blendEnable         = VK_TRUE,
        .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .colorBlendOp        = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .alphaBlendOp        = VK_BLEND_OP_ADD,
        .colorWriteMask      = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                               VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    };
    VkPipelineColorBlendStateCreateInfo blend = {
        .sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments    = &blend_att,
    };
    const VkDynamicState dyn_states[] = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR,
    };
    VkPipelineDynamicStateCreateInfo dynamic = {
        .sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = RX_ARRAY_ELEMS(dyn_states),
        .pDynamicStates    = dyn_states,
    };

    /* Dynamic rendering: the pipeline names the attachment format instead
     * of a render pass. */
    VkPipelineRenderingCreateInfo rendering = {
        .sType                   = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount    = 1,
        .pColorAttachmentFormats = &w->format.format,
    };
    VkGraphicsPipelineCreateInfo pinfo = {
        .sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext               = &rendering,
        .stageCount          = RX_ARRAY_ELEMS(stages),
        .pStages             = stages,
        .pVertexInputState   = &vertex,
        .pInputAssemblyState = &assembly,
        .pViewportState      = &viewport,
        .pRasterizationState = &raster,
        .pMultisampleState   = &multisample,
        .pColorBlendState    = &blend,
        .pDynamicState       = &dynamic,
        .layout              = w->wave_layout,
    };
    ret = rx_vk_ret2err(vkCreateGraphicsPipelines(w->dev, VK_NULL_HANDLE, 1, &pinfo, NULL,
                                                  &w->wave_pipe));

fail:
    if (ret < 0)
        rx_err("gpu: unable to create the waveform pipeline (%d)", ret);
    if (vert)
        vkDestroyShaderModule(w->dev, vert, NULL);
    if (frag)
        vkDestroyShaderModule(w->dev, frag, NULL);
    return ret;
}

void rx_wave_pipeline_destroy(RXWindow *w)
{
    if (w->wave_pipe)
        vkDestroyPipeline(w->dev, w->wave_pipe, NULL);
    if (w->wave_layout)
        vkDestroyPipelineLayout(w->dev, w->wave_layout, NULL);
    w->wave_pipe = VK_NULL_HANDLE;
    w->wave_layout = VK_NULL_HANDLE;
}

/* ------------------------------------------------------------------ upload */

RXWaveGpu *rx_wave_upload(RXWindow *w, enum RXWaveKind kind, const uint8_t *columns,
                          int nb_columns)
{
    int stride = kind_stride(kind);
    if (!w || !stride || nb_columns < 0 || (nb_columns && !columns))
        return NULL;

    RXWaveGpu *g = calloc(1, sizeof(*g));
    if (!g)
        return NULL;
    g->kind = kind;
    g->stride = stride;
    g->nb_columns = nb_columns;
    if (!nb_columns)
        return g;

    const size_t bytes = (size_t)nb_columns * stride;
    const VkDeviceSize size = RX_ALIGN(bytes, 4);
    RXUpload up = { 0 };

    int ret = rx_vk_buffer_create(w, &g->buf, size,
                                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                  VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                                  VK_BUFFER_USAGE_TRANSFER_DST_BIT, RX_MEM_DEVICE);
    if (ret < 0)
        goto fail;

    ret = rx_vk_buffer_create(w, &up.staging, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                              RX_MEM_UPLOAD);
    if (ret < 0)
        goto fail;
    memcpy(up.staging.map, columns, bytes);
    memset((uint8_t *)up.staging.map + bytes, 0, size - bytes);
    vmaFlushAllocation(w->vma, up.staging.alloc, 0, VK_WHOLE_SIZE);
    up.dst_buf = g->buf.buf;

    ret = rx_vk_upload(w, &up);
    if (ret < 0)
        goto fail;

    return g;

fail:
    rx_err("gpu: unable to upload a waveform of %d columns (%d)", nb_columns, ret);
    rx_vk_buffer_free(w, &up.staging);
    rx_vk_buffer_free(w, &g->buf);
    free(g);
    return NULL;
}

void rx_wave_release(RXWindow *w, RXWaveGpu *g)
{
    if (!g)
        return;

    if (w && g->buf.buf) {
        RXDeferred d = {
            .buf   = g->buf.buf,
            .alloc = g->buf.alloc,
        };
        rx_vk_defer(w, &d);
    }
    free(g);
}

int rx_wave_columns(const RXWaveGpu *g)
{
    return g ? g->nb_columns : 0;
}

/* -------------------------------------------------------------------- draw */

void rx_wave_draw(RXWindow *w, void *im_draw_list, const RXWaveGpu *g, const RXWaveDraw *d)
{
    if (!w || !im_draw_list || !g || !d || !g->buf.buf || g->nb_columns <= 0)
        return;

    /* Nothing to draw, columns too far out for the shader's integer
     * column numbers, or a rectangle with an infinite edge (Vulkan leaves
     * a primitive with non-finite positions undefined). */
    if (!(d->x1 > d->x0) || !(d->y1 > d->y0) || !(d->col1 > d->col0) || !(d->alpha > 0.0f) ||
        fabs(d->col0) > 1e9 || fabs(d->col1) > 1e9 || !isfinite(d->x0) || !isfinite(d->x1) ||
        !isfinite(d->y0) || !isfinite(d->y1))
        return;

    RXWaveCmd c = {
        .w          = w,
        .addr       = g->buf.addr,
        .nb_columns = g->nb_columns,
        .kind       = (int32_t)g->kind,
        .d          = *d,
    };
    rx_imgui_add_wave(im_draw_list, &c);
}

void rx_wave_record(const RXWaveCmd *c, VkCommandBuffer cmd, const RXWaveView *v)
{
    RXWindow *w = c->w;
    const RXWaveDraw *d = &c->d;

    /* The rectangle in framebuffer pixels. */
    float x0 = (d->x0 - v->off_x) * v->scale_x;
    float x1 = (d->x1 - v->off_x) * v->scale_x;
    float y0 = (d->y0 - v->off_y) * v->scale_y;
    float y1 = (d->y1 - v->off_y) * v->scale_y;

    /* The draw command's clip rectangle (already in the framebuffer)
     * intersected with the rectangle, rounded outwards: the quad itself
     * keeps the rest of the rectangle's edge. */
    float sx0 = fmaxf(v->clip[0], floorf(x0));
    float sy0 = fmaxf(v->clip[1], floorf(y0));
    float sx1 = fminf(v->clip[2], ceilf(x1));
    float sy1 = fminf(v->clip[3], ceilf(y1));
    if (sx1 <= sx0 || sy1 <= sy0)
        return;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, w->wave_pipe);

    VkViewport viewport = {
        .width    = v->fb_width,
        .height   = v->fb_height,
        .maxDepth = 1.0f,
    };
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor = {
        .offset = { (int32_t)sx0, (int32_t)sy0 },
        .extent = { (uint32_t)(sx1 - sx0), (uint32_t)(sy1 - sy0) },
    };
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    /* Columns as an integer base and a float offset: a detail waveform has
     * tens of thousands of columns, more than a float resolves to a
     * fraction of a pixel. */
    double base = floor(d->col0);
    RXWavePush push = {
        .addr         = c->addr,
        .rect         = { x0, y0, x1, y1 },
        .fb_size      = { v->fb_width, v->fb_height },
        .col_off      = (float)(d->col0 - base),
        .col_per_px   = (float)((d->col1 - d->col0) / (double)(x1 - x0)),
        .col_base     = (int32_t)base,
        .nb_columns   = c->nb_columns,
        .kind         = c->kind,
        .mode         = (int32_t)d->mode,
        .inset_top    = fmaxf(d->inset_top, 0.0f) * v->scale_y,
        .inset_bottom = fmaxf(d->inset_bottom, 0.0f) * v->scale_y,
        .unit         = fmaxf(v->scale_y, 1.0f),
        .alpha        = fminf(d->alpha, 1.0f),
        .dim          = RX_CLAMP(d->dim, 0.0f, 1.0f),
        .dim_off      = (float)RX_CLAMP(d->dim_until - base, -1e9, 1e9),
        .flags        = d->detail ? RX_WAVE_FLAG_DETAIL : 0,
        .gain         = d->gain > 0.0f && isfinite(d->gain) ? fminf(d->gain, 127.0f) : 1.0f,
    };
    vkCmdPushConstants(cmd, w->wave_layout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(push), &push);
    vkCmdDraw(cmd, 4, 1, 0, 0);
}
