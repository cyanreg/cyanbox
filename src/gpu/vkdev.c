/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* The Vulkan set-up the window, the compute context and the model share
 * (vkdev.h). Taken from libcraw's tests/vkdev.c by way of vk.c, where each
 * of the three had a copy. The loader is linked (vulkan_dep), so the
 * global prototypes are used; VMA, built with dynamic entry points only,
 * is handed vkGetInstanceProcAddr and vkGetDeviceProcAddr. */

#include "vkdev.h"

#include <stdlib.h>
#include <string.h>

#include "rxbox/common.h"

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
        return RX_ERROR(ETIMEDOUT);
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
    RXVkDev *d = opaque;

    if (sev & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        __atomic_fetch_add(&d->validation_errors, 1, __ATOMIC_RELAXED);
        rx_err("%s: %s", d->tag, data->pMessage);
    } else if (sev & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        rx_warn("%s: %s", d->tag, data->pMessage);
    } else {
        rx_debug("%s: %s", d->tag, data->pMessage);
    }

    return VK_FALSE;
}

int rx_vkdev_validation_errors(const RXVkDev *d)
{
    return __atomic_load_n(&d->validation_errors, __ATOMIC_RELAXED);
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

int rx_vkdev_instance(RXVkDev *d, const RXVkDevOptions *o)
{
    d->tag = o->tag;
    uint32_t version = 0;
    if (vkEnumerateInstanceVersion(&version) != VK_SUCCESS || version < VK_API_VERSION_1_3) {
        rx_debug("%s: the Vulkan loader is older than 1.3", d->tag);
        return RX_ERROR(ENODEV);
    }

    /* The user's extensions, then the validation layer's two. */
    const char **exts = calloc(o->nb_exts + 2, sizeof(*exts));
    if (!exts)
        return RX_ERROR(ENOMEM);
    uint32_t nb_exts = 0;
    for (uint32_t i = 0; i < o->nb_exts; i++)
        exts[nb_exts++] = o->exts[i];

    const char *layers[1];
    uint32_t nb_layers = 0;
    bool debug_utils = false, features = false;
    if (o->validation) {
        if (have_layer("VK_LAYER_KHRONOS_validation")) {
            layers[nb_layers++] = "VK_LAYER_KHRONOS_validation";
            features = have_instance_ext("VK_LAYER_KHRONOS_validation",
                                         VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
            if (features)
                exts[nb_exts++] = VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME;
        } else {
            rx_warn("%s: VK_LAYER_KHRONOS_validation is not installed", d->tag);
        }
        debug_utils = have_instance_ext(NULL, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) ||
                      (nb_layers && have_instance_ext("VK_LAYER_KHRONOS_validation",
                                                      VK_EXT_DEBUG_UTILS_EXTENSION_NAME));
        if (debug_utils)
            exts[nb_exts++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    }

    /* Synchronisation validation as well as the core checks: the barriers
     * (between frames, uploads, dispatches, copies and the host's reads)
     * are what the users get wrong if they get anything wrong. */
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
    VkResult res = vkCreateInstance(&info, NULL, &d->inst);
    free(exts);
    if (res != VK_SUCCESS) {
        rx_debug("%s: no Vulkan instance: %s", d->tag, rx_vk_strerror(res));
        d->inst = VK_NULL_HANDLE;
        return rx_vk_ret2err(res);
    }

    if (debug_utils) {
        PFN_vkCreateDebugUtilsMessengerEXT create = (PFN_vkCreateDebugUtilsMessengerEXT)
            vkGetInstanceProcAddr(d->inst, "vkCreateDebugUtilsMessengerEXT");
        VkDebugUtilsMessengerCreateInfoEXT dinfo = {
            .sType           = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
            .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
                               VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT,
            .messageType     = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                               VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                               (o->performance ? VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT
                                               : 0),
            .pfnUserCallback = debug_cb,
            .pUserData       = d,
        };
        if (create)
            create(d->inst, &dinfo, NULL, &d->messenger);
    }
    if (nb_layers)
        rx_info("%s: validation enabled%s", d->tag,
                features ? " (with synchronisation checks)" : "");

    return 0;
}

/* ------------------------------------------------------------------ device */

bool rx_vkdev_has_ext(VkPhysicalDevice pd, const char *name)
{
    uint32_t nb = 0;
    if (vkEnumerateDeviceExtensionProperties(pd, NULL, &nb, NULL) != VK_SUCCESS || !nb)
        return false;
    VkExtensionProperties *exts = calloc(nb, sizeof(*exts));
    if (!exts)
        return false;
    bool found = false;
    if (vkEnumerateDeviceExtensionProperties(pd, NULL, &nb, exts) == VK_SUCCESS) {
        for (uint32_t i = 0; i < nb && !found; i++)
            found = !strcmp(exts[i].extensionName, name);
    }
    free(exts);
    return found;
}

const char *rx_vkdev_compute_family(VkPhysicalDevice pd, uint32_t *qfam)
{
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
    case VK_PHYSICAL_DEVICE_TYPE_CPU:            return 1;
    default:                                     return 0;
    }
}

int rx_vkdev_pick(RXVkDev *d, const RXVkDevOptions *o)
{
    uint32_t nb = 0;
    VkResult res = vkEnumeratePhysicalDevices(d->inst, &nb, NULL);
    if (res != VK_SUCCESS || !nb) {
        rx_debug("%s: no Vulkan devices", d->tag);
        return RX_ERROR(ENODEV);
    }

    VkPhysicalDevice *devs = calloc(nb, sizeof(*devs));
    if (!devs)
        return RX_ERROR(ENOMEM);
    vkEnumeratePhysicalDevices(d->inst, &nb, devs);

    const char *want = o->device && o->device[0] ? o->device : NULL;
    int best_score = -1;
    bool named = false;
    for (uint32_t i = 0; i < nb; i++) {
        VkPhysicalDeviceProperties props;
        uint32_t qfam = 0;

        vkGetPhysicalDeviceProperties(devs[i], &props);
        bool match = want && rx_str_contains_fold(props.deviceName, want);
        const char *why = props.apiVersion < VK_API_VERSION_1_3 ? "Vulkan 1.3 is required"
                        : o->check(o->opaque, devs[i], &props, &qfam);
        if (!why && props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU && !match && !o->cpu)
            why = "a CPU implementation (used only when named)";
        if (why) {
            rx_debug("%s: %s: %s", d->tag, props.deviceName, why);
            if (match)
                rx_warn("%s: %s cannot be used: %s", d->tag, props.deviceName, why);
            continue;
        }

        int score = type_score(props.deviceType) + (match ? 100 : 0);
        rx_debug("%s: %s: usable, score %d", d->tag, props.deviceName, score);
        if (score > best_score) {
            best_score = score;
            named = match;
            d->phys = devs[i];
            d->props = props;
            d->qfam = qfam;
        }
    }
    free(devs);

    if (best_score < 0)
        return RX_ERROR(ENODEV);
    if (want && !named)
        rx_warn("%s: no usable device matches \"%s\"; using %s", d->tag, want,
                d->props.deviceName);
    /* Once more for the one chosen: what the check keeps is its. */
    o->check(o->opaque, d->phys, &d->props, &d->qfam);
    return 0;
}

int rx_vkdev_device(RXVkDev *d, const void *features, const char *const *exts,
                    uint32_t nb_exts)
{
    const float prio = 1.0f;
    VkDeviceQueueCreateInfo qinfo = {
        .sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = d->qfam,
        .queueCount       = 1,
        .pQueuePriorities = &prio,
    };
    VkDeviceCreateInfo info = {
        .sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext                   = features,
        .queueCreateInfoCount    = 1,
        .pQueueCreateInfos       = &qinfo,
        .enabledExtensionCount   = nb_exts,
        .ppEnabledExtensionNames = nb_exts ? exts : NULL,
    };
    VkResult res = vkCreateDevice(d->phys, &info, NULL, &d->dev);
    if (res != VK_SUCCESS) {
        rx_warn("%s: unable to create a device on %s: %s", d->tag, d->props.deviceName,
                rx_vk_strerror(res));
        d->dev = VK_NULL_HANDLE;
        return rx_vk_ret2err(res);
    }
    vkGetDeviceQueue(d->dev, d->qfam, 0, &d->queue);

    /* Timestamps only measure; a queue without them goes without. */
    uint32_t nb_qf = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(d->phys, &nb_qf, NULL);
    VkQueueFamilyProperties *qf = calloc(RX_MAX(nb_qf, 1), sizeof(*qf));
    if (!qf)
        return RX_ERROR(ENOMEM);
    vkGetPhysicalDeviceQueueFamilyProperties(d->phys, &nb_qf, qf);
    uint32_t ts_bits = d->qfam < nb_qf ? qf[d->qfam].timestampValidBits : 0;
    free(qf);
    d->ts_mask = ts_bits >= 64 ? UINT64_MAX : ts_bits ? (UINT64_C(1) << ts_bits) - 1 : 0;

    VmaVulkanFunctions fn = {
        .vkGetInstanceProcAddr = vkGetInstanceProcAddr,
        .vkGetDeviceProcAddr   = vkGetDeviceProcAddr,
    };
    VmaAllocatorCreateInfo vinfo = {
        .flags            = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT,
        .physicalDevice   = d->phys,
        .device           = d->dev,
        .pVulkanFunctions = &fn,
        .instance         = d->inst,
        .vulkanApiVersion = VK_API_VERSION_1_3,
    };
    res = vmaCreateAllocator(&vinfo, &d->vma);
    if (res != VK_SUCCESS) {
        rx_warn("%s: unable to create the VMA allocator: %s", d->tag, rx_vk_strerror(res));
        d->vma = NULL;
        return rx_vk_ret2err(res);
    }
    return 0;
}

int rx_vkdev_timeline(RXVkDev *d, VkSemaphore *out)
{
    VkSemaphoreTypeCreateInfo tinfo = {
        .sType         = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
        .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
        .initialValue  = 0,
    };
    VkSemaphoreCreateInfo sinfo = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        .pNext = &tinfo,
    };
    return rx_vk_ret2err(vkCreateSemaphore(d->dev, &sinfo, NULL, out));
}

void rx_vkdev_destroy(RXVkDev *d)
{
    if (d->vma)
        vmaDestroyAllocator(d->vma);
    if (d->dev)
        vkDestroyDevice(d->dev, NULL);
    if (d->messenger) {
        PFN_vkDestroyDebugUtilsMessengerEXT destroy = (PFN_vkDestroyDebugUtilsMessengerEXT)
            vkGetInstanceProcAddr(d->inst, "vkDestroyDebugUtilsMessengerEXT");
        if (destroy)
            destroy(d->inst, d->messenger, NULL);
    }
    if (d->inst)
        vkDestroyInstance(d->inst, NULL);

    const char *tag = d->tag;
    *d = (RXVkDev) { .tag = tag };
}
