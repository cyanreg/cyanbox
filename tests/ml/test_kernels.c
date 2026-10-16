/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* The model's kernels against plain C in double precision, on random
 * inputs of awkward sizes (rows and columns that do not fill a tile), and,
 * with -b, the speed of the matrix products at the model's sizes.
 * Each check prints the largest error relative to the reference's
 * largest magnitude and fails above its tolerance; with
 * CYANBOX_GPU_VALIDATION=1, any error the validation layer reports fails it
 * too. Skipped (77) without a device that can run the model. */

#include <math.h>
#include <stdio.h>

#include "ml_priv.h"
#include "kernels.h"

static MLGpu gpu_main;
/* The device the helpers and the tests run on: gpu_main, but for
 * test_gemm_waves's second. */
static MLGpu *dev = &gpu_main;
static int failures;

/* ---------------------------------------------------------------- helpers */

static uint64_t rng = 0x9e3779b97f4a7c15ull;

static double frand(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return (double)(rng >> 11) / (double)(1ull << 53) * 2.0 - 1.0;
}

static uint16_t to_half(float f)
{
    _Float16 h = (_Float16)f;
    uint16_t u;
    memcpy(&u, &h, 2);
    return u;
}

static float from_half(uint16_t u)
{
    _Float16 h;
    memcpy(&h, &u, 2);
    return (float)h;
}

static MLBuf upload(const void *data, size_t size)
{
    MLBuf st, b;
    if (ml_buf_create(dev, &st, size, ML_MEM_UPLOAD) < 0 ||
        ml_buf_create(dev, &b, size, ML_MEM_DEVICE) < 0) {
        fprintf(stderr, "out of memory\n");
        exit(1);
    }
    memcpy(st.map, data, size);
    ml_buf_flush(dev, &st, size);
    ml_gpu_copy(dev, &st, 0, &b, 0, size);
    ml_gpu_barrier(dev);
    ml_gpu_run(dev);
    ml_buf_free(dev, &st);
    return b;
}

static MLBuf zeros(size_t size)
{
    MLBuf b;
    if (ml_buf_create(dev, &b, size, ML_MEM_DEVICE) < 0) {
        fprintf(stderr, "out of memory\n");
        exit(1);
    }
    ml_gpu_fill(dev, &b, 0, size, 0);
    ml_gpu_barrier(dev);
    return b;
}

static void download(const MLBuf *b, void *dst, size_t size)
{
    MLBuf rb;
    if (ml_buf_create(dev, &rb, size, ML_MEM_READBACK) < 0) {
        fprintf(stderr, "out of memory\n");
        exit(1);
    }
    ml_gpu_barrier(dev);
    ml_gpu_copy(dev, b, 0, &rb, 0, size);
    if (ml_gpu_run(dev) < 0) {
        printf("the GPU failed\n");
        failures++;
    }
    ml_buf_invalidate(dev, &rb);
    memcpy(dst, rb.map, size);
    ml_buf_free(dev, &rb);
}

static void check(const char *name, const float *got, const double *want, size_t n, double tol)
{
    double maxref = 1e-30, maxerr = 0.0;
    size_t worst = 0;
    for (size_t i = 0; i < n; i++)
        maxref = fmax(maxref, fabs(want[i]));
    for (size_t i = 0; i < n; i++) {
        double e = fabs(got[i] - want[i]);
        if (!(e <= maxerr)) {
            maxerr = isnan(e) ? INFINITY : e;
            worst = i;
        }
    }
    double rel = maxerr / maxref;
    bool ok = rel <= tol;
    printf("%-28s %s  max err %.2e of %.2e (at %zu: %g vs %g)\n", name, ok ? "ok  " : "FAIL",
           rel, maxref, worst, got[worst], want[worst]);
    failures += !ok;
}

/* ------------------------------------------------------------------- Q4_K */

/* Random Q4_K rows, with their dequantised values (ggml's
 * dequantize_row_q4_K). */
static void make_q4k(uint8_t *blocks, float *deq, int rows, int k)
{
    int nb = k / 256;
    for (int r = 0; r < rows; r++) {
        for (int b = 0; b < nb; b++) {
            uint8_t *blk = blocks + (size_t)(r * nb + b) * 144;
            for (int i = 4; i < 144; i++)
                blk[i] = (uint8_t)(rng = rng * 6364136223846793005ull + 1442695040888963407ull,
                                   rng >> 56);
            uint16_t d = to_half((float)(0.01 + 0.02 * fabs(frand())));
            uint16_t dmin = to_half((float)(0.01 * fabs(frand())));
            memcpy(blk, &d, 2);
            memcpy(blk + 2, &dmin, 2);

            const uint8_t *sc = blk + 4, *q = blk + 16;
            float dd = from_half(d), dm = from_half(dmin);
            float *y = deq + (size_t)r * k + b * 256;
            for (int j = 0; j < 4; j++) {
                for (int h = 0; h < 2; h++) {
                    int s = 2 * j + h;
                    uint8_t scv, mv;
                    if (s < 4) {
                        scv = sc[s] & 63;
                        mv = sc[s + 4] & 63;
                    } else {
                        scv = (sc[s + 4] & 0xf) | ((sc[s - 4] >> 6) << 4);
                        mv = (sc[s + 4] >> 4) | ((sc[s] >> 6) << 4);
                    }
                    for (int l = 0; l < 32; l++) {
                        int v = h ? q[32 * j + l] >> 4 : q[32 * j + l] & 15;
                        y[64 * j + 32 * h + l] = dd * scv * v - dm * mv;
                    }
                }
            }
        }
    }
}

/* ------------------------------------------------------------------- GEMM */

static double gelu_ref(double x)
{
    return 0.5 * x * (1.0 + erf(x / sqrt(2.0)));
}

