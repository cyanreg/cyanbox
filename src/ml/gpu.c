/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* The model's GPU: a Vulkan instance with no surface, the best device that
 * can run the kernels, one compute queue, VMA (all set up by
 * src/gpu/vkdev.c, as the window's and the compute context's are), a
 * timeline semaphore, the pipelines and one command buffer. Kept like
 * cyanbox's compute context (src/gpu/compute.c), with what the model needs
 * on top.
 *
 * Choices, and why:
 *  - Its own device (ml_priv.h says why not cyanbox's compute context). The
 *    device is picked by the same rules: a named one wins, else discrete
 *    before integrated, and a CPU implementation (llvmpipe) only when
 *    named: the model is 4 billion parameters.
 *  - The kernels need 16-bit floats (the weights are F16, the cache is
 *    F16), 8- and 16-bit storage (Q4_K's bytes and halves), 64-bit
 *    integers (address arithmetic), subgroup arithmetic and shuffles, and
 *    48 kB of shared memory (the attention's tiles).
 *  - Three kernels count on the subgroup's size: gemv.comp sums a row's 64
 *    lanes in subgroups of 16 to 64 (its shared memory has room for four,
 *    and a subgroup must not straddle two rows), attn.comp and
 *    attn_dec.comp share sums among 8 neighbouring lanes by shuffles. A
 *    device whose sizes all lie in 16 to 64 (AMD's 32 and 64) runs them as
 *    its driver likes; one that may pick fewer (Intel's 8) is given a size
 *    (Vulkan 1.3's subgroup size control), and one that cannot be is not
 *    used. Those three ask for full subgroups where the device has them.
 *  - The products (gemm.comp) run in subgroups of 32 where the device lets
 *    a pipeline ask for it: on RDNA, whose drivers pick 64 for compute,
 *    they are 10 to 15 % faster so. They use no subgroup operations, so
 *    every output is the same sums in the same order either way; the
 *    kernels whose sums go through subgroups keep the driver's size.
 *    CYANBOX_ML_WAVE32=0 leaves the products the driver's size too.
 *  - The products of more than a few dozen rows go through K 64 columns
 *    at a time where the device has the 64 kB of shared memory that takes
 *    (AMD's), 32 elsewhere (48 kB, NVIDIA's): some 5 % faster on the 6900
 *    XT, and every output is the same sum in the same order either way.
 *  - A compute-only queue family when there is one, as the compute context
 *    does: the desktop's graphics are not held up behind a transformer
 *    layer.
 *  - One command buffer, recorded and submitted by the one thread using
 *    the model; a dispatch is followed by a global barrier only where the
 *    caller says one depends on another (ml_gpu_barrier). Every submission
 *    ends with one more, to the host's reads and to the next submission's
 *    dispatches and copies: ml_gpu_run waits for the timeline, which orders
 *    the work but makes none of its writes visible.
 *  - A wait that fails (a minute without an answer, or the device lost)
 *    waits for the device to go idle, so that nothing the submission uses
 *    is freed or recorded again under it, and the context is not used
 *    again: every later run fails with -EIO.
 *  - No descriptors: every shader takes its buffers as device addresses in
 *    its push constants, so one pipeline layout serves all. */

#include "ml_priv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mel_comp.spv.h"
#include "mel_norm_comp.spv.h"
#include "conv1_comp.spv.h"
#include "conv_permute_comp.spv.h"
#include "stem_permute_comp.spv.h"
#include "gemm_f16_comp.spv.h"
#include "gemm_f16_bk32_comp.spv.h"
#include "gemm_f16_conv_comp.spv.h"
#include "gemm_q4k_comp.spv.h"
#include "gemm_q4k_bk32_comp.spv.h"
#include "gemm_q4k_small_comp.spv.h"
#include "gemv_q4k_comp.spv.h"
#include "layernorm_comp.spv.h"
#include "rmsnorm_comp.spv.h"
#include "act_comp.spv.h"
#include "enc_attn_comp.spv.h"
#include "qk_rope_comp.spv.h"
#include "attn_comp.spv.h"
#include "attn_fcomb_comp.spv.h"
#include "attn_dec_comp.spv.h"
#include "attn_comb_comp.spv.h"
#include "embed_comp.spv.h"
#include "argmax_comp.spv.h"

static const struct {
    const uint8_t *code;
    size_t size;
    const char *name;
} kernels[ML_NB_KERNELS] = {
#define K(e, n) [e] = { cb_##n##_comp_spv, sizeof(cb_##n##_comp_spv), #n }
    K(ML_K_MEL,           mel),
    K(ML_K_MEL_NORM,      mel_norm),
    K(ML_K_CONV1,         conv1),
    K(ML_K_CONV_PERMUTE,  conv_permute),
    K(ML_K_STEM_PERMUTE,  stem_permute),
    K(ML_K_GEMM_F16,      gemm_f16),
    K(ML_K_GEMM_F16_BK32, gemm_f16_bk32),
    K(ML_K_GEMM_F16_CONV, gemm_f16_conv),
    K(ML_K_GEMM_Q4K,      gemm_q4k),
    K(ML_K_GEMM_Q4K_BK32, gemm_q4k_bk32),
    K(ML_K_GEMM_Q4K_SMALL, gemm_q4k_small),
    K(ML_K_GEMV_Q4K,      gemv_q4k),
    K(ML_K_LAYERNORM,     layernorm),
    K(ML_K_RMSNORM,       rmsnorm),
    K(ML_K_ACT,           act),
    K(ML_K_ENC_ATTN,      enc_attn),
    K(ML_K_QK_ROPE,       qk_rope),
    K(ML_K_ATTN,          attn),
    K(ML_K_ATTN_FCOMB,    attn_fcomb),
    K(ML_K_ATTN_DEC,      attn_dec),
    K(ML_K_ATTN_COMB,     attn_comb),
    K(ML_K_EMBED,         embed),
    K(ML_K_ARGMAX,        argmax),
#undef K
};

/* ------------------------------------------------------------------ device */

#define NEED_SHARED (48 * 1024)

/* The subgroup sizes gemv.comp works with (see the top of the file). */
#define SUB_MIN 16
#define SUB_MAX 64

/* What a device is set up with beyond the defaults. */
typedef struct DevChoice {
    uint32_t sub_size;          /* the size the subgroup kernels require, 0: any it picks */
    bool full_sub;              /* computeFullSubgroups */
    bool gemm_wave32;           /* the products may ask for subgroups of 32 */
} DevChoice;

/* Why a device cannot run the model, or NULL (CBVkDevCheck): `qfam` gets
 * the queue family to use, the DevChoice `opaque` how the subgroup
 * kernels are run. */
static const char *device_unusable(void *opaque, VkPhysicalDevice pd,
                                   const VkPhysicalDeviceProperties *props, uint32_t *qfam)
{
    DevChoice *c = opaque;
    VkPhysicalDeviceVulkan13Features f13 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
    };
    VkPhysicalDeviceVulkan12Features f12 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
        .pNext = &f13,
    };
    VkPhysicalDeviceVulkan11Features f11 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
        .pNext = &f12,
    };
    VkPhysicalDeviceFeatures2 f2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext = &f11,
    };
    vkGetPhysicalDeviceFeatures2(pd, &f2);
    if (!f12.bufferDeviceAddress)
        return "no bufferDeviceAddress";
    if (!f12.timelineSemaphore)
        return "no timelineSemaphore";
    if (!f13.synchronization2)
        return "no synchronization2";
    if (!f12.shaderFloat16 || !f12.shaderInt8)
        return "no 16-bit floats or 8-bit integers in shaders";
    if (!f11.storageBuffer16BitAccess || !f12.storageBuffer8BitAccess)
        return "no 16- or 8-bit storage";
    if (!f2.features.shaderInt64)
        return "no 64-bit integers in shaders";

    VkPhysicalDeviceVulkan13Properties p13 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES,
    };
    VkPhysicalDeviceSubgroupProperties sub = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES,
        .pNext = &p13,
    };
    VkPhysicalDeviceProperties2 p2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
        .pNext = &sub,
    };
    vkGetPhysicalDeviceProperties2(pd, &p2);
    const VkSubgroupFeatureFlags need = VK_SUBGROUP_FEATURE_BASIC_BIT |
                                        VK_SUBGROUP_FEATURE_ARITHMETIC_BIT |
                                        VK_SUBGROUP_FEATURE_SHUFFLE_BIT;
    if (!(sub.supportedStages & VK_SHADER_STAGE_COMPUTE_BIT) ||
        (sub.supportedOperations & need) != need)
        return "no subgroup arithmetic and shuffles";

    /* Subgroups of 16 to 64 lanes: any the device has, or one it is told
     * to use. Sizes are powers of two. */
    c->sub_size = 0;
    c->full_sub = f13.computeFullSubgroups;
    c->gemm_wave32 = f13.subgroupSizeControl &&
                     (p13.requiredSubgroupSizeStages & VK_SHADER_STAGE_COMPUTE_BIT) &&
                     p13.minSubgroupSize <= 32 && p13.maxSubgroupSize >= 32 &&
                     32u * p13.maxComputeWorkgroupSubgroups >= 256;
    if (p13.minSubgroupSize < SUB_MIN || p13.maxSubgroupSize > SUB_MAX) {
        uint32_t size = CB_MAX(p13.minSubgroupSize, SUB_MIN);
        if (!f13.subgroupSizeControl ||
            !(p13.requiredSubgroupSizeStages & VK_SHADER_STAGE_COMPUTE_BIT) ||
            size > CB_MIN(p13.maxSubgroupSize, SUB_MAX) ||
            (uint64_t)size * p13.maxComputeWorkgroupSubgroups < 256)
            return "no subgroups of 16 to 64 lanes";
        c->sub_size = size;
    }

    const VkPhysicalDeviceLimits *l = &props->limits;
    if (l->maxComputeSharedMemorySize < NEED_SHARED)
        return "too little shared memory";
    if (l->maxComputeWorkGroupInvocations < 256 || l->maxComputeWorkGroupSize[0] < 256)
        return "workgroups too small";
    if (l->maxPushConstantsSize < ML_PUSH_SIZE)
        return "push constants too small";
    return cb_vkdev_compute_family(pd, qfam);
}

