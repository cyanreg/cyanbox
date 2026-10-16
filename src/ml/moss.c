/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* MOSS-Audio (cyanbox/ml.h): the network, run kernel by kernel on the GPU.
 * The reference is OpenMOSS's modeling_moss_audio.py and
 * processing_moss_audio.py (github.com/OpenMOSS/MOSS-Audio); every step
 * below names the part of it that it does.
 *
 *   audio, 16 kHz  -> Whisper's 128-bin log-mel, 100 frames a second
 *   mel            -> chunks of 400 frames (4 s), each on its own: three
 *                     3x3 stride-2 convolutions with GELU (to 50 frames
 *                     of 480 x 16), a projection to 1280, sinusoidal
 *                     positions from 0 in each chunk
 *   encoder        -> 32 Whisper layers whose attention stays inside its
 *                     chunk; the outputs of layers 8, 16 and 24 are kept
 *                     (the "deepstack"), the last is layer-normed
 *   adapter        -> a SwiGLU MLP to the decoder's 2560: one row per 80 ms
 *                     of audio, scattered into the prompt at its audio
 *                     tokens; the three kept outputs, each through its own
 *                     MLP, are added to the audio tokens' rows after the
 *                     decoder's first three layers
 *   decoder        -> Qwen3-4B (36 layers, GQA 32/8, q/k norms, RoPE)
 *
 * Choices, and why:
 *  - The prompt is the processor's default one with the audio and its time
 *    markers (the seconds, every 2 s, as digits between the audio tokens;
 *    MOSS-Audio's own scripts turn them on), and the question after the
 *    audio. Everything up to the audio's end is read into the cache once
 *    (cb_ml_listen); each question starts again from there.
 *  - Questions asked together (ml_ask_many; cb_ml_tag asks eighteen) go
 *    through the decoder together, each at the positions after the audio
 *    but in slots of the cache of its own, and their answers a token each
 *    a step. Each comes out as it would alone, bit for bit (Segment says
 *    how), several times faster: a question of a few dozen tokens alone
 *    keeps most of the GPU idle.
 *  - The prefill goes through the decoder 2048 tokens a pass, in chunks of
 *    512 that each keep the attention they would have alone (the GEMM is
 *    faster on more rows), so its scratch buffers do not grow with the
 *    audio; the decoder's single tokens use the GEMV kernel, which reads
 *    each weight once.
 *  - The cache is f16, [layer][kv head][position][128]: a block of keys
 *    is one contiguous read for the attention kernel.
 *  - Tables (the DFT's basis, RoPE's angles, the encoder's positions) are
 *    computed here in double precision and uploaded once.
 *  - The audio's scratch is freed as soon as nothing reads it: the
 *    encoder's buffers (1.2 GB for 9.5 minutes) once its rows are made,
 *    the adapter's rows once the audio is in the cache; the questions
 *    after it read none of it, and the GPU is shared with the desktop and
 *    the analyses. Each listen allocates it again, at the size it needs.
 *  - The context left for the questions is what they need, measured by the
 *    caller (cb_ml_tag: the colour question grows with the legend's
 *    words), and never less than QUESTION_ROOM. */

#include "ml_priv.h"
#include "kernels.h"

#include <math.h>
#include <stdio.h>

/* The shape of MOSS-Audio-4B; the file's metadata is checked against it. */
#define N_MEL      128
#define N_FFT      400
#define HOP        160
#define CHUNK      400          /* mel frames a chunk */
#define CHUNK_TOK  50           /* tokens a chunk */
#define E_DIM      1280
#define E_HEADS    20
#define E_FFN      5120
#define E_LAYERS   32
#define C_DIM      480
#define A_HIDDEN   8192
#define L_DIM      2560
#define L_HEADS    32
#define L_KV       8
#define L_HEAD     128
#define L_FFN      9728
#define L_LAYERS   36
#define VOCAB      ML_VOCAB
#define DEEPSTACK  3

/* Chunks through the convolutions at once; tokens through the decoder at
 * once; rows through the adapter at once. */
#define CONV_BATCH 8
#define LLM_CHUNK  512
#define ADAPT_ROWS 2048

/* Rows of a pass through the decoder at most when reading the audio: its
 * chunks of LLM_CHUNK tokens go through four at a time, each keeping its
 * own attention (Segment), as the GEMM is faster on more rows. */
#define PASS_ROWS  2048

#define TOK_ENDOFTEXT 151643
#define TOK_IM_START  151644
#define TOK_IM_END    151645
#define TOK_AUDIO     151654
#define TOK_AUDIO_BOS 151669
#define TOK_AUDIO_EOS 151670
#define TOK_DIGIT0    15

typedef struct EncLayer {
    const MLTensor *ln1_w, *ln1_b, *q_w, *q_b, *k_w, *v_w, *v_b, *o_w, *o_b;
    const MLTensor *ln2_w, *ln2_b, *fc1_w, *fc1_b, *fc2_w, *fc2_b;
} EncLayer;

typedef struct LLMLayer {
    const MLTensor *attn_norm, *q, *k, *v, *o, *q_norm, *k_norm;
    const MLTensor *ffn_norm, *gate, *up, *down;
} LLMLayer;

typedef struct MLP {
    const MLTensor *gate, *up, *down;
} MLP;

/* A scratch buffer that grows to the largest size asked of it. */
typedef struct Scratch {
    MLBuf b;
} Scratch;

struct CBML {
    MLGpu g;
    MLGguf f;
    MLTokenizer *tok;
    int max_ctx;

    const MLTensor *mel_filters, *mel_window;
    const MLTensor *conv1_w, *conv1_b, *conv2_b, *conv3_b, *stem_w, *stem_b;
    const MLTensor *enc_norm_w, *enc_norm_b;
    EncLayer enc[E_LAYERS];
    MLP adapter, ds[DEEPSTACK];
    const MLTensor *embed, *final_norm, *lm_head;
    LLMLayer llm[L_LAYERS];

    MLBuf conv_w[2];            /* conv2, conv3: [out][3][3][in] */
    MLBuf tables;               /* DFT basis, encoder positions, RoPE */
    VkDeviceAddress basis, posemb, rope;
    MLBuf kc, vc;               /* [layer][kv][max_ctx][128] f16 */

    /* Scratch: the audio's, then the decoder's. */
    Scratch pcm, mel, maxbits, c1, c2, c3, stem;
    Scratch ex, eh, eq, ek, ev, eo, ef, ds_x[DEEPSTACK];
    Scratch ag, au, audio, ds_out[DEEPSTACK];
    Scratch x, h, q, k, v, o, fg, fu, logits, ids, aidx, pos, next, part;
    MLBuf up, down;             /* host buffers: ids and positions in, tokens or logits out */

    /* The context: what the cache holds. */
    int nb_prefix;              /* tokens of the prompt up to the audio's end */
    int pass_rows;              /* PASS_ROWS, or fewer (ml_set_listen) */
    bool listen_logits;         /* the prompt's last logits too (ml_set_listen) */
    int nb_audio;               /* audio rows in `audio`, `ds_out` */

    double listen_ms, ask_ms;
    MLStats stats;
};

/* ----------------------------------------------------------------- tensors */

/* The tensor `fmt` (with i), of exactly this type, shape and size: the
 * kernels read as much of it as the shape says, unchecked. The sizes past
 * a tensor's own dimensions are 1 (gguf.c), so the four say everything. */
static const MLTensor *tensor4(CBML *ml, const char *fmt, int i, enum MLType type,
                               int64_t ne0, int64_t ne1, int64_t ne2, int64_t ne3)
{
    char name[96];
    snprintf(name, sizeof(name), fmt, i);
    const MLTensor *t = ml_gguf_tensor(&ml->f, name);
    if (!t) {
        cb_err("ml: the model has no tensor %s", name);
        return NULL;
    }
    uint64_t n = (uint64_t)(ne0 * ne1 * ne2 * ne3);
    uint64_t size = type == ML_TYPE_Q4_K ? n / ML_QK_K * ML_Q4K_BYTES :
                    n * (type == ML_TYPE_F16 ? 2 : 4);
    if (t->type != type || t->ne[0] != ne0 || t->ne[1] != ne1 || t->ne[2] != ne2 ||
        t->ne[3] != ne3 || t->size != size) {
        cb_err("ml: tensor %s is type %d, %lld x %lld x %lld x %lld; expected type %d, "
               "%lld x %lld x %lld x %lld", name, t->type, (long long)t->ne[0],
               (long long)t->ne[1], (long long)t->ne[2], (long long)t->ne[3], type,
               (long long)ne0, (long long)ne1, (long long)ne2, (long long)ne3);
        return NULL;
    }
    return t;
}

static const MLTensor *tensor(CBML *ml, const char *fmt, int i, enum MLType type,
                              int64_t ne0, int64_t ne1)
{
    return tensor4(ml, fmt, i, type, ne0, ne1, 1, 1);
}

#define T_(dst, ...) do { if (!((dst) = tensor(ml, __VA_ARGS__))) return CB_ERROR(EINVAL); } while (0)

static int find_tensors(CBML *ml)
{
    T_(ml->mel_filters, "audio.mel_filters", 0, ML_TYPE_F32, N_MEL, N_FFT / 2 + 1);
    T_(ml->mel_window, "audio.mel_window", 0, ML_TYPE_F32, N_FFT, 1);
    if (!(ml->conv1_w = tensor4(ml, "enc.conv1.weight", 0, ML_TYPE_F16, 3, 3, 1, C_DIM)))
        return CB_ERROR(EINVAL);
    /* conv2 and conv3 are read once, by permute_convs: checked here, before
     * the weights go up. */
    for (int i = 2; i <= 3; i++) {
        if (!tensor4(ml, "enc.conv%d.weight", i, ML_TYPE_F16, 3, 3, C_DIM, C_DIM))
            return CB_ERROR(EINVAL);
    }
    T_(ml->conv1_b, "enc.conv1.bias", 0, ML_TYPE_F32, C_DIM, 1);
    T_(ml->conv2_b, "enc.conv2.bias", 0, ML_TYPE_F32, C_DIM, 1);
    T_(ml->conv3_b, "enc.conv3.bias", 0, ML_TYPE_F32, C_DIM, 1);
    T_(ml->stem_w, "enc.stem_proj.weight", 0, ML_TYPE_F16, C_DIM * 16, E_DIM);
    T_(ml->stem_b, "enc.stem_proj.bias", 0, ML_TYPE_F32, E_DIM, 1);
    T_(ml->enc_norm_w, "enc.norm.weight", 0, ML_TYPE_F32, E_DIM, 1);
    T_(ml->enc_norm_b, "enc.norm.bias", 0, ML_TYPE_F32, E_DIM, 1);
    for (int i = 0; i < E_LAYERS; i++) {
        EncLayer *l = &ml->enc[i];
        T_(l->ln1_w, "enc.blk.%d.attn_norm.weight", i, ML_TYPE_F32, E_DIM, 1);
        T_(l->ln1_b, "enc.blk.%d.attn_norm.bias", i, ML_TYPE_F32, E_DIM, 1);
        T_(l->q_w, "enc.blk.%d.attn.q.weight", i, ML_TYPE_F16, E_DIM, E_DIM);
        T_(l->q_b, "enc.blk.%d.attn.q.bias", i, ML_TYPE_F32, E_DIM, 1);
        T_(l->k_w, "enc.blk.%d.attn.k.weight", i, ML_TYPE_F16, E_DIM, E_DIM);
        T_(l->v_w, "enc.blk.%d.attn.v.weight", i, ML_TYPE_F16, E_DIM, E_DIM);
        T_(l->v_b, "enc.blk.%d.attn.v.bias", i, ML_TYPE_F32, E_DIM, 1);
        T_(l->o_w, "enc.blk.%d.attn.o.weight", i, ML_TYPE_F16, E_DIM, E_DIM);
        T_(l->o_b, "enc.blk.%d.attn.o.bias", i, ML_TYPE_F32, E_DIM, 1);
        T_(l->ln2_w, "enc.blk.%d.ffn_norm.weight", i, ML_TYPE_F32, E_DIM, 1);
        T_(l->ln2_b, "enc.blk.%d.ffn_norm.bias", i, ML_TYPE_F32, E_DIM, 1);
        T_(l->fc1_w, "enc.blk.%d.ffn.fc1.weight", i, ML_TYPE_F16, E_DIM, E_FFN);
        T_(l->fc1_b, "enc.blk.%d.ffn.fc1.bias", i, ML_TYPE_F32, E_FFN, 1);
        T_(l->fc2_w, "enc.blk.%d.ffn.fc2.weight", i, ML_TYPE_F16, E_FFN, E_DIM);
        T_(l->fc2_b, "enc.blk.%d.ffn.fc2.bias", i, ML_TYPE_F32, E_DIM, 1);
    }
    T_(ml->adapter.gate, "adapter.gate.weight", 0, ML_TYPE_F16, E_DIM, A_HIDDEN);
    T_(ml->adapter.up, "adapter.up.weight", 0, ML_TYPE_F16, E_DIM, A_HIDDEN);
    T_(ml->adapter.down, "adapter.down.weight", 0, ML_TYPE_F16, A_HIDDEN, L_DIM);
    for (int i = 0; i < DEEPSTACK; i++) {
        T_(ml->ds[i].gate, "deepstack.%d.gate.weight", i, ML_TYPE_F16, E_DIM, A_HIDDEN);
        T_(ml->ds[i].up, "deepstack.%d.up.weight", i, ML_TYPE_F16, E_DIM, A_HIDDEN);
        T_(ml->ds[i].down, "deepstack.%d.down.weight", i, ML_TYPE_F16, A_HIDDEN, L_DIM);
    }
    T_(ml->embed, "llm.embed.weight", 0, ML_TYPE_Q4_K, L_DIM, VOCAB);
    T_(ml->final_norm, "llm.final_norm.weight", 0, ML_TYPE_F32, L_DIM, 1);
    T_(ml->lm_head, "llm.lm_head.weight", 0, ML_TYPE_Q4_K, L_DIM, VOCAB);
    for (int i = 0; i < L_LAYERS; i++) {
        LLMLayer *l = &ml->llm[i];
        T_(l->attn_norm, "llm.blk.%d.attn_norm.weight", i, ML_TYPE_F32, L_DIM, 1);
        T_(l->q, "llm.blk.%d.attn.q.weight", i, ML_TYPE_Q4_K, L_DIM, L_HEADS * L_HEAD);
        T_(l->k, "llm.blk.%d.attn.k.weight", i, ML_TYPE_Q4_K, L_DIM, L_KV * L_HEAD);
        T_(l->v, "llm.blk.%d.attn.v.weight", i, ML_TYPE_Q4_K, L_DIM, L_KV * L_HEAD);
        T_(l->o, "llm.blk.%d.attn.o.weight", i, ML_TYPE_Q4_K, L_HEADS * L_HEAD, L_DIM);
        T_(l->q_norm, "llm.blk.%d.attn.q_norm.weight", i, ML_TYPE_F32, L_HEAD, 1);
        T_(l->k_norm, "llm.blk.%d.attn.k_norm.weight", i, ML_TYPE_F32, L_HEAD, 1);
        T_(l->ffn_norm, "llm.blk.%d.ffn_norm.weight", i, ML_TYPE_F32, L_DIM, 1);
        T_(l->gate, "llm.blk.%d.ffn.gate.weight", i, ML_TYPE_Q4_K, L_DIM, L_FFN);
        T_(l->up, "llm.blk.%d.ffn.up.weight", i, ML_TYPE_Q4_K, L_DIM, L_FFN);
        T_(l->down, "llm.blk.%d.ffn.down.weight", i, ML_TYPE_Q4_K, L_FFN, L_DIM);
    }
    return 0;
}

#undef T_

static int check_shape(CBML *ml)
{
    static const struct { const char *key; int64_t want; } keys[] = {
        { "moss_audio.enc.num_mel_bins", N_MEL },
        { "moss_audio.enc.encoder_layers", E_LAYERS },
        { "moss_audio.enc.d_model", E_DIM },
        { "moss_audio.enc.encoder_attention_heads", E_HEADS },
        { "moss_audio.enc.encoder_ffn_dim", E_FFN },
        { "moss_audio.enc.downsample_hidden_size", C_DIM },
        { "moss_audio.adapter.hidden_size", A_HIDDEN },
        { "moss_audio.llm.hidden_size", L_DIM },
        { "moss_audio.llm.num_layers", L_LAYERS },
        { "moss_audio.llm.num_heads", L_HEADS },
        { "moss_audio.llm.num_kv_heads", L_KV },
        { "moss_audio.llm.head_dim", L_HEAD },
        { "moss_audio.llm.intermediate_size", L_FFN },
        { "moss_audio.llm.vocab_size", VOCAB },
        { "moss_audio.deepstack.num_taps", DEEPSTACK },
        { "moss_audio.deepstack.tap.0", 8 },
        { "moss_audio.deepstack.tap.1", 16 },
        { "moss_audio.deepstack.tap.2", 24 },
        { "moss_audio.audio_token_id", TOK_AUDIO },
        { "moss_audio.audio_start_id", TOK_AUDIO_BOS },
        { "moss_audio.audio_end_id", TOK_AUDIO_EOS },
    };
    const MLValue *arch = ml_gguf_value(&ml->f, "general.architecture");
    if (!arch || arch->type != ML_GGUF_STRING || strcmp(arch->s, "moss_audio")) {
        cb_err("ml: the file is not a MOSS-Audio model");
        return CB_ERROR(EINVAL);
    }
    for (size_t i = 0; i < CB_ARRAY_ELEMS(keys); i++) {
        int64_t v = ml_gguf_int(&ml->f, keys[i].key, -1);
        if (v != keys[i].want) {
            cb_err("ml: %s is %lld, this runs %lld", keys[i].key, (long long)v,
                   (long long)keys[i].want);
            return CB_ERROR(EINVAL);
        }
    }
    return 0;
}

/* ---------------------------------------------------------------- helpers */

static int reserve(CBML *ml, Scratch *s, size_t size)
{
    if (s->b.size >= size)
        return 0;
    /* A buffer that grows grows by half again, so a slowly growing need
     * does not reallocate every time; the first is the size asked. */
    size_t want = s->b.size ? size + size / 2 : size;
    ml_buf_free(&ml->g, &s->b);
    return ml_buf_create(&ml->g, &s->b, CB_ALIGN(want, 65536), ML_MEM_DEVICE);
}

/* The audio's buffers: the encoder's scratch and the rows it made, which
 * only cb_ml_listen reads. */
static void free_audio_scratch(CBML *ml)
{
    Scratch *all[] = {
        &ml->pcm, &ml->mel, &ml->maxbits, &ml->c1, &ml->c2, &ml->c3, &ml->stem,
        &ml->ex, &ml->eh, &ml->eq, &ml->ek, &ml->ev, &ml->eo, &ml->ef,
        &ml->ds_x[0], &ml->ds_x[1], &ml->ds_x[2], &ml->ag, &ml->au,
        &ml->audio, &ml->ds_out[0], &ml->ds_out[1], &ml->ds_out[2],
    };
    for (size_t i = 0; i < CB_ARRAY_ELEMS(all); i++)
        ml_buf_free(&ml->g, &all[i]->b);
}

/* The encoder's scratch, once its rows are made: the decoder reads only
 * the adapter's and the deepstack's. */
static void free_encoder_scratch(CBML *ml)
{
    Scratch *all[] = {
        &ml->pcm, &ml->mel, &ml->maxbits, &ml->c1, &ml->c2, &ml->c3, &ml->stem,
        &ml->ex, &ml->eh, &ml->eq, &ml->ek, &ml->ev, &ml->eo, &ml->ef,
        &ml->ds_x[0], &ml->ds_x[1], &ml->ds_x[2], &ml->ag, &ml->au,
    };
    for (size_t i = 0; i < CB_ARRAY_ELEMS(all); i++)
        ml_buf_free(&ml->g, &all[i]->b);
}

/* The decoder's larger scratch, which the questions asked together grow
 * past what reading the audio needs: freed before the encoder's is made,
 * so that the two are not on the GPU at once. */
static void free_decoder_scratch(CBML *ml)
{
    Scratch *all[] = {
        &ml->x, &ml->h, &ml->q, &ml->k, &ml->v, &ml->o, &ml->fg, &ml->fu, &ml->logits, &ml->part,
    };
    for (size_t i = 0; i < CB_ARRAY_ELEMS(all); i++)
        ml_buf_free(&ml->g, &all[i]->b);
}

/* x and y dims of a dispatch of `groups` workgroups along one axis. */
static void groups_2d(uint64_t groups, uint32_t *x, uint32_t *y)
{
    *x = (uint32_t)CB_MIN(groups, 65535);
    *y = (uint32_t)((groups + *x - 1) / *x);
}

static void dispatch_elems(CBML *ml, enum MLKernel k, const void *push, size_t size,
                           uint64_t count)
{
    uint32_t x, y;
    groups_2d((count + 255) / 256, &x, &y);
    ml_gpu_dispatch(&ml->g, k, push, size, x, CB_MAX(y, 1), 1);
}

/* y[M][N] (ldy) = x[M][K] (ldx) w^T, with flags. */
static void gemm(CBML *ml, VkDeviceAddress x, uint32_t ldx, const MLTensor *w,
                 VkDeviceAddress bias, VkDeviceAddress y, uint32_t ldy, uint32_t M,
                 uint32_t flags)
{
    uint32_t K = (uint32_t)w->ne[0], N = (uint32_t)w->ne[1];
    GemmPush p = {
        .x = x, .w = w->addr, .bias = bias, .y = y,
        .M = M, .N = N, .K = K,
        .ldx = ldx, .ldy = ldy,
        .flags = flags | (bias ? FLAG_BIAS : 0),
    };
    if (w->type == ML_TYPE_Q4_K && M <= 8) {
        ml_gpu_dispatch(&ml->g, ML_K_GEMV_Q4K, &p, sizeof(p), (N + 3) / 4, 1, 1);
        return;
    }
    /* A question's few dozen tokens in tiles of 32 rows rather than 128:
     * the product is bound by reading the weights, not by arithmetic. */
    if (w->type == ML_TYPE_Q4_K && M <= 64) {
        ml_gpu_dispatch(&ml->g, ML_K_GEMM_Q4K_SMALL, &p, sizeof(p), (N + 127) / 128,
                        (M + 31) / 32, 1);
        return;
    }
    enum MLKernel k = w->type == ML_TYPE_Q4_K ? ML_K_GEMM_Q4K : ML_K_GEMM_F16;
    ml_gpu_dispatch(&ml->g, k, &p, sizeof(p), (N + 127) / 128, (M + 127) / 128, 1);
}

static void norm(CBML *ml, bool rms, VkDeviceAddress x, const MLTensor *w, const MLTensor *b,
                 VkDeviceAddress y, uint32_t rows, uint32_t n, float eps)
{
    NormPush p = {
        .x = x, .weight = w->addr, .bias = b ? b->addr : 0, .y = y,
        .n = n, .ldx = n, .ldy = n,
        .eps = eps,
    };
    ml_gpu_dispatch(&ml->g, rms ? ML_K_RMSNORM : ML_K_LAYERNORM, &p, sizeof(p), rows, 1, 1);
}

static void act(CBML *ml, int op, VkDeviceAddress a, VkDeviceAddress b, VkDeviceAddress idx,
                uint64_t count, uint32_t n, uint32_t period)
{
    ActPush p = {
        .a = a, .b = b, .idx = idx,
        .count = (uint32_t)count, .n = n, .period = period, .op = (uint32_t)op,
    };
    dispatch_elems(ml, ML_K_ACT, &p, sizeof(p), count);
}

/* A SwiGLU MLP (MOSS-Audio's GatedMLP): y = down(silu(gate(x)) * up(x)),
 * from E_DIM to L_DIM, ADAPT_ROWS rows at a time. */
static int mlp(CBML *ml, const MLP *m, VkDeviceAddress x, VkDeviceAddress y, uint32_t rows)
{
    int ret;
    uint32_t step = CB_MIN(rows, ADAPT_ROWS);
    if ((ret = reserve(ml, &ml->ag, (size_t)step * A_HIDDEN * 4)) < 0 ||
        (ret = reserve(ml, &ml->au, (size_t)step * A_HIDDEN * 4)) < 0)
        return ret;
    for (uint32_t r = 0; r < rows; r += step) {
        uint32_t n = CB_MIN(step, rows - r);
        VkDeviceAddress xr = x + (VkDeviceAddress)r * E_DIM * 4;
        gemm(ml, xr, E_DIM, m->gate, 0, ml->ag.b.addr, A_HIDDEN, n, 0);
        gemm(ml, xr, E_DIM, m->up, 0, ml->au.b.addr, A_HIDDEN, n, 0);
        ml_gpu_barrier(&ml->g);
        act(ml, ACT_SWIGLU, ml->ag.b.addr, ml->au.b.addr, 0, (uint64_t)n * A_HIDDEN, 0, 0);
        ml_gpu_barrier(&ml->g);
        gemm(ml, ml->ag.b.addr, A_HIDDEN, m->down, 0, y + (VkDeviceAddress)r * L_DIM * 4, L_DIM,
             n, 0);
        ml_gpu_barrier(&ml->g);
    }
    return 0;
}

/* --------------------------------------------------------------- tables */

static int make_tables(CBML *ml)
{
    size_t nb_basis = (size_t)N_FFT * (N_FFT / 2 + 1) * 2;
    size_t nb_pos = (size_t)CHUNK_TOK * E_DIM;
    size_t nb_rope = (size_t)ml->max_ctx * (L_HEAD / 2) * 2;
    size_t total = nb_basis + nb_pos + nb_rope;
    float *t = malloc(total * sizeof(float));
    if (!t)
        return CB_ERROR(ENOMEM);

    /* The DFT, [n][k] (cos, -sin): X_k = sum x_n e^(-2 pi i k n / 400).
     * (The sign of the sine does not matter to the power.) */
    float *basis = t;
    for (int n = 0; n < N_FFT; n++) {
        for (int k = 0; k <= N_FFT / 2; k++) {
            double a = 2.0 * M_PI * (double)((int64_t)k * n % N_FFT) / N_FFT;
            basis[2 * (n * (N_FFT / 2 + 1) + k)] = (float)cos(a);
            basis[2 * (n * (N_FFT / 2 + 1) + k) + 1] = (float)-sin(a);
        }
    }

    /* SinusoidsPositionEmbedding: [sin(t w_i), cos(t w_i)], w_i =
     * 10000^(-i / 639), i < 640. */
    float *pos = basis + nb_basis;
    double inc = log(10000.0) / (E_DIM / 2 - 1);
    for (int p = 0; p < CHUNK_TOK; p++) {
        for (int i = 0; i < E_DIM / 2; i++) {
            double a = p * exp(-inc * i);
            pos[p * E_DIM + i] = (float)sin(a);
            pos[p * E_DIM + E_DIM / 2 + i] = (float)cos(a);
        }
    }

    /* RoPE: inv_freq_i = theta^(-2 i / 128), (cos, sin) of p inv_freq_i. */
    float *rope = pos + nb_pos;
    double theta = ml_gguf_float(&ml->f, "moss_audio.llm.rope_theta", 1000000.0);
    for (int p = 0; p < ml->max_ctx; p++) {
        for (int i = 0; i < L_HEAD / 2; i++) {
            double a = p * pow(theta, -2.0 * i / L_HEAD);
            rope[2 * (p * (L_HEAD / 2) + i)] = (float)cos(a);
            rope[2 * (p * (L_HEAD / 2) + i) + 1] = (float)sin(a);
        }
    }

    MLBuf staging;
    int ret = ml_buf_create(&ml->g, &staging, total * sizeof(float), ML_MEM_UPLOAD);
    if (ret >= 0)
        ret = ml_buf_create(&ml->g, &ml->tables, total * sizeof(float), ML_MEM_DEVICE);
    if (ret >= 0) {
        memcpy(staging.map, t, total * sizeof(float));
        ml_buf_flush(&ml->g, &staging, total * sizeof(float));
        ml_gpu_copy(&ml->g, &staging, 0, &ml->tables, 0, total * sizeof(float));
        ret = ml_gpu_run(&ml->g);
    }
    ml_buf_free(&ml->g, &staging);
    free(t);
    if (ret < 0)
        return ret;

    ml->basis = ml->tables.addr;
    ml->posemb = ml->basis + nb_basis * sizeof(float);
    ml->rope = ml->posemb + nb_pos * sizeof(float);
    return 0;
}

static int permute_convs(CBML *ml)
{
    for (int i = 0; i < 2; i++) {
        const MLTensor *w = tensor4(ml, "enc.conv%d.weight", i + 2, ML_TYPE_F16, 3, 3, C_DIM,
                                    C_DIM);
        if (!w)
            return CB_ERROR(EINVAL);
        int ret = ml_buf_create(&ml->g, &ml->conv_w[i], w->size, ML_MEM_DEVICE);
        if (ret < 0)
            return ret;
        PermutePush p = {
            .src = w->addr, .dst = ml->conv_w[i].addr,
            .cout = C_DIM, .cin = C_DIM,
        };
        dispatch_elems(ml, ML_K_CONV_PERMUTE, &p, sizeof(p), (uint64_t)C_DIM * C_DIM * 9);
    }
    return ml_gpu_run(&ml->g);
}

/* ------------------------------------------------------------- open/close */

void cb_ml_options_default(CBMLOptions *o)
{
    *o = (CBMLOptions) { 0 };
}

int cb_ml_open(CBML **out, const char *path, const CBMLOptions *opt)
{
    *out = NULL;
    CBMLOptions defopt;
    if (!opt) {
        cb_ml_options_default(&defopt);
        opt = &defopt;
    }

    CBML *ml = calloc(1, sizeof(*ml));
    if (!ml)
        return CB_ERROR(ENOMEM);
    ml->f.fd = -1;
    ml->max_ctx = opt->max_context > 0 ? opt->max_context : 8192;
    ml->max_ctx = CB_ALIGN(ml->max_ctx, 64);
    ml->pass_rows = PASS_ROWS;

    int64_t t0 = cb_time_us();
    int ret;
    if ((ret = ml_gguf_open(&ml->f, path)) < 0)
        goto fail;
    if ((ret = check_shape(ml)) < 0 || (ret = find_tensors(ml)) < 0)
        goto fail;
    if ((ret = ml_tok_create(&ml->tok, &ml->f)) < 0)
        goto fail;
    if ((ret = ml_gpu_create(&ml->g, opt->device, opt->validation)) < 0)
        goto fail;
    cb_info("ml: %s on %s", path, ml->g.vk.props.deviceName);
    if ((ret = ml_gguf_upload(&ml->f, &ml->g)) < 0)
        goto fail;
    if ((ret = permute_convs(ml)) < 0 || (ret = make_tables(ml)) < 0)
        goto fail;

    size_t cache = (size_t)L_LAYERS * L_KV * ml->max_ctx * L_HEAD * 2;
    if ((ret = ml_buf_create(&ml->g, &ml->kc, cache, ML_MEM_DEVICE)) < 0 ||
        (ret = ml_buf_create(&ml->g, &ml->vc, cache, ML_MEM_DEVICE)) < 0)
        goto fail;
    if ((ret = ml_buf_create(&ml->g, &ml->up, (size_t)ml->max_ctx * 16, ML_MEM_UPLOAD)) < 0 ||
        (ret = ml_buf_create(&ml->g, &ml->down, (size_t)VOCAB * 8 * 4, ML_MEM_READBACK)) < 0)
        goto fail;

    cb_debug("ml: ready in %.1f s, context of %d tokens", (cb_time_us() - t0) * 1e-6,
             ml->max_ctx);
    *out = ml;
    return 0;

fail:
    cb_ml_close(&ml);
    return ret;
}

void cb_ml_close(CBML **pml)
{
    CBML *ml = *pml;
    if (!ml)
        return;
    if (ml->g.vk.dev) {
        ml_gpu_profile_print(&ml->g);
        vkDeviceWaitIdle(ml->g.vk.dev);
        Scratch *all[] = {
            &ml->pcm, &ml->mel, &ml->maxbits, &ml->c1, &ml->c2, &ml->c3, &ml->stem,
            &ml->ex, &ml->eh, &ml->eq, &ml->ek, &ml->ev, &ml->eo, &ml->ef,
            &ml->ds_x[0], &ml->ds_x[1], &ml->ds_x[2],
            &ml->ag, &ml->au, &ml->audio, &ml->ds_out[0], &ml->ds_out[1], &ml->ds_out[2],
            &ml->x, &ml->h, &ml->q, &ml->k, &ml->v, &ml->o, &ml->fg, &ml->fu,
            &ml->logits, &ml->ids, &ml->aidx, &ml->pos, &ml->next, &ml->part,
        };
        for (size_t i = 0; i < CB_ARRAY_ELEMS(all); i++)
            ml_buf_free(&ml->g, &all[i]->b);
        ml_buf_free(&ml->g, &ml->up);
        ml_buf_free(&ml->g, &ml->down);
        ml_buf_free(&ml->g, &ml->kc);
        ml_buf_free(&ml->g, &ml->vc);
        ml_buf_free(&ml->g, &ml->tables);
        ml_buf_free(&ml->g, &ml->conv_w[0]);
        ml_buf_free(&ml->g, &ml->conv_w[1]);
    }
    ml_gguf_close(&ml->f, ml->g.vk.dev ? &ml->g : NULL);
    ml_tok_free(&ml->tok);
    ml_gpu_destroy(&ml->g);
    free(ml);
    *pml = NULL;
}

const char *cb_ml_device_name(const CBML *ml)
{
    return ml->g.vk.props.deviceName;
}

void cb_ml_times(const CBML *ml, double *listen_ms, double *ask_ms)
{
    if (listen_ms)
        *listen_ms = ml->listen_ms;
    if (ask_ms)
        *ask_ms = ml->ask_ms;
}

/* ---------------------------------------------------------------- encoder */

/* With CYANBOX_ML_DUMP=DIR, the encoder's intermediate tensors are written
 * there as raw f32, for checking the network against the reference
 * implementation outside of the model (tools/ml_check.py). Waits for the
 * GPU. */
static void dump(CBML *ml, const char *name, const MLBuf *b, size_t size)
{
    const char *dir = getenv("CYANBOX_ML_DUMP");
    if (!dir || !dir[0] || ml_gpu_run(&ml->g) < 0)
        return;
    MLBuf rb;
    if (ml_buf_create(&ml->g, &rb, size, ML_MEM_READBACK) < 0)
        return;
    ml_gpu_copy(&ml->g, b, 0, &rb, 0, size);
    if (ml_gpu_run(&ml->g) >= 0 && ml_buf_invalidate(&ml->g, &rb) >= 0) {
        char *path = cb_asprintf("%s/%s.f32", dir, name);
        FILE *f = path ? fopen(path, "wb") : NULL;
        if (f) {
            fwrite(rb.map, 1, size, f);
            fclose(f);
        }
        free(path);
    }
    ml_buf_free(&ml->g, &rb);
}

/* (L - 1) / 2 + 1, three times: a chunk's tokens from its frames. */
static int chunk_tokens(int frames)
{
    for (int i = 0; i < 3; i++)
        frames = (frames - 1) / 2 + 1;
    return frames;
}

/* The audio to the adapter's rows (ml->audio) and the deepstack's
 * (ml->ds_out); returns the number of rows. */
static int encode(CBML *ml, const float *pcm, int64_t n, CBCancel *cancel)
{
    MLGpu *g = &ml->g;
    int ret;
    int nb_frames = (int)(n / HOP);
    int nb_chunks = (nb_frames + CHUNK - 1) / CHUNK;
    int tail = nb_frames - (nb_chunks - 1) * CHUNK;
    int tail_tok = chunk_tokens(tail);
    int rows = nb_chunks * CHUNK_TOK;
    int nb_audio = (nb_chunks - 1) * CHUNK_TOK + tail_tok;

    /* The samples go up through a staging buffer of their own, freed as
     * soon as the GPU has them: tens of MB that would otherwise stay
     * pinned between tracks. */
    MLBuf staging;
    if ((ret = reserve(ml, &ml->pcm, n * 4)) < 0 ||
        (ret = reserve(ml, &ml->mel, (size_t)nb_frames * N_MEL * 4)) < 0 ||
        (ret = reserve(ml, &ml->maxbits, 4)) < 0)
        return ret;
    if ((ret = ml_buf_create(g, &staging, n * 4, ML_MEM_UPLOAD)) < 0)
        return ret;
    memcpy(staging.map, pcm, n * 4);
    ml_buf_flush(g, &staging, n * 4);
    ml_gpu_copy(g, &staging, 0, &ml->pcm.b, 0, n * 4);
    ml_gpu_fill(g, &ml->maxbits.b, 0, 4, 0);
    ml_gpu_barrier(g);

    /* Whisper's features. */
    for (int f0 = 0; f0 < nb_frames; f0 += 8 * 32768) {
        MelPush mp = {
            .pcm = ml->pcm.b.addr, .window = ml->mel_window->addr, .basis = ml->basis,
            .filters = ml->mel_filters->addr, .dst = ml->mel.b.addr,
            .maxbits = ml->maxbits.b.addr,
            .nb_samples = (uint32_t)n, .nb_frames = (uint32_t)nb_frames, .frame0 = (uint32_t)f0,
        };
        int nb = CB_MIN(nb_frames - f0, 8 * 32768);
        ml_gpu_dispatch(g, ML_K_MEL, &mp, sizeof(mp), (nb + 7) / 8, 1, 1);
    }
    ml_gpu_barrier(g);
    MelNormPush np = {
        .mel = ml->mel.b.addr, .maxbits = ml->maxbits.b.addr,
        .count = (uint32_t)nb_frames * N_MEL,
    };
    dispatch_elems(ml, ML_K_MEL_NORM, &np, sizeof(np), (uint64_t)nb_frames * N_MEL);
    ml_gpu_barrier(g);
    ret = ml_gpu_run(g);
    ml_buf_free(g, &staging);
    if (ret < 0)
        return ret;
    dump(ml, "pcm", &ml->pcm.b, n * 4);
    dump(ml, "mel", &ml->mel.b, (size_t)nb_frames * N_MEL * 4);

    /* The convolutions and the stem, CONV_BATCH chunks at a time, into the
     * encoder's rows. */
    size_t xs = (size_t)rows * E_DIM * 4;
    if ((ret = reserve(ml, &ml->c1, (size_t)CONV_BATCH * 64 * 200 * C_DIM * 4)) < 0 ||
        (ret = reserve(ml, &ml->c2, (size_t)CONV_BATCH * 32 * 100 * C_DIM * 4)) < 0 ||
        (ret = reserve(ml, &ml->c3, (size_t)CONV_BATCH * 16 * 50 * C_DIM * 4)) < 0 ||
        (ret = reserve(ml, &ml->stem, (size_t)CONV_BATCH * 50 * C_DIM * 16 * 4)) < 0 ||
        (ret = reserve(ml, &ml->ex, xs)) < 0 || (ret = reserve(ml, &ml->eh, xs)) < 0 ||
        (ret = reserve(ml, &ml->eq, xs)) < 0 || (ret = reserve(ml, &ml->ek, xs)) < 0 ||
        (ret = reserve(ml, &ml->ev, xs)) < 0 || (ret = reserve(ml, &ml->eo, xs)) < 0 ||
        (ret = reserve(ml, &ml->ef, (size_t)rows * E_FFN * 4)) < 0)
        return ret;
    for (int i = 0; i < DEEPSTACK; i++) {
        if ((ret = reserve(ml, &ml->ds_x[i], xs)) < 0)
            return ret;
    }

    for (int c0 = 0; c0 < nb_chunks; c0 += CONV_BATCH) {
        int nb = CB_MIN(CONV_BATCH, nb_chunks - c0);
        Conv1Push cp = {
            .mel = ml->mel.b.addr, .weight = ml->conv1_w->addr, .bias = ml->conv1_b->addr,
            .dst = ml->c1.b.addr,
            .nb_frames = (uint32_t)nb_frames, .chunk0 = (uint32_t)c0, .nb_chunks = (uint32_t)nb,
        };
        dispatch_elems(ml, ML_K_CONV1, &cp, sizeof(cp), (uint64_t)nb * 64 * 200 * C_DIM);
        ml_gpu_barrier(g);

        GemmPush p = {
            .x = ml->c1.b.addr, .w = ml->conv_w[0].addr, .bias = ml->conv2_b->addr,
            .y = ml->c2.b.addr,
            .M = (uint32_t)nb * 32 * 100, .N = C_DIM, .K = 9 * C_DIM,
            .ldx = 0, .ldy = C_DIM,
            .flags = FLAG_BIAS | FLAG_GELU,
            .fin = 64, .tin = 200, .fout = 32, .tout = 100, .cin = C_DIM,
        };
        ml_gpu_dispatch(g, ML_K_GEMM_F16_CONV, &p, sizeof(p), (C_DIM + 127) / 128,
                        (p.M + 127) / 128, 1);
        ml_gpu_barrier(g);

        p.x = ml->c2.b.addr;
        p.w = ml->conv_w[1].addr;
        p.bias = ml->conv3_b->addr;
        p.y = ml->c3.b.addr;
        p.M = (uint32_t)nb * 16 * 50;
        p.fin = 32;
        p.tin = 100;
        p.fout = 16;
        p.tout = 50;
        ml_gpu_dispatch(g, ML_K_GEMM_F16_CONV, &p, sizeof(p), (C_DIM + 127) / 128,
                        (p.M + 127) / 128, 1);
        ml_gpu_barrier(g);

        StemPush sp = { .src = ml->c3.b.addr, .dst = ml->stem.b.addr, .nb_chunks = (uint32_t)nb };
        dispatch_elems(ml, ML_K_STEM_PERMUTE, &sp, sizeof(sp), (uint64_t)nb * 50 * 7680);
        ml_gpu_barrier(g);

        /* The stem's projection, plus the positions: x is filled with
         * them first and the projection added. */
        VkDeviceAddress xr = ml->ex.b.addr + (VkDeviceAddress)c0 * CHUNK_TOK * E_DIM * 4;
        ml_gpu_fill(g, &ml->ex.b, (VkDeviceSize)c0 * CHUNK_TOK * E_DIM * 4,
                    (VkDeviceSize)nb * CHUNK_TOK * E_DIM * 4, 0);
        ml_gpu_barrier(g);
        act(ml, ACT_ADD_TILE, xr, ml->posemb, 0, (uint64_t)nb * CHUNK_TOK * E_DIM, 0,
            CHUNK_TOK * E_DIM);
        ml_gpu_barrier(g);
        gemm(ml, ml->stem.b.addr, C_DIM * 16, ml->stem_w, ml->stem_b->addr, xr, E_DIM,
             (uint32_t)nb * CHUNK_TOK, FLAG_ADD);
        ml_gpu_barrier(g);
        if ((ret = ml_gpu_run(g)) < 0)
            return ret;
        if (cb_cancelled(cancel))
            return CB_ERROR(ECANCELED);
    }

    dump(ml, "stem", &ml->ex.b, xs);

    /* The transformer, over every chunk at once (attention stays inside
     * each). */
    VkDeviceAddress x = ml->ex.b.addr, h = ml->eh.b.addr;
    for (int i = 0; i < E_LAYERS; i++) {
        const EncLayer *l = &ml->enc[i];
        norm(ml, false, x, l->ln1_w, l->ln1_b, h, rows, E_DIM, 1e-5f);
        ml_gpu_barrier(g);
        gemm(ml, h, E_DIM, l->q_w, l->q_b->addr, ml->eq.b.addr, E_DIM, rows, 0);
        gemm(ml, h, E_DIM, l->k_w, 0, ml->ek.b.addr, E_DIM, rows, 0);
        gemm(ml, h, E_DIM, l->v_w, l->v_b->addr, ml->ev.b.addr, E_DIM, rows, 0);
        ml_gpu_barrier(g);
        EncAttnPush ap = {
            .q = ml->eq.b.addr, .k = ml->ek.b.addr, .v = ml->ev.b.addr, .o = ml->eo.b.addr,
            .nb_chunks = (uint32_t)nb_chunks, .tail = (uint32_t)tail_tok,
            .scale = 0.125f,
        };
        ml_gpu_dispatch(g, ML_K_ENC_ATTN, &ap, sizeof(ap), nb_chunks, E_HEADS, 1);
        ml_gpu_barrier(g);
        gemm(ml, ml->eo.b.addr, E_DIM, l->o_w, l->o_b->addr, x, E_DIM, rows, FLAG_ADD);
        ml_gpu_barrier(g);
        norm(ml, false, x, l->ln2_w, l->ln2_b, h, rows, E_DIM, 1e-5f);
        ml_gpu_barrier(g);
        gemm(ml, h, E_DIM, l->fc1_w, l->fc1_b->addr, ml->ef.b.addr, E_FFN, rows, FLAG_GELU);
        ml_gpu_barrier(g);
        gemm(ml, ml->ef.b.addr, E_FFN, l->fc2_w, l->fc2_b->addr, x, E_DIM, rows, FLAG_ADD);
        ml_gpu_barrier(g);
        for (int d = 0; d < DEEPSTACK; d++) {
            if (i == 8 * (d + 1)) {
                ml_gpu_copy(g, &ml->ex.b, 0, &ml->ds_x[d].b, 0, xs);
                ml_gpu_barrier(g);
            }
        }
        if (i == 0)
            dump(ml, "enc0", &ml->ex.b, xs);
        if ((i % 8) == 7) {
            if ((ret = ml_gpu_run(g)) < 0)
                return ret;
            if (cb_cancelled(cancel))
                return CB_ERROR(ECANCELED);
        }
    }
    dump(ml, "enc31", &ml->ex.b, xs);
    norm(ml, false, x, ml->enc_norm_w, ml->enc_norm_b, h, rows, E_DIM, 1e-5f);
    ml_gpu_barrier(g);

    /* The adapter and the deepstack's mergers, on the valid rows alone
     * (they are the first nb_audio: only the last chunk is short). */
    size_t as = (size_t)nb_audio * L_DIM * 4;
    if ((ret = reserve(ml, &ml->audio, as)) < 0)
        return ret;
    for (int d = 0; d < DEEPSTACK; d++) {
        if ((ret = reserve(ml, &ml->ds_out[d], as)) < 0)
            return ret;
    }
    if ((ret = mlp(ml, &ml->adapter, h, ml->audio.b.addr, nb_audio)) < 0)
        return ret;
    for (int d = 0; d < DEEPSTACK; d++) {
        if ((ret = mlp(ml, &ml->ds[d], ml->ds_x[d].b.addr, ml->ds_out[d].b.addr, nb_audio)) < 0)
            return ret;
    }
    if ((ret = ml_gpu_run(g)) < 0)
        return ret;
    dump(ml, "ds8", &ml->ds_x[0].b, xs);
    dump(ml, "adapter", &ml->audio.b, as);
    dump(ml, "deepstack0", &ml->ds_out[0].b, as);
    return nb_audio;
}

/* ---------------------------------------------------------------- decoder */

/* A run of one prompt's tokens through the decoder: m rows from `row` of a
 * pass, the tokens at positions [pos0, pos0 + m), whose keys and values go
 * to the cache's slots [slot0, slot0 + m). The prompt's keys before brk are
 * at their own slots (the head and the audio, which every question after
 * them shares); from brk on they are slot0 - pos0 slots further (the
 * question's own, which another question asked at the same time would
 * otherwise overwrite).
 *
 * A segment is what one decode() of the prompt alone would put through the
 * decoder at once: a chunk of at most LLM_CHUNK tokens. Its attention cuts
 * the keys as that would (attn_plan, from m and pos0 alone), and every
 * other kernel works row by row, so its rows come out the same, bit for
 * bit, whatever else is in the pass, provided the pass's products are of
 * the same kind: a pass of eight rows or fewer goes through the GEMV,
 * whose sums run in another order than the GEMM's, so a segment of eight
 * tokens or fewer only shares a pass with others like it. */
typedef struct Segment {
    int row, m, pos0, slot0, brk;
} Segment;

/* How attention cuts the keys of m queries at [pos0, pos0 + m), and the
 * bytes of partial results it needs: flash decoding (split over the keys)
 * for one or two tokens; flash attention for more, the keys split until
 * there are some 256 workgroups, each with 256 keys or more, or for a
 * handful of queries, whose 64-row tiles are mostly empty, 1024
 * workgroups of 128 keys. */
typedef struct AttnPlan {
    bool dec;
    uint32_t nsplit, split_len;
    size_t part;
} AttnPlan;

static AttnPlan attn_plan(uint32_t m, uint32_t pos0)
{
    AttnPlan a = { 0 };
    if (m * (L_HEADS / L_KV) <= ATTN_DEC_ROWS) {
        a.dec = true;
        a.nsplit = (pos0 + m + ATTN_SPLIT - 1) / ATTN_SPLIT;
        a.part = (size_t)L_KV * a.nsplit * ATTN_DEC_ROWS * ATTN_PART * 4;
        return a;
    }
    uint32_t qblocks = (m + 63) / 64, nkeys = pos0 + m;
    uint32_t want = m <= 16 ? 1024 : 256, least = m <= 16 ? 128 : 256;
    uint32_t nsplit = CB_MAX(1, want / (qblocks * L_HEADS));
    nsplit = CB_MIN(nsplit, CB_MAX(1, nkeys / least));
    nsplit = CB_MIN(nsplit, ATTN_MAX_PARTS / m);
    nsplit = CB_MAX(nsplit, 1);
    a.split_len = CB_ALIGN((nkeys + nsplit - 1) / nsplit, 32);
    a.nsplit = (nkeys + a.split_len - 1) / a.split_len;
    a.part = a.nsplit > 1 ? (size_t)a.nsplit * m * L_HEADS * ATTN_PART * 4 : 0;
    return a;
}

/* The partial results the segments' attention needs, one after another. */
static size_t attn_part(const Segment *segs, int nb)
{
    size_t part = 0;
    for (int s = 0; s < nb; s++)
        part += attn_plan((uint32_t)segs[s].m, (uint32_t)segs[s].pos0).part;
    return part;
}

/* Layer i's attention for the segments' queries (in ml->q, after qk_rope)
 * over the cache, into ml->o: every segment's parts, then their merges. */
static void attention(CBML *ml, int i, const Segment *segs, int nb)
{
    size_t layer_cache = (size_t)L_KV * ml->max_ctx * L_HEAD * 2;
    VkDeviceAddress kc = ml->kc.addr + i * layer_cache, vc = ml->vc.addr + i * layer_cache;
    float scale = (float)(1.0 / sqrt((double)L_HEAD));
    VkDeviceSize row_bytes = (VkDeviceSize)L_HEADS * L_HEAD * 4;

    for (int phase = 0; phase < 2; phase++) {
        VkDeviceAddress part = ml->part.b.addr;
        bool any = false;
        for (int s = 0; s < nb; s++) {
            const Segment *sg = &segs[s];
            uint32_t m = (uint32_t)sg->m, pos0 = (uint32_t)sg->pos0;
            AttnPlan a = attn_plan(m, pos0);
            VkDeviceAddress q = ml->q.b.addr + sg->row * row_bytes;
            VkDeviceAddress o = ml->o.b.addr + sg->row * row_bytes;
            uint32_t brk = (uint32_t)sg->brk, shift = (uint32_t)(sg->slot0 - sg->pos0);
            if (a.dec) {
                AttnDecPush p = {
                    .q = q, .o = o, .kc = kc, .vc = vc, .part = part,
                    .nq = m, .pos0 = pos0, .max_ctx = (uint32_t)ml->max_ctx,
                    .nb_q = L_HEADS, .nb_kv = L_KV, .nsplit = a.nsplit,
                    .scale = scale, .brk = brk, .shift = shift,
                };
                if (!phase)
                    ml_gpu_dispatch(&ml->g, ML_K_ATTN_DEC, &p, sizeof(p), a.nsplit, L_KV, 1);
                else
                    ml_gpu_dispatch(&ml->g, ML_K_ATTN_COMB, &p, sizeof(p), m * (L_HEADS / L_KV),
                                    L_KV, 1);
                any = true;
            } else if (!phase || a.nsplit > 1) {
                AttnPush p = {
                    .q = q, .o = o, .kc = kc, .vc = vc, .part = part,
                    .nq = m, .pos0 = pos0, .max_ctx = (uint32_t)ml->max_ctx,
                    .nb_q = L_HEADS, .nb_kv = L_KV, .nsplit = a.nsplit, .split_len = a.split_len,
                    .scale = scale, .brk = brk, .shift = shift,
                };
                /* A segment that would leave half of its last block of 64
                 * queries empty goes in blocks of 32 (the same sums). */
                bool b32 = m % 64 && m % 64 <= 32;
                uint32_t rows = b32 ? 32 : 64;
                if (!phase)
                    ml_gpu_dispatch(&ml->g, b32 ? ML_K_ATTN_32 : ML_K_ATTN, &p, sizeof(p),
                                    (m + rows - 1) / rows, L_HEADS, a.nsplit);
                else
                    ml_gpu_dispatch(&ml->g, ML_K_ATTN_FCOMB, &p, sizeof(p), m, L_HEADS, 1);
                any = true;
            }
            part += a.part;
        }
        if (any)
            ml_gpu_barrier(&ml->g);
    }
}

/* The decoder's scratch for passes of `rows` rows, `part` bytes of
 * attention's partial results and `logits` rows of logits (and as many
 * next tokens). */
static int reserve_decoder(CBML *ml, int rows, size_t part, int logits)
{
    int ret;
    if ((ret = reserve(ml, &ml->x, (size_t)rows * L_DIM * 4)) < 0 ||
        (ret = reserve(ml, &ml->h, (size_t)rows * L_DIM * 4)) < 0 ||
        (ret = reserve(ml, &ml->q, (size_t)rows * L_HEADS * L_HEAD * 4)) < 0 ||
        (ret = reserve(ml, &ml->k, (size_t)rows * L_KV * L_HEAD * 4)) < 0 ||
        (ret = reserve(ml, &ml->v, (size_t)rows * L_KV * L_HEAD * 4)) < 0 ||
        (ret = reserve(ml, &ml->o, (size_t)rows * L_HEADS * L_HEAD * 4)) < 0 ||
        (ret = reserve(ml, &ml->fg, (size_t)rows * L_FFN * 4)) < 0 ||
        (ret = reserve(ml, &ml->fu, (size_t)rows * L_FFN * 4)) < 0 ||
        (ret = reserve(ml, &ml->logits, (size_t)VOCAB * CB_MAX(logits, 8) * 4)) < 0 ||
        (ret = reserve(ml, &ml->ids, (size_t)ml->max_ctx * 4)) < 0 ||
        (ret = reserve(ml, &ml->aidx, (size_t)ml->max_ctx * 4)) < 0 ||
        (ret = reserve(ml, &ml->pos, (size_t)ml->max_ctx * 8)) < 0 ||
        (ret = reserve(ml, &ml->next, (size_t)CB_MAX(logits, 8) * 4)) < 0 ||
        (ret = reserve(ml, &ml->part, CB_MAX(part, 4))) < 0)
        return ret;
    return 0;
}

/* A pass's rows on the GPU: their tokens, the audio row each stands for
 * (or -1), and their (position, slot) pairs, or 0 for the one segment's
 * own positions. */
typedef struct Rows {
    VkDeviceAddress ids, aidx, pos;
    int n;
    bool audio;                 /* some row is audio: the deepstack's rows go in */
} Rows;

/* The rows, every segment's, through the decoder and into the cache; then
 * the logits of rows lrows[0 .. nl) into ml->logits, rows [out0, out0 +
 * nl). With no logits wanted, the last layer goes only as far as the
 * cache: what comes after its keys and values is read by nothing but the
 * head. Recorded, not run. */
static void pass(CBML *ml, const Rows *r, const Segment *segs, int nb_segs, const int *lrows,
                 int nl, int out0)
{
    MLGpu *g = &ml->g;
    uint32_t m = (uint32_t)r->n;
    size_t layer_cache = (size_t)L_KV * ml->max_ctx * L_HEAD * 2;
    VkDeviceAddress x = ml->x.b.addr, h = ml->h.b.addr;
    EmbedPush ep = {
        .table = ml->embed->addr,
        .ids = r->ids,
        .audio_idx = r->aidx,
        .audio = ml->audio.b.addr,
        .dst = x,
        .n = L_DIM,
    };
    ml_gpu_dispatch(g, ML_K_EMBED, &ep, sizeof(ep), m, 1, 1);
    ml_gpu_barrier(g);

    for (int i = 0; i < L_LAYERS; i++) {
        const LLMLayer *l = &ml->llm[i];
        norm(ml, true, x, l->attn_norm, NULL, h, m, L_DIM, 1e-6f);
        ml_gpu_barrier(g);
        gemm(ml, h, L_DIM, l->q, 0, ml->q.b.addr, L_HEADS * L_HEAD, m, 0);
        gemm(ml, h, L_DIM, l->k, 0, ml->k.b.addr, L_KV * L_HEAD, m, 0);
        gemm(ml, h, L_DIM, l->v, 0, ml->v.b.addr, L_KV * L_HEAD, m, 0);
        ml_gpu_barrier(g);
        RopePush rp = {
            .q = ml->q.b.addr, .k = ml->k.b.addr, .v = ml->v.b.addr,
            .qw = l->q_norm->addr, .kw = l->k_norm->addr, .rope = ml->rope,
            .kc = ml->kc.addr + i * layer_cache, .vc = ml->vc.addr + i * layer_cache,
            .pos = r->pos,
            .pos0 = (uint32_t)segs[0].pos0, .max_ctx = (uint32_t)ml->max_ctx,
            .nb_q = L_HEADS, .nb_kv = L_KV,
            .eps = 1e-6f,
        };
        ml_gpu_dispatch(g, ML_K_QK_ROPE, &rp, sizeof(rp), m, 1, 1);
        ml_gpu_barrier(g);
        if (!nl && i == L_LAYERS - 1)
            return;
        attention(ml, i, segs, nb_segs);
        gemm(ml, ml->o.b.addr, L_HEADS * L_HEAD, l->o, 0, x, L_DIM, m, FLAG_ADD);
        ml_gpu_barrier(g);
        norm(ml, true, x, l->ffn_norm, NULL, h, m, L_DIM, 1e-6f);
        ml_gpu_barrier(g);
        gemm(ml, h, L_DIM, l->gate, 0, ml->fg.b.addr, L_FFN, m, 0);
        gemm(ml, h, L_DIM, l->up, 0, ml->fu.b.addr, L_FFN, m, 0);
        ml_gpu_barrier(g);
        act(ml, ACT_SWIGLU, ml->fg.b.addr, ml->fu.b.addr, 0, (uint64_t)m * L_FFN, 0, 0);
        ml_gpu_barrier(g);
        gemm(ml, ml->fg.b.addr, L_FFN, l->down, 0, x, L_DIM, m, FLAG_ADD);
        ml_gpu_barrier(g);
        /* MOSS-Audio's deepstack hooks: after layer i < 3, the audio
         * tokens' rows get the i-th merger's rows. */
        if (r->audio && i < DEEPSTACK) {
            act(ml, ACT_ADD_ROWS, x, ml->ds_out[i].b.addr, r->aidx, (uint64_t)m * L_DIM, L_DIM, 0);
            ml_gpu_barrier(g);
        }
    }

    /* The final norm of the rows whose logits are wanted, gathered, then
     * the head: eight rows at a time, the most the GEMV takes. */
    if (!nl)
        return;
    for (int j = 0; j < nl; j++)
        norm(ml, true, x + (VkDeviceAddress)lrows[j] * L_DIM * 4, ml->final_norm, NULL,
             h + (VkDeviceAddress)j * L_DIM * 4, 1, L_DIM, 1e-6f);
    ml_gpu_barrier(g);
    for (int j = 0; j < nl; j += 8)
        gemm(ml, h + (VkDeviceAddress)j * L_DIM * 4, L_DIM, ml->lm_head, 0,
             ml->logits.b.addr + (VkDeviceAddress)(out0 + j) * VOCAB * 4, VOCAB,
             (uint32_t)CB_MIN(8, nl - j), 0);
    ml_gpu_barrier(g);
}

/* Tokens ids[0 .. n) at positions [pos0, pos0 + n) through the decoder, into
 * the cache; aidx[i] >= 0 puts that audio row at token i instead of its
 * embedding (and adds the deepstack's rows after the first layers). The
 * logits of the last nb_logits tokens (at most 8, and n when n is no more
 * than LLM_CHUNK; 0: none) end in ml->logits, [nb_logits][VOCAB]. The
 * tokens are read in chunks of LLM_CHUNK, a segment each, up to PASS_ROWS
 * a pass. */
static int decode(CBML *ml, const int32_t *ids, const int32_t *aidx, int n, int pos0,
                  int nb_logits, CBCancel *cancel)
{
    MLGpu *g = &ml->g;
    int ret;
    if (pos0 + n > ml->max_ctx)
        return CB_ERROR(E2BIG);

    int step = CB_MIN(n, LLM_CHUNK), per_pass = CB_MAX(ml->pass_rows / LLM_CHUNK, 1);
    size_t part = CB_MAX((size_t)L_KV * (ml->max_ctx / ATTN_SPLIT + 1) * ATTN_DEC_ROWS,
                         (size_t)ATTN_MAX_PARTS * L_HEADS) * ATTN_PART * 4;
    if ((ret = reserve_decoder(ml, CB_MIN(n, step * per_pass), part, 8)) < 0)
        return ret;

    int32_t *up = ml->up.map;
    memcpy(up, ids, n * 4);
    bool audio = false;
    for (int i = 0; i < n; i++) {
        up[ml->max_ctx + i] = aidx ? aidx[i] : -1;
        audio |= aidx && aidx[i] >= 0;
    }
    ml_buf_flush(g, &ml->up, (size_t)ml->max_ctx * 8);
    ml_gpu_copy(g, &ml->up, 0, &ml->ids.b, 0, n * 4);
    ml_gpu_copy(g, &ml->up, (size_t)ml->max_ctx * 4, &ml->aidx.b, 0, n * 4);
    ml_gpu_barrier(g);

    for (int p0 = 0; p0 < n; p0 += step * per_pass) {
        Segment segs[PASS_ROWS / LLM_CHUNK + 1];
        int ns = 0, rows = 0, last = 0;
        for (int c0 = p0; c0 < n && ns < per_pass; c0 += step) {
            last = CB_MIN(step, n - c0);
            segs[ns++] = (Segment) {
                .row = rows, .m = last, .pos0 = pos0 + c0, .slot0 = pos0 + c0,
            };
            rows += last;
        }
        Rows r = {
            .ids = ml->ids.b.addr + (VkDeviceAddress)p0 * 4,
            .aidx = ml->aidx.b.addr + (VkDeviceAddress)p0 * 4,
            .n = rows,
            .audio = audio,
        };
        int lrows[8], nl = 0;
        if (p0 + rows == n && nb_logits > 0) {
            nl = CB_CLAMP(nb_logits, 1, CB_MIN(8, last));
            for (int j = 0; j < nl; j++)
                lrows[j] = rows - nl + j;
        }
        pass(ml, &r, segs, ns, lrows, nl, 0);
        if ((ret = ml_gpu_run(g)) < 0)
            return ret;
        if (cb_cancelled(cancel))
            return CB_ERROR(ECANCELED);
    }
    return 0;
}

/* --------------------------------------------------------------- prompts */

/* A prompt: token ids, and for each the audio row it stands for, or -1. */
typedef struct Tokens {
    int32_t *ids, *aidx;
    int nb, cap;
} Tokens;

static int tok_push(Tokens *t, int32_t id, int32_t a)
{
    if (t->nb == t->cap) {
        int c = CB_MAX(t->cap * 2, 256);
        int32_t *ids = realloc(t->ids, c * sizeof(*ids));
        if (!ids)
            return CB_ERROR(ENOMEM);
        t->ids = ids;
        int32_t *aidx = realloc(t->aidx, c * sizeof(*aidx));
        if (!aidx)
            return CB_ERROR(ENOMEM);
        t->aidx = aidx;
        t->cap = c;
    }
    t->ids[t->nb] = id;
    t->aidx[t->nb] = a;
    t->nb++;
    return 0;
}

static int tok_text(CBML *ml, Tokens *t, const char *text)
{
    int32_t *ids = NULL;
    int nb = 0, cap = 0;
    int ret = ml_tok_encode(ml->tok, text, &ids, &nb, &cap);
    for (int i = 0; i < nb && ret >= 0; i++)
        ret = tok_push(t, ids[i], -1);
    free(ids);
    return ret;
}

static void tok_free(Tokens *t)
{
    free(t->ids);
    free(t->aidx);
    *t = (Tokens) { 0 };
}

/* The audio's tokens as the processor lays them out with time markers
 * (_build_audio_tokens_with_time_markers): after every 25 audio tokens (2
 * s), the digits of the seconds so far. */
static int tok_audio(Tokens *t, int nb_audio)
{
    int full = (int)(nb_audio / 12.5);
    int done = 0, ret = 0;
    for (int sec = 2; sec <= full && ret >= 0; sec += 2) {
        int mark = sec / 2 * 25;
        while (done < mark && ret >= 0) {
            ret = tok_push(t, TOK_AUDIO, done);
            done++;
        }
        char digits[16];
        snprintf(digits, sizeof(digits), "%d", sec);
        for (int i = 0; digits[i] && ret >= 0; i++)
            ret = tok_push(t, TOK_DIGIT0 + (digits[i] - '0'), -1);
    }
    while (done < nb_audio && ret >= 0) {
        ret = tok_push(t, TOK_AUDIO, done);
        done++;
    }
    return ret;
}

/* "<|im_start|>system\nYou are a helpful assistant.<|im_end|>\n
 * <|im_start|>user\n" (the processor's _build_default_prompt). */
static int tok_head(CBML *ml, Tokens *t)
{
    int ret;
    if ((ret = tok_push(t, TOK_IM_START, -1)) < 0 ||
        (ret = tok_text(ml, t, "system\nYou are a helpful assistant.")) < 0 ||
        (ret = tok_push(t, TOK_IM_END, -1)) < 0 || (ret = tok_text(ml, t, "\n")) < 0 ||
        (ret = tok_push(t, TOK_IM_START, -1)) < 0 || (ret = tok_text(ml, t, "user\n")) < 0)
        return ret;
    return 0;
}

/* The prompt's tokens around the audio (tok_head and the audio's
 * delimiters, with room to spare), and what a question and its answer are
 * left at least. */
#define HEAD_TOKENS   32
#define QUESTION_ROOM 256

/* The tokens n samples of audio take in the prompt: one per 8 mel frames
 * (chunk_tokens over the chunks), and the time markers' digits. */
static int audio_prompt_tokens(int64_t n)
{
    int64_t frames = n / HOP;
    int nb = (int)((frames + 7) / 8);
    int full = (int)(nb / 12.5), tokens = nb;
    for (int sec = 2; sec <= full; sec += 2)
        tokens += sec < 10 ? 1 : sec < 100 ? 2 : sec < 1000 ? 3 : 4;
    return tokens;
}

int ml_max_context(const CBML *ml)
{
    return ml->max_ctx;
}

const MLStats *ml_stats(const CBML *ml)
{
    return &ml->stats;
}

void ml_set_listen(CBML *ml, int pass_rows, bool logits)
{
    ml->pass_rows = CB_CLAMP(pass_rows, LLM_CHUNK, PASS_ROWS);
    ml->listen_logits = logits;
}

int ml_listen(CBML *ml, const float *pcm, int64_t n, int room, CBCancel *cancel)
{
    int64_t t0 = cb_time_us();
    ml->nb_prefix = 0;
    ml->nb_audio = 0;
    room = CB_MAX(room, QUESTION_ROOM);

    /* What fits: the head, the audio and its markers, and room for the
     * questions and their answers; whole seconds are cut from the end
     * until it does. */
    int64_t fit = n;
    while (fit > CB_ML_RATE && HEAD_TOKENS + audio_prompt_tokens(fit) + room > ml->max_ctx)
        fit -= CB_ML_RATE;
    if (fit < n) {
        cb_debug("ml: %.1f s of audio, %.1f s fit in the context beside %d tokens of questions",
                 n / (double)CB_ML_RATE, fit / (double)CB_ML_RATE, room);
        n = fit;
    }
    if (n < N_FFT || HEAD_TOKENS + audio_prompt_tokens(n) + room > ml->max_ctx)
        return CB_ERROR(E2BIG);

    Tokens t = { 0 };
    uint64_t peak = 0;
    ml->stats = (MLStats) { 0 };
    free_decoder_scratch(ml);
    int ret = encode(ml, pcm, n, cancel);
    if (ret < 0)
        goto end;
    int nb_audio = ret;
    int64_t t1 = cb_time_us();
    ml->stats.encode_ms = (t1 - t0) * 1e-3;
    peak = ml_gpu_allocated(&ml->g);
    free_encoder_scratch(ml);
    if ((ret = tok_head(ml, &t)) < 0 || (ret = tok_push(&t, TOK_AUDIO_BOS, -1)) < 0 ||
        (ret = tok_audio(&t, nb_audio)) < 0 || (ret = tok_push(&t, TOK_AUDIO_EOS, -1)) < 0)
        goto end;
    if (t.nb + room > ml->max_ctx) {
        ret = CB_ERROR(E2BIG);
        goto end;
    }
    if ((ret = decode(ml, t.ids, t.aidx, t.nb, 0, ml->listen_logits, cancel)) < 0)
        goto end;
    ml->nb_prefix = t.nb;
    ml->nb_audio = nb_audio;
    ml->stats.prefill_ms = (cb_time_us() - t1) * 1e-3;
    ml->stats.prefill_tokens = t.nb;

end:
    tok_free(&t);
    if (ret < 0)
        ml_gpu_discard(&ml->g);
    uint64_t during = CB_MAX(peak, ml_gpu_allocated(&ml->g));
    free_audio_scratch(ml);
    cb_debug("ml: %.2f GB on the GPU while listening, %.2f GB after", during * 1e-9,
             ml_gpu_allocated(&ml->g) * 1e-9);
    ml->listen_ms = (cb_time_us() - t0) * 1e-3;
    ml->ask_ms = 0.0;
    return ret;
}

int cb_ml_listen(CBML *ml, const float *pcm, int64_t n, CBCancel *cancel)
{
    return ml_listen(ml, pcm, n, QUESTION_ROOM, cancel);
}

void cb_ml_forget(CBML *ml)
{
    ml->nb_prefix = 0;
    ml->nb_audio = 0;
}

/* The question's tokens, after the cached head and audio (or after a head
 * of its own when there is none). */
static int tok_question(CBML *ml, Tokens *t, const char *question, const char *prefix,
                        bool after_audio)
{
    int ret;
    if (!after_audio && (ret = tok_head(ml, t)) < 0)
        return ret;
    char *q = cb_asprintf("%s%s", after_audio ? "\n" : "", question);
    if (!q)
        return CB_ERROR(ENOMEM);
    ret = tok_text(ml, t, q);
    free(q);
    if (ret < 0 || (ret = tok_push(t, TOK_IM_END, -1)) < 0 || (ret = tok_text(ml, t, "\n")) < 0 ||
        (ret = tok_push(t, TOK_IM_START, -1)) < 0 || (ret = tok_text(ml, t, "assistant\n")) < 0)
        return ret;
    if (prefix && prefix[0])
        ret = tok_text(ml, t, prefix);
    return ret;
}

int ml_question_tokens(CBML *ml, const char *question, const char *prefix)
{
    Tokens t = { 0 };
    int ret = tok_question(ml, &t, question, prefix, true);
    if (ret >= 0)
        ret = t.nb;
    tok_free(&t);
    return ret;
}

/* ------------------------------------------------------------- questions */

/* Questions asked together (ml_ask_many) go through the decoder together:
 * the tag's eighteen questions of 15 to 80 tokens each are, one at a time,
 * too few rows for the GEMM to keep the GPU busy (a fifth of its speed on
 * a prefill), and together a pass of some 800. Each question keeps slots
 * of its own in the cache past the audio, for its tokens and its answer's;
 * its rows come out as they would asked alone (Segment says why). */

/* Rows of a pass of questions at most: the scratch of the decoder grows
 * to it. */
#define BATCH_ROWS 1024

static bool is_end(int32_t tok)
{
    return tok == TOK_IM_END || tok == TOK_ENDOFTEXT;
}

static int reserve_down(CBML *ml, size_t size)
{
    if (ml->down.size >= size)
        return 0;
    ml_buf_free(&ml->g, &ml->down);
    return ml_buf_create(&ml->g, &ml->down, size, ML_MEM_READBACK);
}

/* The rows of a batch, laid out pass after pass: for each, its segment, the
 * question it is of, and whether it is that question's last. */
typedef struct Plan {
    Segment *segs;
    int *query;
    int nb_segs, nb_rows;
    int (*passes)[2];           /* first segment, segments */
    int nb_passes;
} Plan;

static void plan_free(Plan *p)
{
    free(p->segs);
    free(p->query);
    free(p->passes);
    *p = (Plan) { 0 };
}

/* The questions' segments, cut as decode() would cut each (chunks of
 * LLM_CHUNK tokens), in rounds: every question's first chunk, then every
 * second chunk, which attends to the first through the cache. In a round,
 * the segments of more than eight tokens go into passes of up to
 * BATCH_ROWS rows, the others into passes of up to eight. */
static int plan_batch(Plan *p, const Tokens *t, const int *slot, int nb, int prefix)
{
    *p = (Plan) { 0 };
    int rounds = 0, cap = 0;
    for (int i = 0; i < nb; i++) {
        int c = (t[i].nb + LLM_CHUNK - 1) / LLM_CHUNK;
        rounds = CB_MAX(rounds, c);
        cap += c;
    }
    p->segs = calloc(CB_MAX(cap, 1), sizeof(*p->segs));
    p->query = calloc(CB_MAX(cap, 1), sizeof(*p->query));
    p->passes = calloc(CB_MAX(cap, 1), sizeof(*p->passes));
    if (!p->segs || !p->query || !p->passes) {
        plan_free(p);
        return CB_ERROR(ENOMEM);
    }

    for (int r = 0; r < rounds; r++) {
        for (int few = 0; few < 2; few++) {
            int limit = few ? 8 : BATCH_ROWS, rows = 0;
            bool open = false;
            for (int i = 0; i < nb; i++) {
                int c0 = r * LLM_CHUNK;
                if (c0 >= t[i].nb)
                    continue;
                int m = CB_MIN(LLM_CHUNK, t[i].nb - c0);
                if ((m <= 8) != few)
                    continue;
                if (!open || rows + m > limit) {
                    p->passes[p->nb_passes][0] = p->nb_segs;
                    p->passes[p->nb_passes][1] = 0;
                    p->nb_passes++;
                    rows = 0;
                    open = true;
                }
                p->segs[p->nb_segs] = (Segment) {
                    .row = rows, .m = m, .pos0 = prefix + c0, .slot0 = slot[i] + c0,
                    .brk = prefix,
                };
                p->query[p->nb_segs] = i;
                p->nb_segs++;
                p->passes[p->nb_passes - 1][1]++;
                rows += m;
                p->nb_rows += m;
            }
        }
    }
    return 0;
}

/* An answer being generated. */
typedef struct Gen {
    MLQuery *q;
    int32_t tok;                /* the next token, from the last logits */
    int i, pos, slot;
    char *out;
    size_t len, cap;
} Gen;

/* nb questions whose slots in the cache, from slot[i], do not overlap:
 * read, then answered a token at a time, every answer of the batch at
 * once. */
static int ask_batch(CBML *ml, MLQuery *qs, const Tokens *t, const int *slot, int nb,
                     CBCancel *cancel)
{
    MLGpu *g = &ml->g;
    int prefix = ml->nb_prefix, ret;
    int64_t t0 = cb_time_us();
    Plan p;
    Gen *gen = NULL;
    int *lidx = NULL;
    if ((ret = plan_batch(&p, t, slot, nb, prefix)) < 0)
        return ret;

    int max_rows = 0, nb_gen = 0, nb_scored = 0;
    size_t part = 0;
    for (int k = 0; k < p.nb_passes; k++) {
        const Segment *s = p.segs + p.passes[k][0];
        int n = 0;
        for (int j = 0; j < p.passes[k][1]; j++)
            n += s[j].m;
        max_rows = CB_MAX(max_rows, n);
        part = CB_MAX(part, attn_part(s, p.passes[k][1]));
    }
    for (int i = 0; i < nb; i++) {
        nb_gen += qs[i].answer_wanted;
        nb_scored += !!qs[i].logits;
    }
    /* The answers' steps: a token each, eight at most in a pass. */
    Segment worst = { .m = 1, .pos0 = ml->max_ctx - 1 };
    part = CB_MAX(part, 8 * attn_part(&worst, 1));
    if ((ret = reserve_decoder(ml, CB_MAX(max_rows, 8), part, nb)) < 0 ||
        (ret = reserve_down(ml, (size_t)CB_MAX(nb_scored, 1) * VOCAB * 4)) < 0)
        goto end;
    if (!(lidx = calloc(nb, sizeof(*lidx))) || !(gen = calloc(CB_MAX(nb_gen, 1), sizeof(*gen)))) {
        ret = CB_ERROR(ENOMEM);
        goto end;
    }

    /* Every row's token and (position, slot), pass after pass. */
    int32_t *up = ml->up.map;
    for (int s = 0, row = 0; s < p.nb_segs; s++) {
        const Segment *sg = &p.segs[s];
        const Tokens *qt = &t[p.query[s]];
        int c0 = sg->pos0 - prefix;
        for (int j = 0; j < sg->m; j++, row++) {
            up[row] = qt->ids[c0 + j];
            up[ml->max_ctx + row] = -1;
            up[2 * ml->max_ctx + 2 * row] = sg->pos0 + j;
            up[2 * ml->max_ctx + 2 * row + 1] = sg->slot0 + j;
        }
    }
    ml_buf_flush(g, &ml->up, (size_t)ml->max_ctx * 16);
    ml_gpu_copy(g, &ml->up, 0, &ml->ids.b, 0, (size_t)p.nb_rows * 4);
    ml_gpu_copy(g, &ml->up, (size_t)ml->max_ctx * 4, &ml->aidx.b, 0, (size_t)p.nb_rows * 4);
    ml_gpu_copy(g, &ml->up, (size_t)ml->max_ctx * 8, &ml->pos.b, 0, (size_t)p.nb_rows * 8);
    ml_gpu_barrier(g);

    /* The questions, the logits of each at its last token in the order
     * they come. */
    int nl = 0, row0 = 0;
    for (int k = 0; k < p.nb_passes; k++) {
        const Segment *s = p.segs + p.passes[k][0];
        const int *sq = p.query + p.passes[k][0];
        int ns = p.passes[k][1], n = 0, lrows[BATCH_ROWS], nl0 = nl, cnt = 0;
        for (int j = 0; j < ns; j++) {
            n += s[j].m;
            if (s[j].pos0 - prefix + s[j].m == t[sq[j]].nb) {
                lrows[cnt++] = s[j].row + s[j].m - 1;
                lidx[sq[j]] = nl++;
            }
        }
        Rows r = {
            .ids = ml->ids.b.addr + (VkDeviceAddress)row0 * 4,
            .aidx = ml->aidx.b.addr + (VkDeviceAddress)row0 * 4,
            .pos = ml->pos.b.addr + (VkDeviceAddress)row0 * 8,
            .n = n,
        };
        pass(ml, &r, s, ns, lrows, cnt, nl0);
        if ((ret = ml_gpu_run(g)) < 0)
            goto end;
        if (cb_cancelled(cancel)) {
            ret = CB_ERROR(ECANCELED);
            goto end;
        }
        row0 += n;
    }
    ml->stats.read_tokens += p.nb_rows;

    /* The scored questions' logits, and the answers' first tokens. */
    int ng = 0;
    for (int i = 0; i < nb; i++) {
        if (qs[i].logits)
            ml_gpu_copy(g, &ml->logits.b, (VkDeviceSize)lidx[i] * VOCAB * 4, &ml->down,
                        (VkDeviceSize)lidx[i] * VOCAB * 4, (VkDeviceSize)VOCAB * 4);
    }
    if ((ret = ml_gpu_run(g)) < 0 || (ret = ml_buf_invalidate(g, &ml->down)) < 0)
        goto end;
    for (int i = 0; i < nb; i++) {
        if (qs[i].logits)
            memcpy(qs[i].logits, (const float *)ml->down.map + (size_t)lidx[i] * VOCAB,
                   (size_t)VOCAB * 4);
        if (!qs[i].answer_wanted)
            continue;
        Gen *ge = &gen[ng++];
        ge->q = &qs[i];
        ge->pos = prefix + t[i].nb;
        ge->slot = slot[i] + t[i].nb;
        ge->cap = 256;
        if (!(ge->out = calloc(1, ge->cap))) {
            ret = CB_ERROR(ENOMEM);
            goto end;
        }
    }
    int64_t t1 = cb_time_us();
    ml->stats.read_ms += (t1 - t0) * 1e-3;
    if (!nb_gen)
        goto end;

    /* The answers, greedily, as cb_ml_ask's loop would make each alone, a
     * step at a time for all of them: an answer's token through the
     * decoder is a segment of one row, eight to a pass (a pass's GEMV
     * takes no more). act: the answers whose next token is in ml->next,
     * in its order. */
    int *act = calloc(nb_gen, sizeof(*act)), nact = 0;
    if (!act) {
        ret = CB_ERROR(ENOMEM);
        goto end;
    }
    for (int k = 0; k < nb_gen; k++) {
        if (gen[k].q->max_tokens <= 0)
            continue;
        ArgmaxPush ap = {
            .logits = ml->logits.b.addr + (VkDeviceAddress)lidx[gen[k].q - qs] * VOCAB * 4,
            .dst = ml->next.b.addr + (VkDeviceAddress)nact * 4,
            .n = VOCAB,
        };
        ml_gpu_dispatch(g, ML_K_ARGMAX, &ap, sizeof(ap), 1, 1, 1);
        act[nact++] = k;
    }
    while (nact) {
        ml_gpu_barrier(g);
        ml_gpu_copy(g, &ml->next.b, 0, &ml->down, 0, (VkDeviceSize)nact * 4);
        if ((ret = ml_gpu_run(g)) < 0 || (ret = ml_buf_invalidate(g, &ml->down)) < 0)
            break;
        const int32_t *toks = ml->down.map;
        for (int j = 0; j < nact; j++)
            gen[act[j]].tok = toks[j];
        if (cb_cancelled(cancel)) {
            ret = CB_ERROR(ECANCELED);
            break;
        }

        /* Each answer's token, and whether it goes on. */
        int nn = 0;
        for (int j = 0; j < nact && ret >= 0; j++) {
            Gen *ge = &gen[act[j]];
            if (is_end(ge->tok))
                continue;
            if ((ret = ml_tok_decode(ml->tok, ge->tok, &ge->out, &ge->len, &ge->cap)) < 0)
                break;
            ml->stats.answer_tokens++;
            if (ge->i + 1 == ge->q->max_tokens || ge->pos + 1 >= ml->max_ctx)
                continue;
            if (ge->q->settled && ge->q->settled(ge->out))
                continue;
            act[nn++] = act[j];
        }
        if (ret < 0 || !nn)
            break;

        for (int j = 0; j < nn; j++) {
            const Gen *ge = &gen[act[j]];
            up[j] = ge->tok;
            up[ml->max_ctx + j] = -1;
            up[2 * ml->max_ctx + 2 * j] = ge->pos;
            up[2 * ml->max_ctx + 2 * j + 1] = ge->slot;
        }
        ml_buf_flush(g, &ml->up, (size_t)ml->max_ctx * 16);
        ml_gpu_copy(g, &ml->up, 0, &ml->ids.b, 0, (size_t)nn * 4);
        ml_gpu_copy(g, &ml->up, (size_t)ml->max_ctx * 4, &ml->aidx.b, 0, (size_t)nn * 4);
        ml_gpu_copy(g, &ml->up, (size_t)ml->max_ctx * 8, &ml->pos.b, 0, (size_t)nn * 8);
        ml_gpu_barrier(g);
        for (int p0 = 0; p0 < nn; p0 += 8) {
            int np = CB_MIN(8, nn - p0);
            Segment segs[8];
            int lrows[8];
            for (int j = 0; j < np; j++) {
                const Gen *ge = &gen[act[p0 + j]];
                segs[j] = (Segment) {
                    .row = j, .m = 1, .pos0 = ge->pos, .slot0 = ge->slot, .brk = prefix,
                };
                lrows[j] = j;
            }
            Rows r = {
                .ids = ml->ids.b.addr + (VkDeviceAddress)p0 * 4,
                .aidx = ml->aidx.b.addr + (VkDeviceAddress)p0 * 4,
                .pos = ml->pos.b.addr + (VkDeviceAddress)p0 * 8,
                .n = np,
            };
            pass(ml, &r, segs, np, lrows, np, p0);
        }
        for (int j = 0; j < nn; j++) {
            ArgmaxPush ap = {
                .logits = ml->logits.b.addr + (VkDeviceAddress)j * VOCAB * 4,
                .dst = ml->next.b.addr + (VkDeviceAddress)j * 4,
                .n = VOCAB,
            };
            ml_gpu_dispatch(g, ML_K_ARGMAX, &ap, sizeof(ap), 1, 1, 1);
            Gen *ge = &gen[act[j]];
            ge->i++;
            ge->pos++;
            ge->slot++;
        }
        nact = nn;
    }
    free(act);
    if (ret < 0)
        goto end;
    for (int k = 0; k < nb_gen; k++) {
        gen[k].q->answer = gen[k].out;
        gen[k].out = NULL;
    }
    ml->stats.answer_ms += (cb_time_us() - t1) * 1e-3;

end:
    for (int k = 0; gen && k < nb_gen; k++)
        free(gen[k].out);
    free(gen);
    free(lidx);
    plan_free(&p);
    return ret < 0 ? ret : 0;
}

int ml_ask_many(CBML *ml, MLQuery *qs, int nb, CBCancel *cancel)
{
    int64_t t0 = cb_time_us();
    Tokens *t = calloc(CB_MAX(nb, 1), sizeof(*t));
    int *slot = calloc(CB_MAX(nb, 1), sizeof(*slot));
    int ret = t && slot ? 0 : CB_ERROR(ENOMEM);
    for (int i = 0; i < nb; i++)
        qs[i].answer = NULL;
    for (int i = 0; i < nb && ret >= 0; i++) {
        ret = tok_question(ml, &t[i], qs[i].question, qs[i].prefix, ml->nb_prefix);
        if (ret >= 0 && ml->nb_prefix + t[i].nb > ml->max_ctx)
            ret = CB_ERROR(E2BIG);
    }

    /* As many questions a batch as have room in the cache past the audio,
     * each for its tokens and the answer's (the last of an answer's tokens
     * is never read back in). One that has no room beside the others goes
     * first in a batch of its own, at the slots it would have alone. */
    int avail = ml->max_ctx - ml->nb_prefix;
    for (int i = 0; i < nb && ret >= 0;) {
        int used = 0, j = i;
        while (j < nb) {
            int answer = qs[j].answer_wanted ? CB_MAX(qs[j].max_tokens - 1, 0) : 0;
            int need = CB_MIN(t[j].nb + answer, avail);
            if (j > i && used + need > avail)
                break;
            slot[j] = ml->nb_prefix + used;
            used += need;
            j++;
        }
        cb_debug("ml: questions %d to %d of %d together, in %d of the %d slots past the audio",
                 i + 1, j, nb, used, avail);
        ret = ask_batch(ml, qs + i, t + i, slot + i, j - i, cancel);
        i = j;
    }

    for (int i = 0; t && i < nb; i++)
        tok_free(&t[i]);
    free(t);
    free(slot);
    if (ret < 0) {
        ml_gpu_discard(&ml->g);
        for (int i = 0; i < nb; i++) {
            free(qs[i].answer);
            qs[i].answer = NULL;
        }
    }
    ml->ask_ms += (cb_time_us() - t0) * 1e-3;
    return ret;
}

int ml_choice_ids(CBML *ml, const char *const *choices, int nb, int32_t *ids)
{
    for (int i = 0; i < nb; i++) {
        int32_t *c = NULL;
        int cn = 0, ccap = 0;
        int ret = ml_tok_encode(ml->tok, choices[i], &c, &cn, &ccap);
        if (ret >= 0 && cn != 1) {
            cb_err("ml: choice \"%s\" is %d tokens, not one", choices[i], cn);
            ret = CB_ERROR(EINVAL);
        }
        if (ret >= 0)
            ids[i] = c[0];
        free(c);
        if (ret < 0)
            return ret;
    }
    return 0;
}

int ml_choice_probs(const float *logits, const int32_t *ids, int nb, float *probs, float *mass)
{
    /* Softmax over the whole vocabulary, in double. A logit that is not a
     * number (a fault on the GPU) would make it uniform, or every choice
     * 0: an error, not an answer. */
    double mx = -INFINITY;
    for (int i = 0; i < VOCAB; i++) {
        if (!isfinite(logits[i])) {
            cb_err("ml: the model's logits are not finite");
            return CB_ERROR(EIO);
        }
        mx = fmax(mx, logits[i]);
    }
    double z = 0.0;
    for (int i = 0; i < VOCAB; i++)
        z += exp(logits[i] - mx);
    double sum = 0.0;
    for (int i = 0; i < nb; i++) {
        probs[i] = (float)(exp(logits[ids[i]] - mx) / z);
        sum += probs[i];
    }
    if (!(sum > 0.0) || !isfinite(sum)) {
        cb_err("ml: the choices have no probability at all");
        return CB_ERROR(EIO);
    }
    for (int i = 0; i < nb; i++)
        probs[i] = (float)(probs[i] / sum);
    if (mass)
        *mass = (float)sum;
    return 0;
}

int cb_ml_ask(CBML *ml, const char *question, int max_tokens, char **answer)
{
    MLQuery q = { .question = question, .answer_wanted = true, .max_tokens = max_tokens };
    int ret = ml_ask_many(ml, &q, 1, NULL);
    *answer = q.answer;
    return ret;
}

int cb_ml_choose(CBML *ml, const char *question, const char *prefix,
                 const char *const *choices, int nb, float *probs, float *mass)
{
    int32_t *ids = calloc(CB_MAX(nb, 1), sizeof(*ids));
    float *logits = malloc((size_t)VOCAB * sizeof(*logits));
    int ret = ids && logits ? 0 : CB_ERROR(ENOMEM);
    if (ret >= 0)
        ret = ml_choice_ids(ml, choices, nb, ids);
    if (ret >= 0) {
        MLQuery q = { .question = question, .prefix = prefix, .logits = logits };
        ret = ml_ask_many(ml, &q, 1, NULL);
    }
    if (ret >= 0)
        ret = ml_choice_probs(logits, ids, nb, probs, mass);
    free(ids);
    free(logits);
    return ret;
}

/* log softmax(logits)[id], in double. */
static double log_prob(const float *logits, int32_t id)
{
    double mx = -INFINITY;
    for (int i = 0; i < VOCAB; i++)
        mx = fmax(mx, logits[i]);
    double z = 0.0;
    for (int i = 0; i < VOCAB; i++)
        z += exp(logits[i] - mx);
    return logits[id] - mx - log(z);
}

/* The logits of the last decode(), rows [0, nb), into ml->down. */
static int read_logits(CBML *ml, int nb)
{
    ml_gpu_copy(&ml->g, &ml->logits.b, 0, &ml->down, 0, (size_t)nb * VOCAB * 4);
    int ret = ml_gpu_run(&ml->g);
    if (ret < 0)
        return ret;
    return ml_buf_invalidate(&ml->g, &ml->down);
}

int cb_ml_rank(CBML *ml, const char *question, const char *prefix,
               const char *const *answers, int nb, double *logprob)
{
    int64_t t0 = cb_time_us();
    Tokens t = { 0 };
    float *first = NULL;
    int ret = tok_question(ml, &t, question, prefix, ml->nb_prefix);
    if (ret < 0)
        goto end;
    int pos = ml->nb_prefix + t.nb;
    if ((ret = decode(ml, t.ids, NULL, t.nb, ml->nb_prefix, 1, NULL)) < 0 ||
        (ret = read_logits(ml, 1)) < 0)
        goto end;

    /* The answer's first token is predicted by the question's last
     * logits: keep them. */
    if (!(first = malloc((size_t)VOCAB * sizeof(*first)))) {
        ret = CB_ERROR(ENOMEM);
        goto end;
    }
    memcpy(first, ml->down.map, (size_t)VOCAB * sizeof(*first));

    for (int a = 0; a < nb && ret >= 0; a++) {
        Tokens at = { 0 };
        if ((ret = tok_text(ml, &at, answers[a])) < 0 ||
            (ret = tok_push(&at, TOK_IM_END, -1)) < 0) {
            tok_free(&at);
            break;
        }
        /* The answer and its end, all but the end through the decoder at
         * once: their logits predict the tokens after them. */
        int k = at.nb - 1;
        if (k < 1 || k > 8 || pos + k > ml->max_ctx) {
            cb_err("ml: answer \"%s\" is %d tokens; 1 to 8 can be ranked", answers[a], k);
            tok_free(&at);
            ret = CB_ERROR(EINVAL);
            break;
        }
        double lp = log_prob(first, at.ids[0]);
        if ((ret = decode(ml, at.ids, NULL, k, pos, k, NULL)) >= 0 &&
            (ret = read_logits(ml, k)) >= 0) {
            const float *rows = ml->down.map;
            for (int i = 0; i < k; i++)
                lp += log_prob(rows + (size_t)i * VOCAB, at.ids[i + 1]);
            logprob[a] = lp;
        }
        tok_free(&at);
    }

end:
    free(first);
    tok_free(&t);
    if (ret < 0)
        ml_gpu_discard(&ml->g);
    ml->ask_ms += (cb_time_us() - t0) * 1e-3;
    return ret;
}