static void test_gemm(bool q4k, int M, int N, int K, uint32_t flags, bool bench)
{
    size_t nx = (size_t)M * K, nw = (size_t)N * K, ny = (size_t)M * N;
    float *x = malloc(nx * 4), *w = malloc(nw * 4), *bias = malloc(N * 4), *y0 = malloc(ny * 4);
    float *got = malloc(ny * 4);
    double *want = malloc(ny * 8);
    uint16_t *wh = malloc(nw * 2);
    uint8_t *wq = malloc(nw / 256 * 144 + 16);
    for (size_t i = 0; i < nx; i++)
        x[i] = (float)frand();
    for (int i = 0; i < N; i++)
        bias[i] = (float)frand();
    for (size_t i = 0; i < ny; i++)
        y0[i] = (float)frand();
    if (q4k) {
        make_q4k(wq, w, N, K);
    } else {
        for (size_t i = 0; i < nw; i++) {
            wh[i] = to_half((float)(frand() * 0.1));
            w[i] = from_half(wh[i]);
        }
    }

    if (!bench) {
        for (int m = 0; m < M; m++) {
            for (int n = 0; n < N; n++) {
                double s = 0.0;
                for (int k = 0; k < K; k++)
                    s += (double)x[(size_t)m * K + k] * w[(size_t)n * K + k];
                if (flags & FLAG_BIAS)
                    s += bias[n];
                if (flags & FLAG_GELU)
                    s = gelu_ref(s);
                if (flags & FLAG_ADD)
                    s += y0[(size_t)m * N + n];
                want[(size_t)m * N + n] = s;
            }
        }
    }

    MLBuf bx = upload(x, nx * 4), bb = upload(bias, N * 4), by = upload(y0, ny * 4);
    MLBuf bw = q4k ? upload(wq, nw / 256 * 144) : upload(wh, nw * 2);
    GemmPush p = {
        .x = bx.addr, .w = bw.addr, .bias = bb.addr, .y = by.addr,
        .M = (uint32_t)M, .N = (uint32_t)N, .K = (uint32_t)K,
        .ldx = (uint32_t)K, .ldy = (uint32_t)N,
        .flags = flags,
    };
    bool gemv = q4k && M <= 8;
    bool small = q4k && !gemv && M <= 64;
    enum MLKernel k = gemv ? ML_K_GEMV_Q4K : small ? ML_K_GEMM_Q4K_SMALL :
                      q4k ? ML_K_GEMM_Q4K : ML_K_GEMM_F16;
    int reps = bench ? 20 : 1;
    int64_t t0 = cb_time_us();
    for (int r = 0; r < reps; r++) {
        if (gemv)
            ml_gpu_dispatch(dev, k, &p, sizeof(p), (N + 3) / 4, 1, 1);
        else if (small)
            ml_gpu_dispatch(dev, k, &p, sizeof(p), (N + 127) / 128, (M + 31) / 32, 1);
        else
            ml_gpu_dispatch(dev, k, &p, sizeof(p), (N + 127) / 128, (M + 127) / 128, 1);
        ml_gpu_barrier(dev);
    }
    ml_gpu_run(dev);
    double ms = (cb_time_us() - t0) * 1e-3 / reps;

    char name[64];
    snprintf(name, sizeof(name), "%s %dx%dx%d%s%s%s",
             gemv ? "gemv_q4k" : small ? "gemm_q4k_small" : q4k ? "gemm_q4k" : "gemm_f16",
             M, N, K, flags & FLAG_BIAS ? " b" : "", flags & FLAG_GELU ? " g" : "",
             flags & FLAG_ADD ? " a" : "");
    if (bench) {
        double bytes = q4k ? nw / 256.0 * 144 : nw * 2.0;
        printf("%-28s %8.3f ms  %6.2f TFLOPS  %6.1f GB/s of weights\n", name, ms,
               2.0 * M * N * K / (ms * 1e9), bytes / (ms * 1e6));
    } else {
        download(&by, got, ny * 4);
        check(name, got, want, ny, 2e-5 * sqrt((double)K));
    }

    ml_buf_free(dev, &bx);
    ml_buf_free(dev, &bb);
    ml_buf_free(dev, &by);
    ml_buf_free(dev, &bw);
    free(x); free(w); free(bias); free(y0); free(got); free(want); free(wh); free(wq);
}

/* Bitwise equality of two runs' outputs: questions asked together must
 * come out as asked alone (moss.c, Segment). */
static void check_same(const char *name, const void *a, const void *b, size_t size)
{
    bool ok = !memcmp(a, b, size);
    printf("%-28s %s  %s\n", name, ok ? "ok  " : "FAIL", ok ? "bit for bit" : "differs");
    failures += !ok;
}

/* A row of the Q4_K product is the same whatever rows come with it: the
 * GEMM's tiles of 128 and of 32 rows sum each output in the same order,
 * and so does the GEMV for any of its one to eight rows (not the GEMM's
 * order: a pass mixes no rows of the two). */
static void test_gemm_rows(void)
{
    const int M = 150, N = 260, K = 512;
    float *x = malloc((size_t)M * K * 4), *w = malloc((size_t)N * K * 4);
    uint8_t *wq = malloc((size_t)N * K / 256 * 144);
    float *all = malloc((size_t)M * N * 4), *part = malloc((size_t)M * N * 4);
    for (size_t i = 0; i < (size_t)M * K; i++)
        x[i] = (float)frand();
    make_q4k(wq, w, N, K);
    MLBuf bx = upload(x, (size_t)M * K * 4), bw = upload(wq, (size_t)N * K / 256 * 144);
    MLBuf by = zeros((size_t)M * N * 4), bp = zeros((size_t)M * N * 4);

    /* kernel, first row, rows */
    static const struct { enum MLKernel k; int m0, m; } runs[] = {
        { ML_K_GEMM_Q4K, 0, M }, { ML_K_GEMM_Q4K_SMALL, 100, 37 }, { ML_K_GEMM_Q4K, 3, 70 },
        { ML_K_GEMV_Q4K, 5, 8 }, { ML_K_GEMV_Q4K, 7, 1 }, { ML_K_GEMV_Q4K, 9, 3 },
    };
    for (size_t r = 0; r < CB_ARRAY_ELEMS(runs); r++) {
        GemmPush p = {
            .x = bx.addr + (VkDeviceAddress)runs[r].m0 * K * 4, .w = bw.addr,
            .y = (r ? bp.addr : by.addr) + (VkDeviceAddress)runs[r].m0 * N * 4,
            .M = (uint32_t)runs[r].m, .N = N, .K = K, .ldx = K, .ldy = N,
        };
        enum MLKernel k = runs[r].k;
        uint32_t gx = k == ML_K_GEMV_Q4K ? (N + 3) / 4 : (N + 127) / 128;
        uint32_t gy = k == ML_K_GEMV_Q4K ? 1 : k == ML_K_GEMM_Q4K_SMALL ? (p.M + 31) / 32 :
                      (p.M + 127) / 128;
        ml_gpu_dispatch(dev, k, &p, sizeof(p), gx, gy, 1);
        ml_gpu_barrier(dev);
        download(r ? &bp : &by, r ? part : all, (size_t)M * N * 4);
        if (r == 3) {
            /* The GEMV's reference: its own rows, alone. */
            memcpy(all + (size_t)5 * N, part + (size_t)5 * N, (size_t)8 * N * 4);
            continue;
        }
        char name[64];
        snprintf(name, sizeof(name), "%s rows %d-%d", k == ML_K_GEMV_Q4K ? "gemv_q4k" :
                 k == ML_K_GEMM_Q4K_SMALL ? "gemm_q4k_small" : "gemm_q4k", runs[r].m0,
                 runs[r].m0 + runs[r].m - 1);
        if (r)
            check_same(name, part + (size_t)runs[r].m0 * N, all + (size_t)runs[r].m0 * N,
                       (size_t)runs[r].m * N * 4);
    }
    ml_buf_free(dev, &bx); ml_buf_free(dev, &bw); ml_buf_free(dev, &by); ml_buf_free(dev, &bp);
    free(x); free(w); free(wq); free(all); free(part);
}

