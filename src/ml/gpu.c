/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* The model's GPU: a Vulkan instance with no surface, the best device that
 * can run the kernels, one compute queue, VMA, a timeline semaphore, the
 * pipelines and one command buffer. Cut from rxbox's compute context
 * (src/gpu/compute.c) and kept like it, with what the model needs on top.
 *
 * Choices, and why:
 *  - Its own device (ml_priv.h says why not rxbox's compute context). The
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
#include "gemm_f16_conv_comp.spv.h"
#include "gemm_q4k_comp.spv.h"
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
#define K(e, n) [e] = { rx_##n##_comp_spv, sizeof(rx_##n##_comp_spv), #n }
    K(ML_K_MEL,           mel),
    K(ML_K_MEL_NORM,      mel_norm),
    K(ML_K_CONV1,         conv1),
    K(ML_K_CONV_PERMUTE,  conv_permute),
    K(ML_K_STEM_PERMUTE,  stem_permute),
    K(ML_K_GEMM_F16,      gemm_f16),
    K(ML_K_GEMM_F16_CONV, gemm_f16_conv),
    K(ML_K_GEMM_Q4K,      gemm_q4k),
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

/* ------------------------------------------------------------------ errors */

static int ret2err(VkResult res)
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

static const char *vk_strerror(VkResult res)
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

/* -------------------------------------------------------------- validation */

static VKAPI_ATTR VkBool32 VKAPI_CALL debug_cb(VkDebugUtilsMessageSeverityFlagBitsEXT sev,
                                               VkDebugUtilsMessageTypeFlagsEXT type,
                                               const VkDebugUtilsMessengerCallbackDataEXT *data,
                                               void *opaque)
{
    MLGpu *g = opaque;

    if (sev & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        atomic_fetch_add(&g->validation_errors, 1);
        rx_err("ml: %s", data->pMessage);
    } else if (sev & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        rx_warn("ml: %s", data->pMessage);
    } else {
        rx_debug("ml: %s", data->pMessage);
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

static int create_instance(MLGpu *g, bool validation)
{
    uint32_t version = 0;
    if (vkEnumerateInstanceVersion(&version) != VK_SUCCESS || version < VK_API_VERSION_1_3) {
        rx_debug("ml: the Vulkan loader is older than 1.3");
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
            rx_warn("ml: VK_LAYER_KHRONOS_validation is not installed");
        }
        debug_utils = have_instance_ext(NULL, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) ||
                      (nb_layers && have_instance_ext("VK_LAYER_KHRONOS_validation",
                                                      VK_EXT_DEBUG_UTILS_EXTENSION_NAME));
        if (debug_utils)
            exts[nb_exts++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    }

    /* Synchronisation validation too, as the compute context has: the
     * barriers between dispatches, copies and the host's reads are what
     * the model's recording gets wrong if it gets anything wrong. */
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
    VkResult res = vkCreateInstance(&info, NULL, &g->inst);
    if (res != VK_SUCCESS) {
        rx_debug("ml: no Vulkan instance: %s", vk_strerror(res));
        g->inst = VK_NULL_HANDLE;
        return ret2err(res);
    }

    if (debug_utils) {
        PFN_vkCreateDebugUtilsMessengerEXT create = (PFN_vkCreateDebugUtilsMessengerEXT)
            vkGetInstanceProcAddr(g->inst, "vkCreateDebugUtilsMessengerEXT");
        VkDebugUtilsMessengerCreateInfoEXT dinfo = {
            .sType           = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
            .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
                               VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT,
            .messageType     = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                               VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT,
            .pfnUserCallback = debug_cb,
            .pUserData       = g,
        };
        if (create)
            create(g->inst, &dinfo, NULL, &g->messenger);
    }
    if (nb_layers)
        rx_info("ml: validation enabled%s", features ? " (with synchronisation checks)" : "");

    return 0;
}

/* ------------------------------------------------------------------ device */

#define NEED_SHARED (48 * 1024)

/* The subgroup sizes gemv.comp works with (see the top of the file). */
#define SUB_MIN 16
#define SUB_MAX 64

/* What a device is set up with beyond the defaults. */
typedef struct DevChoice {
    uint32_t qfam;
    uint32_t sub_size;          /* the size the subgroup kernels require, 0: any it picks */
    bool full_sub;              /* computeFullSubgroups */
} DevChoice;

/* Why a device cannot run the model, or NULL; `c` gets the queue family
 * to use and how the subgroup kernels are run. */
static const char *device_unusable(VkPhysicalDevice pd, const VkPhysicalDeviceProperties *props,
                                   DevChoice *c)
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
    if (p13.minSubgroupSize < SUB_MIN || p13.maxSubgroupSize > SUB_MAX) {
        uint32_t size = RX_MAX(p13.minSubgroupSize, SUB_MIN);
        if (!f13.subgroupSizeControl ||
            !(p13.requiredSubgroupSizeStages & VK_SHADER_STAGE_COMPUTE_BIT) ||
            size > RX_MIN(p13.maxSubgroupSize, SUB_MAX) ||
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

    c->qfam = (uint32_t)(only >= 0 ? only : any);
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

static int pick_device(MLGpu *g, const char *want)
{
    uint32_t nb = 0;
    VkResult res = vkEnumeratePhysicalDevices(g->inst, &nb, NULL);
    if (res != VK_SUCCESS || !nb) {
        rx_debug("ml: no Vulkan devices");
        return RX_ERROR(ENODEV);
    }

    VkPhysicalDevice *devs = calloc(nb, sizeof(*devs));
    if (!devs)
        return RX_ERROR(ENOMEM);
    vkEnumeratePhysicalDevices(g->inst, &nb, devs);

    int best_score = -1;
    bool named = false;
    for (uint32_t i = 0; i < nb; i++) {
        VkPhysicalDeviceProperties props;
        DevChoice choice = { 0 };

        vkGetPhysicalDeviceProperties(devs[i], &props);
        bool match = want && rx_str_contains_fold(props.deviceName, want);
        const char *why = device_unusable(devs[i], &props, &choice);
        if (!why && props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU && !match)
            why = "a CPU implementation (used only when named)";
        if (why) {
            rx_debug("ml: %s: %s", props.deviceName, why);
            if (match)
                rx_warn("ml: %s cannot be used: %s", props.deviceName, why);
            continue;
        }

        int score = type_score(props.deviceType) + (match ? 100 : 0);
        rx_debug("ml: %s: usable, score %d", props.deviceName, score);
        if (score > best_score) {
            best_score = score;
            named = match;
            g->phys = devs[i];
            g->props = props;
            g->qfam = choice.qfam;
            g->sub_size = choice.sub_size;
            g->full_sub = choice.full_sub;
        }
    }
    free(devs);

    if (best_score < 0) {
        rx_debug("ml: no Vulkan device can run the model");
        return RX_ERROR(ENODEV);
    }
    if (want && !named)
        rx_warn("ml: no usable device matches \"%s\"; using %s", want, g->props.deviceName);

    return 0;
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
    VkResult res = vkCreatePipelineLayout(g->dev, &linfo, NULL, &g->layout);
    if (res != VK_SUCCESS)
        return ret2err(res);

    for (int k = 0; k < ML_NB_KERNELS; k++) {
        VkShaderModuleCreateInfo minfo = {
            .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = kernels[k].size,
            .pCode    = (const uint32_t *)kernels[k].code,
        };
        VkShaderModule mod = VK_NULL_HANDLE;
        res = vkCreateShaderModule(g->dev, &minfo, NULL, &mod);
        if (res != VK_SUCCESS)
            return ret2err(res);

        bool sub = k == ML_K_GEMV_Q4K || k == ML_K_ATTN || k == ML_K_ATTN_DEC;
        VkPipelineShaderStageRequiredSubgroupSizeCreateInfo size = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO,
            .requiredSubgroupSize = g->sub_size,
        };
        VkComputePipelineCreateInfo pinfo = {
            .sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .stage  = {
                .sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .pNext  = sub && g->sub_size ? &size : NULL,
                .flags  = sub && g->full_sub ?
                          VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT : 0,
                .stage  = VK_SHADER_STAGE_COMPUTE_BIT,
                .module = mod,
                .pName  = "main",
            },
            .layout = g->layout,
        };
        res = vkCreateComputePipelines(g->dev, VK_NULL_HANDLE, 1, &pinfo, NULL, &g->pipes[k]);
        vkDestroyShaderModule(g->dev, mod, NULL);
        if (res != VK_SUCCESS) {
            rx_warn("ml: unable to create the %s pipeline: %s", kernels[k].name, vk_strerror(res));
            return ret2err(res);
        }
    }
    return 0;
}

static int create_device(MLGpu *g)
{
    VkPhysicalDeviceVulkan13Features f13 = {
        .sType                = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
        .synchronization2     = VK_TRUE,
        .subgroupSizeControl  = g->sub_size != 0,
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

    const float prio = 1.0f;
    VkDeviceQueueCreateInfo qinfo = {
        .sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = g->qfam,
        .queueCount       = 1,
        .pQueuePriorities = &prio,
    };
    VkDeviceCreateInfo info = {
        .sType                = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext                = &f2,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos    = &qinfo,
    };
    VkResult res = vkCreateDevice(g->phys, &info, NULL, &g->dev);
    if (res != VK_SUCCESS) {
        rx_warn("ml: unable to create a device on %s: %s", g->props.deviceName, vk_strerror(res));
        g->dev = VK_NULL_HANDLE;
        return ret2err(res);
    }
    vkGetDeviceQueue(g->dev, g->qfam, 0, &g->queue);

    uint32_t nb_qf = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(g->phys, &nb_qf, NULL);
    VkQueueFamilyProperties *qf = calloc(RX_MAX(nb_qf, 1), sizeof(*qf));
    if (!qf)
        return RX_ERROR(ENOMEM);
    vkGetPhysicalDeviceQueueFamilyProperties(g->phys, &nb_qf, qf);
    uint32_t ts_bits = g->qfam < nb_qf ? qf[g->qfam].timestampValidBits : 0;
    free(qf);
    g->ts_mask = ts_bits >= 64 ? UINT64_MAX : ts_bits ? (UINT64_C(1) << ts_bits) - 1 : 0;

    VmaVulkanFunctions fn = {
        .vkGetInstanceProcAddr = vkGetInstanceProcAddr,
        .vkGetDeviceProcAddr   = vkGetDeviceProcAddr,
    };
    VmaAllocatorCreateInfo vinfo = {
        .flags            = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT,
        .physicalDevice   = g->phys,
        .device           = g->dev,
        .pVulkanFunctions = &fn,
        .instance         = g->inst,
        .vulkanApiVersion = VK_API_VERSION_1_3,
    };
    res = vmaCreateAllocator(&vinfo, &g->vma);
    if (res != VK_SUCCESS) {
        g->vma = NULL;
        return ret2err(res);
    }

    VkSemaphoreTypeCreateInfo tinfo = {
        .sType         = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
        .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
    };
    VkSemaphoreCreateInfo sinfo = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        .pNext = &tinfo,
    };
    res = vkCreateSemaphore(g->dev, &sinfo, NULL, &g->timeline);
    if (res != VK_SUCCESS)
        return ret2err(res);

    VkCommandPoolCreateInfo pinfo = {
        .sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = g->qfam,
    };
    res = vkCreateCommandPool(g->dev, &pinfo, NULL, &g->pool);
    if (res != VK_SUCCESS)
        return ret2err(res);
    VkCommandBufferAllocateInfo cinfo = {
        .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool        = g->pool,
        .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    res = vkAllocateCommandBuffers(g->dev, &cinfo, &g->cmd);
    if (res != VK_SUCCESS)
        return ret2err(res);

    if (g->profile && g->ts_mask) {
        VkQueryPoolCreateInfo qpinfo = {
            .sType      = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
            .queryType  = VK_QUERY_TYPE_TIMESTAMP,
            .queryCount = RX_ARRAY_ELEMS(g->time_kernel) + 1,
        };
        res = vkCreateQueryPool(g->dev, &qpinfo, NULL, &g->times);
        if (res != VK_SUCCESS)
            return ret2err(res);
    } else {
        g->profile = false;
    }

    return create_pipelines(g);
}

int ml_gpu_create(MLGpu *g, const char *device, bool validation)
{
    *g = (MLGpu) { 0 };
    atomic_init(&g->validation_errors, 0);

    const char *want = device && device[0] ? device : getenv("RXBOX_GPU_DEVICE");
    if (want && !want[0])
        want = NULL;
    const char *val = getenv("RXBOX_GPU_VALIDATION");
    validation = validation || (val && !strcmp(val, "1"));
    const char *prof = getenv("RXBOX_ML_PROFILE");
    g->profile = prof && !strcmp(prof, "1");

    int ret;
    if ((ret = create_instance(g, validation)) < 0)
        goto fail;
    if ((ret = pick_device(g, want)) < 0)
        goto fail;
    if ((ret = create_device(g)) < 0)
        goto fail;

    char sub[32] = "as the driver picks";
    if (g->sub_size)
        snprintf(sub, sizeof(sub), "of %u", g->sub_size);
    rx_debug("ml: %s, Vulkan %u.%u, queue family %u%s, subgroups %s", g->props.deviceName,
             VK_API_VERSION_MAJOR(g->props.apiVersion), VK_API_VERSION_MINOR(g->props.apiVersion),
             g->qfam, g->ts_mask ? "" : " (no timestamps)", sub);
    return 0;

fail:
    ml_gpu_destroy(g);
    return ret;
}

void ml_gpu_destroy(MLGpu *g)
{
    if (g->dev) {
        vkDeviceWaitIdle(g->dev);
        for (int k = 0; k < ML_NB_KERNELS; k++) {
            if (g->pipes[k])
                vkDestroyPipeline(g->dev, g->pipes[k], NULL);
        }
        if (g->layout)
            vkDestroyPipelineLayout(g->dev, g->layout, NULL);
        if (g->times)
            vkDestroyQueryPool(g->dev, g->times, NULL);
        if (g->pool)
            vkDestroyCommandPool(g->dev, g->pool, NULL);
        if (g->timeline)
            vkDestroySemaphore(g->dev, g->timeline, NULL);
        if (g->vma)
            vmaDestroyAllocator(g->vma);
        vkDestroyDevice(g->dev, NULL);
    }

    if (g->messenger) {
        PFN_vkDestroyDebugUtilsMessengerEXT destroy = (PFN_vkDestroyDebugUtilsMessengerEXT)
            vkGetInstanceProcAddr(g->inst, "vkDestroyDebugUtilsMessengerEXT");
        if (destroy)
            destroy(g->inst, g->messenger, NULL);
    }
    if (g->inst)
        vkDestroyInstance(g->inst, NULL);

    *g = (MLGpu) { 0 };
}

/* ----------------------------------------------------------------- buffers */

int ml_buf_create(MLGpu *g, MLBuf *b, VkDeviceSize size, enum MLMem mem)
{
    *b = (MLBuf) { 0 };
    if (!size)
        return RX_ERROR(EINVAL);

    VkBufferCreateInfo info = {
        .sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size        = size,
        .usage       = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                       VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                       VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };

    /* As rxbox's compute context: host memory for both directions, never
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
    VkResult res = vmaCreateBuffer(g->vma, &info, &ci, &b->buf, &b->alloc, &ai);
    if (res != VK_SUCCESS) {
        rx_warn("ml: unable to allocate a %llu byte buffer: %s", (unsigned long long)size,
                vk_strerror(res));
        *b = (MLBuf) { 0 };
        return ret2err(res);
    }

    b->size = size;
    b->map = ai.pMappedData;

    VkBufferDeviceAddressInfo ainfo = {
        .sType  = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
        .buffer = b->buf,
    };
    b->addr = vkGetBufferDeviceAddress(g->dev, &ainfo);
    return 0;
}

void ml_buf_free(MLGpu *g, MLBuf *b)
{
    if (!b || !b->buf)
        return;
    vmaDestroyBuffer(g->vma, b->buf, b->alloc);
    *b = (MLBuf) { 0 };
}

uint64_t ml_gpu_allocated(MLGpu *g)
{
    VmaBudget budgets[VK_MAX_MEMORY_HEAPS];
    vmaGetHeapBudgets(g->vma, budgets);
    const VkPhysicalDeviceMemoryProperties *mp = NULL;
    vmaGetMemoryProperties(g->vma, &mp);
    uint64_t total = 0;
    for (uint32_t i = 0; i < mp->memoryHeapCount; i++)
        total += budgets[i].statistics.allocationBytes;
    return total;
}

int ml_buf_invalidate(MLGpu *g, MLBuf *b)
{
    if (!b->map)
        return 0;
    return ret2err(vmaInvalidateAllocation(g->vma, b->alloc, 0, VK_WHOLE_SIZE));
}

int ml_buf_flush(MLGpu *g, MLBuf *b, VkDeviceSize size)
{
    if (!b->map)
        return 0;
    return ret2err(vmaFlushAllocation(g->vma, b->alloc, 0, size));
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
        vkCmdResetQueryPool(g->cmd, g->times, 0, RX_ARRAY_ELEMS(g->time_kernel) + 1);
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
    if (g->profile && g->nb_times < (int)RX_ARRAY_ELEMS(g->time_kernel)) {
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
    uint64_t ts[RX_ARRAY_ELEMS(g->time_kernel) + 1];
    if (vkGetQueryPoolResults(g->dev, g->times, 0, (uint32_t)g->nb_times + 1, sizeof(ts), ts,
                              sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
        return;
    double period = g->props.limits.timestampPeriod * 1e-6;
    for (int i = 0; i < g->nb_times; i++) {
        uint64_t d = (ts[i + 1] - ts[i]) & g->ts_mask;
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
        return RX_ERROR(EIO);
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
        rx_warn("ml: recording failed: %s", vk_strerror(res));
        vkResetCommandBuffer(g->cmd, 0);
        g->nb_times = 0;
        return ret2err(res);
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
    res = vkQueueSubmit2(g->queue, 1, &info, VK_NULL_HANDLE);
    if (res != VK_SUCCESS) {
        rx_warn("ml: submission failed: %s", vk_strerror(res));
        vkResetCommandBuffer(g->cmd, 0);
        g->nb_times = 0;
        return ret2err(res);
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
    res = vkWaitSemaphores(g->dev, &winfo, UINT64_C(60000000000));
    if (res != VK_SUCCESS) {
        /* The command buffer may still be running, or never end: wait
         * until the device is idle (a hung GPU is reset by its driver,
         * and the wait ends with the device lost) before its buffers can
         * be freed, and use the context no more. */
        rx_err("ml: waiting for the GPU failed: %s; the model stops", vk_strerror(res));
        vkDeviceWaitIdle(g->dev);
        g->lost = true;
        vkResetCommandBuffer(g->cmd, 0);
        g->nb_times = 0;
        return ret2err(res);
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
    rx_info("ml: GPU time by kernel, %.1f ms in all:", total);
    for (int k = 0; k < ML_NB_KERNELS; k++) {
        if (!g->kernel_calls[k])
            continue;
        rx_info("  %-14s %9.1f ms %5.1f %%  %7lld calls", kernels[k].name, g->kernel_ms[k],
                total > 0 ? 100.0 * g->kernel_ms[k] / total : 0.0,
                (long long)g->kernel_calls[k]);
    }
}
