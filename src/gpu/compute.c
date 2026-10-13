/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* The compute context (rxbox/compute.h): a Vulkan instance with no surface,
 * the best device that can run the analysis' shaders, one compute queue,
 * VMA, a timeline semaphore and the pipelines. Cut from vk.c's device code
 * (itself libcraw's tests/vkdev.c), without SDL and without presentation.
 *
 * Choices, and why:
 *  - Its own instance and device, also in the interface. Sharing the
 *    window's VkDevice would put the analysis' submissions, made from the
 *    jobs pool's worker threads, on the queue the frame loop owns: every
 *    frame would have to take their lock. Two devices on one GPU cost a
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

/* ------------------------------------------------------------------ errors */

int rx_compute_ret2err(VkResult res)
{
    switch (res) {
    case VK_SUCCESS:
        return 0;
    case VK_ERROR_OUT_OF_HOST_MEMORY:
    case VK_ERROR_OUT_OF_DEVICE_MEMORY:
        return RX_ERROR(ENOMEM);
    case VK_ERROR_DEVICE_LOST:
        return RX_ERROR(EIO);
    case VK_ERROR_INITIALIZATION_FAILED:
    case VK_ERROR_INCOMPATIBLE_DRIVER:
    case VK_ERROR_EXTENSION_NOT_PRESENT:
    case VK_ERROR_FEATURE_NOT_PRESENT:
    case VK_ERROR_LAYER_NOT_PRESENT:
        return RX_ERROR(ENODEV);
    case VK_TIMEOUT:
    case VK_NOT_READY:
        return RX_ERROR(ETIMEDOUT);
    default:
        return RX_ERROR(EINVAL);
    }
}

const char *rx_compute_strerror(VkResult res)
{
    switch (res) {
    case VK_SUCCESS:                      return "success";
    case VK_TIMEOUT:                      return "timeout";
    case VK_ERROR_OUT_OF_HOST_MEMORY:     return "out of host memory";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY:   return "out of device memory";
    case VK_ERROR_INITIALIZATION_FAILED:  return "initialization failed";
    case VK_ERROR_DEVICE_LOST:            return "device lost";
    case VK_ERROR_LAYER_NOT_PRESENT:      return "layer not present";
    case VK_ERROR_EXTENSION_NOT_PRESENT:  return "extension not present";
    case VK_ERROR_FEATURE_NOT_PRESENT:    return "feature not present";
    case VK_ERROR_INCOMPATIBLE_DRIVER:    return "incompatible driver";
    default:                              return "error";
    }
}

/* ----------------------------------------------------------------- options */

void rx_compute_options_default(RXComputeOptions *o)
{
    *o = (RXComputeOptions) { 0 };
}

enum RXComputeMode rx_compute_mode(void)
{
    const char *env = getenv("RXBOX_ANALYSIS_GPU");
    if (!env || !env[0])
        return RX_COMPUTE_AUTO;
    if (!strcmp(env, "0"))
        return RX_COMPUTE_OFF;
    if (!strcmp(env, "1"))
        return RX_COMPUTE_REQUIRED;
    rx_warn("compute: RXBOX_ANALYSIS_GPU is \"%s\", not 0 or 1; ignored", env);
    return RX_COMPUTE_AUTO;
}

/* -------------------------------------------------------------- validation */

static VKAPI_ATTR VkBool32 VKAPI_CALL debug_cb(VkDebugUtilsMessageSeverityFlagBitsEXT sev,
                                               VkDebugUtilsMessageTypeFlagsEXT type,
                                               const VkDebugUtilsMessengerCallbackDataEXT *data,
                                               void *opaque)
{
    RXCompute *c = opaque;

    if (sev & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        atomic_fetch_add(&c->validation_errors, 1);
        rx_err("compute: %s", data->pMessage);
    } else if (sev & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        rx_warn("compute: %s", data->pMessage);
    } else {
        rx_debug("compute: %s", data->pMessage);
    }

    return VK_FALSE;
}

static bool have_layer(const char *name)
{
    uint32_t nb = 0;
    if (vkEnumerateInstanceLayerProperties(&nb, NULL) != VK_SUCCESS || !nb)
        return false;

    VkLayerProperties *props = calloc(nb, sizeof(*props));
    if (!props)
        return false;

    bool found = false;
    if (vkEnumerateInstanceLayerProperties(&nb, props) == VK_SUCCESS) {
        for (uint32_t i = 0; i < nb && !found; i++)
            found = !strcmp(props[i].layerName, name);
    }

    free(props);
    return found;
}

static bool have_instance_ext(const char *layer, const char *name)
{
    uint32_t nb = 0;
    if (vkEnumerateInstanceExtensionProperties(layer, &nb, NULL) != VK_SUCCESS || !nb)
        return false;

    VkExtensionProperties *props = calloc(nb, sizeof(*props));
    if (!props)
        return false;

    bool found = false;
    if (vkEnumerateInstanceExtensionProperties(layer, &nb, props) == VK_SUCCESS) {
        for (uint32_t i = 0; i < nb && !found; i++)
            found = !strcmp(props[i].extensionName, name);
    }

    free(props);
    return found;
}

/* ---------------------------------------------------------------- instance */

static int create_instance(RXCompute *c, bool validation)
{
    uint32_t version = 0;
    if (vkEnumerateInstanceVersion(&version) != VK_SUCCESS || version < VK_API_VERSION_1_3) {
        rx_debug("compute: the Vulkan loader is older than 1.3");
        return RX_ERROR(ENODEV);
    }

    const char *exts[2];
    uint32_t nb_exts = 0;
    const char *layers[1];
    uint32_t nb_layers = 0;
    bool debug_utils = false, features = false;
    if (validation) {
        if (have_layer("VK_LAYER_KHRONOS_validation")) {
            layers[nb_layers++] = "VK_LAYER_KHRONOS_validation";
            features = have_instance_ext("VK_LAYER_KHRONOS_validation",
                                         VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
            if (features)
                exts[nb_exts++] = VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME;
        } else {
            rx_warn("compute: VK_LAYER_KHRONOS_validation is not installed");
        }
        debug_utils = have_instance_ext(NULL, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) ||
                      (nb_layers && have_instance_ext("VK_LAYER_KHRONOS_validation",
                                                      VK_EXT_DEBUG_UTILS_EXTENSION_NAME));
        if (debug_utils)
            exts[nb_exts++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    }

    /* Synchronisation validation too: the barriers between the uploads,
     * the dispatches and the host's reads are what this file and
     * compute_dsp.c get wrong if they get anything wrong. */
    const VkValidationFeatureEnableEXT enables[] = {
        VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT,
    };
    VkValidationFeaturesEXT vfeat = {
        .sType                         = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT,
        .enabledValidationFeatureCount = RX_ARRAY_ELEMS(enables),
        .pEnabledValidationFeatures    = enables,
    };

    VkApplicationInfo app = {
        .sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName   = "rxbox",
        .applicationVersion = 1,
        .pEngineName        = "rxbox",
        .engineVersion      = 1,
        .apiVersion         = VK_API_VERSION_1_3,
    };
    VkInstanceCreateInfo info = {
        .sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pNext                   = features ? &vfeat : NULL,
        .pApplicationInfo        = &app,
        .enabledLayerCount       = nb_layers,
        .ppEnabledLayerNames     = nb_layers ? layers : NULL,
        .enabledExtensionCount   = nb_exts,
        .ppEnabledExtensionNames = nb_exts ? exts : NULL,
    };
    VkResult res = vkCreateInstance(&info, NULL, &c->inst);
    if (res != VK_SUCCESS) {
        rx_debug("compute: no Vulkan instance: %s", rx_compute_strerror(res));
        c->inst = VK_NULL_HANDLE;
        return rx_compute_ret2err(res);
    }

    if (debug_utils) {
        PFN_vkCreateDebugUtilsMessengerEXT create = (PFN_vkCreateDebugUtilsMessengerEXT)
            vkGetInstanceProcAddr(c->inst, "vkCreateDebugUtilsMessengerEXT");
        VkDebugUtilsMessengerCreateInfoEXT dinfo = {
            .sType           = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
            .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
                               VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT,
            .messageType     = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                               VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                               VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
            .pfnUserCallback = debug_cb,
            .pUserData       = c,
        };
        if (create)
            create(c->inst, &dinfo, NULL, &c->messenger);
    }
    if (nb_layers)
        rx_info("compute: validation enabled%s", features ? " (with synchronisation checks)" : "");

    return 0;
}

/* ------------------------------------------------------------------ device */

/* The shaders' needs: an FFT of 4096 complex points in shared memory
 * (32 kB), 256 invocations a workgroup, buffer device addresses, and a
 * timeline semaphore. */
#define NEED_SHARED (4096 * 8)

/* Why a device cannot run the shaders, or NULL; `qfam` gets the queue
 * family to use: compute-only if there is one, else any with compute. */
static const char *device_unusable(VkPhysicalDevice pd, const VkPhysicalDeviceProperties *props,
                                   uint32_t *qfam)
{
    if (props->apiVersion < VK_API_VERSION_1_3)
        return "Vulkan 1.3 is required";

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
    if (l->maxComputeWorkGroupInvocations < RX_COMPUTE_THREADS ||
        l->maxComputeWorkGroupSize[0] < RX_COMPUTE_THREADS)
        return "workgroups too small";
    if (l->maxPushConstantsSize < RX_COMPUTE_PUSH_SIZE)
        return "push constants too small";

    uint32_t nb = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nb, NULL);
    VkQueueFamilyProperties *qf = calloc(RX_MAX(nb, 1), sizeof(*qf));
    if (!qf)
        return "out of memory";
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nb, qf);
    int any = -1, only = -1;
    for (uint32_t i = 0; i < nb; i++) {
        if (!(qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) || !qf[i].queueCount)
            continue;
        if (any < 0)
            any = (int)i;
        if (only < 0 && !(qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT))
            only = (int)i;
    }
    free(qf);
    if (any < 0)
        return "no compute queue";

    *qfam = (uint32_t)(only >= 0 ? only : any);
    return NULL;
}

static int type_score(VkPhysicalDeviceType type)
{
    switch (type) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   return 4;
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 3;
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    return 2;
    default:                                     return 0;
    }
}