/* One product on the current device, into out[M][N]. */
static void gemm_on_dev(enum MLKernel k, const void *x, const void *w, size_t wsize,
                        const float *bias, int M, int N, int K, uint32_t flags, float *out)
{
    MLBuf bx = upload(x, (size_t)M * K * 4), bw = upload(w, wsize), bb = upload(bias, N * 4);
    MLBuf by = upload(out, (size_t)M * N * 4);
    GemmPush p = {
        .x = bx.addr, .w = bw.addr, .bias = bb.addr, .y = by.addr,
        .M = (uint32_t)M, .N = (uint32_t)N, .K = (uint32_t)K, .ldx = (uint32_t)K,
        .ldy = (uint32_t)N, .flags = flags,
    };
    uint32_t rows = k == ML_K_GEMM_Q4K_SMALL ? 32 : 128;
    ml_gpu_dispatch(dev, k, &p, sizeof(p), (N + 127) / 128, (M + rows - 1) / rows, 1);
    ml_gpu_barrier(dev);
    download(&by, out, (size_t)M * N * 4);
    ml_buf_free(dev, &bx); ml_buf_free(dev, &bw); ml_buf_free(dev, &bb); ml_buf_free(dev, &by);
}

/* The products in subgroups of 32 (gpu.c) and in the driver's size (a
 * second device, with CYANBOX_ML_WAVE32=0): the same, bit for bit, as
 * their invocations share no sums. */
static void test_gemm_waves(void)
{
    if (!gpu_main.gemm_wave32) {
        printf("%-28s skipped: the device picks its products' subgroups\n", "gemm waves");
        return;
    }
    MLGpu other;
    setenv("CYANBOX_ML_WAVE32", "0", 1);
    int ret = ml_gpu_create(&other, NULL, false);
    unsetenv("CYANBOX_ML_WAVE32");
    if (ret < 0 || other.gemm_wave32) {
        printf("%-28s FAIL  no second device in the driver's subgroups\n", "gemm waves");
        failures++;
        if (ret >= 0)
            ml_gpu_destroy(&other);
        return;
    }

    const int M = 150, N = 260, K = 512;
    float *x = malloc((size_t)M * K * 4), *w = malloc((size_t)N * K * 4), *bias = malloc(N * 4);
    float *y0 = malloc((size_t)M * N * 4), *a = malloc((size_t)M * N * 4);
    float *b = malloc((size_t)M * N * 4);
    uint8_t *wq = malloc((size_t)N * K / 256 * 144);
    uint16_t *wh = malloc((size_t)N * K * 2);
    for (size_t i = 0; i < (size_t)M * K; i++)
        x[i] = (float)frand();
    for (int i = 0; i < N; i++)
        bias[i] = (float)frand();
    for (size_t i = 0; i < (size_t)M * N; i++)
        y0[i] = (float)frand();
    for (size_t i = 0; i < (size_t)N * K; i++)
        wh[i] = to_half((float)(frand() * 0.1));
    make_q4k(wq, w, N, K);

    static const struct { enum MLKernel k; const char *name; int m; uint32_t flags; } runs[] = {
        { ML_K_GEMM_Q4K, "gemm_q4k waves", 150, FLAG_ADD },
        { ML_K_GEMM_Q4K_SMALL, "gemm_q4k_small waves", 37, 0 },
        { ML_K_GEMM_F16, "gemm_f16 waves b g a", 130, FLAG_BIAS | FLAG_GELU | FLAG_ADD },
    };
    for (size_t r = 0; r < CB_ARRAY_ELEMS(runs); r++) {
        bool q4k = runs[r].k != ML_K_GEMM_F16;
        const void *wd = q4k ? (const void *)wq : (const void *)wh;
        size_t wsize = q4k ? (size_t)N * K / 256 * 144 : (size_t)N * K * 2;
        memcpy(a, y0, (size_t)M * N * 4);
        memcpy(b, y0, (size_t)M * N * 4);
        gemm_on_dev(runs[r].k, x, wd, wsize, bias, runs[r].m, N, K, runs[r].flags, a);
        dev = &other;
        gemm_on_dev(runs[r].k, x, wd, wsize, bias, runs[r].m, N, K, runs[r].flags, b);
        dev = &gpu_main;
        check_same(runs[r].name, a, b, (size_t)runs[r].m * N * 4);
    }
    ml_gpu_destroy(&other);
    free(x); free(w); free(bias); free(y0); free(a); free(b); free(wq); free(wh);
}

