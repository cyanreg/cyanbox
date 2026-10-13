/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* MOSS-Audio (rxbox/ml.h): the network, run kernel by kernel on the GPU.
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
 *    (rx_ml_listen); each question starts again from there.
 *  - The prefill goes through the decoder 512 tokens at a time, so its
 *    scratch buffers do not grow with the audio; the decoder's single
 *    tokens use the GEMV kernel, which reads each weight once.
 *  - The cache is f16, [layer][kv head][position][128]: a block of keys
 *    is one contiguous read for the attention kernel.
 *  - Tables (the DFT's basis, RoPE's angles, the encoder's positions) are
 *    computed here in double precision and uploaded once. */

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
#define VOCAB      151936
#define DEEPSTACK  3

/* Chunks through the convolutions at once; tokens through the decoder at
 * once; rows through the adapter at once. */
#define CONV_BATCH 8
#define LLM_CHUNK  512
#define ADAPT_ROWS 2048

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

struct RXML {
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
    Scratch x, h, q, k, v, o, fg, fu, logits, ids, aidx, part;
    MLBuf up, down;             /* small host buffers: ids in, a token or logits out */

    /* The context: what the cache holds. */
    int nb_prefix;              /* tokens of the prompt up to the audio's end */
    int nb_audio;               /* audio rows in `audio`, `ds_out` */

    double listen_ms, ask_ms;
};

/* ----------------------------------------------------------------- tensors */

static const MLTensor *tensor(RXML *ml, const char *fmt, int i, enum MLType type,
                              int64_t ne0, int64_t ne1)
{
    char name[96];
    snprintf(name, sizeof(name), fmt, i);
    const MLTensor *t = ml_gguf_tensor(&ml->f, name);
    if (!t) {
        rx_err("ml: the model has no tensor %s", name);
        return NULL;
    }
    if (t->type != type || t->ne[0] != ne0 || t->ne[1] != ne1) {
        rx_err("ml: tensor %s is type %d, %lld x %lld; expected type %d, %lld x %lld", name,
               t->type, (long long)t->ne[0], (long long)t->ne[1], type, (long long)ne0,
               (long long)ne1);
        return NULL;
    }
    return t;
}

#define T_(dst, ...) do { if (!((dst) = tensor(ml, __VA_ARGS__))) return RX_ERROR(EINVAL); } while (0)

static int find_tensors(RXML *ml)
{
    T_(ml->mel_filters, "audio.mel_filters", 0, ML_TYPE_F32, N_MEL, N_FFT / 2 + 1);
    T_(ml->mel_window, "audio.mel_window", 0, ML_TYPE_F32, N_FFT, 1);
    T_(ml->conv1_w, "enc.conv1.weight", 0, ML_TYPE_F16, 3, 3);
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

static int check_shape(RXML *ml)
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
        rx_err("ml: the file is not a MOSS-Audio model");
        return RX_ERROR(EINVAL);
    }
    for (size_t i = 0; i < RX_ARRAY_ELEMS(keys); i++) {
        int64_t v = ml_gguf_int(&ml->f, keys[i].key, -1);
        if (v != keys[i].want) {
            rx_err("ml: %s is %lld, this runs %lld", keys[i].key, (long long)v,
                   (long long)keys[i].want);
            return RX_ERROR(EINVAL);
        }
    }
    return 0;
}

/* ---------------------------------------------------------------- helpers */

static int reserve(RXML *ml, Scratch *s, size_t size)
{
    if (s->b.size >= size)
        return 0;
    ml_buf_free(&ml->g, &s->b);
    /* Grow by half again, so a slowly growing need does not reallocate
     * every time. */
    return ml_buf_create(&ml->g, &s->b, RX_ALIGN(size + size / 2, 65536), ML_MEM_DEVICE);
}

/* x and y dims of a dispatch of `groups` workgroups along one axis. */
static void groups_2d(uint64_t groups, uint32_t *x, uint32_t *y)
{
    *x = (uint32_t)RX_MIN(groups, 65535);
    *y = (uint32_t)((groups + *x - 1) / *x);
}

static void dispatch_elems(RXML *ml, enum MLKernel k, const void *push, size_t size,
                           uint64_t count)
{
    uint32_t x, y;
    groups_2d((count + 255) / 256, &x, &y);
    ml_gpu_dispatch(&ml->g, k, push, size, x, RX_MAX(y, 1), 1);
}

