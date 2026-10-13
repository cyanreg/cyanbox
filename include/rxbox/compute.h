/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

#ifndef RXBOX_COMPUTE_H
#define RXBOX_COMPUTE_H

/* Vulkan compute without a window: the GPU the analysis' spectral front end
 * runs on. A context is an instance, the chosen device, one compute queue,
 * VMA and a timeline semaphore; it opens no window and needs no compositor,
 * so the CLI, the jobs pool, the tools and the tests all use it alike. The
 * interface makes its own (in its jobs pool) on the device the window's
 * --device names rather than share the window's VkDevice, whose queue is
 * the frame loop's alone.
 *
 * What runs on it is deliberately algorithm-neutral: framed real FFTs with
 * band energies, and band envelopes around chosen instants. The caller (the
 * analysis, src/analysis/spectrum.c) builds every table (windows, filter
 * weights, masks) and decides everything from the results in plain C. The
 * results equal the CPU's to float rounding, not bit for bit
 * (docs/BEATS.md, section 1).
 *
 * Thread safety: a context may be shared by any number of threads. A plan
 * belongs to the thread using it (it holds its own command pool, buffers and
 * timestamps); submissions to the one queue are serialised inside, and each
 * waits on its own timeline value, so threads never wait for each other's
 * work.
 *
 * Environment:
 *   RXBOX_ANALYSIS_GPU=0   no context is made (rx_compute_create: -ENODEV):
 *                          everything runs on the CPU
 *   RXBOX_ANALYSIS_GPU=1   a GPU is required: callers that can treat its
 *                          absence as an error do (rx_compute_mode)
 *   RXBOX_GPU_DEVICE=NAME  prefer the device whose name contains NAME
 *                          (case-insensitive), unless the options name one
 *   RXBOX_GPU_VALIDATION=1 the Khronos validation layer, as options.validation */

#include "rxbox/common.h"

typedef struct RXCompute RXCompute;

typedef struct RXComputeOptions {
    const char *device;         /* substring of the GPU's name; NULL: $RXBOX_GPU_DEVICE, else the
                                   best: discrete before integrated. A CPU implementation
                                   (llvmpipe) only when named */
    bool validation;            /* VK_LAYER_KHRONOS_validation; errors are counted */
} RXComputeOptions;
void rx_compute_options_default(RXComputeOptions *o);

/* -ENODEV when RXBOX_ANALYSIS_GPU=0 or no device can run the shaders
 * (Vulkan 1.3 with buffer device addresses and 32 kB of shared memory). */
int  rx_compute_create(RXCompute **c, const RXComputeOptions *o);
/* Waits for the queue; every plan must have been freed. */
void rx_compute_destroy(RXCompute **c);
const char *rx_compute_device_name(const RXCompute *c);
/* Validation errors reported so far (0 without validation). */
int  rx_compute_validation_errors(const RXCompute *c);

/* What RXBOX_ANALYSIS_GPU asks for. */
enum RXComputeMode {
    RX_COMPUTE_AUTO = 0,        /* unset: a GPU when there is one, else the CPU */
    RX_COMPUTE_OFF,             /* 0: the CPU only */
    RX_COMPUTE_REQUIRED,        /* 1: a GPU, and its absence is an error where it can be */
};
enum RXComputeMode rx_compute_mode(void);

/* ------------------------------------------------------------ STFT bands */

/* Band energies of a short-time Fourier transform. Frame f covers samples
 * [f * hop - nfft / 2, f * hop + nfft / 2) of the signal (zero outside it),
 * multiplied by `window`; its real DFT X (unnormalised, X_k = sum x_n
 * e^(-2 pi i k n / nfft)) gives the power |X_k|^2 of bins 0 .. nfft / 2,
 * and band b sums the power of bins [k0[b], k1[b]) under its weights; the
 * sum s is returned as log(1 + scale * sqrt(s)) (as s when scale is 0). */
typedef struct RXComputeSTFT {
    int nfft;                   /* power of two, 64 .. 8192 */
    int hop;
    const float *window;        /* nfft */
    int nb_bands;               /* 1 .. 256 */
    const int *k0, *k1;         /* nb_bands */
    const float *weights;       /* band after band, k1[b] - k0[b] each */
    float scale;
} RXComputeSTFT;

typedef struct RXComputeSTFTPlan RXComputeSTFTPlan;
int  rx_compute_stft_plan(RXCompute *c, const RXComputeSTFT *p, RXComputeSTFTPlan **plan);
void rx_compute_stft_free(RXComputeSTFTPlan **plan);

/* Called for consecutive runs of frames, in order: bands[i * nb_bands + b]
 * is band b of frame f0 + i. A negative return stops the run with it. */
typedef int (*RXComputeFramesFn)(void *opaque, int64_t f0, int nb, const float *bands);

/* Frames [0, nb_frames) of x[0 .. n). The signal is uploaded a stretch at a
 * time, two stretches in flight, so the callback for one runs while the GPU
 * transforms the next. */
int  rx_compute_stft_run(RXComputeSTFTPlan *plan, const float *x, int64_t n, int64_t nb_frames,
                         RXComputeFramesFn fn, void *opaque);

/* ------------------------------------------------------- band envelopes */

/* Band envelopes of windows of a signal. Each window of nfft samples
 * (zero outside the signal) is multiplied by `taper`, transformed (real
 * DFT as above), multiplied by each band's real gains (zero phase; they
 * carry the inverse transform's 1 / nfft), and transformed back
 * (unnormalised). Of each band's signal y, the output is the root of the
 * mean of y^2 over [p - h, p + h] (h = smooth[b]) at the points
 * p = nfft / 2 + (i - half) * step, i in [0, 2 * half]. */
typedef struct RXComputeEnv {
    int nfft;                   /* power of two, 64 .. 8192 */
    const float *taper;         /* nfft */
    int nb_bands;               /* 1 .. 8 */
    const float *gains;         /* [nb_bands][nfft / 2 + 1] */
    const int *smooth;          /* [nb_bands] */
    int step, half;             /* the points must stay inside the window */
} RXComputeEnv;

typedef struct RXComputeEnvPlan RXComputeEnvPlan;
int  rx_compute_env_plan(RXCompute *c, const RXComputeEnv *p, RXComputeEnvPlan **plan);
void rx_compute_env_free(RXComputeEnvPlan **plan);

/* Window i starts at sample start[i] of x[0 .. n). out is
 * [nb][nb_bands][2 * half + 1]. */
int  rx_compute_env_run(RXComputeEnvPlan *plan, const float *x, int64_t n,
                        const int64_t *start, int nb, float *out);

/* GPU time of a plan's work so far, from timestamps (0 when the queue has
 * none), and the wall time its runs took, in milliseconds. */
void rx_compute_stft_times(const RXComputeSTFTPlan *plan, double *gpu_ms, double *wall_ms);
void rx_compute_env_times(const RXComputeEnvPlan *plan, double *gpu_ms, double *wall_ms);

#endif /* RXBOX_COMPUTE_H */