/* A 3x3, stride 2, padding 1 convolution through the GEMM's gather. */
static void test_conv(void)
{
    const int B = 2, FIN = 9, TIN = 13, CIN = 64, COUT = 40;
    const int FOUT = (FIN - 1) / 2 + 1, TOUT = (TIN - 1) / 2 + 1;
    size_t nin = (size_t)B * FIN * TIN * CIN, nw = (size_t)COUT * 9 * CIN;
    size_t nout = (size_t)B * FOUT * TOUT * COUT;
    float *in = malloc(nin * 4), *bias = malloc(COUT * 4), *got = malloc(nout * 4);
    uint16_t *w = malloc(nw * 2);
    double *want = malloc(nout * 8);
    for (size_t i = 0; i < nin; i++)
        in[i] = (float)frand();
    for (size_t i = 0; i < nw; i++)
        w[i] = to_half((float)(frand() * 0.1));      /* [o][kh][kw][ci] */
    for (int i = 0; i < COUT; i++)
        bias[i] = (float)frand();

    for (int b = 0; b < B; b++)
    for (int fo = 0; fo < FOUT; fo++)
    for (int to = 0; to < TOUT; to++)
    for (int o = 0; o < COUT; o++) {
        double s = bias[o];
        for (int kh = 0; kh < 3; kh++)
        for (int kw = 0; kw < 3; kw++) {
            int fi = 2 * fo + kh - 1, ti = 2 * to + kw - 1;
            if (fi < 0 || fi >= FIN || ti < 0 || ti >= TIN)
                continue;
            for (int ci = 0; ci < CIN; ci++)
                s += (double)in[((b * FIN + fi) * TIN + ti) * CIN + ci] *
                     from_half(w[((o * 3 + kh) * 3 + kw) * CIN + ci]);
        }
        want[((b * FOUT + fo) * TOUT + to) * COUT + o] = gelu_ref(s);
    }

    MLBuf bi = upload(in, nin * 4), bw = upload(w, nw * 2), bb = upload(bias, COUT * 4);
    MLBuf bo = zeros(nout * 4);
    GemmPush p = {
        .x = bi.addr, .w = bw.addr, .bias = bb.addr, .y = bo.addr,
        .M = (uint32_t)(B * FOUT * TOUT), .N = COUT, .K = 9 * CIN,
        .ldy = COUT,
        .flags = FLAG_BIAS | FLAG_GELU,
        .fin = FIN, .tin = TIN, .fout = FOUT, .tout = TOUT, .cin = CIN,
    };
    ml_gpu_dispatch(dev, ML_K_GEMM_F16_CONV, &p, sizeof(p), (COUT + 127) / 128,
                    (p.M + 127) / 128, 1);
    download(&bo, got, nout * 4);
    check("gemm_f16_conv", got, want, nout, 1e-4);

    ml_buf_free(dev, &bi); ml_buf_free(dev, &bw); ml_buf_free(dev, &bb);
    ml_buf_free(dev, &bo);
    free(in); free(bias); free(got); free(w); free(want);
}

/* -------------------------------------------------------------- attention */

/* The attention kernel's dispatches for the test: q, o, the cache and
 * parts in buffers; the keys from brk on shift slots further. */
static void run_attn(const MLBuf *bq, const MLBuf *bo, const MLBuf *bk, const MLBuf *bv, int nq,
                     int pos0, bool dec, bool split, uint32_t brk, uint32_t shift)
{
    const int H = 32, KV = 8, D = 128, CTX = 4096;
    int nk = pos0 + nq;
    double scale = 1.0 / sqrt(D);
    MLBuf bp;
    if (dec) {
        uint32_t nsplit = (nk + ATTN_SPLIT - 1) / ATTN_SPLIT;
        bp = zeros((size_t)KV * nsplit * ATTN_DEC_ROWS * ATTN_PART * 4);
        AttnDecPush p = {
            .q = bq->addr, .o = bo->addr, .kc = bk->addr, .vc = bv->addr, .part = bp.addr,
            .nq = (uint32_t)nq, .pos0 = (uint32_t)pos0, .max_ctx = CTX,
            .nb_q = H, .nb_kv = KV, .nsplit = nsplit,
            .scale = (float)scale, .brk = brk, .shift = shift,
        };
        ml_gpu_dispatch(dev, ML_K_ATTN_DEC, &p, sizeof(p), nsplit, KV, 1);
        ml_gpu_barrier(dev);
        ml_gpu_dispatch(dev, ML_K_ATTN_COMB, &p, sizeof(p), nq * (H / KV), KV, 1);
    } else {
        /* Split the keys whenever there are 128 or more. */
        uint32_t fsplit = CB_MAX(1, nk / 128), len = CB_ALIGN((nk + fsplit - 1) / fsplit, 32);
        fsplit = (nk + len - 1) / len;
        bp = zeros((size_t)fsplit * nq * H * ATTN_PART * 4);
        AttnPush p = {
            .q = bq->addr, .o = bo->addr, .kc = bk->addr, .vc = bv->addr, .part = bp.addr,
            .nq = (uint32_t)nq, .pos0 = (uint32_t)pos0, .max_ctx = CTX,
            .nb_q = H, .nb_kv = KV, .nsplit = split ? fsplit : 1, .split_len = split ? len : 4096,
            .scale = (float)scale, .brk = brk, .shift = shift,
        };
        ml_gpu_dispatch(dev, ML_K_ATTN, &p, sizeof(p), (nq + 63) / 64, H, p.nsplit);
        if (p.nsplit > 1) {
            ml_gpu_barrier(dev);
            ml_gpu_dispatch(dev, ML_K_ATTN_FCOMB, &p, sizeof(p), nq, H, 1);
        }
    }
    ml_gpu_barrier(dev);
    ml_gpu_run(dev);
    ml_buf_free(dev, &bp);
}

/* Causal attention of nq queries at [pos0, pos0 + nq) over a cache that
 * holds positions [0, pos0 + nq), GQA 32/8, 128 dimensions; and the same
 * with the keys from brk on moved `shift` slots further, junk in the slots
 * between, which must give the same, bit for bit (a question asked with
 * others keeps its keys past theirs, moss.c). */
