/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* The compute context (cyanbox/compute.h): a Vulkan instance with no surface,
 * the best device that can run the analysis' shaders, one compute queue,
 * VMA (all set up by vkdev.c, as the window's and the model's are), a
 * timeline semaphore and the pipelines.
 *
 * Choices, and why:
 *  - Its own instance and device, also in the interface. Sharing the
 *    window's VkDevice would put the analysis' submissions, made from the
 *    jobs pool's worker threads, on the queue the frame loop owns: every
 *    frame would have to take their lock; and a device lost to a hung
 *    shader would take the window with it. Two devices on one GPU cost a
 *    few MB; the device is picked by the same name (--device).
 *  - A compute-only queue family when the device has one (AMD's
 *    asynchronous compute queues): the desktop's and the window's graphics
 *    work is not held up behind a transform, nor it behind them.
 *  - Discrete before integrated; a CPU implementation (llvmpipe) only when
 *    named: on the CPU the shaders would be slower than the C they replace
 *    and would compete with the analyses' own threads.
 *  - One queue, a mutex around vkQueueSubmit2, a timeline semaphore that
 *    submission i signals to i: a thread waits for its own submission
 *    alone, never for another thread's work and never on a fence it would
 *    have to reset. Command pools belong to the plans (compute_dsp.c), one
 *    per thread of use, so recording needs no lock. VMA locks itself.
 *  - A wait that fails (a minute without an answer, or the device lost)
 *    waits for the whole device to go idle, under the queue's lock, before
 *    it returns: the caller frees the buffers that submission uses, which
 *    the GPU might otherwise still write. The context is then lost: every
 *    later submission fails with -EIO, and the analyses go on on the
 *    CPU.
 *  - No descriptors: every shader takes its buffers as device addresses in
 *    its push constants, as the waveform shader does, so one pipeline
 *    layout serves all and there are no descriptor pools to manage.
 *  - Pipelines are made on first use for the transform size asked (a
 *    specialisation constant), under a lock, and kept until the end. */

#include "compute_priv.h"

#include <stdlib.h>
#include <string.h>

#include "stft_comp.spv.h"
#include "env_comp.spv.h"

/* ----------------------------------------------------------------- options */

void cb_compute_options_default(CBComputeOptions *o)
{
    *o = (CBComputeOptions) { 0 };
}

enum CBComputeMode cb_compute_mode(void)
{
    const char *env = getenv("CYANBOX_ANALYSIS_GPU");
    if (!env || !env[0])
        return CB_COMPUTE_AUTO;
    if (!strcmp(env, "0"))
        return CB_COMPUTE_OFF;
    if (!strcmp(env, "1"))
        return CB_COMPUTE_REQUIRED;
    cb_warn("compute: CYANBOX_ANALYSIS_GPU is \"%s\", not 0 or 1; ignored", env);
    return CB_COMPUTE_AUTO;
}

/* ------------------------------------------------------------------ device */

/* The shaders' needs: an FFT of 4096 complex points in shared memory
 * (32 kB), 256 invocations a workgroup, buffer device addresses, and a
 * timeline semaphore. */
#define NEED_SHARED (4096 * 8)

/* Why a device cannot run the shaders, or NULL (CBVkDevCheck); `qfam`
 * gets the queue family to use: compute-only if there is one, else any
 * with compute. */
static const char *device_unusable(void *opaque, VkPhysicalDevice pd,
                                   const VkPhysicalDeviceProperties *props, uint32_t *qfam)
{
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
    if (!f12.bufferDeviceAddress)
        return "no bufferDeviceAddress";
    if (!f12.timelineSemaphore)
        return "no timelineSemaphore";
    if (!f13.synchronization2)
        return "no synchronization2";

    const VkPhysicalDeviceLimits *l = &props->limits;
    if (l->maxComputeSharedMemorySize < NEED_SHARED)
        return "too little shared memory";
    if (l->maxComputeWorkGroupInvocations < CB_COMPUTE_THREADS ||
        l->maxComputeWorkGroupSize[0] < CB_COMPUTE_THREADS)
        return "workgroups too small";
    if (l->maxPushConstantsSize < CB_COMPUTE_PUSH_SIZE)
        return "push constants too small";
    return cb_vkdev_compute_family(pd, qfam);
}

