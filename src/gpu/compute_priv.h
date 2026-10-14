/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* Private to the compute context (rxbox/compute.h): compute.c owns the
 * device (set up as the window's and the model's are, vkdev.h, but its
 * own), the timeline semaphore and the pipelines; compute_dsp.c the plans
 * that run the shaders. */

#ifndef RXBOX_GPU_COMPUTE_PRIV_H
#define RXBOX_GPU_COMPUTE_PRIV_H

#include "rxbox/compute.h"

#include <pthread.h>
#include <stdatomic.h>

#include "vkdev.h"

/* Every shader runs 256 invocations a workgroup; the FFT's register
 * arrays (shaders/fft.glsl) are sized for that and a transform of at most
 * 4096 complex points. */
#define RX_COMPUTE_THREADS  256
#define RX_COMPUTE_MAX_LOG2 13      /* 8192-point real transforms */
#define RX_COMPUTE_MIN_LOG2 6

/* The push constants are at most 128 bytes, what every device has. */
#define RX_COMPUTE_PUSH_SIZE 128

enum RXComputeKernel {
    RX_KERNEL_STFT,                 /* shaders/stft.comp */
    RX_KERNEL_ENV,                  /* shaders/env.comp */
    RX_NB_KERNELS,
};

struct RXCompute {
    RXVkDev vk;

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
    VkPipeline pipes[RX_NB_KERNELS][RX_COMPUTE_MAX_LOG2 + 1];
};

/* A buffer with its device address; mapped when host visible. */
typedef struct RXCBuf {
    VkBuffer        buf;
    VmaAllocation   alloc;
    VkDeviceSize    size;
    VkDeviceAddress addr;
    void           *map;
    bool            device_local;   /* the GPU reads it at full speed (VRAM, or a UMA's memory) */
} RXCBuf;

enum RXCMem {
    RX_CMEM_DEVICE,                 /* device local, never mapped */
    RX_CMEM_UPLOAD,                 /* host memory written sequentially by the CPU */
    RX_CMEM_READBACK,               /* host memory, cached, that the GPU writes */
};

int  rx_cbuf_create(RXCompute *c, RXCBuf *b, VkDeviceSize size, enum RXCMem mem);
void rx_cbuf_free(RXCompute *c, RXCBuf *b);
/* Makes what the GPU wrote visible to the CPU (non-coherent memory). */
int  rx_cbuf_invalidate(RXCompute *c, RXCBuf *b);
/* Makes what the CPU wrote visible to the GPU (non-coherent memory). */
int  rx_cbuf_flush(RXCompute *c, RXCBuf *b, VkDeviceSize size);

/* The pipeline of `kernel` for transforms of 2^log2n real points. */
int  rx_compute_pipeline(RXCompute *c, enum RXComputeKernel kernel, int log2n, VkPipeline *out);

/* Submits one recorded command buffer; *value is the timeline value that
 * says it has completed. -EIO once a wait has failed. */
int  rx_compute_submit(RXCompute *c, VkCommandBuffer cmd, uint64_t *value);
/* Waits for a submission. On an error the device is idle when it returns
 * (nothing is running any more), so the submission's buffers can go. */
int  rx_compute_wait(RXCompute *c, uint64_t value);

#endif /* RXBOX_GPU_COMPUTE_PRIV_H */
