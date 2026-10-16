/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* The kernels' push constants, as the shaders declare them (std430: the
 * addresses first, 8-byte aligned, then the 4-byte fields), shared by
 * moss.c and the kernel tests. */

#ifndef CYANBOX_ML_KERNELS_H
#define CYANBOX_ML_KERNELS_H

#include <stdint.h>

/* gemm.comp, gemv.comp */
#define FLAG_GELU 1u
#define FLAG_ADD  2u
#define FLAG_BIAS 4u

/* act.comp */
enum {
    ACT_GELU = 0,
    ACT_SWIGLU,
    ACT_ADD,
    ACT_ADD_ROWS,
    ACT_ADD_TILE,
};

typedef struct GemmPush {
    uint64_t x, w, bias, y;
    uint32_t M, N, K;
    uint32_t ldx, ldy;
    uint32_t flags;
    uint32_t fin, tin, fout, tout, cin;
} GemmPush;

typedef struct NormPush {
    uint64_t x, weight, bias, y;
    uint32_t n, ldx, ldy;
    float eps;
} NormPush;

typedef struct ActPush {
    uint64_t a, b, idx;
    uint32_t count, n, period, op;
} ActPush;

typedef struct PermutePush {
    uint64_t src, dst;
    uint32_t cout, cin;
} PermutePush;

typedef struct MelPush {
    uint64_t pcm, window, basis, filters, dst, maxbits;
    uint32_t nb_samples, nb_frames, frame0;
} MelPush;

typedef struct MelNormPush {
    uint64_t mel, maxbits;
    uint32_t count;
} MelNormPush;

typedef struct Conv1Push {
    uint64_t mel, weight, bias, dst;
    uint32_t nb_frames, chunk0, nb_chunks;
} Conv1Push;

typedef struct StemPush {
    uint64_t src, dst;
    uint32_t nb_chunks;
} StemPush;

typedef struct EncAttnPush {
    uint64_t q, k, v, o;
    uint32_t nb_chunks, tail;
    float scale;
} EncAttnPush;

typedef struct EmbedPush {
    uint64_t table, ids, audio_idx, audio, dst;
    uint32_t n;
} EmbedPush;

typedef struct RopePush {
    uint64_t q, k, v, qw, kw, rope, kc, vc;
    uint32_t pos0, max_ctx, nb_q, nb_kv;
    float eps;
} RopePush;

/* attn.comp: at most ATTN_MAX_PARTS (query, split) pairs when the keys
 * are split. */
#define ATTN_MAX_PARTS 1024

typedef struct AttnPush {
    uint64_t q, o, kc, vc, part;
    uint32_t nq, pos0, max_ctx, nb_q, nb_kv, nsplit, split_len;
    float scale;
} AttnPush;

/* attn_dec.comp: splits of ATTN_SPLIT keys, at most ATTN_DEC_ROWS rows of
 * (query, head); a row's part is 128 outputs, its maximum and its sum. */
#define ATTN_SPLIT    128
#define ATTN_DEC_ROWS 8
#define ATTN_PART     130

typedef struct AttnDecPush {
    uint64_t q, o, kc, vc, part;
    uint32_t nq, pos0, max_ctx, nb_q, nb_kv, nsplit;
    float scale;
} AttnDecPush;

typedef struct ArgmaxPush {
    uint64_t logits, dst;
    uint32_t n;
} ArgmaxPush;

#define ML_PUSH_CHECK(t) static_assert(sizeof(t) <= 128, #t " is too big for push constants")
ML_PUSH_CHECK(GemmPush);
ML_PUSH_CHECK(RopePush);

#endif /* CYANBOX_ML_KERNELS_H */
