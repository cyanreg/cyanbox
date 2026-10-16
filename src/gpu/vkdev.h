/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* A Vulkan device as cyanbox's three users of the GPU set one up: the window
 * (vk.c), the analysis' compute context (compute.c) and the model
 * (src/ml/gpu.c). Each has an instance and a device of its own, on
 * purpose (compute.c says why): a device lost to a hung kernel takes no
 * other with it, the window's frame loop never waits on a lock the
 * analyses' threads take, and the model's 4 GB go with its device. What
 * they set up the same way is here:
 *
 *  - the instance: Vulkan 1.3, the extensions the user needs (the
 *    window's surface ones), and when asked the validation layer with
 *    synchronisation checks, its messages logged under the user's tag
 *    and its errors counted;
 *  - the physical device: a named one (a case-insensitive part of its
 *    name) before any other, else discrete before integrated before
 *    virtual; a CPU implementation (llvmpipe) only when named or when the
 *    user allows it; a device the user's check refuses never;
 *  - the logical device with one queue, the queue's timestamp mask, and
 *    VMA with buffer device addresses;
 *  - a timeline semaphore, and VkResult as an errno and as words.
 *
 * What a device must have, which queue family serves and which features
 * are enabled stay the user's: it says them through CBVkDevOptions.check
 * and the feature chain it hands cb_vkdev_device. Nothing here is atomic
 * C: the window's C++ includes it (gpu_priv.h). */

#ifndef CYANBOX_GPU_VKDEV_H
#define CYANBOX_GPU_VKDEV_H

#include <stdbool.h>
#include <stdint.h>

#include <vulkan/vulkan.h>

/* VMA tests extension macros that only exist on Windows with #if. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wundef"
#include "vk_mem_alloc.h"
#pragma GCC diagnostic pop

typedef struct CBVkDev {
    const char *tag;                    /* the log prefix: "gpu", "compute", "ml" */
    VkInstance inst;
    VkDebugUtilsMessengerEXT messenger;
    int validation_errors;              /* atomic: cb_vkdev_validation_errors */
    VkPhysicalDevice phys;
    VkPhysicalDeviceProperties props;
    uint32_t qfam;                      /* the queue's family */
    VkDevice dev;
    VkQueue queue;
    VmaAllocator vma;
    uint64_t ts_mask;                   /* the queue's valid timestamp bits, 0 = none */
} CBVkDev;

/* Why a device cannot serve, or NULL, and then *qfam the queue family it
 * would use. Called for every device that has Vulkan 1.3, and once more,
 * last, for the one chosen: what it keeps in `opaque` is that device's. */
typedef const char *(*CBVkDevCheck)(void *opaque, VkPhysicalDevice pd,
                                    const VkPhysicalDeviceProperties *props, uint32_t *qfam);

typedef struct CBVkDevOptions {
    const char *tag;
    bool validation;
    bool performance;                   /* the layer's performance warnings too */
    const char *const *exts;            /* instance extensions the user needs */
    uint32_t nb_exts;
    const char *device;                 /* the name asked for, NULL or "" none */
    bool cpu;                           /* a CPU implementation without being named */
    CBVkDevCheck check;
    void *opaque;
} CBVkDevOptions;

/* The three steps, in order: the window makes its surface between the
 * first two. Each fails with a negative errno, logged at debug level
 * only where the user decides what a failure means (no GPU is an
 * ordinary answer for the analyses), and at warning level where it is
 * not one (a device that passed the checks and cannot be made).
 * cb_vkdev_destroy takes what they made, after the user's own objects;
 * it is safe on a part-made or zeroed one. */
int  cb_vkdev_instance(CBVkDev *d, const CBVkDevOptions *o);
int  cb_vkdev_pick(CBVkDev *d, const CBVkDevOptions *o);
int  cb_vkdev_device(CBVkDev *d, const void *features, const char *const *exts,
                     uint32_t nb_exts);
void cb_vkdev_destroy(CBVkDev *d);

int  cb_vkdev_timeline(CBVkDev *d, VkSemaphore *out);
int  cb_vkdev_validation_errors(const CBVkDev *d);

/* For the checks: whether a device has an extension; a queue family with
 * compute, a compute-only one when there is one (AMD's asynchronous
 * queues: the desktop's graphics are not held up behind the user's
 * work), or why there is none. */
bool        cb_vkdev_has_ext(VkPhysicalDevice pd, const char *name);
const char *cb_vkdev_compute_family(VkPhysicalDevice pd, uint32_t *qfam);

int         cb_vk_ret2err(VkResult res);
const char *cb_vk_strerror(VkResult res);

#endif /* CYANBOX_GPU_VKDEV_H */