/* As vk.c: a named device (a case-insensitive substring) wins; otherwise,
 * or when none matches (with a warning), discrete before integrated. */
static int pick_device(RXCompute *c, const char *want)
{
    uint32_t nb = 0;
    VkResult res = vkEnumeratePhysicalDevices(c->inst, &nb, NULL);
    if (res != VK_SUCCESS || !nb) {
        rx_debug("compute: no Vulkan devices");
        return RX_ERROR(ENODEV);
    }

    VkPhysicalDevice *devs = calloc(nb, sizeof(*devs));
    if (!devs)
        return RX_ERROR(ENOMEM);
    vkEnumeratePhysicalDevices(c->inst, &nb, devs);

    int best_score = -1;
    bool named = false;
    for (uint32_t i = 0; i < nb; i++) {
        VkPhysicalDeviceProperties props;
        uint32_t qfam = 0;

        vkGetPhysicalDeviceProperties(devs[i], &props);
        bool match = want && rx_str_contains_fold(props.deviceName, want);
        const char *why = device_unusable(devs[i], &props, &qfam);
        if (!why && props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU && !match)
            why = "a CPU implementation (used only when named)";
        if (why) {
            rx_debug("compute: %s: %s", props.deviceName, why);
            if (match)
                rx_warn("compute: %s cannot be used: %s", props.deviceName, why);
            continue;
        }

        int score = type_score(props.deviceType) + (match ? 100 : 0);
        rx_debug("compute: %s: usable, score %d", props.deviceName, score);
        if (score > best_score) {
            best_score = score;
            named = match;
            c->phys = devs[i];
            c->props = props;
            c->qfam = qfam;
        }
    }
    free(devs);

    if (best_score < 0) {
        rx_debug("compute: no Vulkan 1.3 device can run the analysis shaders");
        return RX_ERROR(ENODEV);
    }
    if (want && !named)
        rx_warn("compute: no usable device matches \"%s\"; using %s", want, c->props.deviceName);

    return 0;
}