static int create_pipelines(MLGpu *g)
{
    VkPushConstantRange range = {
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .size       = ML_PUSH_SIZE,
    };
    VkPipelineLayoutCreateInfo linfo = {
        .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges    = &range,
    };
    VkResult res = vkCreatePipelineLayout(g->vk.dev, &linfo, NULL, &g->layout);
    if (res != VK_SUCCESS)
        return cb_vk_ret2err(res);

    bool bk64 = g->vk.props.limits.maxComputeSharedMemorySize >= 64 * 1024;
    for (int k = 0; k < ML_NB_KERNELS; k++) {
        /* The products in tiles of 64 columns of K need 64 kB of shared
         * memory: elsewhere they are their tiles of 32. */
        int code = k;
        if (!bk64 && (k == ML_K_GEMM_F16 || k == ML_K_GEMM_Q4K))
            code = k == ML_K_GEMM_F16 ? ML_K_GEMM_F16_BK32 : ML_K_GEMM_Q4K_BK32;
        VkShaderModuleCreateInfo minfo = {
            .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = kernels[code].size,
            .pCode    = (const uint32_t *)kernels[code].code,
        };
        VkShaderModule mod = VK_NULL_HANDLE;
        res = vkCreateShaderModule(g->vk.dev, &minfo, NULL, &mod);
        if (res != VK_SUCCESS)
            return cb_vk_ret2err(res);

        bool sub = k == ML_K_GEMV_Q4K || k == ML_K_ATTN || k == ML_K_ATTN_DEC;
        bool gemm = k == ML_K_GEMM_F16 || k == ML_K_GEMM_F16_BK32 || k == ML_K_GEMM_F16_CONV ||
                    k == ML_K_GEMM_Q4K || k == ML_K_GEMM_Q4K_BK32 || k == ML_K_GEMM_Q4K_SMALL;
        VkPipelineShaderStageRequiredSubgroupSizeCreateInfo size = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO,
            .requiredSubgroupSize = gemm ? 32 : g->sub_size,
        };
        VkComputePipelineCreateInfo pinfo = {
            .sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .stage  = {
                .sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .pNext  = (sub && g->sub_size) || (gemm && g->gemm_wave32) ? &size : NULL,
                .flags  = sub && g->full_sub ?
                          VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT : 0,
                .stage  = VK_SHADER_STAGE_COMPUTE_BIT,
                .module = mod,
                .pName  = "main",
            },
            .layout = g->layout,
        };
        res = vkCreateComputePipelines(g->vk.dev, VK_NULL_HANDLE, 1, &pinfo, NULL, &g->pipes[k]);
        vkDestroyShaderModule(g->vk.dev, mod, NULL);
        if (res != VK_SUCCESS) {
            cb_warn("ml: unable to create the %s pipeline: %s", kernels[k].name,
                    cb_vk_strerror(res));
            return cb_vk_ret2err(res);
        }
    }
    return 0;
}