static int create_device(CBCompute *c)
{
    VkPhysicalDeviceVulkan13Features f13 = {
        .sType            = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
        .synchronization2 = VK_TRUE,
    };
    VkPhysicalDeviceVulkan12Features f12 = {
        .sType               = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
        .pNext               = &f13,
        .bufferDeviceAddress = VK_TRUE,
        .timelineSemaphore   = VK_TRUE,
    };
    VkPhysicalDeviceFeatures2 f2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext = &f12,
    };
    int ret = cb_vkdev_device(&c->vk, &f2, NULL, 0);
    if (ret < 0 || (ret = cb_vkdev_timeline(&c->vk, &c->timeline)) < 0)
        return ret;

    VkPushConstantRange range = {
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .size       = CB_COMPUTE_PUSH_SIZE,
    };
    VkPipelineLayoutCreateInfo linfo = {
        .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges    = &range,
    };
    return cb_vk_ret2err(vkCreatePipelineLayout(c->vk.dev, &linfo, NULL, &c->layout));
}

int cb_compute_create(CBCompute **out, const CBComputeOptions *opt)
{
    *out = NULL;
    if (cb_compute_mode() == CB_COMPUTE_OFF) {
        cb_debug("compute: CYANBOX_ANALYSIS_GPU=0, no GPU");
        return CB_ERROR(ENODEV);
    }

    CBComputeOptions defopt;
    if (!opt) {
        cb_compute_options_default(&defopt);
        opt = &defopt;
    }
    const char *want = opt->device && opt->device[0] ? opt->device : getenv("CYANBOX_GPU_DEVICE");
    if (want && !want[0])
        want = NULL;
    const char *val = getenv("CYANBOX_GPU_VALIDATION");
    bool validation = opt->validation || (val && !strcmp(val, "1"));

    CBCompute *c = calloc(1, sizeof(*c));
    if (!c)
        return CB_ERROR(ENOMEM);
    atomic_init(&c->lost, false);
    int ret = pthread_mutex_init(&c->qlock, NULL);
    if (ret) {
        free(c);
        return CB_ERROR(ret);
    }
    ret = pthread_mutex_init(&c->plock, NULL);
    if (ret) {
        pthread_mutex_destroy(&c->qlock);
        free(c);
        return CB_ERROR(ret);
    }

    CBVkDevOptions vo = {
        .tag         = "compute",
        .validation  = validation,
        .performance = true,
        .device      = want,
        .check       = device_unusable,
    };
    if ((ret = cb_vkdev_instance(&c->vk, &vo)) < 0)
        goto fail;
    if ((ret = cb_vkdev_pick(&c->vk, &vo)) < 0) {
        cb_debug("compute: no Vulkan 1.3 device can run the analysis shaders");
        goto fail;
    }
    if ((ret = create_device(c)) < 0)
        goto fail;

    cb_debug("compute: %s, Vulkan %u.%u, queue family %u%s", c->vk.props.deviceName,
             VK_API_VERSION_MAJOR(c->vk.props.apiVersion),
             VK_API_VERSION_MINOR(c->vk.props.apiVersion), c->vk.qfam,
             c->vk.ts_mask ? "" : " (no timestamps)");
    *out = c;
    return 0;

fail:
    cb_compute_destroy(&c);
    return ret;
}

void cb_compute_destroy(CBCompute **pc)
{
    CBCompute *c = *pc;
    if (!c)
        return;

    if (c->vk.dev) {
        vkDeviceWaitIdle(c->vk.dev);
        for (int k = 0; k < CB_NB_KERNELS; k++) {
            for (int i = 0; i <= CB_COMPUTE_MAX_LOG2; i++) {
                if (c->pipes[k][i])
                    vkDestroyPipeline(c->vk.dev, c->pipes[k][i], NULL);
            }
        }
        if (c->layout)
            vkDestroyPipelineLayout(c->vk.dev, c->layout, NULL);
        if (c->timeline)
            vkDestroySemaphore(c->vk.dev, c->timeline, NULL);
    }
    cb_vkdev_destroy(&c->vk);

    pthread_mutex_destroy(&c->plock);
    pthread_mutex_destroy(&c->qlock);
    free(c);
    *pc = NULL;
}