static int create_device(RXCompute *c)
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

    const float prio = 1.0f;
    VkDeviceQueueCreateInfo qinfo = {
        .sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = c->qfam,
        .queueCount       = 1,
        .pQueuePriorities = &prio,
    };
    VkDeviceCreateInfo info = {
        .sType                = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext                = &f2,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos    = &qinfo,
    };
    VkResult res = vkCreateDevice(c->phys, &info, NULL, &c->dev);
    if (res != VK_SUCCESS) {
        rx_warn("compute: unable to create a device on %s: %s", c->props.deviceName,
                rx_compute_strerror(res));
        c->dev = VK_NULL_HANDLE;
        return rx_compute_ret2err(res);
    }
    vkGetDeviceQueue(c->dev, c->qfam, 0, &c->queue);

    uint32_t nb_qf = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(c->phys, &nb_qf, NULL);
    VkQueueFamilyProperties *qf = calloc(RX_MAX(nb_qf, 1), sizeof(*qf));
    if (!qf)
        return RX_ERROR(ENOMEM);
    vkGetPhysicalDeviceQueueFamilyProperties(c->phys, &nb_qf, qf);
    uint32_t ts_bits = c->qfam < nb_qf ? qf[c->qfam].timestampValidBits : 0;
    free(qf);
    c->ts_mask = ts_bits >= 64 ? UINT64_MAX : ts_bits ? (UINT64_C(1) << ts_bits) - 1 : 0;

    VmaVulkanFunctions fn = {
        .vkGetInstanceProcAddr = vkGetInstanceProcAddr,
        .vkGetDeviceProcAddr   = vkGetDeviceProcAddr,
    };
    VmaAllocatorCreateInfo vinfo = {
        .flags            = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT,
        .physicalDevice   = c->phys,
        .device           = c->dev,
        .pVulkanFunctions = &fn,
        .instance         = c->inst,
        .vulkanApiVersion = VK_API_VERSION_1_3,
    };
    res = vmaCreateAllocator(&vinfo, &c->vma);
    if (res != VK_SUCCESS) {
        rx_warn("compute: unable to create the VMA allocator: %s", rx_compute_strerror(res));
        c->vma = NULL;
        return rx_compute_ret2err(res);
    }

    VkSemaphoreTypeCreateInfo tinfo = {
        .sType         = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
        .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
        .initialValue  = 0,
    };
    VkSemaphoreCreateInfo sinfo = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        .pNext = &tinfo,
    };
    res = vkCreateSemaphore(c->dev, &sinfo, NULL, &c->timeline);
    if (res != VK_SUCCESS)
        return rx_compute_ret2err(res);

    VkPushConstantRange range = {
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .size       = RX_COMPUTE_PUSH_SIZE,
    };
    VkPipelineLayoutCreateInfo linfo = {
        .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges    = &range,
    };
    return rx_compute_ret2err(vkCreatePipelineLayout(c->dev, &linfo, NULL, &c->layout));
}

