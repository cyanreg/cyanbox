/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

#ifndef CYANBOX_COMPUTE_H
#define CYANBOX_COMPUTE_H

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
 *   CYANBOX_ANALYSIS_GPU=0   no context is made (cb_compute_create: -ENODEV):
 *                          everything runs on the CPU
 *   CYANBOX_ANALYSIS_GPU=1   a GPU is required: callers that can treat its
 *                          absence as an error do (cb_compute_mode)
 *   CYANBOX_GPU_DEVICE=NAME  prefer the device whose name contains NAME
 *                          (case-insensitive), unless the options name one
 *   CYANBOX_GPU_VALIDATION=1 the Khronos validation layer, as options.validation */

#include "cyanbox/common.h"

typedef struct CBCompute CBCompute;

typedef struct CBComputeOptions {
    const char *device;         /* substring of the GPU's name; NULL: $CYANBOX_GPU_DEVICE, else the
                                   best: discrete before integrated. A CPU implementation
                                   (llvmpipe) only when named */
    bool validation;            /* VK_LAYER_KHRONOS_validation; errors are counted */
} CBComputeOptions;
void cb_compute_options_default(CBComputeOptions *o);

/* -ENODEV when CYANBOX_ANALYSIS_GPU=0 or no device can run the shaders
 * (Vulkan 1.3 with buffer device addresses and 32 kB of shared memory). */
int  cb_compute_create(CBCompute **c, const CBComputeOptions *o);
/* Waits for the queue; every plan must have been freed. */
void cb_compute_destroy(CBCompute **c);
const char *cb_compute_device_name(const CBCompute *c);
/* Validation errors reported so far (0 without validation). */
int  cb_compute_validation_errors(const CBCompute *c);

/* What CYANBOX_ANALYSIS_GPU asks for. */
enum CBComputeMode {
    CB_COMPUTE_AUTO = 0,        /* unset: a GPU when there is one, else the CPU */
    CB_COMPUTE_OFF,             /* 0: the CPU only */
    CB_COMPUTE_REQUIRED,        /* 1: a GPU, and its absence is an error where it can be */
};
enum CBComputeMode cb_compute_mode(void);

/* ------------------------------------------------------------ STFT bands */

/* Band energies of a short-time Fourier transform. Frame f covers samples
 * [f * hop - nfft / 2, f * hop + nfft / 2) of the signal (zero outside it),
 * multiplied by `window`; its real DFT X (unnormalised, X_k = sum x_n
 * e^(-2 pi i k n / nfft)) gives the power |X_k|^2 of bins 0 .. nfft / 2,
 * and band b sums the power of bins [k0[b], k1[b]) under its weights; the
 * sum s is returned as log(1 + scale * sqrt(s)) (as s when scale is 0). */
typedef struct CBComputeSTFT {
    int nfft;                   /* power of two, 64 .. 8192 */
    int hop;
    const float *window;        /* nfft */
    int nb_bands;               /* 1 .. 256 */
    const int *k0, *k1;         /* nb_bands */
    const float *weights;       /* band after band, k1[b] - k0[b] each */
    float scale;
} CBComputeSTFT;

typedef struct CBComputeSTFTPlan CBComputeSTFTPlan;
int  cb_compute_stft_plan(CBCompute *c, const CBComputeSTFT *p, CBComputeSTFTPlan **plan);
void cb_compute_stft_free(CBComputeSTFTPlan **plan);

/* Called for consecutive runs of frames, in order: bands[i * nb_bands + b]
 * is band b of frame f0 + i. A negative return stops the run with it. */
typedef int (*CBComputeFramesFn)(void *opaque, int64_t f0, int nb, const float *bands);

/* Frames [0, nb_frames) of x[0 .. n). The signal is uploaded a stretch at a
 * time, two stretches in flight, so the callback for one runs while the GPU
 * transforms the next. */
int  cb_compute_stft_run(CBComputeSTFTPlan *plan, const float *x, int64_t n, int64_t nb_frames,
                         CBComputeFramesFn fn, void *opaque);

/* ------------------------------------------------------- band envelopes */

/* Band envelopes of windows of a signal. Each window of nfft samples
 * (zero outside the signal) is multiplied by `taper`, transformed (real
 * DFT as above), multiplied by each band's real gains (zero phase; they
 * carry the inverse transform's 1 / nfft), and transformed back
 * (unnormalised). Of each band's signal y, the output is the root of the
 * mean of y^2 over [p - h, p + h] (h = smooth[b]) at the points
 * p = nfft / 2 + (i - half) * step, i in [0, 2 * half]. */
typedef struct CBComputeEnv {
    int nfft;                   /* power of two, 64 .. 8192 */
    const float *taper;         /* nfft */
    int nb_bands;               /* 1 .. 8 */
    const float *gains;         /* [nb_bands][nfft / 2 + 1] */
    const int *smooth;          /* [nb_bands] */
    int step, half;             /* the points must stay inside the window */
} CBComputeEnv;

typedef struct CBComputeEnvPlan CBComputeEnvPlan;
int  cb_compute_env_plan(CBCompute *c, const CBComputeEnv *p, CBComputeEnvPlan **plan);
void cb_compute_env_free(CBComputeEnvPlan **plan);

/* Window i starts at sample start[i] of x[0 .. n). out is
 * [nb][nb_bands][2 * half + 1]. */
int  cb_compute_env_run(CBComputeEnvPlan *plan, const float *x, int64_t n,
                        const int64_t *start, int nb, float *out);

/* GPU time of a plan's work so far, from timestamps (0 when the queue has
 * none), and the wall time its runs took, in milliseconds. */
void cb_compute_stft_times(const CBComputeSTFTPlan *plan, double *gpu_ms, double *wall_ms);
void cb_compute_env_times(const CBComputeEnvPlan *plan, double *gpu_ms, double *wall_ms);

#endif /* CYANBOX_COMPUTE_H */
