/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* The plans of rxbox/compute.h: STFT band energies (shaders/stft.comp) and
 * band envelopes of windows (shaders/env.comp). A plan holds its tables on
 * the GPU, its own command pool, two slots of buffers and timestamps; it
 * is used by one thread at a time (the analysis that made it).
 *
 * Choices, and why:
 *  - Two slots in flight. While the GPU transforms one stretch of the
 *    signal (or one batch of windows), the CPU copies the next into the
 *    other slot and then hands the finished one's results to the caller, so
 *    the copies, the transfer and the caller's own work overlap.
 *  - The signal goes up a stretch at a time, never whole: the GPU never
 *    holds more than two stretches (4096 frames, 28 s, is 4.8 MB), however
 *    long the track (a mix of an hour would otherwise be 635 MB of VRAM per
 *    analysis, eleven at once). The envelopes' windows are gathered on the
 *    CPU for the same reason; they are 15 % of the track.
 *  - Uploads are written into host memory and copied by the GPU into its
 *    own memory (discrete), or read where they are (an integrated GPU,
 *    whose memory that is). Results are written by the shaders straight
 *    into cached host memory: they are a seventh of the input or less.
 *  - Slot buffers grow to the largest run seen, so a short track or a
 *    single window does not allocate for a whole stretch.
 *  - Every table is computed on the CPU in double precision (twiddles) or
 *    handed in by the caller as floats (windows, weights, gains), so the
 *    GPU multiplies by exactly the numbers the C code does. */

#include "compute_priv.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Frames per STFT stretch, windows per envelope batch. */
#define STFT_CHUNK 4096
#define ENV_BATCH  256

typedef struct Slot {
    VkCommandBuffer cmd;
    RXCBuf in, in_dev;              /* in_dev only when `in` is not device local */
    RXCBuf out;
    uint64_t value;                 /* the timeline value of its submission */
    int64_t first;                  /* frame or window */
    int nb;
} Slot;

/* What both kinds of plan share. */
typedef struct Plan {
    RXCompute *c;
    int log2n, nfft;
    VkPipeline pipe;
    RXCBuf tables;
    VkCommandPool pool;
    VkQueryPool times;              /* two per slot, when the queue has timestamps */
    Slot slots[2];
    double gpu_ms, wall_ms;
} Plan;

struct RXComputeSTFTPlan {
    Plan p;
    int hop, nb_bands;
    float scale;
    VkDeviceAddress tw, window, bands, weights;
};

struct RXComputeEnvPlan {
    Plan p;
    int nb_bands, step, half;
    VkDeviceAddress tw, taper, gains, smooth;
};

/* The push constants of stft.comp and env.comp (std430). */
typedef struct STFTPush {
    VkDeviceAddress tw, window, bands, weights, samples, dst;
    uint32_t hop, nb_bands, nb_frames;
    float    scale;
} STFTPush;

typedef struct EnvPush {
    VkDeviceAddress tw, taper, gains, widths, windows, dst;
    uint32_t step, half_len, nb_bands, nb_windows;
} EnvPush;

static_assert(sizeof(STFTPush) == 64, "push constant layout");
static_assert(sizeof(EnvPush) == 64, "push constant layout");

/* ------------------------------------------------------------------ plans */

static int log2_exact(int n)
{
    int l = 0;
    while ((1 << l) < n)
        l++;
    return (1 << l) == n ? l : -1;
}

static void plan_uninit(Plan *p)
{
    RXCompute *c = p->c;
    if (!c)
        return;
    for (int i = 0; i < 2; i++) {
        Slot *s = &p->slots[i];
        /* Every run waits for its submissions before it returns. */
        rx_cbuf_free(c, &s->in);
        rx_cbuf_free(c, &s->in_dev);
        rx_cbuf_free(c, &s->out);
    }
    rx_cbuf_free(c, &p->tables);
    if (p->times)
        vkDestroyQueryPool(c->dev, p->times, NULL);
    if (p->pool)
        vkDestroyCommandPool(c->dev, p->pool, NULL);
    memset(p, 0, sizeof(*p));
}