int rx_compute_create(RXCompute **out, const RXComputeOptions *opt)
{
    *out = NULL;
    if (rx_compute_mode() == RX_COMPUTE_OFF) {
        rx_debug("compute: RXBOX_ANALYSIS_GPU=0, no GPU");
        return RX_ERROR(ENODEV);
    }

    RXComputeOptions defopt;
    if (!opt) {
        rx_compute_options_default(&defopt);
        opt = &defopt;
    }
    const char *want = opt->device && opt->device[0] ? opt->device : getenv("RXBOX_GPU_DEVICE");
    if (want && !want[0])
        want = NULL;
    const char *val = getenv("RXBOX_GPU_VALIDATION");
    bool validation = opt->validation || (val && !strcmp(val, "1"));

    RXCompute *c = calloc(1, sizeof(*c));
    if (!c)
        return RX_ERROR(ENOMEM);
    atomic_init(&c->validation_errors, 0);
    atomic_init(&c->lost, false);
    int ret = pthread_mutex_init(&c->qlock, NULL);
    if (ret) {
        free(c);
        return RX_ERROR(ret);
    }
    ret = pthread_mutex_init(&c->plock, NULL);
    if (ret) {
        pthread_mutex_destroy(&c->qlock);
        free(c);
        return RX_ERROR(ret);
    }

    if ((ret = create_instance(c, validation)) < 0)
        goto fail;
    if ((ret = pick_device(c, want)) < 0)
        goto fail;
    if ((ret = create_device(c)) < 0)
        goto fail;

    rx_debug("compute: %s, Vulkan %u.%u, queue family %u%s", c->props.deviceName,
            VK_API_VERSION_MAJOR(c->props.apiVersion), VK_API_VERSION_MINOR(c->props.apiVersion),
            c->qfam, c->ts_mask ? "" : " (no timestamps)");
    *out = c;
    return 0;

fail:
    rx_compute_destroy(&c);
    return ret;
}