const char *cb_compute_device_name(const CBCompute *c)
{
    return c->vk.props.deviceName;
}

int cb_compute_validation_errors(const CBCompute *c)
{
    return cb_vkdev_validation_errors(&c->vk);
}

/* ----------------------------------------------------------------- buffers */

int cb_cbuf_create(CBCompute *c, CBCBuf *b, VkDeviceSize size, enum CBCMem mem)
{
    *b = (CBCBuf) { 0 };
    if (!size)
        return CB_ERROR(EINVAL);

    /* Every buffer can be a shader's storage (through its address) and
     * either end of a copy: which one is which depends on whether the
     * host memory is the device's own (an integrated GPU reads an upload
     * buffer directly; a discrete one copies it into its VRAM first). */
    VkBufferCreateInfo info = {
        .sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size        = size,
        .usage       = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                       VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                       VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };

    /* Host memory for both directions, never the BAR window into VRAM:
     * there the CPU's own writes would cross the bus at its speed, where a
     * copy in system memory is a memcpy and the GPU's copy engine moves it.
     * RANDOM asks for cached memory, which the CPU reads quickly. */
    VmaAllocationCreateInfo ci = { 0 };
    switch (mem) {
    case CB_CMEM_UPLOAD:
        ci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
        ci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                   VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
        break;
    case CB_CMEM_READBACK:
        ci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
        ci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                   VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
        break;
    case CB_CMEM_DEVICE:
    default:
        ci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        break;
    }

    VmaAllocationInfo ai = { 0 };
    VkResult res = vmaCreateBuffer(c->vk.vma, &info, &ci, &b->buf, &b->alloc, &ai);
    if (res != VK_SUCCESS) {
        cb_warn("compute: unable to allocate a %llu byte buffer: %s", (unsigned long long)size,
                cb_vk_strerror(res));
        *b = (CBCBuf) { 0 };
        return cb_vk_ret2err(res);
    }

    VkMemoryPropertyFlags props = 0;
    vmaGetAllocationMemoryProperties(c->vk.vma, b->alloc, &props);
    b->size = size;
    b->map = ai.pMappedData;
    b->device_local = props & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

    VkBufferDeviceAddressInfo ainfo = {
        .sType  = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
        .buffer = b->buf,
    };
    b->addr = vkGetBufferDeviceAddress(c->vk.dev, &ainfo);
    return 0;
}

void cb_cbuf_free(CBCompute *c, CBCBuf *b)
{
    if (!b || !b->buf)
        return;
    vmaDestroyBuffer(c->vk.vma, b->buf, b->alloc);
    *b = (CBCBuf) { 0 };
}

int cb_cbuf_invalidate(CBCompute *c, CBCBuf *b)
{
    if (!b->map)
        return 0;
    return cb_vk_ret2err(vmaInvalidateAllocation(c->vk.vma, b->alloc, 0, VK_WHOLE_SIZE));
}

int cb_cbuf_flush(CBCompute *c, CBCBuf *b, VkDeviceSize size)
{
    if (!b->map)
        return 0;
    return cb_vk_ret2err(vmaFlushAllocation(c->vk.vma, b->alloc, 0, size));
}

/* --------------------------------------------------------------- pipelines */