static int plan_init(Plan *p, RXCompute *c, enum RXComputeKernel kernel, int nfft)
{
    p->c = c;
    p->nfft = nfft;
    p->log2n = log2_exact(nfft);
    if (p->log2n < RX_COMPUTE_MIN_LOG2 || p->log2n > RX_COMPUTE_MAX_LOG2)
        return RX_ERROR(EINVAL);

    int ret = rx_compute_pipeline(c, kernel, p->log2n, &p->pipe);
    if (ret < 0)
        return ret;

    VkCommandPoolCreateInfo pinfo = {
        .sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = c->qfam,
    };
    VkResult res = vkCreateCommandPool(c->dev, &pinfo, NULL, &p->pool);
    if (res != VK_SUCCESS)
        return rx_compute_ret2err(res);
    VkCommandBuffer cmds[2];
    VkCommandBufferAllocateInfo cinfo = {
        .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool        = p->pool,
        .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 2,
    };
    res = vkAllocateCommandBuffers(c->dev, &cinfo, cmds);
    if (res != VK_SUCCESS)
        return rx_compute_ret2err(res);
    p->slots[0].cmd = cmds[0];
    p->slots[1].cmd = cmds[1];

    if (c->ts_mask) {
        VkQueryPoolCreateInfo qinfo = {
            .sType      = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
            .queryType  = VK_QUERY_TYPE_TIMESTAMP,
            .queryCount = 4,
        };
        res = vkCreateQueryPool(c->dev, &qinfo, NULL, &p->times);
        if (res != VK_SUCCESS)
            return rx_compute_ret2err(res);
    }
    return 0;
}

/* The twiddles of a real transform of n points: exp(-2 pi i k / n), k in
 * [0, n), as (re, im) floats (shaders/fft.glsl). */
static void twiddles(float *dst, int n)
{
    for (int k = 0; k < n; k++) {
        double a = 2.0 * M_PI * k / n;
        dst[2 * k] = (float)cos(a);
        dst[2 * k + 1] = (float)-sin(a);
    }
}

static void begin(VkCommandBuffer cmd)
{
    VkCommandBufferBeginInfo binfo = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    vkBeginCommandBuffer(cmd, &binfo);
}

static void barrier(VkCommandBuffer cmd, VkPipelineStageFlags2 src, VkAccessFlags2 src_access,
                    VkPipelineStageFlags2 dst, VkAccessFlags2 dst_access)
{
    VkMemoryBarrier2 mb = {
        .sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask  = src,
        .srcAccessMask = src_access,
        .dstStageMask  = dst,
        .dstAccessMask = dst_access,
    };
    VkDependencyInfo dep = {
        .sType              = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .memoryBarrierCount = 1,
        .pMemoryBarriers    = &mb,
    };
    vkCmdPipelineBarrier2(cmd, &dep);
}

/* The tables, uploaded once: read where they are when the upload memory is
 * the device's, else copied into device memory (one submission, waited
 * for). */