static void test_attn(int nq, int pos0, bool dec, bool split, int brk, int shift)
{
    const int H = 32, KV = 8, D = 128, CTX = 4096;
    int nk = pos0 + nq;
    size_t nqv = (size_t)nq * H * D, nc = (size_t)KV * CTX * D;
    float *q = malloc(nqv * 4), *got = malloc(nqv * 4);
    uint16_t *kc = calloc(nc, 2), *vc = calloc(nc, 2);
    double *want = malloc(nqv * 8), *s = malloc(nk * 8);
    for (size_t i = 0; i < nqv; i++)
        q[i] = (float)(frand() * 2.0);
    for (int h = 0; h < KV; h++) {
        for (int p = 0; p < nk; p++) {
            for (int d = 0; d < D; d++) {
                kc[((size_t)h * CTX + p) * D + d] = to_half((float)(frand() * 2.0));
                vc[((size_t)h * CTX + p) * D + d] = to_half((float)frand());
            }
        }
    }
    double scale = 1.0 / sqrt(D);
    for (int i = 0; i < nq; i++) {
        for (int h = 0; h < H; h++) {
            int kh = h / (H / KV);
            int last = pos0 + i;
            double mx = -INFINITY;
            for (int j = 0; j <= last; j++) {
                double a = 0.0;
                for (int d = 0; d < D; d++) {
                    /* The flash kernel holds the scaled query as f16. */
                    float qs = (float)(q[((size_t)i * H + h) * D + d] * scale);
                    double qv = dec ? qs : from_half(to_half(qs));
                    a += qv * from_half(kc[((size_t)kh * CTX + j) * D + d]);
                }
                s[j] = a;
                mx = fmax(mx, a);
            }
            double z = 0.0;
            for (int j = 0; j <= last; j++)
                z += (s[j] = exp(s[j] - mx));
            for (int d = 0; d < D; d++) {
                double o = 0.0;
                for (int j = 0; j <= last; j++)
                    o += s[j] * from_half(vc[((size_t)kh * CTX + j) * D + d]);
                want[((size_t)i * H + h) * D + d] = o / z;
            }
        }
    }

    MLBuf bq = upload(q, nqv * 4), bk = upload(kc, nc * 2), bv = upload(vc, nc * 2);
    MLBuf bo = zeros(nqv * 4);
    run_attn(&bq, &bo, &bk, &bv, nq, pos0, dec, split, 0, 0);
    download(&bo, got, nqv * 4);
    char name[64];
    snprintf(name, sizeof(name), "%s nq %d pos0 %d", dec ? "attn_dec" : split ? "attn split" : "attn",
             nq, pos0);
    check(name, got, want, nqv, 2e-3);

    if (shift) {
        /* The keys from brk on, shift slots further; junk where they were. */
        uint16_t *kc2 = malloc(nc * 2), *vc2 = malloc(nc * 2);
        for (size_t i = 0; i < nc; i++) {
            kc2[i] = to_half((float)(frand() * 50.0));
            vc2[i] = to_half((float)(frand() * 50.0));
        }
        for (int h = 0; h < KV; h++) {
            for (int p = 0; p < nk; p++) {
                size_t from = ((size_t)h * CTX + p) * D;
                size_t to = ((size_t)h * CTX + p + (p >= brk ? shift : 0)) * D;
                memcpy(kc2 + to, kc + from, D * 2);
                memcpy(vc2 + to, vc + from, D * 2);
            }
        }
        MLBuf bk2 = upload(kc2, nc * 2), bv2 = upload(vc2, nc * 2), bo2 = zeros(nqv * 4);
        float *got2 = malloc(nqv * 4);
        run_attn(&bq, &bo2, &bk2, &bv2, nq, pos0, dec, split, (uint32_t)brk, (uint32_t)shift);
        download(&bo2, got2, nqv * 4);
        snprintf(name, sizeof(name), "%s keys from %d +%d", dec ? "attn_dec" : "attn", brk, shift);
        check_same(name, got2, got, nqv * 4);
        ml_buf_free(dev, &bk2); ml_buf_free(dev, &bv2); ml_buf_free(dev, &bo2);
        free(kc2); free(vc2); free(got2);
    }

    ml_buf_free(dev, &bq); ml_buf_free(dev, &bk); ml_buf_free(dev, &bv);
    ml_buf_free(dev, &bo);
    free(q); free(got); free(kc); free(vc); free(want); free(s);
}

/* GPU time of one layer's attention: nq queries after pos0 keys. */
static void bench_attn(int nq, int pos0)
{
    const int H = 32, KV = 8, D = 128, CTX = 8192;
    size_t nqv = (size_t)nq * H * D, nc = (size_t)KV * CTX * D;
    MLBuf bq = zeros(nqv * 4), bo = zeros(nqv * 4), bk = zeros(nc * 2), bv = zeros(nc * 2);
    MLBuf bp = zeros((size_t)ATTN_MAX_PARTS * H * ATTN_PART * 4);
    int nk = pos0 + nq;
    int reps = 20;
    int64_t t0 = cb_time_us();
    for (int r = 0; r < reps; r++) {
        if (nq * 4 <= ATTN_DEC_ROWS) {
            uint32_t nsplit = (nk + ATTN_SPLIT - 1) / ATTN_SPLIT;
            AttnDecPush p = {
                .q = bq.addr, .o = bo.addr, .kc = bk.addr, .vc = bv.addr, .part = bp.addr,
                .nq = (uint32_t)nq, .pos0 = (uint32_t)pos0, .max_ctx = CTX,
                .nb_q = H, .nb_kv = KV, .nsplit = nsplit, .scale = 0.1f,
            };
            ml_gpu_dispatch(dev, ML_K_ATTN_DEC, &p, sizeof(p), nsplit, KV, 1);
            ml_gpu_barrier(dev);
            ml_gpu_dispatch(dev, ML_K_ATTN_COMB, &p, sizeof(p), nq * 4, KV, 1);
        } else {
            uint32_t qblocks = (nq + 63) / 64;
            uint32_t nsplit = CB_MAX(1, 256 / (qblocks * H));
            nsplit = CB_MIN(nsplit, CB_MAX(1, nk / 256));
            nsplit = CB_MAX(1, CB_MIN(nsplit, ATTN_MAX_PARTS / nq));
            uint32_t len = CB_ALIGN((nk + nsplit - 1) / nsplit, 32);
            nsplit = (nk + len - 1) / len;
            AttnPush p = {
                .q = bq.addr, .o = bo.addr, .kc = bk.addr, .vc = bv.addr, .part = bp.addr,
                .nq = (uint32_t)nq, .pos0 = (uint32_t)pos0, .max_ctx = CTX,
                .nb_q = H, .nb_kv = KV, .nsplit = nsplit, .split_len = len, .scale = 0.1f,
            };
            ml_gpu_dispatch(dev, ML_K_ATTN, &p, sizeof(p), qblocks, H, nsplit);
            if (nsplit > 1) {
                ml_gpu_barrier(dev);
                ml_gpu_dispatch(dev, ML_K_ATTN_FCOMB, &p, sizeof(p), nq, H, 1);
            }
        }
        ml_gpu_barrier(dev);
    }
    ml_gpu_run(dev);
    double ms = (cb_time_us() - t0) * 1e-3 / reps;
    double flops = 4.0 * H * D * ((double)nq * pos0 + (double)nq * nq / 2);
    printf("attn %4d queries after %5d   %8.3f ms  %6.2f TFLOPS\n", nq, pos0, ms,
           flops / (ms * 1e9));
    ml_buf_free(dev, &bq); ml_buf_free(dev, &bo); ml_buf_free(dev, &bk);
    ml_buf_free(dev, &bv); ml_buf_free(dev, &bp);
}