/* y[M][N] (ldy) = x[M][K] (ldx) w^T, with flags. */
static void gemm(RXML *ml, VkDeviceAddress x, uint32_t ldx, const MLTensor *w,
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

static void norm(RXML *ml, bool rms, VkDeviceAddress x, const MLTensor *w, const MLTensor *b,
                 VkDeviceAddress y, uint32_t rows, uint32_t n, float eps)
{
    NormPush p = {
        .x = x, .weight = w->addr, .bias = b ? b->addr : 0, .y = y,
        .n = n, .ldx = n, .ldy = n,
        .eps = eps,
    };
    ml_gpu_dispatch(&ml->g, rms ? ML_K_RMSNORM : ML_K_LAYERNORM, &p, sizeof(p), rows, 1, 1);
}

static void act(RXML *ml, int op, VkDeviceAddress a, VkDeviceAddress b, VkDeviceAddress idx,
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
static int mlp(RXML *ml, const MLP *m, VkDeviceAddress x, VkDeviceAddress y, uint32_t rows)
{
    int ret;
    uint32_t step = RX_MIN(rows, ADAPT_ROWS);
    if ((ret = reserve(ml, &ml->ag, (size_t)step * A_HIDDEN * 4)) < 0 ||
        (ret = reserve(ml, &ml->au, (size_t)step * A_HIDDEN * 4)) < 0)
        return ret;
    for (uint32_t r = 0; r < rows; r += step) {
        uint32_t n = RX_MIN(step, rows - r);
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

static int make_tables(RXML *ml)
{
    size_t nb_basis = (size_t)N_FFT * (N_FFT / 2 + 1) * 2;
    size_t nb_pos = (size_t)CHUNK_TOK * E_DIM;
    size_t nb_rope = (size_t)ml->max_ctx * (L_HEAD / 2) * 2;
    size_t total = nb_basis + nb_pos + nb_rope;
    float *t = malloc(total * sizeof(float));
    if (!t)
        return RX_ERROR(ENOMEM);

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

static int permute_convs(RXML *ml)
{
    for (int i = 0; i < 2; i++) {
        const MLTensor *w = tensor(ml, "enc.conv%d.weight", i + 2, ML_TYPE_F16, 3, 3);
        if (!w || w->ne[2] != C_DIM || w->ne[3] != C_DIM)
            return RX_ERROR(EINVAL);
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

void rx_ml_options_default(RXMLOptions *o)
{
    *o = (RXMLOptions) { 0 };
}

int rx_ml_open(RXML **out, const char *path, const RXMLOptions *opt)
{
    *out = NULL;
    RXMLOptions defopt;
    if (!opt) {
        rx_ml_options_default(&defopt);
        opt = &defopt;
    }

    RXML *ml = calloc(1, sizeof(*ml));
    if (!ml)
        return RX_ERROR(ENOMEM);
    ml->f.fd = -1;
    ml->max_ctx = opt->max_context > 0 ? opt->max_context : 8192;
    ml->max_ctx = RX_ALIGN(ml->max_ctx, 64);

    int64_t t0 = rx_time_us();
    int ret;
    if ((ret = ml_gguf_open(&ml->f, path)) < 0)
        goto fail;
    if ((ret = check_shape(ml)) < 0 || (ret = find_tensors(ml)) < 0)
        goto fail;
    if ((ret = ml_tok_create(&ml->tok, &ml->f)) < 0)
        goto fail;
    if ((ret = ml_gpu_create(&ml->g, opt->device, opt->validation)) < 0)
        goto fail;
    rx_info("ml: %s on %s", path, ml->g.props.deviceName);
    if ((ret = ml_gguf_upload(&ml->f, &ml->g)) < 0)
        goto fail;
    if ((ret = permute_convs(ml)) < 0 || (ret = make_tables(ml)) < 0)
        goto fail;

    size_t cache = (size_t)L_LAYERS * L_KV * ml->max_ctx * L_HEAD * 2;
    if ((ret = ml_buf_create(&ml->g, &ml->kc, cache, ML_MEM_DEVICE)) < 0 ||
        (ret = ml_buf_create(&ml->g, &ml->vc, cache, ML_MEM_DEVICE)) < 0)
        goto fail;
    if ((ret = ml_buf_create(&ml->g, &ml->up, (size_t)ml->max_ctx * 8, ML_MEM_UPLOAD)) < 0 ||
        (ret = ml_buf_create(&ml->g, &ml->down, (size_t)VOCAB * 8 * 4, ML_MEM_READBACK)) < 0)
        goto fail;

    rx_debug("ml: ready in %.1f s, context of %d tokens", (rx_time_us() - t0) * 1e-6,
             ml->max_ctx);
    *out = ml;
    return 0;

fail:
    rx_ml_close(&ml);
    return ret;
}

void rx_ml_close(RXML **pml)
{
    RXML *ml = *pml;
    if (!ml)
        return;
    if (ml->g.dev) {
        ml_gpu_profile_print(&ml->g);
        vkDeviceWaitIdle(ml->g.dev);
        Scratch *all[] = {
            &ml->pcm, &ml->mel, &ml->maxbits, &ml->c1, &ml->c2, &ml->c3, &ml->stem,
            &ml->ex, &ml->eh, &ml->eq, &ml->ek, &ml->ev, &ml->eo, &ml->ef,
            &ml->ds_x[0], &ml->ds_x[1], &ml->ds_x[2],
            &ml->ag, &ml->au, &ml->audio, &ml->ds_out[0], &ml->ds_out[1], &ml->ds_out[2],
            &ml->x, &ml->h, &ml->q, &ml->k, &ml->v, &ml->o, &ml->fg, &ml->fu,
            &ml->logits, &ml->ids, &ml->aidx, &ml->part,
        };
        for (size_t i = 0; i < RX_ARRAY_ELEMS(all); i++)
            ml_buf_free(&ml->g, &all[i]->b);
        ml_buf_free(&ml->g, &ml->up);
        ml_buf_free(&ml->g, &ml->down);
        ml_buf_free(&ml->g, &ml->kc);
        ml_buf_free(&ml->g, &ml->vc);
        ml_buf_free(&ml->g, &ml->tables);
        ml_buf_free(&ml->g, &ml->conv_w[0]);
        ml_buf_free(&ml->g, &ml->conv_w[1]);
    }
    ml_gguf_close(&ml->f, ml->g.dev ? &ml->g : NULL);
    ml_tok_free(&ml->tok);
    ml_gpu_destroy(&ml->g);
    free(ml);
    *pml = NULL;
}

const char *rx_ml_device_name(const RXML *ml)
{
    return ml->g.props.deviceName;
}

void rx_ml_times(const RXML *ml, double *listen_ms, double *ask_ms)
{
    if (listen_ms)
        *listen_ms = ml->listen_ms;
    if (ask_ms)
        *ask_ms = ml->ask_ms;
}

/* ---------------------------------------------------------------- encoder */

/* With RXBOX_ML_DUMP=DIR, the encoder's intermediate tensors are written
 * there as raw f32, for checking the network against the reference
 * implementation outside of the model (tools/ml_check.py). Waits for the
 * GPU. */
static void dump(RXML *ml, const char *name, const MLBuf *b, size_t size)
{
    const char *dir = getenv("RXBOX_ML_DUMP");
    if (!dir || !dir[0] || ml_gpu_run(&ml->g) < 0)
        return;
    MLBuf rb;
    if (ml_buf_create(&ml->g, &rb, size, ML_MEM_READBACK) < 0)
        return;
    ml_gpu_copy(&ml->g, b, 0, &rb, 0, size);
    if (ml_gpu_run(&ml->g) >= 0 && ml_buf_invalidate(&ml->g, &rb) >= 0) {
        char *path = rx_asprintf("%s/%s.f32", dir, name);
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
static int encode(RXML *ml, const float *pcm, int64_t n, RXCancel *cancel)
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
        int nb = RX_MIN(nb_frames - f0, 8 * 32768);
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
        int nb = RX_MIN(CONV_BATCH, nb_chunks - c0);
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
        if (rx_cancelled(cancel))
            return RX_ERROR(ECANCELED);
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
            if (rx_cancelled(cancel))
                return RX_ERROR(ECANCELED);
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

/* Layer i's attention for m queries at [pos0, pos0 + m) (in ml->q, after
 * qk_rope) over the cache, into ml->o: flash attention for a prefill,
 * flash decoding (split over the keys) for one or two tokens. */
static void attention(RXML *ml, int i, uint32_t m, int pos0)
{
    size_t layer_cache = (size_t)L_KV * ml->max_ctx * L_HEAD * 2;
    VkDeviceAddress kc = ml->kc.addr + i * layer_cache, vc = ml->vc.addr + i * layer_cache;
    float scale = (float)(1.0 / sqrt((double)L_HEAD));

    if (m * (L_HEADS / L_KV) <= ATTN_DEC_ROWS) {
        uint32_t nsplit = (pos0 + m + ATTN_SPLIT - 1) / ATTN_SPLIT;
        AttnDecPush p = {
            .q = ml->q.b.addr, .o = ml->o.b.addr, .kc = kc, .vc = vc, .part = ml->part.b.addr,
            .nq = m, .pos0 = (uint32_t)pos0, .max_ctx = (uint32_t)ml->max_ctx,
            .nb_q = L_HEADS, .nb_kv = L_KV, .nsplit = nsplit,
            .scale = scale,
        };
        ml_gpu_dispatch(&ml->g, ML_K_ATTN_DEC, &p, sizeof(p), nsplit, L_KV, 1);
        ml_gpu_barrier(&ml->g);
        ml_gpu_dispatch(&ml->g, ML_K_ATTN_COMB, &p, sizeof(p), m * (L_HEADS / L_KV), L_KV, 1);
        ml_gpu_barrier(&ml->g);
        return;
    }

    /* Few queries over many keys: the keys are split until there are some
     * 256 workgroups, each with 256 keys or more; for a handful of queries,
     * whose 64-row tiles are mostly empty, 1024 workgroups of 128 keys. */
    uint32_t qblocks = (m + 63) / 64, nkeys = pos0 + m;
    uint32_t want = m <= 16 ? 1024 : 256, least = m <= 16 ? 128 : 256;
    uint32_t nsplit = RX_MAX(1, want / (qblocks * L_HEADS));
    nsplit = RX_MIN(nsplit, RX_MAX(1, nkeys / least));
    nsplit = RX_MIN(nsplit, ATTN_MAX_PARTS / m);
    nsplit = RX_MAX(nsplit, 1);
    uint32_t split_len = RX_ALIGN((nkeys + nsplit - 1) / nsplit, 32);
    nsplit = (nkeys + split_len - 1) / split_len;
    AttnPush p = {
        .q = ml->q.b.addr, .o = ml->o.b.addr, .kc = kc, .vc = vc, .part = ml->part.b.addr,
        .nq = m, .pos0 = (uint32_t)pos0, .max_ctx = (uint32_t)ml->max_ctx,
        .nb_q = L_HEADS, .nb_kv = L_KV, .nsplit = nsplit, .split_len = split_len,
        .scale = scale,
    };
    ml_gpu_dispatch(&ml->g, ML_K_ATTN, &p, sizeof(p), qblocks, L_HEADS, nsplit);
    ml_gpu_barrier(&ml->g);
    if (nsplit > 1) {
        ml_gpu_dispatch(&ml->g, ML_K_ATTN_FCOMB, &p, sizeof(p), m, L_HEADS, 1);
        ml_gpu_barrier(&ml->g);
    }
}

/* Tokens ids[0 .. n) at positions [pos0, pos0 + n) through the decoder, into
 * the cache; aidx[i] >= 0 puts that audio row at token i instead of its
 * embedding (and adds the deepstack's rows after the first layers). The
 * logits of the last nb_logits tokens (at most 8, and n when n is no more
 * than LLM_CHUNK) end in ml->logits, [nb_logits][VOCAB]. */
static int decode(RXML *ml, const int32_t *ids, const int32_t *aidx, int n, int pos0,
                  int nb_logits, RXCancel *cancel)
{
    MLGpu *g = &ml->g;
    int ret;
    if (pos0 + n > ml->max_ctx)
        return RX_ERROR(E2BIG);

    int step = RX_MIN(n, LLM_CHUNK);
    if ((ret = reserve(ml, &ml->x, (size_t)step * L_DIM * 4)) < 0 ||
        (ret = reserve(ml, &ml->h, (size_t)step * L_DIM * 4)) < 0 ||
        (ret = reserve(ml, &ml->q, (size_t)step * L_HEADS * L_HEAD * 4)) < 0 ||
        (ret = reserve(ml, &ml->k, (size_t)step * L_KV * L_HEAD * 4)) < 0 ||
        (ret = reserve(ml, &ml->v, (size_t)step * L_KV * L_HEAD * 4)) < 0 ||
        (ret = reserve(ml, &ml->o, (size_t)step * L_HEADS * L_HEAD * 4)) < 0 ||
        (ret = reserve(ml, &ml->fg, (size_t)step * L_FFN * 4)) < 0 ||
        (ret = reserve(ml, &ml->fu, (size_t)step * L_FFN * 4)) < 0 ||
        (ret = reserve(ml, &ml->logits, (size_t)VOCAB * 8 * 4)) < 0 ||
        (ret = reserve(ml, &ml->ids, (size_t)ml->max_ctx * 4)) < 0 ||
        (ret = reserve(ml, &ml->aidx, (size_t)ml->max_ctx * 4)) < 0 ||
        (ret = reserve(ml, &ml->part,
                       RX_MAX((size_t)L_KV * (ml->max_ctx / ATTN_SPLIT + 1) * ATTN_DEC_ROWS,
                              (size_t)ATTN_MAX_PARTS * L_HEADS) * ATTN_PART * 4)) < 0)
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

    size_t layer_cache = (size_t)L_KV * ml->max_ctx * L_HEAD * 2;
    for (int c0 = 0; c0 < n; c0 += step) {
        uint32_t m = (uint32_t)RX_MIN(step, n - c0);
        VkDeviceAddress x = ml->x.b.addr, h = ml->h.b.addr;
        EmbedPush ep = {
            .table = ml->embed->addr,
            .ids = ml->ids.b.addr + (VkDeviceAddress)c0 * 4,
            .audio_idx = ml->aidx.b.addr + (VkDeviceAddress)c0 * 4,
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
                .pos0 = (uint32_t)(pos0 + c0), .max_ctx = (uint32_t)ml->max_ctx,
                .nb_q = L_HEADS, .nb_kv = L_KV,
                .eps = 1e-6f,
            };
            ml_gpu_dispatch(g, ML_K_QK_ROPE, &rp, sizeof(rp), m, 1, 1);
            ml_gpu_barrier(g);
            attention(ml, i, m, pos0 + c0);
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
            if (audio && i < DEEPSTACK) {
                act(ml, ACT_ADD_ROWS, x, ml->ds_out[i].b.addr,
                    ml->aidx.b.addr + (VkDeviceAddress)c0 * 4, (uint64_t)m * L_DIM, L_DIM, 0);
                ml_gpu_barrier(g);
            }
        }

        if (c0 + (int)m == n) {
            uint32_t nl = (uint32_t)RX_CLAMP(nb_logits, 1, RX_MIN(8, (int)m));
            VkDeviceAddress last = x + (VkDeviceAddress)(m - nl) * L_DIM * 4;
            norm(ml, true, last, ml->final_norm, NULL, h, nl, L_DIM, 1e-6f);
            ml_gpu_barrier(g);
            gemm(ml, h, L_DIM, ml->lm_head, 0, ml->logits.b.addr, VOCAB, nl, 0);
            ml_gpu_barrier(g);
        }
        if ((ret = ml_gpu_run(g)) < 0)
            return ret;
        if (rx_cancelled(cancel))
            return RX_ERROR(ECANCELED);
    }
    return 0;
}

/* The most likely next token after decode(). */
static int next_token(RXML *ml, int32_t *tok)
{
    ArgmaxPush p = {
        .logits = ml->logits.b.addr,
        .dst = ml->ids.b.addr + (VkDeviceAddress)(ml->max_ctx - 1) * 4,
        .n = VOCAB,
    };
    ml_gpu_dispatch(&ml->g, ML_K_ARGMAX, &p, sizeof(p), 1, 1, 1);
    ml_gpu_barrier(&ml->g);
    ml_gpu_copy(&ml->g, &ml->ids.b, (VkDeviceSize)(ml->max_ctx - 1) * 4, &ml->down, 0, 4);
    int ret = ml_gpu_run(&ml->g);
    if (ret < 0)
        return ret;
    ml_buf_invalidate(&ml->g, &ml->down);
    *tok = *(int32_t *)ml->down.map;
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
        int c = RX_MAX(t->cap * 2, 256);
        int32_t *ids = realloc(t->ids, c * sizeof(*ids));
        if (!ids)
            return RX_ERROR(ENOMEM);
        t->ids = ids;
        int32_t *aidx = realloc(t->aidx, c * sizeof(*aidx));
        if (!aidx)
            return RX_ERROR(ENOMEM);
        t->aidx = aidx;
        t->cap = c;
    }
    t->ids[t->nb] = id;
    t->aidx[t->nb] = a;
    t->nb++;
    return 0;
}

static int tok_text(RXML *ml, Tokens *t, const char *text)
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
static int tok_head(RXML *ml, Tokens *t)
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

int rx_ml_listen(RXML *ml, const float *pcm, int64_t n, RXCancel *cancel)
{
    int64_t t0 = rx_time_us();
    ml->nb_prefix = 0;
    ml->nb_audio = 0;

    /* What fits: the head, the audio and its markers, and room for a
     * question and its answer; whole seconds are cut from the end until it
     * does. */
    int64_t fit = n;
    while (fit > RX_ML_RATE && HEAD_TOKENS + audio_prompt_tokens(fit) + QUESTION_ROOM > ml->max_ctx)
        fit -= RX_ML_RATE;
    if (fit < n) {
        rx_debug("ml: %.1f s of audio, %.1f s fit in the context", n / (double)RX_ML_RATE,
                 fit / (double)RX_ML_RATE);
        n = fit;
    }
    if (n < N_FFT || HEAD_TOKENS + audio_prompt_tokens(n) + QUESTION_ROOM > ml->max_ctx)
        return RX_ERROR(E2BIG);

    int nb_audio = encode(ml, pcm, n, cancel);
    if (nb_audio < 0)
        return nb_audio;

    Tokens t = { 0 };
    int ret;
    if ((ret = tok_head(ml, &t)) < 0 || (ret = tok_push(&t, TOK_AUDIO_BOS, -1)) < 0 ||
        (ret = tok_audio(&t, nb_audio)) < 0 || (ret = tok_push(&t, TOK_AUDIO_EOS, -1)) < 0)
        goto end;
    if (t.nb + QUESTION_ROOM > ml->max_ctx) {
        ret = RX_ERROR(E2BIG);
        goto end;
    }
    if ((ret = decode(ml, t.ids, t.aidx, t.nb, 0, 1, cancel)) < 0)
        goto end;
    ml->nb_prefix = t.nb;
    ml->nb_audio = nb_audio;

end:
    tok_free(&t);
    ml->listen_ms = (rx_time_us() - t0) * 1e-3;
    ml->ask_ms = 0.0;
    return ret;
}

void rx_ml_forget(RXML *ml)
{
    ml->nb_prefix = 0;
    ml->nb_audio = 0;
}

/* The question's tokens, after the cached head and audio (or after a head
 * of its own when there is no audio). */
static int tok_question(RXML *ml, Tokens *t, const char *question, const char *prefix)
{
    int ret;
    if (!ml->nb_prefix && (ret = tok_head(ml, t)) < 0)
        return ret;
    char *q = rx_asprintf("%s%s", ml->nb_prefix ? "\n" : "", question);
    if (!q)
        return RX_ERROR(ENOMEM);
    ret = tok_text(ml, t, q);
    free(q);
    if (ret < 0 || (ret = tok_push(t, TOK_IM_END, -1)) < 0 || (ret = tok_text(ml, t, "\n")) < 0 ||
        (ret = tok_push(t, TOK_IM_START, -1)) < 0 || (ret = tok_text(ml, t, "assistant\n")) < 0)
        return ret;
    if (prefix && prefix[0])
        ret = tok_text(ml, t, prefix);
    return ret;
}

int rx_ml_ask(RXML *ml, const char *question, int max_tokens, char **answer)
{
    *answer = NULL;
    int64_t t0 = rx_time_us();
    Tokens t = { 0 };
    char *out = NULL;
    size_t len = 0, cap = 0;
    int ret = tok_question(ml, &t, question, NULL);
    if (ret < 0)
        goto end;

    int pos = ml->nb_prefix;
    if ((ret = decode(ml, t.ids, NULL, t.nb, pos, 1, NULL)) < 0)
        goto end;
    pos += t.nb;

    cap = 256;
    if (!(out = calloc(1, cap))) {
        ret = RX_ERROR(ENOMEM);
        goto end;
    }
    for (int i = 0; i < max_tokens; i++) {
        int32_t tok;
        if ((ret = next_token(ml, &tok)) < 0)
            goto end;
        if (tok == TOK_IM_END || tok == TOK_ENDOFTEXT)
            break;
        if ((ret = ml_tok_decode(ml->tok, tok, &out, &len, &cap)) < 0)
            goto end;
        if (i + 1 == max_tokens || pos + 1 >= ml->max_ctx)
            break;
        if ((ret = decode(ml, &tok, NULL, 1, pos, 1, NULL)) < 0)
            goto end;
        pos++;
    }
    *answer = out;
    out = NULL;

end:
    free(out);
    tok_free(&t);
    ml->ask_ms += (rx_time_us() - t0) * 1e-3;
    return ret;
}

int rx_ml_choose(RXML *ml, const char *question, const char *prefix,
                 const char *const *choices, int nb, float *probs, float *mass)
{
    int64_t t0 = rx_time_us();
    int32_t *ids = calloc(RX_MAX(nb, 1), sizeof(*ids));
    if (!ids)
        return RX_ERROR(ENOMEM);

    Tokens t = { 0 };
    int ret = 0;
    for (int i = 0; i < nb; i++) {
        int32_t *c = NULL;
        int cn = 0, ccap = 0;
        ret = ml_tok_encode(ml->tok, choices[i], &c, &cn, &ccap);
        if (ret >= 0 && cn != 1) {
            rx_err("ml: choice \"%s\" is %d tokens, not one", choices[i], cn);
            ret = RX_ERROR(EINVAL);
        }
        if (ret >= 0)
            ids[i] = c[0];
        free(c);
        if (ret < 0)
            goto end;
    }

    if ((ret = tok_question(ml, &t, question, prefix)) < 0)
        goto end;
    if ((ret = decode(ml, t.ids, NULL, t.nb, ml->nb_prefix, 1, NULL)) < 0)
        goto end;

    ml_gpu_copy(&ml->g, &ml->logits.b, 0, &ml->down, 0, (size_t)VOCAB * 4);
    if ((ret = ml_gpu_run(&ml->g)) < 0)
        goto end;
    ml_buf_invalidate(&ml->g, &ml->down);
    const float *logits = ml->down.map;

    /* Softmax over the whole vocabulary, in double. */
    double mx = -INFINITY;
    for (int i = 0; i < VOCAB; i++)
        mx = fmax(mx, logits[i]);
    double z = 0.0;
    for (int i = 0; i < VOCAB; i++)
        z += exp(logits[i] - mx);
    double sum = 0.0;
    for (int i = 0; i < nb; i++) {
        probs[i] = (float)(exp(logits[ids[i]] - mx) / z);
        sum += probs[i];
    }
    for (int i = 0; i < nb; i++)
        probs[i] = sum > 0.0 ? (float)(probs[i] / sum) : 1.0f / nb;
    if (mass)
        *mass = (float)sum;

end:
    free(ids);
    tok_free(&t);
    ml->ask_ms += (rx_time_us() - t0) * 1e-3;
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
static int read_logits(RXML *ml, int nb)
{
    ml_gpu_copy(&ml->g, &ml->logits.b, 0, &ml->down, 0, (size_t)nb * VOCAB * 4);
    int ret = ml_gpu_run(&ml->g);
    if (ret < 0)
        return ret;
    return ml_buf_invalidate(&ml->g, &ml->down);
}

int rx_ml_rank(RXML *ml, const char *question, const char *prefix,
               const char *const *answers, int nb, double *logprob)
{
    int64_t t0 = rx_time_us();
    Tokens t = { 0 };
    float *first = NULL;
    int ret = tok_question(ml, &t, question, prefix);
    if (ret < 0)
        goto end;
    int pos = ml->nb_prefix + t.nb;
    if ((ret = decode(ml, t.ids, NULL, t.nb, ml->nb_prefix, 1, NULL)) < 0 ||
        (ret = read_logits(ml, 1)) < 0)
        goto end;

    /* The answer's first token is predicted by the question's last
     * logits: keep them. */
    if (!(first = malloc((size_t)VOCAB * sizeof(*first)))) {
        ret = RX_ERROR(ENOMEM);
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
            rx_err("ml: answer \"%s\" is %d tokens; 1 to 8 can be ranked", answers[a], k);
            tok_free(&at);
            ret = RX_ERROR(EINVAL);
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
    ml->ask_ms += (rx_time_us() - t0) * 1e-3;
    return ret;
}