static int upload_tables(Plan *p, const void *data, size_t size)
{
    RXCompute *c = p->c;
    RXCBuf staging;
    int ret = rx_cbuf_create(c, &staging, size, RX_CMEM_UPLOAD);
    if (ret < 0)
        return ret;
    memcpy(staging.map, data, size);
    if ((ret = rx_cbuf_flush(c, &staging, size)) < 0) {
        rx_cbuf_free(c, &staging);
        return ret;
    }
    if (staging.device_local) {
        p->tables = staging;
        return 0;
    }

    ret = rx_cbuf_create(c, &p->tables, size, RX_CMEM_DEVICE);
    if (ret < 0) {
        rx_cbuf_free(c, &staging);
        return ret;
    }
    VkCommandBuffer cmd = p->slots[0].cmd;
    begin(cmd);
    VkBufferCopy region = { .size = size };
    vkCmdCopyBuffer(cmd, staging.buf, p->tables.buf, 1, &region);
    barrier(cmd, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    vkEndCommandBuffer(cmd);
    uint64_t value = 0;
    ret = rx_compute_submit(c, cmd, &value);
    if (ret >= 0)
        ret = rx_compute_wait(c, value);
    rx_cbuf_free(c, &staging);
    return ret;
}

/* Grows a slot's buffers to hold `in` input and `out` output bytes. */
static int slot_reserve(Plan *p, Slot *s, size_t in, size_t out)
{
    RXCompute *c = p->c;
    int ret = 0;
    if (s->in.size < in) {
        rx_cbuf_free(c, &s->in);
        rx_cbuf_free(c, &s->in_dev);
        if ((ret = rx_cbuf_create(c, &s->in, in, RX_CMEM_UPLOAD)) < 0)
            return ret;
        if (!s->in.device_local && (ret = rx_cbuf_create(c, &s->in_dev, in, RX_CMEM_DEVICE)) < 0)
            return ret;
    }
    if (s->out.size < out) {
        rx_cbuf_free(c, &s->out);
        if ((ret = rx_cbuf_create(c, &s->out, out, RX_CMEM_READBACK)) < 0)
            return ret;
    }
    return 0;
}

/* Begins a slot's command buffer: the barrier after its last use, the
 * first timestamp, the copy of its input into device memory when needed,
 * the pipeline. The caller pushes its constants and dispatches, and
 * slot_submit ends it. Returns the address the shader reads its input
 * from. */
static VkDeviceAddress slot_begin(Plan *p, int i, size_t in_bytes)
{
    Slot *s = &p->slots[i];
    VkCommandBuffer cmd = s->cmd;
    begin(cmd);
    /* The slot's buffers were last used by its previous submission, which
     * has been waited for; this orders the new writes after its reads for
     * the device too. */
    barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_COPY_BIT,
            VK_ACCESS_2_NONE,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_COPY_BIT,
            VK_ACCESS_2_NONE);
    if (p->times) {
        vkCmdResetQueryPool(cmd, p->times, 2 * i, 2);
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, p->times, 2 * i);
    }
    VkDeviceAddress addr = s->in.addr;
    if (s->in_dev.buf) {
        VkBufferCopy region = { .size = in_bytes };
        vkCmdCopyBuffer(cmd, s->in.buf, s->in_dev.buf, 1, &region);
        barrier(cmd, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
        addr = s->in_dev.addr;
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p->pipe);
    return addr;
}

static int slot_submit(Plan *p, int i, size_t in_bytes)
{
    Slot *s = &p->slots[i];
    VkCommandBuffer cmd = s->cmd;
    /* The results are read by the host once the timeline says so. */
    barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
    if (p->times)
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, p->times, 2 * i + 1);
    VkResult res = vkEndCommandBuffer(cmd);
    if (res != VK_SUCCESS)
        return rx_compute_ret2err(res);
    int ret = rx_cbuf_flush(p->c, &s->in, in_bytes);
    if (ret < 0)
        return ret;
    return rx_compute_submit(p->c, cmd, &s->value);
}

