/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* The Vulkan device: instance, physical device choice, logical device and
 * queue, the VMA allocator, buffers, the frames-in-flight slots, deferred
 * destruction and staged uploads. Taken from libcraw's tests/vkdev.c and
 * lib/vk_buffer.c, cut down to what an interface needs.
 *
 * Vulkan 1.3 is the floor: dynamic rendering (no render pass objects),
 * synchronization2 barriers and submits, and buffer device addresses (the
 * waveform shader reads its column buffer through a pointer in its push
 * constants). The loader is linked (vulkan_dep), so the global prototypes
 * are used; VMA, built with dynamic entry points only, is handed
 * vkGetInstanceProcAddr and vkGetDeviceProcAddr.
 *
 * Destruction is deferred by submission number rather than by frame slot:
 * every submission gets the next serial, a resource released while the
 * frame that may use it is being built is tagged with that frame's serial,
 * and it is freed once a fence says the submission has finished. A fence
 * signal covers every earlier submission on the queue, so "completed" is
 * simply the highest serial whose fence has signalled.
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

/* ------------------------------------------------------------------ errors */

int rx_vk_ret2err(VkResult res)
{
    switch (res) {
    case VK_SUCCESS:
        return 0;
    case VK_ERROR_OUT_OF_HOST_MEMORY:
    case VK_ERROR_OUT_OF_DEVICE_MEMORY:
    case VK_ERROR_OUT_OF_POOL_MEMORY:
    case VK_ERROR_FRAGMENTED_POOL:
        return RX_ERROR(ENOMEM);
    case VK_ERROR_DEVICE_LOST:
        return RX_ERROR(EIO);
    case VK_ERROR_INITIALIZATION_FAILED:
    case VK_ERROR_INCOMPATIBLE_DRIVER:
    case VK_ERROR_SURFACE_LOST_KHR:
    case VK_ERROR_EXTENSION_NOT_PRESENT:
    case VK_ERROR_FEATURE_NOT_PRESENT:
    case VK_ERROR_LAYER_NOT_PRESENT:
        return RX_ERROR(ENODEV);
    case VK_TIMEOUT:
    case VK_NOT_READY:
        return RX_ERROR(EAGAIN);
    default:
        return RX_ERROR(EINVAL);
    }
}

const char *rx_vk_strerror(VkResult res)
{
    switch (res) {
    case VK_SUCCESS:                      return "success";
    case VK_NOT_READY:                    return "not ready";
    case VK_TIMEOUT:                      return "timeout";
    case VK_SUBOPTIMAL_KHR:               return "suboptimal";
    case VK_ERROR_OUT_OF_HOST_MEMORY:     return "out of host memory";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY:   return "out of device memory";
    case VK_ERROR_INITIALIZATION_FAILED:  return "initialization failed";
    case VK_ERROR_DEVICE_LOST:            return "device lost";
    case VK_ERROR_LAYER_NOT_PRESENT:      return "layer not present";
    case VK_ERROR_EXTENSION_NOT_PRESENT:  return "extension not present";
    case VK_ERROR_FEATURE_NOT_PRESENT:    return "feature not present";
    case VK_ERROR_INCOMPATIBLE_DRIVER:    return "incompatible driver";
    case VK_ERROR_SURFACE_LOST_KHR:       return "surface lost";
    case VK_ERROR_OUT_OF_DATE_KHR:        return "out of date";
    case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR: return "native window in use";
    default:                              return "error";
    }
}

/* -------------------------------------------------------------- validation */