void rx_compute_destroy(RXCompute **pc)
{
    RXCompute *c = *pc;
    if (!c)
        return;

    if (c->dev) {
        vkDeviceWaitIdle(c->dev);
        for (int k = 0; k < RX_NB_KERNELS; k++) {
            for (int i = 0; i <= RX_COMPUTE_MAX_LOG2; i++) {
                if (c->pipes[k][i])
                    vkDestroyPipeline(c->dev, c->pipes[k][i], NULL);
            }
        }
        if (c->layout)
            vkDestroyPipelineLayout(c->dev, c->layout, NULL);
        if (c->timeline)
            vkDestroySemaphore(c->dev, c->timeline, NULL);
        if (c->vma)
            vmaDestroyAllocator(c->vma);
        vkDestroyDevice(c->dev, NULL);
    }

    if (c->messenger) {
        PFN_vkDestroyDebugUtilsMessengerEXT destroy = (PFN_vkDestroyDebugUtilsMessengerEXT)
            vkGetInstanceProcAddr(c->inst, "vkDestroyDebugUtilsMessengerEXT");
        if (destroy)
            destroy(c->inst, c->messenger, NULL);
    }
    if (c->inst)
        vkDestroyInstance(c->inst, NULL);

    pthread_mutex_destroy(&c->plock);
    pthread_mutex_destroy(&c->qlock);
    free(c);
    *pc = NULL;
}

const char *rx_compute_device_name(const RXCompute *c)
{
    return c->props.deviceName;
}

int rx_compute_validation_errors(const RXCompute *c)
{
    return atomic_load(&((RXCompute *)c)->validation_errors);
}

/* ----------------------------------------------------------------- buffers */

int rx_cbuf_create(RXCompute *c, RXCBuf *b, VkDeviceSize size, enum RXCMem mem)
{
    *b = (RXCBuf) { 0 };
    if (!size)
        return RX_ERROR(EINVAL);

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
    case RX_CMEM_UPLOAD:
        ci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
        ci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                   VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
        break;
    case RX_CMEM_READBACK:
        ci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
        ci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                   VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
        break;
    case RX_CMEM_DEVICE:
    default:
        ci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        break;
    }

    VmaAllocationInfo ai = { 0 };
    VkResult res = vmaCreateBuffer(c->vma, &info, &ci, &b->buf, &b->alloc, &ai);
    if (res != VK_SUCCESS) {
        rx_warn("compute: unable to allocate a %llu byte buffer: %s", (unsigned long long)size,
                rx_compute_strerror(res));
        *b = (RXCBuf) { 0 };
        return rx_compute_ret2err(res);
    }

    VkMemoryPropertyFlags props = 0;
    vmaGetAllocationMemoryProperties(c->vma, b->alloc, &props);
    b->size = size;
    b->map = ai.pMappedData;
    b->device_local = props & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

    VkBufferDeviceAddressInfo ainfo = {
        .sType  = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
        .buffer = b->buf,
    };
    b->addr = vkGetBufferDeviceAddress(c->dev, &ainfo);
    return 0;
}

void rx_cbuf_free(RXCompute *c, RXCBuf *b)
{
    if (!b || !b->buf)
        return;
    vmaDestroyBuffer(c->vma, b->buf, b->alloc);
    *b = (RXCBuf) { 0 };
}