static void test_enc_attn(void)
{
    const int C = 3, T = 50, W = 1280, H = 20, D = 64, TAIL = 17;
    size_t n = (size_t)C * T * W;
    float *q = malloc(n * 4), *k = malloc(n * 4), *v = malloc(n * 4), *got = malloc(n * 4);
    double *want = calloc(n, 8), s[50];
    for (size_t i = 0; i < n; i++) {
        q[i] = (float)frand();
        k[i] = (float)frand();
        v[i] = (float)frand();
    }
    for (int c = 0; c < C; c++) {
        int len = c == C - 1 ? TAIL : T;
        for (int h = 0; h < H; h++) {
            for (int i = 0; i < len; i++) {
                double mx = -INFINITY;
                for (int j = 0; j < len; j++) {
                    double a = 0.0;
                    for (int d = 0; d < D; d++)
                        a += (double)q[(size_t)(c * T + i) * W + h * D + d] * 0.125 *
                             k[(size_t)(c * T + j) * W + h * D + d];
                    s[j] = a;
                    mx = fmax(mx, a);
                }
                double z = 0.0;
                for (int j = 0; j < len; j++)
                    z += (s[j] = exp(s[j] - mx));
                for (int d = 0; d < D; d++) {
                    double o = 0.0;
                    for (int j = 0; j < len; j++)
                        o += s[j] * v[(size_t)(c * T + j) * W + h * D + d];
                    want[(size_t)(c * T + i) * W + h * D + d] = o / z;
                }
            }
        }
    }
    MLBuf bq = upload(q, n * 4), bk = upload(k, n * 4), bv = upload(v, n * 4), bo = zeros(n * 4);
    EncAttnPush p = {
        .q = bq.addr, .k = bk.addr, .v = bv.addr, .o = bo.addr,
        .nb_chunks = C, .tail = TAIL,
        .scale = 0.125f,
    };
    ml_gpu_dispatch(dev, ML_K_ENC_ATTN, &p, sizeof(p), C, H, 1);
    download(&bo, got, n * 4);
    /* Rows past the tail are padding nobody reads. */
    for (size_t i = (size_t)((C - 1) * T + TAIL) * W; i < n; i++)
        got[i] = 0.0f;
    check("enc_attn", got, want, n, 1e-5);
    ml_buf_free(dev, &bq); ml_buf_free(dev, &bk); ml_buf_free(dev, &bv);
    ml_buf_free(dev, &bo);
    free(q); free(k); free(v); free(got); free(want);
}

/* ------------------------------------------------------------------ RoPE */