static VKAPI_ATTR VkBool32 VKAPI_CALL debug_cb(VkDebugUtilsMessageSeverityFlagBitsEXT sev,
                                               VkDebugUtilsMessageTypeFlagsEXT type,
                                               const VkDebugUtilsMessengerCallbackDataEXT *data,
                                               void *opaque)
{
    RxWindow *w = opaque;

    if (sev & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        w->validation_errors++;
        rx_err("vulkan: %s", data->pMessage);
    } else if (sev & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        rx_warn("vulkan: %s", data->pMessage);
    } else {
        rx_debug("vulkan: %s", data->pMessage);
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

static int create_instance(RxWindow *w)
{
    Uint32 nb_sdl = 0;
    const char *const *sdl_exts = SDL_Vulkan_GetInstanceExtensions(&nb_sdl);
    if (!sdl_exts) {
        rx_err("gpu: SDL_Vulkan_GetInstanceExtensions: %s", SDL_GetError());
        return RX_ERROR(ENODEV);
    }

    /* SDL's list (surface + wayland_surface) plus the debug extensions. */
    const char **exts = calloc(nb_sdl + 2, sizeof(*exts));
    if (!exts)
        return RX_ERROR(ENOMEM);
    uint32_t nb_exts = 0;
    for (Uint32 i = 0; i < nb_sdl; i++)
        exts[nb_exts++] = sdl_exts[i];

    const char *layers[1];
    uint32_t nb_layers = 0;
    bool debug_utils = false, features = false;
    if (w->opts.validation) {
        if (have_layer("VK_LAYER_KHRONOS_validation")) {
            layers[nb_layers++] = "VK_LAYER_KHRONOS_validation";
            features = have_instance_ext("VK_LAYER_KHRONOS_validation",
                                         VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
            if (features)
                exts[nb_exts++] = VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME;
        } else {
            rx_warn("gpu: VK_LAYER_KHRONOS_validation is not installed");
        }
        debug_utils = have_instance_ext(NULL, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) ||
                      (nb_layers && have_instance_ext("VK_LAYER_KHRONOS_validation",
                                                      VK_EXT_DEBUG_UTILS_EXTENSION_NAME));
        if (debug_utils)
            exts[nb_exts++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    }

    /* Synchronisation validation as well as the core checks: the barriers
     * and the frames-in-flight logic are what this module gets wrong if
     * it gets anything wrong. */
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
        .ppEnabledExtensionNames = exts,
    };
    VkResult res = vkCreateInstance(&info, NULL, &w->inst);
    free(exts);
    if (res != VK_SUCCESS) {
        rx_err("gpu: unable to create a Vulkan instance: %s", rx_vk_strerror(res));
        return rx_vk_ret2err(res);
    }

    if (debug_utils) {
        PFN_vkCreateDebugUtilsMessengerEXT create = (PFN_vkCreateDebugUtilsMessengerEXT)
            vkGetInstanceProcAddr(w->inst, "vkCreateDebugUtilsMessengerEXT");
        VkDebugUtilsMessengerCreateInfoEXT dinfo = {
            .sType           = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
            .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
                               VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT,
            .messageType     = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                               VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                               VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
            .pfnUserCallback = debug_cb,
            .pUserData       = w,
        };
        if (create)
            create(w->inst, &dinfo, NULL, &w->messenger);
    }

    if (nb_layers)
        rx_info("gpu: validation enabled%s", features ? " (with synchronisation checks)" : "");

    return 0;
}

/* ------------------------------------------------------------------ device */

/* Why a device cannot run the interface, or NULL if it can; `qfam` gets a
 * queue family that draws and presents to the window's surface. */
static const char *device_unusable(RxWindow *w, VkPhysicalDevice pd,
                                   const VkPhysicalDeviceProperties *props, uint32_t *qfam)
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
    if (!f13.dynamicRendering)
        return "no dynamicRendering";
    if (!f13.synchronization2)
        return "no synchronization2";
    if (!f12.bufferDeviceAddress)
        return "no bufferDeviceAddress";

    uint32_t nb = 0;
    vkEnumerateDeviceExtensionProperties(pd, NULL, &nb, NULL);
    VkExtensionProperties *exts = calloc(RX_MAX(nb, 1), sizeof(*exts));
    if (!exts)
        return "out of memory";
    vkEnumerateDeviceExtensionProperties(pd, NULL, &nb, exts);
    bool swapchain = false;
    for (uint32_t i = 0; i < nb && !swapchain; i++)
        swapchain = !strcmp(exts[i].extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    free(exts);
    if (!swapchain)
        return "no VK_KHR_swapchain";

    nb = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nb, NULL);
    VkQueueFamilyProperties *qf = calloc(RX_MAX(nb, 1), sizeof(*qf));
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

static int type_score(VkPhysicalDeviceType type)
{
    switch (type) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   return 4;
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 3;
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    return 2;
    case VK_PHYSICAL_DEVICE_TYPE_CPU:            return 1;
    default:                                     return 0;
    }
}

/* The first suitable device of the best type: discrete before integrated
 * before software. A device named by options.device (a case-insensitive
 * substring) wins over all of them; when none matches, the choice falls
 * back to the usual order with a warning rather than failing. */
static int pick_device(RxWindow *w)
{
    uint32_t nb = 0;
    VkResult res = vkEnumeratePhysicalDevices(w->inst, &nb, NULL);
    if (res != VK_SUCCESS || !nb) {
        rx_err("gpu: no Vulkan devices");
        return RX_ERROR(ENODEV);
    }

    VkPhysicalDevice *devs = calloc(nb, sizeof(*devs));
    if (!devs)
        return RX_ERROR(ENOMEM);
    vkEnumeratePhysicalDevices(w->inst, &nb, devs);

    const char *want = w->device && w->device[0] ? w->device : NULL;
    int best_score = -1;
    bool named = false;
    for (uint32_t i = 0; i < nb; i++) {
        VkPhysicalDeviceProperties props;
        uint32_t qfam = 0;

        vkGetPhysicalDeviceProperties(devs[i], &props);
        const char *why = device_unusable(w, devs[i], &props, &qfam);
        if (why) {
            rx_debug("gpu: %s: %s", props.deviceName, why);
            if (want && strcasestr(props.deviceName, want))
                rx_warn("gpu: %s cannot be used: %s", props.deviceName, why);
            continue;
        }

        bool match = want && strcasestr(props.deviceName, want);
        int score = type_score(props.deviceType) + (match ? 100 : 0);
        rx_debug("gpu: %s: usable, score %d", props.deviceName, score);
        if (score > best_score) {
            best_score = score;
            named = match;
            w->phys = devs[i];
            w->props = props;
            w->qfam = qfam;
        }
    }
    free(devs);

    if (best_score < 0) {
        rx_err("gpu: no Vulkan 1.3 device can draw to this window");
        return RX_ERROR(ENODEV);
    }
    if (want && !named)
        rx_warn("gpu: no usable device matches \"%s\"", want);

    return 0;
}

static int create_device(RxWindow *w)
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

    const float prio = 1.0f;
    VkDeviceQueueCreateInfo qinfo = {
        .sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = w->qfam,
        .queueCount       = 1,
        .pQueuePriorities = &prio,
    };
    const char *exts[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    VkDeviceCreateInfo info = {
        .sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext                   = &f2,
        .queueCreateInfoCount    = 1,
        .pQueueCreateInfos       = &qinfo,
        .enabledExtensionCount   = RX_ARRAY_ELEMS(exts),
        .ppEnabledExtensionNames = exts,
    };
    VkResult res = vkCreateDevice(w->phys, &info, NULL, &w->dev);
    if (res != VK_SUCCESS) {
        rx_err("gpu: unable to create a device on %s: %s", w->props.deviceName,
               rx_vk_strerror(res));
        return rx_vk_ret2err(res);
    }
    vkGetDeviceQueue(w->dev, w->qfam, 0, &w->queue);

    VmaVulkanFunctions fn = {
        .vkGetInstanceProcAddr = vkGetInstanceProcAddr,
        .vkGetDeviceProcAddr   = vkGetDeviceProcAddr,
    };
    VmaAllocatorCreateInfo vinfo = {
        .flags            = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT,
        .physicalDevice   = w->phys,
        .device           = w->dev,
        .pVulkanFunctions = &fn,
        .instance         = w->inst,
        .vulkanApiVersion = VK_API_VERSION_1_3,
    };
    res = vmaCreateAllocator(&vinfo, &w->vma);
    if (res != VK_SUCCESS) {
        rx_err("gpu: unable to create the VMA allocator: %s", rx_vk_strerror(res));
        return rx_vk_ret2err(res);
    }

    return 0;
}

static int create_frames(RxWindow *w)
{
    for (int i = 0; i < RX_FRAMES_IN_FLIGHT; i++) {
        RxFrame *f = &w->frames[i];

        /* The pool is reset as a whole each time the slot comes round. */
        VkCommandPoolCreateInfo pinfo = {
            .sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
            .queueFamilyIndex = w->qfam,
        };
        VkResult res = vkCreateCommandPool(w->dev, &pinfo, NULL, &f->pool);
        if (res != VK_SUCCESS)
            return rx_vk_ret2err(res);

        VkCommandBufferAllocateInfo cinfo = {
            .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool        = f->pool,
            .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1,
        };
        res = vkAllocateCommandBuffers(w->dev, &cinfo, &f->cmd);
        if (res != VK_SUCCESS)
            return rx_vk_ret2err(res);

        /* Signalled, so the first wait on a slot that never ran returns. */
        VkFenceCreateInfo finfo = {
            .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
            .flags = VK_FENCE_CREATE_SIGNALED_BIT,
        };
        res = vkCreateFence(w->dev, &finfo, NULL, &f->fence);
        if (res != VK_SUCCESS)
            return rx_vk_ret2err(res);

        VkSemaphoreCreateInfo sinfo = {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        };
        res = vkCreateSemaphore(w->dev, &sinfo, NULL, &f->acquired);
        if (res != VK_SUCCESS)
            return rx_vk_ret2err(res);
    }

    return 0;
}

int rx_vk_create(RxWindow *w)
{
    rx_array_init(&w->garbage, sizeof(RxDeferred));
    rx_array_init(&w->uploads, sizeof(RxUpload));
    rx_array_init(&w->textures, sizeof(RxTexture));

    int ret = create_instance(w);
    if (ret < 0)
        return ret;

    if (!SDL_Vulkan_CreateSurface(w->win, w->inst, NULL, &w->surface)) {
        rx_err("gpu: SDL_Vulkan_CreateSurface: %s", SDL_GetError());
        return RX_ERROR(ENODEV);
    }

    ret = pick_device(w);
    if (ret < 0)
        return ret;

    ret = create_device(w);
    if (ret < 0)
        return ret;

    ret = create_frames(w);
    if (ret < 0) {
        rx_err("gpu: unable to create the frame resources");
        return ret;
    }

    rx_info("gpu: %s, Vulkan %u.%u, queue family %u", w->props.deviceName,
            VK_API_VERSION_MAJOR(w->props.apiVersion),
            VK_API_VERSION_MINOR(w->props.apiVersion), w->qfam);

    return 0;
}

const char *rx_window_gpu_name(const RxWindow *w)
{
    return w->props.deviceName;
}

static void free_upload(RxWindow *w, RxUpload *up)
{
    rx_vk_buffer_free(w, &up->staging);
}

void rx_vk_destroy(RxWindow *w)
{
    if (w->dev) {
        vkDeviceWaitIdle(w->dev);

        rx_vk_collect(w, true);
        for (size_t i = 0; i < w->uploads.nb; i++)
            free_upload(w, rx_array_at(&w->uploads, i));
        rx_array_clear(&w->uploads);

        for (int i = 0; i < RX_FRAMES_IN_FLIGHT; i++) {
            RxFrame *f = &w->frames[i];
            if (f->acquired)
                vkDestroySemaphore(w->dev, f->acquired, NULL);
            if (f->fence)
                vkDestroyFence(w->dev, f->fence, NULL);
            if (f->pool)
                vkDestroyCommandPool(w->dev, f->pool, NULL);
            *f = (RxFrame) { 0 };
        }

        if (w->vma)
            vmaDestroyAllocator(w->vma);
        w->vma = NULL;
        vkDestroyDevice(w->dev, NULL);
        w->dev = VK_NULL_HANDLE;
    }

    /* SDL made the surface, so SDL unmakes it: it knows which loader entry
     * point it used. Before the instance, while the window still exists. */
    if (w->surface && w->inst)
        SDL_Vulkan_DestroySurface(w->inst, w->surface, NULL);
    w->surface = VK_NULL_HANDLE;

    if (w->messenger) {
        PFN_vkDestroyDebugUtilsMessengerEXT destroy = (PFN_vkDestroyDebugUtilsMessengerEXT)
            vkGetInstanceProcAddr(w->inst, "vkDestroyDebugUtilsMessengerEXT");
        if (destroy)
            destroy(w->inst, w->messenger, NULL);
        w->messenger = VK_NULL_HANDLE;
    }

    if (w->inst)
        vkDestroyInstance(w->inst, NULL);
    w->inst = VK_NULL_HANDLE;

    rx_array_free(&w->garbage);
    rx_array_free(&w->uploads);
    rx_array_free(&w->textures);
}

/* ----------------------------------------------------------------- buffers */

int rx_vk_buffer_create(RxWindow *w, RxVkBuffer *b, VkDeviceSize size,
                        VkBufferUsageFlags usage, enum RxMemUsage mem)
{
    *b = (RxVkBuffer) { 0 };
    if (!size)
        return RX_ERROR(EINVAL);

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
    case RX_MEM_UPLOAD:
        ci.usage = VMA_MEMORY_USAGE_AUTO;
        ci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                   VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
        break;
    case RX_MEM_READBACK:
        ci.usage = VMA_MEMORY_USAGE_AUTO;
        ci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                   VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
        break;
    case RX_MEM_DEVICE:
    default:
        ci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        break;
    }

    VmaAllocationInfo ai = { 0 };
    VkResult res = vmaCreateBuffer(w->vma, &info, &ci, &b->buf, &b->alloc, &ai);
    if (res != VK_SUCCESS) {
        rx_err("gpu: unable to allocate a %llu byte buffer: %s", (unsigned long long)size,
               rx_vk_strerror(res));
        *b = (RxVkBuffer) { 0 };
        return rx_vk_ret2err(res);
    }

    b->size = size;
    b->map  = ai.pMappedData;

    if (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) {
        VkBufferDeviceAddressInfo ainfo = {
            .sType  = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
            .buffer = b->buf,
        };
        b->addr = vkGetBufferDeviceAddress(w->dev, &ainfo);
    }

    return 0;
}

void rx_vk_buffer_free(RxWindow *w, RxVkBuffer *b)
{
    if (!b || !b->buf)
        return;
    vmaDestroyBuffer(w->vma, b->buf, b->alloc);
    *b = (RxVkBuffer) { 0 };
}

int rx_vk_buffer_invalidate(RxWindow *w, RxVkBuffer *b)
{
    if (!b->map)
        return 0;
    return rx_vk_ret2err(vmaInvalidateAllocation(w->vma, b->alloc, 0, VK_WHOLE_SIZE));
}

/* ------------------------------------------------------ deferred destruction */

static void free_deferred(RxWindow *w, const RxDeferred *d)
{
    if (d->tex_id)
        rx_imgui_remove_texture(w, d->tex_id);
    if (d->view)
        vkDestroyImageView(w->dev, d->view, NULL);
    if (d->img)
        vmaDestroyImage(w->vma, d->img, d->alloc);
    else if (d->buf)
        vmaDestroyBuffer(w->vma, d->buf, d->alloc);
    else if (d->alloc)
        vmaFreeMemory(w->vma, d->alloc);
}

static void defer_at(RxWindow *w, const RxDeferred *d, uint64_t serial)
{
    RxDeferred *item = rx_array_push(&w->garbage, d);
    if (item) {
        item->serial = serial;
        return;
    }

    /* Out of memory to remember it: the only safe way to free it now is
     * to wait for the GPU first. */
    vkDeviceWaitIdle(w->dev);
    w->completed = w->serial;
    free_deferred(w, d);
}

void rx_vk_defer(RxWindow *w, const RxDeferred *d)
{
    defer_at(w, d, w->serial + 1);
}

void rx_vk_poll_completed(RxWindow *w)
{
    for (int i = 0; i < RX_FRAMES_IN_FLIGHT; i++) {
        RxFrame *f = &w->frames[i];
        if (f->serial > w->completed && vkGetFenceStatus(w->dev, f->fence) == VK_SUCCESS)
            w->completed = f->serial;
    }
}

void rx_vk_collect(RxWindow *w, bool all)
{
    size_t kept = 0;

    for (size_t i = 0; i < w->garbage.nb; i++) {
        RxDeferred *d = rx_array_at(&w->garbage, i);
        if (all || d->serial <= w->completed) {
            free_deferred(w, d);
            continue;
        }
        if (kept != i)
            memcpy(rx_array_at(&w->garbage, kept), d, sizeof(*d));
        kept++;
    }

    w->garbage.nb = kept;
}

/* ----------------------------------------------------------------- uploads */

int rx_vk_upload(RxWindow *w, const RxUpload *up)
{
    if (!rx_array_push(&w->uploads, up))
        return RX_ERROR(ENOMEM);
    return 0;
}

void rx_vk_record_uploads(RxWindow *w, VkCommandBuffer cmd, uint64_t serial)
{
    if (!w->uploads.nb)
        return;

    for (size_t i = 0; i < w->uploads.nb; i++) {
        RxUpload *up = rx_array_at(&w->uploads, i);

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
        RxDeferred d = {
            .buf   = up->staging.buf,
            .alloc = up->staging.alloc,
        };
        defer_at(w, &d, serial);
    }

    rx_array_clear(&w->uploads);
}