int rx_cbuf_invalidate(RXCompute *c, RXCBuf *b)
{
    if (!b->map)
        return 0;
    return rx_compute_ret2err(vmaInvalidateAllocation(c->vma, b->alloc, 0, VK_WHOLE_SIZE));
}

int rx_cbuf_flush(RXCompute *c, RXCBuf *b, VkDeviceSize size)
{
    if (!b->map)
        return 0;
    return rx_compute_ret2err(vmaFlushAllocation(c->vma, b->alloc, 0, size));
}

/* --------------------------------------------------------------- pipelines */

int rx_compute_pipeline(RXCompute *c, enum RXComputeKernel kernel, int log2n, VkPipeline *out)
{
    if (kernel < 0 || kernel >= RX_NB_KERNELS || log2n < RX_COMPUTE_MIN_LOG2 ||
        log2n > RX_COMPUTE_MAX_LOG2)
        return RX_ERROR(EINVAL);

    pthread_mutex_lock(&c->plock);
    if (c->pipes[kernel][log2n]) {
        *out = c->pipes[kernel][log2n];
        pthread_mutex_unlock(&c->plock);
        return 0;
    }

    const uint8_t *code = kernel == RX_KERNEL_STFT ? rx_stft_comp_spv : rx_env_comp_spv;
    size_t size = kernel == RX_KERNEL_STFT ? rx_stft_comp_spv_size : rx_env_comp_spv_size;
    VkShaderModuleCreateInfo minfo = {
        .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = size,
        .pCode    = (const uint32_t *)code,
    };
    VkShaderModule mod = VK_NULL_HANDLE;
    VkResult res = vkCreateShaderModule(c->dev, &minfo, NULL, &mod);
    if (res != VK_SUCCESS) {
        pthread_mutex_unlock(&c->plock);
        return rx_compute_ret2err(res);
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
    res = vkCreateComputePipelines(c->dev, VK_NULL_HANDLE, 1, &pinfo, NULL, &pipe);
    vkDestroyShaderModule(c->dev, mod, NULL);
    if (res == VK_SUCCESS)
        c->pipes[kernel][log2n] = pipe;
    pthread_mutex_unlock(&c->plock);
    if (res != VK_SUCCESS) {
        rx_warn("compute: unable to create a pipeline: %s", rx_compute_strerror(res));
        return rx_compute_ret2err(res);
    }

    *out = pipe;
    return 0;
}

/* ------------------------------------------------------------- submission */

int rx_compute_submit(RXCompute *c, VkCommandBuffer cmd, uint64_t *value)
{
    pthread_mutex_lock(&c->qlock);
    if (atomic_load(&c->lost)) {
        pthread_mutex_unlock(&c->qlock);
        return RX_ERROR(EIO);
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
    VkResult res = vkQueueSubmit2(c->queue, 1, &info, VK_NULL_HANDLE);
    if (res == VK_SUCCESS)
        c->submitted = v;
    pthread_mutex_unlock(&c->qlock);

    if (res != VK_SUCCESS) {
        rx_warn("compute: submission failed: %s", rx_compute_strerror(res));
        return rx_compute_ret2err(res);
    }
    *value = v;
    return 0;
}

int rx_compute_wait(RXCompute *c, uint64_t value)
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
    VkResult res = vkWaitSemaphores(c->dev, &info, timeout_ns);
    if (res != VK_SUCCESS) {
        /* The submission may still be running, or never end (a hung GPU
         * is reset by its driver, and then the device is lost): nothing it
         * uses may be freed before the device is idle. vkDeviceWaitIdle
         * needs the queue to itself. */
        if (!atomic_exchange(&c->lost, true))
            rx_err("compute: waiting for the GPU failed: %s; the GPU is not used again",
                   rx_compute_strerror(res));
        pthread_mutex_lock(&c->qlock);
        vkDeviceWaitIdle(c->dev);
        pthread_mutex_unlock(&c->qlock);
        return rx_compute_ret2err(res);
    }
    return 0;
}