static void test_rope(void)
{
    const int NT = 5, H = 32, KV = 8, D = 128, CTX = 64, POS0 = 37;
    size_t nq = (size_t)NT * H * D, nk = (size_t)NT * KV * D, nc = (size_t)KV * CTX * D;
    float *q = malloc(nq * 4), *k = malloc(nk * 4), *v = malloc(nk * 4);
    float qw[128], kw[128], *rope = malloc((size_t)CTX * 64 * 2 * 4), *gq = malloc(nq * 4);
    uint16_t *gk = malloc(nc * 2);
    double *wq = malloc(nq * 8), *wk = calloc(nc, 8);
    float *gkf = calloc(nc, 4);
    for (size_t i = 0; i < nq; i++)
        q[i] = (float)(frand() * 3.0);
    for (size_t i = 0; i < nk; i++) {
        k[i] = (float)(frand() * 3.0);
        v[i] = (float)frand();
    }
    for (int d = 0; d < D; d++) {
        qw[d] = (float)(1.0 + frand());
        kw[d] = (float)(1.0 + frand());
    }
    for (int p = 0; p < CTX; p++) {
        for (int i = 0; i < 64; i++) {
            double a = p * pow(1e6, -2.0 * i / 128);
            rope[2 * (p * 64 + i)] = (float)cos(a);
            rope[2 * (p * 64 + i) + 1] = (float)sin(a);
        }
    }
    for (int t = 0; t < NT; t++) {
        int pos = POS0 + t;
        for (int h = 0; h < H + KV; h++) {
            const float *x = h < H ? q + ((size_t)t * H + h) * D : k + ((size_t)t * KV + h - H) * D;
            const float *w = h < H ? qw : kw;
            double ss = 0.0, y[128];
            for (int d = 0; d < D; d++)
                ss += (double)x[d] * x[d];
            double r = 1.0 / sqrt(ss / D + 1e-6);
            for (int d = 0; d < D; d++)
                y[d] = x[d] * r * w[d];
            for (int d = 0; d < D; d++) {
                double a = pos * pow(1e6, -2.0 * (d % 64) / 128), c = cos(a), s = sin(a);
                double o = d < 64 ? y[d] * c - y[d + 64] * s : y[d] * c + y[d - 64] * s;
                if (h < H)
                    wq[((size_t)t * H + h) * D + d] = o;
                else
                    wk[((size_t)(h - H) * CTX + pos) * D + d] = o;
            }
        }
    }
    MLBuf bq = upload(q, nq * 4), bk = upload(k, nk * 4), bv = upload(v, nk * 4);
    MLBuf bqw = upload(qw, 512), bkw = upload(kw, 512), br = upload(rope, (size_t)CTX * 512);
    MLBuf bkc = zeros(nc * 2), bvc = zeros(nc * 2);
    RopePush p = {
        .q = bq.addr, .k = bk.addr, .v = bv.addr, .qw = bqw.addr, .kw = bkw.addr,
        .rope = br.addr, .kc = bkc.addr, .vc = bvc.addr,
        .pos0 = POS0, .max_ctx = CTX, .nb_q = H, .nb_kv = KV,
        .eps = 1e-6f,
    };
    ml_gpu_dispatch(dev, ML_K_QK_ROPE, &p, sizeof(p), NT, 1, 1);
    download(&bq, gq, nq * 4);
    download(&bkc, gk, nc * 2);
    for (size_t i = 0; i < nc; i++)
        gkf[i] = from_half(gk[i]);
    check("qk_rope q", gq, wq, nq, 1e-5);
    check("qk_rope k (f16)", gkf, wk, nc, 1e-3);

    /* The same tokens at the same positions from a table, their keys and
     * values to other slots: the same, bit for bit, where they go. */
    int32_t *pos = malloc(2 * NT * sizeof(*pos));
    for (int t = 0; t < NT; t++) {
        pos[2 * t] = POS0 + t;
        pos[2 * t + 1] = 3 + 2 * t;
    }
    MLBuf bq2 = upload(q, nq * 4), bkc2 = zeros(nc * 2), bvc2 = zeros(nc * 2);
    MLBuf bpos = upload(pos, 2 * NT * sizeof(*pos));
    p.q = bq2.addr;
    p.kc = bkc2.addr;
    p.vc = bvc2.addr;
    p.pos = bpos.addr;
    p.pos0 = 0;
    ml_gpu_dispatch(dev, ML_K_QK_ROPE, &p, sizeof(p), NT, 1, 1);
    float *gq2 = malloc(nq * 4);
    uint16_t *gk2 = malloc(nc * 2), *gv = malloc(nc * 2), *gv2 = malloc(nc * 2);
    download(&bq2, gq2, nq * 4);
    download(&bkc2, gk2, nc * 2);
    download(&bvc, gv, nc * 2);
    download(&bvc2, gv2, nc * 2);
    check_same("qk_rope q, positions table", gq2, gq, nq * 4);
    bool same = true;
    for (int h = 0; h < KV; h++) {
        for (int t = 0; t < NT; t++) {
            size_t a = ((size_t)h * CTX + POS0 + t) * D, b = ((size_t)h * CTX + 3 + 2 * t) * D;
            same &= !memcmp(gk + a, gk2 + b, D * 2) && !memcmp(gv + a, gv2 + b, D * 2);
        }
    }
    check_same("qk_rope k, v to the slots", &same, &(bool) { true }, sizeof(same));
    ml_buf_free(dev, &bq2); ml_buf_free(dev, &bkc2); ml_buf_free(dev, &bvc2);
    ml_buf_free(dev, &bpos);
    free(gq2); free(gk2); free(gv); free(gv2); free(pos);
    ml_buf_free(dev, &bq); ml_buf_free(dev, &bk); ml_buf_free(dev, &bv);
    ml_buf_free(dev, &bqw); ml_buf_free(dev, &bkw); ml_buf_free(dev, &br);
    ml_buf_free(dev, &bkc); ml_buf_free(dev, &bvc);
    free(q); free(k); free(v); free(rope); free(gq); free(gk); free(wq); free(wk); free(gkf);
}

/* ------------------------------------------------------------------- mel */

static void test_mel(void)
{
    const int N = 16000 * 3 + 123, FR = N / 160;
    float *pcm = malloc(N * 4), win[400], *filt = malloc(201 * 128 * 4);
    float *basis = malloc(400 * 201 * 2 * 4), *got = malloc((size_t)FR * 128 * 4);
    double *want = malloc((size_t)FR * 128 * 8);
    for (int i = 0; i < N; i++)
        pcm[i] = (float)(0.3 * sin(i * 0.05) + 0.1 * frand());
    for (int i = 0; i < 400; i++)
        win[i] = (float)(0.5 - 0.5 * cos(2 * M_PI * i / 400));
    for (int i = 0; i < 201 * 128; i++)
        filt[i] = (float)fabs(frand()) * 0.01f;
    for (int n = 0; n < 400; n++) {
        for (int k = 0; k <= 200; k++) {
            double a = 2.0 * M_PI * (double)((int64_t)k * n % 400) / 400;
            basis[2 * (n * 201 + k)] = (float)cos(a);
            basis[2 * (n * 201 + k) + 1] = (float)-sin(a);
        }
    }
    double mx = -INFINITY;
    for (int t = 0; t < FR; t++) {
        double pw[201];
        for (int k = 0; k <= 200; k++) {
            double re = 0, im = 0;
            for (int j = 0; j < 400; j++) {
                int idx = t * 160 + j - 200;
                if (idx < 0)
                    idx = -idx;
                if (idx >= N)
                    idx = 2 * (N - 1) - idx;
                double x = (double)pcm[idx] * win[j];
                re += x * cos(2 * M_PI * k * j / 400);
                im -= x * sin(2 * M_PI * k * j / 400);
            }
            pw[k] = re * re + im * im;
        }
        for (int m = 0; m < 128; m++) {
            double s = 0;
            for (int k = 0; k <= 200; k++)
                s += filt[k * 128 + m] * pw[k];
            want[t * 128 + m] = log10(fmax(s, 1e-10));
            mx = fmax(mx, want[t * 128 + m]);
        }
    }
    for (int i = 0; i < FR * 128; i++)
        want[i] = (fmax(want[i], mx - 8.0) + 4.0) / 4.0;

    MLBuf bp = upload(pcm, N * 4), bw = upload(win, sizeof(win)), bf = upload(filt, 201 * 128 * 4);
    MLBuf bb = upload(basis, 400 * 201 * 8), bo = zeros((size_t)FR * 128 * 4), bm = zeros(4);
    MelPush p = {
        .pcm = bp.addr, .window = bw.addr, .basis = bb.addr, .filters = bf.addr, .dst = bo.addr,
        .maxbits = bm.addr,
        .nb_samples = N, .nb_frames = FR,
    };
    ml_gpu_dispatch(dev, ML_K_MEL, &p, sizeof(p), (FR + 7) / 8, 1, 1);
    ml_gpu_barrier(dev);
    MelNormPush np = { .mel = bo.addr, .maxbits = bm.addr, .count = FR * 128 };
    ml_gpu_dispatch(dev, ML_K_MEL_NORM, &np, sizeof(np), (FR * 128 + 255) / 256, 1, 1);
    download(&bo, got, (size_t)FR * 128 * 4);
    check("mel", got, want, (size_t)FR * 128, 1e-4);
    ml_buf_free(dev, &bp); ml_buf_free(dev, &bw); ml_buf_free(dev, &bf);
    ml_buf_free(dev, &bb); ml_buf_free(dev, &bo); ml_buf_free(dev, &bm);
    free(pcm); free(filt); free(basis); free(got); free(want);
}

