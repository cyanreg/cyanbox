/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* Private to the compute context (cyanbox/compute.h): compute.c owns the
 * device (set up as the window's and the model's are, vkdev.h, but its
 * own), the timeline semaphore and the pipelines; compute_dsp.c the plans
 * that run the shaders. */

#ifndef CYANBOX_GPU_COMPUTE_PRIV_H
#define CYANBOX_GPU_COMPUTE_PRIV_H

#include "cyanbox/compute.h"

#include <pthread.h>
#include <stdatomic.h>

#include "vkdev.h"

/* Every shader runs 256 invocations a workgroup; the FFT's register
 * arrays (shaders/fft.glsl) are sized for that and a transform of at most
 * 4096 complex points. */
#define CB_COMPUTE_THREADS  256
#define CB_COMPUTE_MAX_LOG2 13      /* 8192-point real transforms */
#define CB_COMPUTE_MIN_LOG2 6

/* The push constants are at most 128 bytes, what every device has. */
#define CB_COMPUTE_PUSH_SIZE 128

enum CBComputeKernel {
    CB_KERNEL_STFT,                 /* shaders/stft.comp */
    CB_KERNEL_ENV,                  /* shaders/env.comp */
    CB_NB_KERNELS,
};

struct CBCompute {
    CBVkDev vk;

    /* Submissions: one queue, so vkQueueSubmit2 is serialised by `qlock`;
     * submission i signals `timeline` to i, and whoever submitted it waits
     * for that value alone. */
    pthread_mutex_t qlock;
    VkSemaphore timeline;
    uint64_t submitted;             /* under qlock */
    atomic_bool lost;               /* a wait failed: no more submissions (compute.c) */

    /* One layout for every kernel (push constants only, no descriptors),
     * and the pipelines by kernel and transform size, made on first use. */
    VkPipelineLayout layout;
    pthread_mutex_t plock;
    VkPipeline pipes[CB_NB_KERNELS][CB_COMPUTE_MAX_LOG2 + 1];
};

/* A buffer with its device address; mapped when host visible. */
typedef struct CBCBuf {
    VkBuffer        buf;
    VmaAllocation   alloc;
    VkDeviceSize    size;
    VkDeviceAddress addr;
    void           *map;
    bool            device_local;   /* the GPU reads it at full speed (VRAM, or a UMA's memory) */
} CBCBuf;

enum CBCMem {
    CB_CMEM_DEVICE,                 /* device local, never mapped */
    CB_CMEM_UPLOAD,                 /* host memory written sequentially by the CPU */
    CB_CMEM_READBACK,               /* host memory, cached, that the GPU writes */
};

int  cb_cbuf_create(CBCompute *c, CBCBuf *b, VkDeviceSize size, enum CBCMem mem);
void cb_cbuf_free(CBCompute *c, CBCBuf *b);
/* Makes what the GPU wrote visible to the CPU (non-coherent memory). */
int  cb_cbuf_invalidate(CBCompute *c, CBCBuf *b);
/* Makes what the CPU wrote visible to the GPU (non-coherent memory). */
int  cb_cbuf_flush(CBCompute *c, CBCBuf *b, VkDeviceSize size);

/* The pipeline of `kernel` for transforms of 2^log2n real points. */
int  cb_compute_pipeline(CBCompute *c, enum CBComputeKernel kernel, int log2n, VkPipeline *out);

/* Submits one recorded command buffer; *value is the timeline value that
 * says it has completed. -EIO once a wait has failed. */
int  cb_compute_submit(CBCompute *c, VkCommandBuffer cmd, uint64_t *value);
/* Waits for a submission. On an error the device is idle when it returns
 * (nothing is running any more), so the submission's buffers can go. */
int  cb_compute_wait(CBCompute *c, uint64_t value);

#endif /* CYANBOX_GPU_COMPUTE_PRIV_H */