int cb_compute_pipeline(CBCompute *c, enum CBComputeKernel kernel, int log2n, VkPipeline *out)
{
    if (kernel < 0 || kernel >= CB_NB_KERNELS || log2n < CB_COMPUTE_MIN_LOG2 ||
        log2n > CB_COMPUTE_MAX_LOG2)
        return CB_ERROR(EINVAL);

    pthread_mutex_lock(&c->plock);
    if (c->pipes[kernel][log2n]) {
        *out = c->pipes[kernel][log2n];
        pthread_mutex_unlock(&c->plock);
        return 0;
    }

    const uint8_t *code = kernel == CB_KERNEL_STFT ? cb_stft_comp_spv : cb_env_comp_spv;
    size_t size = kernel == CB_KERNEL_STFT ? cb_stft_comp_spv_size : cb_env_comp_spv_size;
    VkShaderModuleCreateInfo minfo = {
        .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = size,
        .pCode    = (const uint32_t *)code,
    };
    VkShaderModule mod = VK_NULL_HANDLE;
    VkResult res = vkCreateShaderModule(c->vk.dev, &minfo, NULL, &mod);
    if (res != VK_SUCCESS) {
        pthread_mutex_unlock(&c->plock);
        return cb_vk_ret2err(res);
    }

    /* Constant 0 is the log2 of the complex transform the real one of
     * 2^log2n points is computed with (shaders/fft.glsl). */
    const uint32_t log2m = (uint32_t)(log2n - 1);
    const VkSpecializationMapEntry entry = {
        .constantID = 0,
        .offset     = 0,
        .size       = sizeof(log2m),
    };
    const VkSpecializationInfo spec = {
        .mapEntryCount = 1,
        .pMapEntries   = &entry,
        .dataSize      = sizeof(log2m),
        .pData         = &log2m,
    };
    VkComputePipelineCreateInfo pinfo = {
        .sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage  = {
            .sType               = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage               = VK_SHADER_STAGE_COMPUTE_BIT,
            .module              = mod,
            .pName               = "main",
            .pSpecializationInfo = &spec,
        },
        .layout = c->layout,
    };
    VkPipeline pipe = VK_NULL_HANDLE;
    res = vkCreateComputePipelines(c->vk.dev, VK_NULL_HANDLE, 1, &pinfo, NULL, &pipe);
    vkDestroyShaderModule(c->vk.dev, mod, NULL);
    if (res == VK_SUCCESS)
        c->pipes[kernel][log2n] = pipe;
    pthread_mutex_unlock(&c->plock);
    if (res != VK_SUCCESS) {
        cb_warn("compute: unable to create a pipeline: %s", cb_vk_strerror(res));
        return cb_vk_ret2err(res);
    }

    *out = pipe;
    return 0;
}

/* ------------------------------------------------------------- submission */

int cb_compute_submit(CBCompute *c, VkCommandBuffer cmd, uint64_t *value)
{
    pthread_mutex_lock(&c->qlock);
    if (atomic_load(&c->lost)) {
        pthread_mutex_unlock(&c->qlock);
        return CB_ERROR(EIO);
    }
    uint64_t v = c->submitted + 1;
    VkSemaphoreSubmitInfo signal = {
        .sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .semaphore = c->timeline,
        .value     = v,
        .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
    };
    VkCommandBufferSubmitInfo cinfo = {
        .sType         = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
        .commandBuffer = cmd,
    };
    VkSubmitInfo2 info = {
        .sType                    = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .commandBufferInfoCount   = 1,
        .pCommandBufferInfos      = &cinfo,
        .signalSemaphoreInfoCount = 1,
        .pSignalSemaphoreInfos    = &signal,
    };
    VkResult res = vkQueueSubmit2(c->vk.queue, 1, &info, VK_NULL_HANDLE);
    if (res == VK_SUCCESS)
        c->submitted = v;
    pthread_mutex_unlock(&c->qlock);

    if (res != VK_SUCCESS) {
        cb_warn("compute: submission failed: %s", cb_vk_strerror(res));
        return cb_vk_ret2err(res);
    }
    *value = v;
    return 0;
}

int cb_compute_wait(CBCompute *c, uint64_t value)
{
    /* A transform of a whole stretch takes milliseconds; a minute means
     * the GPU is hung, and the analysis goes on without it. */
    const uint64_t timeout_ns = UINT64_C(60000000000);
    VkSemaphoreWaitInfo info = {
        .sType          = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
        .semaphoreCount = 1,
        .pSemaphores    = &c->timeline,
        .pValues        = &value,
    };
    VkResult res = vkWaitSemaphores(c->vk.dev, &info, timeout_ns);
    if (res != VK_SUCCESS) {
        /* The submission may still be running, or never end (a hung GPU
         * is reset by its driver, and then the device is lost): nothing it
         * uses may be freed before the device is idle. vkDeviceWaitIdle
         * needs the queue to itself. */
        if (!atomic_exchange(&c->lost, true))
            cb_err("compute: waiting for the GPU failed: %s; the GPU is not used again",
                   cb_vk_strerror(res));
        pthread_mutex_lock(&c->qlock);
        vkDeviceWaitIdle(c->vk.dev);
        pthread_mutex_unlock(&c->qlock);
        return cb_vk_ret2err(res);
    }
    return 0;
}