/* ------------------------------------------------------------------ norms */

static void test_norm(bool rms)
{
    const int R = 7, N = 1280;
    float *x = malloc(R * N * 4), w[1280], b[1280], *got = malloc(R * N * 4);
    double *want = malloc(R * N * 8);
    for (int i = 0; i < R * N; i++)
        x[i] = (float)(frand() * 5.0 + 1.0);
    for (int i = 0; i < N; i++) {
        w[i] = (float)frand();
        b[i] = (float)frand();
    }
    for (int r = 0; r < R; r++) {
        double mean = 0, var = 0;
        for (int i = 0; i < N; i++)
            mean += x[r * N + i];
        mean /= N;
        for (int i = 0; i < N; i++) {
            double d = rms ? x[r * N + i] : x[r * N + i] - mean;
            var += d * d;
        }
        double rs = 1.0 / sqrt(var / N + 1e-5);
        for (int i = 0; i < N; i++)
            want[r * N + i] = rms ? x[r * N + i] * rs * w[i] : (x[r * N + i] - mean) * rs * w[i] + b[i];
    }
    MLBuf bx = upload(x, R * N * 4), bw = upload(w, N * 4), bb = upload(b, N * 4), by = zeros(R * N * 4);
    NormPush p = {
        .x = bx.addr, .weight = bw.addr, .bias = bb.addr, .y = by.addr,
        .n = N, .ldx = N, .ldy = N,
        .eps = 1e-5f,
    };
    ml_gpu_dispatch(dev, rms ? ML_K_RMSNORM : ML_K_LAYERNORM, &p, sizeof(p), R, 1, 1);
    download(&by, got, R * N * 4);
    check(rms ? "rmsnorm" : "layernorm", got, want, R * N, 1e-5);
    ml_buf_free(dev, &bx); ml_buf_free(dev, &bw); ml_buf_free(dev, &bb); ml_buf_free(dev, &by);
    free(x); free(got); free(want);
}

int main(int argc, char **argv)
{
    bool bench = argc > 1 && !strcmp(argv[1], "-b");
    int ret = ml_gpu_create(dev, NULL, false);
    if (ret < 0) {
        printf("no device for the model: skipped\n");
        return 77;
    }
    printf("device: %s\n", dev->vk.props.deviceName);

    if (bench) {
        test_gemm(false, 2048, 1280, 1280, FLAG_BIAS, true);
        test_gemm(false, 2048, 5120, 1280, FLAG_BIAS | FLAG_GELU, true);
        test_gemm(false, 2048, 8192, 1280, 0, true);
        test_gemm(true, 512, 4096, 2560, 0, true);
        test_gemm(true, 512, 9728, 2560, 0, true);
        test_gemm(true, 512, 2560, 9728, FLAG_ADD, true);
        test_gemm(true, 1024, 4096, 2560, 0, true);
        test_gemm(true, 1024, 1024, 2560, 0, true);
        test_gemm(true, 1024, 9728, 2560, 0, true);
        test_gemm(true, 1024, 2560, 9728, FLAG_ADD, true);
        test_gemm(true, 2048, 4096, 2560, 0, true);
        test_gemm(true, 2048, 9728, 2560, 0, true);
        test_gemm(true, 2048, 2560, 9728, FLAG_ADD, true);
        test_gemm(true, 1, 9728, 2560, 0, true);
        test_gemm(true, 1, 2560, 9728, 0, true);
        test_gemm(true, 1, 151936, 2560, 0, true);
        test_gemm(true, 8, 9728, 2560, 0, true);
        test_gemm(true, 32, 9728, 2560, 0, true);
        test_gemm(true, 64, 9728, 2560, 0, true);
        test_gemm(true, 65, 9728, 2560, 0, true);
        bench_attn(512, 3584);
        bench_attn(32, 4000);
        bench_attn(1, 4000);
    } else {
        test_gemm(false, 200, 300, 256, FLAG_BIAS, false);
        test_gemm(false, 130, 132, 512, FLAG_BIAS | FLAG_GELU | FLAG_ADD, false);
        test_gemm(true, 150, 260, 512, 0, false);
        test_gemm(true, 129, 128, 768, FLAG_ADD, false);
        test_gemm(true, 1, 300, 512, 0, false);
        test_gemm(true, 5, 77, 1024, FLAG_ADD, false);
        test_gemm(true, 37, 260, 512, FLAG_ADD, false);
        test_conv();
        test_enc_attn();
        test_gemm_rows();
        test_gemm_waves();
        test_attn(70, 30, false, false, 20, 50);
        test_attn(1, 300, false, false, 0, 0);
        test_attn(3, 129, false, false, 0, 0);
        test_attn(130, 1000, false, false, 1000, 333);
        test_attn(70, 30, false, true, 30, 100);
        test_attn(20, 1000, false, true, 1000, 77);
        test_attn(1, 0, true, false, 0, 0);
        test_attn(1, 300, true, false, 250, 129);
        test_attn(2, 1000, true, false, 900, 1000);
        test_attn(2, 127, true, false, 0, 0);
        test_rope();
        test_mel();
        test_norm(false);
        test_norm(true);
    }

    /* With CYANBOX_GPU_VALIDATION=1: what the layer found fails the test, as
     * test_compute's does. */
    int errors = cb_vkdev_validation_errors(&dev->vk);
    if (errors) {
        printf("FAIL %d Vulkan validation errors\n", errors);
        failures++;
    }
    ml_gpu_destroy(dev);
    printf("%s\n", failures ? "FAILED" : "all ok");
    return failures ? 1 : 0;
}