static int create_device(MLGpu *g)
{
    VkPhysicalDeviceVulkan13Features f13 = {
        .sType                = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
        .synchronization2     = VK_TRUE,
        .subgroupSizeControl  = g->sub_size != 0 || g->gemm_wave32,
        .computeFullSubgroups = g->full_sub,
    };
    VkPhysicalDeviceVulkan12Features f12 = {
        .sType                   = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
        .pNext                   = &f13,
        .bufferDeviceAddress     = VK_TRUE,
        .timelineSemaphore       = VK_TRUE,
        .shaderFloat16           = VK_TRUE,
        .shaderInt8              = VK_TRUE,
        .storageBuffer8BitAccess = VK_TRUE,
    };
    VkPhysicalDeviceVulkan11Features f11 = {
        .sType                    = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
        .pNext                    = &f12,
        .storageBuffer16BitAccess = VK_TRUE,
    };
    VkPhysicalDeviceFeatures2 f2 = {
        .sType    = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext    = &f11,
        .features = {
            .shaderInt64 = VK_TRUE,
            .shaderInt16 = VK_TRUE,
        },
    };

    int ret = cb_vkdev_device(&g->vk, &f2, NULL, 0);
    if (ret < 0 || (ret = cb_vkdev_timeline(&g->vk, &g->timeline)) < 0)
        return ret;

    VkCommandPoolCreateInfo pinfo = {
        .sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = g->vk.qfam,
    };
    VkResult res = vkCreateCommandPool(g->vk.dev, &pinfo, NULL, &g->pool);
    if (res != VK_SUCCESS)
        return cb_vk_ret2err(res);
    VkCommandBufferAllocateInfo cinfo = {
        .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool        = g->pool,
        .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    res = vkAllocateCommandBuffers(g->vk.dev, &cinfo, &g->cmd);
    if (res != VK_SUCCESS)
        return cb_vk_ret2err(res);

    if (g->profile && g->vk.ts_mask) {
        VkQueryPoolCreateInfo qpinfo = {
            .sType      = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
            .queryType  = VK_QUERY_TYPE_TIMESTAMP,
            .queryCount = CB_ARRAY_ELEMS(g->time_kernel) + 1,
        };
        res = vkCreateQueryPool(g->vk.dev, &qpinfo, NULL, &g->times);
        if (res != VK_SUCCESS)
            return cb_vk_ret2err(res);
    } else {
        g->profile = false;
    }

    return create_pipelines(g);
}

int ml_gpu_create(MLGpu *g, const char *device, bool validation)
{
    *g = (MLGpu) { 0 };

    const char *want = device && device[0] ? device : getenv("CYANBOX_GPU_DEVICE");
    if (want && !want[0])
        want = NULL;
    const char *val = getenv("CYANBOX_GPU_VALIDATION");
    validation = validation || (val && !strcmp(val, "1"));
    const char *prof = getenv("CYANBOX_ML_PROFILE");
    g->profile = prof && !strcmp(prof, "1");

    DevChoice choice = { 0 };
    CBVkDevOptions vo = {
        .tag        = "ml",
        .validation = validation,
        .device     = want,
        .check      = device_unusable,
        .opaque     = &choice,
    };
    int ret;
    if ((ret = cb_vkdev_instance(&g->vk, &vo)) < 0)
        goto fail;
    if ((ret = cb_vkdev_pick(&g->vk, &vo)) < 0) {
        cb_debug("ml: no Vulkan device can run the model");
        goto fail;
    }
    g->sub_size = choice.sub_size;
    g->full_sub = choice.full_sub;
    const char *w32 = getenv("CYANBOX_ML_WAVE32");
    g->gemm_wave32 = choice.gemm_wave32 && !(w32 && !strcmp(w32, "0"));
    if ((ret = create_device(g)) < 0)
        goto fail;

    char sub[32] = "as the driver picks";
    if (g->sub_size)
        snprintf(sub, sizeof(sub), "of %u", g->sub_size);
    cb_debug("ml: %s, Vulkan %u.%u, queue family %u%s, subgroups %s%s", g->vk.props.deviceName,
             VK_API_VERSION_MAJOR(g->vk.props.apiVersion),
             VK_API_VERSION_MINOR(g->vk.props.apiVersion), g->vk.qfam,
             g->vk.ts_mask ? "" : " (no timestamps)", sub,
             g->gemm_wave32 ? ", of 32 for the products" : "");
    return 0;

fail:
    ml_gpu_destroy(g);
    return ret;
}

void ml_gpu_destroy(MLGpu *g)
{
    if (g->vk.dev) {
        vkDeviceWaitIdle(g->vk.dev);
        for (int k = 0; k < ML_NB_KERNELS; k++) {
            if (g->pipes[k])
                vkDestroyPipeline(g->vk.dev, g->pipes[k], NULL);
        }
        if (g->layout)
            vkDestroyPipelineLayout(g->vk.dev, g->layout, NULL);
        if (g->times)
            vkDestroyQueryPool(g->vk.dev, g->times, NULL);
        if (g->pool)
            vkDestroyCommandPool(g->vk.dev, g->pool, NULL);
        if (g->timeline)
            vkDestroySemaphore(g->vk.dev, g->timeline, NULL);
    }
    cb_vkdev_destroy(&g->vk);

    *g = (MLGpu) { 0 };
}

/* ----------------------------------------------------------------- buffers */

int ml_buf_create(MLGpu *g, MLBuf *b, VkDeviceSize size, enum MLMem mem)
{
    *b = (MLBuf) { 0 };
    if (!size)
        return CB_ERROR(EINVAL);

    VkBufferCreateInfo info = {
        .sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size        = size,
        .usage       = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                       VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                       VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };

    /* As cyanbox's compute context: host memory for both directions, never
     * the BAR window into VRAM. The staging buffers are dedicated
     * allocations, so their mapping starts on a page: the weights are read
     * into them with O_DIRECT (gguf.c). */
    VmaAllocationCreateInfo ci = { 0 };
    switch (mem) {
    case ML_MEM_UPLOAD:
        ci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
        ci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                   VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                   VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
        break;
    case ML_MEM_READBACK:
        ci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
        ci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                   VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
        break;
    case ML_MEM_DEVICE:
    default:
        ci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        break;
    }

    VmaAllocationInfo ai = { 0 };
    VkResult res = vmaCreateBuffer(g->vk.vma, &info, &ci, &b->buf, &b->alloc, &ai);
    if (res != VK_SUCCESS) {
        cb_warn("ml: unable to allocate a %llu byte buffer: %s", (unsigned long long)size,
                cb_vk_strerror(res));
        *b = (MLBuf) { 0 };
        return cb_vk_ret2err(res);
    }

    b->size = size;
    b->map = ai.pMappedData;

    VkBufferDeviceAddressInfo ainfo = {
        .sType  = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
        .buffer = b->buf,
    };
    b->addr = vkGetBufferDeviceAddress(g->vk.dev, &ainfo);
    return 0;
}

void ml_buf_free(MLGpu *g, MLBuf *b)
{
    if (!b || !b->buf)
        return;
    vmaDestroyBuffer(g->vk.vma, b->buf, b->alloc);
    *b = (MLBuf) { 0 };
}

uint64_t ml_gpu_allocated(MLGpu *g)
{
    VmaBudget budgets[VK_MAX_MEMORY_HEAPS];
    vmaGetHeapBudgets(g->vk.vma, budgets);
    const VkPhysicalDeviceMemoryProperties *mp = NULL;
    vmaGetMemoryProperties(g->vk.vma, &mp);
    uint64_t total = 0;
    for (uint32_t i = 0; i < mp->memoryHeapCount; i++)
        total += budgets[i].statistics.allocationBytes;
    return total;
}

int ml_buf_invalidate(MLGpu *g, MLBuf *b)
{
    if (!b->map)
        return 0;
    return cb_vk_ret2err(vmaInvalidateAllocation(g->vk.vma, b->alloc, 0, VK_WHOLE_SIZE));
}

int ml_buf_flush(MLGpu *g, MLBuf *b, VkDeviceSize size)
{
    if (!b->map)
        return 0;
    return cb_vk_ret2err(vmaFlushAllocation(g->vk.vma, b->alloc, 0, size));
}

/* --------------------------------------------------------------- recording */

static void begin(MLGpu *g)
{
    if (g->recording)
        return;
    VkCommandBufferBeginInfo binfo = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    vkBeginCommandBuffer(g->cmd, &binfo);
    g->recording = true;
    if (g->profile) {
        vkCmdResetQueryPool(g->cmd, g->times, 0, CB_ARRAY_ELEMS(g->time_kernel) + 1);
        vkCmdWriteTimestamp2(g->cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, g->times, 0);
        g->nb_times = 0;
    }
}

void ml_gpu_dispatch(MLGpu *g, enum MLKernel k, const void *push, size_t size,
                     uint32_t x, uint32_t y, uint32_t z)
{
    begin(g);
    vkCmdBindPipeline(g->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g->pipes[k]);
    vkCmdPushConstants(g->cmd, g->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, (uint32_t)size, push);
    vkCmdDispatch(g->cmd, x, y, z);

    /* The timestamp after a dispatch waits for it (and for what ran
     * alongside it since the last one): with a barrier after nearly every
     * dispatch, that is the dispatch's own time. */
    if (g->profile && g->nb_times < (int)CB_ARRAY_ELEMS(g->time_kernel)) {
        g->time_kernel[g->nb_times++] = k;
        vkCmdWriteTimestamp2(g->cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, g->times,
                             (uint32_t)g->nb_times);
    }
}

void ml_gpu_barrier(MLGpu *g)
{
    begin(g);
    VkMemoryBarrier2 mb = {
        .sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_COPY_BIT |
                         VK_PIPELINE_STAGE_2_CLEAR_BIT,
        .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .dstStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_COPY_BIT |
                         VK_PIPELINE_STAGE_2_CLEAR_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                         VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
                         VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
    };
    VkDependencyInfo dep = {
        .sType              = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .memoryBarrierCount = 1,
        .pMemoryBarriers    = &mb,
    };
    vkCmdPipelineBarrier2(g->cmd, &dep);
}

void ml_gpu_copy(MLGpu *g, const MLBuf *src, VkDeviceSize src_off, const MLBuf *dst,
                 VkDeviceSize dst_off, VkDeviceSize size)
{
    begin(g);
    VkBufferCopy region = {
        .srcOffset = src_off,
        .dstOffset = dst_off,
        .size      = size,
    };
    vkCmdCopyBuffer(g->cmd, src->buf, dst->buf, 1, &region);
}

void ml_gpu_fill(MLGpu *g, const MLBuf *dst, VkDeviceSize off, VkDeviceSize size, uint32_t v)
{
    begin(g);
    vkCmdFillBuffer(g->cmd, dst->buf, off, size, v);
}

static void read_times(MLGpu *g)
{
    if (!g->profile || !g->nb_times)
        return;
    uint64_t ts[CB_ARRAY_ELEMS(g->time_kernel) + 1];
    if (vkGetQueryPoolResults(g->vk.dev, g->times, 0, (uint32_t)g->nb_times + 1, sizeof(ts), ts,
                              sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
        return;
    double period = g->vk.props.limits.timestampPeriod * 1e-6;
    for (int i = 0; i < g->nb_times; i++) {
        uint64_t d = (ts[i + 1] - ts[i]) & g->vk.ts_mask;
        g->kernel_ms[g->time_kernel[i]] += (double)d * period;
        g->kernel_calls[g->time_kernel[i]]++;
    }
    g->nb_times = 0;
}

void ml_gpu_discard(MLGpu *g)
{
    if (!g->recording)
        return;
    vkEndCommandBuffer(g->cmd);
    vkResetCommandBuffer(g->cmd, 0);
    g->recording = false;
    g->nb_times = 0;
}

int ml_gpu_run(MLGpu *g)
{
    if (g->lost) {
        ml_gpu_discard(g);
        return CB_ERROR(EIO);
    }
    if (!g->recording)
        return 0;

    VkMemoryBarrier2 mb = {
        .sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_COPY_BIT |
                         VK_PIPELINE_STAGE_2_CLEAR_BIT,
        .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .dstStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_COPY_BIT |
                         VK_PIPELINE_STAGE_2_CLEAR_BIT | VK_PIPELINE_STAGE_2_HOST_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                         VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
                         VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT |
                         VK_ACCESS_2_HOST_READ_BIT,
    };
    VkDependencyInfo dep = {
        .sType              = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .memoryBarrierCount = 1,
        .pMemoryBarriers    = &mb,
    };
    vkCmdPipelineBarrier2(g->cmd, &dep);
    VkResult res = vkEndCommandBuffer(g->cmd);
    g->recording = false;
    if (res != VK_SUCCESS) {
        cb_warn("ml: recording failed: %s", cb_vk_strerror(res));
        vkResetCommandBuffer(g->cmd, 0);
        g->nb_times = 0;
        return cb_vk_ret2err(res);
    }

    uint64_t v = g->submitted + 1;
    VkSemaphoreSubmitInfo signal = {
        .sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .semaphore = g->timeline,
        .value     = v,
        .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
    };
    VkCommandBufferSubmitInfo cinfo = {
        .sType         = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
        .commandBuffer = g->cmd,
    };
    VkSubmitInfo2 info = {
        .sType                    = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .commandBufferInfoCount   = 1,
        .pCommandBufferInfos      = &cinfo,
        .signalSemaphoreInfoCount = 1,
        .pSignalSemaphoreInfos    = &signal,
    };
    res = vkQueueSubmit2(g->vk.queue, 1, &info, VK_NULL_HANDLE);
    if (res != VK_SUCCESS) {
        cb_warn("ml: submission failed: %s", cb_vk_strerror(res));
        vkResetCommandBuffer(g->cmd, 0);
        g->nb_times = 0;
        return cb_vk_ret2err(res);
    }
    g->submitted = v;

    /* A layer of the model takes milliseconds; a minute means the GPU is
     * hung. */
    VkSemaphoreWaitInfo winfo = {
        .sType          = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
        .semaphoreCount = 1,
        .pSemaphores    = &g->timeline,
        .pValues        = &v,
    };
    res = vkWaitSemaphores(g->vk.dev, &winfo, UINT64_C(60000000000));
    if (res != VK_SUCCESS) {
        /* The command buffer may still be running, or never end: wait
         * until the device is idle (a hung GPU is reset by its driver,
         * and the wait ends with the device lost) before its buffers can
         * be freed, and use the context no more. */
        cb_err("ml: waiting for the GPU failed: %s; the model stops", cb_vk_strerror(res));
        vkDeviceWaitIdle(g->vk.dev);
        g->lost = true;
        vkResetCommandBuffer(g->cmd, 0);
        g->nb_times = 0;
        return cb_vk_ret2err(res);
    }
    read_times(g);
    vkResetCommandBuffer(g->cmd, 0);
    return 0;
}

void ml_gpu_profile_print(MLGpu *g)
{
    if (!g->profile)
        return;
    double total = 0.0;
    for (int k = 0; k < ML_NB_KERNELS; k++)
        total += g->kernel_ms[k];
    cb_info("ml: GPU time by kernel, %.1f ms in all:", total);
    for (int k = 0; k < ML_NB_KERNELS; k++) {
        if (!g->kernel_calls[k])
            continue;
        cb_info("  %-14s %9.1f ms %5.1f %%  %7lld calls", kernels[k].name, g->kernel_ms[k],
                total > 0 ? 100.0 * g->kernel_ms[k] / total : 0.0,
                (long long)g->kernel_calls[k]);
    }
}