static int slot_wait(Plan *p, int i)
{
    Slot *s = &p->slots[i];
    int ret = rx_compute_wait(p->c, s->value);
    s->value = 0;
    if (ret < 0)
        return ret;
    if (p->times) {
        uint64_t ts[2];
        if (vkGetQueryPoolResults(p->c->dev, p->times, 2 * i, 2, sizeof(ts), ts, sizeof(ts[0]),
                                  VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
            p->gpu_ms += (double)((ts[1] - ts[0]) & p->c->ts_mask) *
                         p->c->props.limits.timestampPeriod * 1e-6;
    }
    return rx_cbuf_invalidate(p->c, &s->out);
}

/* After an error: nothing may still be using the slots. */
static void slots_drain(Plan *p)
{
    for (int i = 0; i < 2; i++) {
        if (p->slots[i].value)
            slot_wait(p, i);
    }
}

/* x[a .. a + len) with zeros outside [0, n). */
static void copy_padded(float *dst, const float *x, int64_t n, int64_t a, int64_t len)
{
    int64_t lo = RX_CLAMP(a, 0, n), hi = RX_CLAMP(a + len, 0, n);
    if (hi <= lo) {
        memset(dst, 0, (size_t)len * sizeof(*dst));
        return;
    }
    if (lo > a)
        memset(dst, 0, (size_t)(lo - a) * sizeof(*dst));
    memcpy(dst + (lo - a), x + lo, (size_t)(hi - lo) * sizeof(*dst));
    if (hi < a + len)
        memset(dst + (hi - a), 0, (size_t)(a + len - hi) * sizeof(*dst));
}

/* ----------------------------------------------------------------- STFT */

int rx_compute_stft_plan(RXCompute *c, const RXComputeSTFT *sp, RXComputeSTFTPlan **out)
{
    *out = NULL;
    if (!sp->window || !sp->k0 || !sp->k1 || !sp->weights || sp->hop <= 0 ||
        sp->nb_bands <= 0 || sp->nb_bands > 256)
        return RX_ERROR(EINVAL);

    RXComputeSTFTPlan *plan = calloc(1, sizeof(*plan));
    if (!plan)
        return RX_ERROR(ENOMEM);
    int ret = plan_init(&plan->p, c, RX_KERNEL_STFT, sp->nfft);
    if (ret < 0)
        goto fail;
    plan->hop = sp->hop;
    plan->nb_bands = sp->nb_bands;
    plan->scale = sp->scale;

    /* The tables, one after the other: twiddles, window, bands, weights. */
    const int n = sp->nfft, nbins = n / 2 + 1;
    int nb_w = 0;
    for (int b = 0; b < sp->nb_bands; b++) {
        if (sp->k0[b] < 0 || sp->k1[b] < sp->k0[b] || sp->k1[b] > nbins) {
            ret = RX_ERROR(EINVAL);
            goto fail;
        }
        nb_w += sp->k1[b] - sp->k0[b];
    }
    size_t o_win = (size_t)n * 2 * sizeof(float);
    size_t o_bands = o_win + (size_t)n * sizeof(float);
    size_t o_w = o_bands + (size_t)sp->nb_bands * 3 * sizeof(int32_t);
    size_t size = o_w + (size_t)RX_MAX(nb_w, 1) * sizeof(float);
    uint8_t *t = calloc(1, size);
    if (!t) {
        ret = RX_ERROR(ENOMEM);
        goto fail;
    }
    twiddles((float *)t, n);
    memcpy(t + o_win, sp->window, (size_t)n * sizeof(float));
    int32_t *bands = (int32_t *)(t + o_bands);
    for (int b = 0, w = 0; b < sp->nb_bands; b++) {
        bands[3 * b] = sp->k0[b];
        bands[3 * b + 1] = sp->k1[b];
        bands[3 * b + 2] = w;
        w += sp->k1[b] - sp->k0[b];
    }
    memcpy(t + o_w, sp->weights, (size_t)nb_w * sizeof(float));
    ret = upload_tables(&plan->p, t, size);
    free(t);
    if (ret < 0)
        goto fail;
    plan->tw = plan->p.tables.addr;
    plan->window = plan->tw + o_win;
    plan->bands = plan->tw + o_bands;
    plan->weights = plan->tw + o_w;

    *out = plan;
    return 0;

fail:
    rx_compute_stft_free(&plan);
    return ret;
}

void rx_compute_stft_free(RXComputeSTFTPlan **pp)
{
    if (!*pp)
        return;
    plan_uninit(&(*pp)->p);
    free(*pp);
    *pp = NULL;
}

int rx_compute_stft_run(RXComputeSTFTPlan *plan, const float *x, int64_t n, int64_t nb_frames,
                        RXComputeFramesFn fn, void *opaque)
{
    Plan *p = &plan->p;
    const int64_t hop = plan->hop, nfft = p->nfft;
    const int chunk = (int)RX_MIN((int64_t)STFT_CHUNK, RX_MAX(nb_frames, 1));
    const size_t in_max = ((size_t)(chunk - 1) * hop + nfft) * sizeof(float);
    const size_t out_max = (size_t)chunk * plan->nb_bands * sizeof(float);
    int64_t t0 = rx_time_us();

    int ret = 0;
    int64_t next = 0;
    int head = 0, inflight = 0;
    while (next < nb_frames || inflight) {
        if (next < nb_frames && inflight < 2) {
            int i = (head + inflight) % 2;
            Slot *s = &p->slots[i];
            if ((ret = slot_reserve(p, s, in_max, out_max)) < 0)
                break;
            s->first = next;
            s->nb = (int)RX_MIN((int64_t)chunk, nb_frames - next);
            int64_t len = (int64_t)(s->nb - 1) * hop + nfft;
            copy_padded(s->in.map, x, n, next * hop - nfft / 2, len);

            VkDeviceAddress src = slot_begin(p, i, (size_t)len * sizeof(float));
            STFTPush push = {
                .tw        = plan->tw,
                .window    = plan->window,
                .bands     = plan->bands,
                .weights   = plan->weights,
                .samples   = src,
                .dst       = s->out.addr,
                .hop       = (uint32_t)hop,
                .nb_bands  = (uint32_t)plan->nb_bands,
                .nb_frames = (uint32_t)s->nb,
                .scale     = plan->scale,
            };
            vkCmdPushConstants(s->cmd, p->c->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                               sizeof(push), &push);
            vkCmdDispatch(s->cmd, (uint32_t)s->nb, 1, 1);
            if ((ret = slot_submit(p, i, (size_t)len * sizeof(float))) < 0)
                break;
            inflight++;
            next += s->nb;
            continue;
        }

        Slot *s = &p->slots[head];
        if ((ret = slot_wait(p, head)) < 0)
            break;
        if ((ret = fn(opaque, s->first, s->nb, s->out.map)) < 0)
            break;
        head = (head + 1) % 2;
        inflight--;
    }
    if (ret < 0)
        slots_drain(p);
    p->wall_ms += (rx_time_us() - t0) / 1000.0;
    return ret;
}

void rx_compute_stft_times(const RXComputeSTFTPlan *plan, double *gpu_ms, double *wall_ms)
{
    *gpu_ms = plan->p.gpu_ms;
    *wall_ms = plan->p.wall_ms;
}

/* ------------------------------------------------------------ envelopes */

int rx_compute_env_plan(RXCompute *c, const RXComputeEnv *ep, RXComputeEnvPlan **out)
{
    *out = NULL;
    if (!ep->taper || !ep->gains || !ep->smooth || ep->nb_bands <= 0 || ep->nb_bands > 8 ||
        ep->step <= 0 || ep->half < 0)
        return RX_ERROR(EINVAL);
    for (int b = 0; b < ep->nb_bands; b++) {
        int64_t reach = (int64_t)ep->half * ep->step + ep->smooth[b];
        if (ep->smooth[b] < 0 || reach >= ep->nfft / 2)
            return RX_ERROR(EINVAL);
    }

    RXComputeEnvPlan *plan = calloc(1, sizeof(*plan));
    if (!plan)
        return RX_ERROR(ENOMEM);
    int ret = plan_init(&plan->p, c, RX_KERNEL_ENV, ep->nfft);
    if (ret < 0)
        goto fail;
    plan->nb_bands = ep->nb_bands;
    plan->step = ep->step;
    plan->half = ep->half;

    /* Twiddles, taper, gains, smoothing widths. */
    const int n = ep->nfft, nbins = n / 2 + 1;
    size_t o_taper = (size_t)n * 2 * sizeof(float);
    size_t o_gains = o_taper + (size_t)n * sizeof(float);
    size_t o_smooth = o_gains + (size_t)ep->nb_bands * nbins * sizeof(float);
    size_t size = o_smooth + (size_t)ep->nb_bands * sizeof(int32_t);
    uint8_t *t = calloc(1, size);
    if (!t) {
        ret = RX_ERROR(ENOMEM);
        goto fail;
    }
    twiddles((float *)t, n);
    memcpy(t + o_taper, ep->taper, (size_t)n * sizeof(float));
    memcpy(t + o_gains, ep->gains, (size_t)ep->nb_bands * nbins * sizeof(float));
    int32_t *smooth = (int32_t *)(t + o_smooth);
    for (int b = 0; b < ep->nb_bands; b++)
        smooth[b] = ep->smooth[b];
    ret = upload_tables(&plan->p, t, size);
    free(t);
    if (ret < 0)
        goto fail;
    plan->tw = plan->p.tables.addr;
    plan->taper = plan->tw + o_taper;
    plan->gains = plan->tw + o_gains;
    plan->smooth = plan->tw + o_smooth;

    *out = plan;
    return 0;

fail:
    rx_compute_env_free(&plan);
    return ret;
}

void rx_compute_env_free(RXComputeEnvPlan **pp)
{
    if (!*pp)
        return;
    plan_uninit(&(*pp)->p);
    free(*pp);
    *pp = NULL;
}

int rx_compute_env_run(RXComputeEnvPlan *plan, const float *x, int64_t n, const int64_t *start,
                       int nb, float *out)
{
    Plan *p = &plan->p;
    const int nfft = p->nfft;
    const int len = 2 * plan->half + 1;
    const size_t per_win = (size_t)plan->nb_bands * len;
    const int batch = RX_MIN(ENV_BATCH, RX_MAX(nb, 1));
    const size_t in_max = (size_t)batch * nfft * sizeof(float);
    const size_t out_max = (size_t)batch * per_win * sizeof(float);
    int64_t t0 = rx_time_us();

    int ret = 0, next = 0;
    int head = 0, inflight = 0;
    while (next < nb || inflight) {
        if (next < nb && inflight < 2) {
            int i = (head + inflight) % 2;
            Slot *s = &p->slots[i];
            if ((ret = slot_reserve(p, s, in_max, out_max)) < 0)
                break;
            s->first = next;
            s->nb = RX_MIN(batch, nb - next);
            float *dst = s->in.map;
            for (int w = 0; w < s->nb; w++)
                copy_padded(dst + (size_t)w * nfft, x, n, start[next + w], nfft);

            size_t bytes = (size_t)s->nb * nfft * sizeof(float);
            VkDeviceAddress src = slot_begin(p, i, bytes);
            EnvPush push = {
                .tw         = plan->tw,
                .taper      = plan->taper,
                .gains      = plan->gains,
                .widths     = plan->smooth,
                .windows    = src,
                .dst        = s->out.addr,
                .step       = (uint32_t)plan->step,
                .half_len   = (uint32_t)plan->half,
                .nb_bands   = (uint32_t)plan->nb_bands,
                .nb_windows = (uint32_t)s->nb,
            };
            vkCmdPushConstants(s->cmd, p->c->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                               sizeof(push), &push);
            vkCmdDispatch(s->cmd, (uint32_t)s->nb, (uint32_t)plan->nb_bands, 1);
            if ((ret = slot_submit(p, i, bytes)) < 0)
                break;
            inflight++;
            next += s->nb;
            continue;
        }

        Slot *s = &p->slots[head];
        if ((ret = slot_wait(p, head)) < 0)
            break;
        memcpy(out + (size_t)s->first * per_win, s->out.map,
               (size_t)s->nb * per_win * sizeof(float));
        head = (head + 1) % 2;
        inflight--;
    }
    if (ret < 0)
        slots_drain(p);
    p->wall_ms += (rx_time_us() - t0) / 1000.0;
    return ret;
}

void rx_compute_env_times(const RXComputeEnvPlan *plan, double *gpu_ms, double *wall_ms)
{
    *gpu_ms = plan->p.gpu_ms;
    *wall_ms = plan->p.wall_ms;
}
