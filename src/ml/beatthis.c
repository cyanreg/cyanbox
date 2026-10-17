/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* Beat This! (cyanbox/beatthis.h): the network, run kernel by kernel on the
 * GPU. The reference is beat_this's model/beat_tracker.py, model/roformer.py
 * and inference.py (github.com/CPJKU/beat_this); every step below names the
 * part of it that it does.
 *   audio, 22050 Hz -> LogMelSpect: 128 mel bins, 50 frames a second
 *                      (shaders/bt_mel.comp)
 *   spectrogram     -> split_piece: chunks of 1500 frames, 6 each side
 *                      overlapping, the first and last padded with zeros
 *   each chunk      -> the stem: batch norm, a 4x3 convolution to 32 channels
 *                      of 32 frequencies (bt_stem.comp); three frontend
 *                      blocks, each attention and a feed-forward across
 *                      frequency, then across time (PartialFTTransformer),
 *                      then a 2x3 convolution halving the frequencies and
 *                      doubling the channels; a projection to 512; six
 *                      roformer layers; the sum head
 *   chunks          -> aggregate_prediction: each chunk's frames but its 6
 *                      at either end, an earlier chunk's where two overlap
 *                      ("keep_first")
 * Choices, and why:
 *  - The activations are channel-last, [chunk][time][frequency][channel],
 *    so every Linear is one product over contiguous rows (gemm.comp), the
 *    attention across frequency reads a token every row and the one across
 *    time a token every F rows (bt_attn.comp), and the frontend's last
 *    "b c f t -> b t (c f)" is the rows as they are, the projection's
 *    columns permuted to match.
 *  - The weights are rearranged once, here, into one buffer: q, k, v and
 *    the heads' gates of an attention in one product (its rows padded to a
 *    multiple of 4 as gemm.comp wants), the convolutions' as their im2col
 *    gather (bt_im2col.comp) reads, the head's two outputs padded to four.
 *    The sum head's beat logit is beat plus downbeat, added here in f32 as
 *    the reference does, not folded into the weights in f16.
 *  - Tables (the DFT's basis, the window, RoPE's angles) are computed in
 *    double precision and uploaded once.
 *  - RMSNorm (F.normalize times sqrt(dim) times gamma) is rmsnorm.comp with
 *    an epsilon of 1e-24, which only keeps a row of zeros finite. */

#include "cyanbox/beatthis.h"

#include "ml_priv.h"
#include "kernels.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SR       22050
#define N_FFT    1024
#define HOP      441
#define BINS     513
#define MELS     128
#define CHUNK    1500
#define BORDER   6
#define STEM     32
#define DIM      512
#define LAYERS   6
#define HEADS    16
#define HD       32
#define FF_MULT  4
#define BLOCKS   3

/* Mel frames a dispatch of the spectrogram, and so samples a window of
 * the piece's (14.5 MB). */
#define MEL_SEG  8192

typedef struct Lin {
    VkDeviceAddress w, b;       /* f16 [N][K]; f32 [N] or 0 */
    uint32_t N, K;
} Lin;

/* An attention with its feed-forward (roformer.Attention, FeedForward). */
typedef struct Layer {
    VkDeviceAddress attn_norm, ff_norm;
    Lin qkvg, out, ff1, ff2;
    uint32_t dim, heads;
} Layer;

struct CBBeatThis {
    MLGpu g;
    MLGguf f;
    int batch;

    MLBuf weights, tables;
    VkDeviceAddress bn_scale, bn_offset, stem_w, stem_b;
    Layer blk[BLOCKS][2];       /* across frequency, across time */
    Lin conv[BLOCKS], proj, head;
    Layer tr[LAYERS];
    VkDeviceAddress final_norm;
    VkDeviceAddress window, basis, filters, rope;

    /* Scratch, sized for `batch` chunks. */
    MLBuf pcm, mel, starts, xa, xb, h, o, qkv, u, col, y;
    MLBuf up, down;
    int64_t mel_frames;         /* what `mel` holds */

    double mel_ms, net_ms;
};

/* ------------------------------------------------------------- weights */

typedef struct Pack {
    uint8_t *data;
    size_t size, cap;
} Pack;

static int64_t pack_room(Pack *p, size_t size)
{
    size_t at = CB_ALIGN(p->size, 256);
    if (at + size > p->cap) {
        size_t cap = CB_MAX(p->cap * 2, at + size + (1 << 20));
        uint8_t *d = realloc(p->data, cap);
        if (!d)
            return CB_ERROR(ENOMEM);
        memset(d + p->cap, 0, cap - p->cap);
        p->data = d;
        p->cap = cap;
    }
    p->size = at + size;
    return (int64_t)at;
}

/* The tensor `name`, of exactly this type and shape (ne[0] fastest),
 * read from the file into *out (malloc'd). */
static int read_tensor(CBBeatThis *bt, const char *name, enum MLType type, int64_t ne0,
                       int64_t ne1, int64_t ne2, int64_t ne3, void **out)
{
    *out = NULL;
    const MLTensor *t = ml_gguf_tensor(&bt->f, name);
    if (!t) {
        cb_err("beatthis: the model has no tensor %s", name);
        return CB_ERROR(EINVAL);
    }
    uint64_t n = (uint64_t)(ne0 * ne1 * ne2 * ne3);
    uint64_t size = n * (type == ML_TYPE_F16 ? 2 : 4);
    if (t->type != type || t->ne[0] != ne0 || t->ne[1] != ne1 || t->ne[2] != ne2 ||
        t->ne[3] != ne3 || t->size != size) {
        cb_err("beatthis: tensor %s is type %d, %lld x %lld x %lld x %lld; expected type %d, "
               "%lld x %lld x %lld x %lld", name, t->type, (long long)t->ne[0],
               (long long)t->ne[1], (long long)t->ne[2], (long long)t->ne[3], type,
               (long long)ne0, (long long)ne1, (long long)ne2, (long long)ne3);
        return CB_ERROR(EINVAL);
    }
    uint8_t *buf = malloc(size);
    if (!buf)
        return CB_ERROR(ENOMEM);
    uint64_t pos = bt->f.data_offset + t->offset;
    for (uint64_t done = 0; done < size;) {
        ssize_t r = pread(bt->f.fd, buf + done, size - done, (off_t)(pos + done));
        if (r <= 0) {
            free(buf);
            cb_err("beatthis: reading tensor %s failed", name);
            return CB_ERROR(EIO);
        }
        done += (uint64_t)r;
    }
    *out = buf;
    return 0;
}

/* Offsets into the pack until it is uploaded; then addresses. */
typedef struct Packer {
    CBBeatThis *bt;
    Pack p;
    int ret;
} Packer;

/* A copy of `size` bytes into the pack; its offset, or -1 after an error. */
static int64_t put(Packer *k, const void *src, size_t size)
{
    if (k->ret < 0)
        return -1;
    int64_t at = pack_room(&k->p, size);
    if (at < 0) {
        k->ret = (int)at;
        return -1;
    }
    if (src)
        memcpy(k->p.data + at, src, size);
    return at;
}

static int64_t put_f32(Packer *k, const char *name, int64_t n)
{
    void *t;
    if (k->ret < 0)
        return -1;
    if ((k->ret = read_tensor(k->bt, name, ML_TYPE_F32, n, 1, 1, 1, &t)) < 0)
        return -1;
    int64_t at = put(k, t, (size_t)n * 4);
    free(t);
    return at;
}

/* An attention's q, k, v and gates in one [3 dim + gpad][dim] product, its
 * bias zero but for the gates'. */
static void put_qkvg(Packer *k, const char *pre, uint32_t dim, uint32_t heads, uint32_t gpad,
                     int64_t *w, int64_t *b)
{
    char name[128];
    void *qkv = NULL, *gw = NULL, *gb = NULL;
    uint32_t N = 3 * dim + gpad;
    *w = *b = -1;
    if (k->ret < 0)
        return;
    snprintf(name, sizeof(name), "%s.to_qkv.weight", pre);
    if ((k->ret = read_tensor(k->bt, name, ML_TYPE_F16, dim, 3 * dim, 1, 1, &qkv)) < 0)
        goto end;
    snprintf(name, sizeof(name), "%s.to_gates.weight", pre);
    if ((k->ret = read_tensor(k->bt, name, ML_TYPE_F16, dim, heads, 1, 1, &gw)) < 0)
        goto end;
    snprintf(name, sizeof(name), "%s.to_gates.bias", pre);
    if ((k->ret = read_tensor(k->bt, name, ML_TYPE_F32, heads, 1, 1, 1, &gb)) < 0)
        goto end;
    if ((*w = put(k, NULL, (size_t)N * dim * 2)) < 0 || (*b = put(k, NULL, (size_t)N * 4)) < 0)
        goto end;
    memcpy(k->p.data + *w, qkv, (size_t)3 * dim * dim * 2);
    memcpy(k->p.data + *w + (size_t)3 * dim * dim * 2, gw, (size_t)heads * dim * 2);
    memcpy(k->p.data + *b + (size_t)3 * dim * 4, gb, (size_t)heads * 4);
end:
    free(qkv);
    free(gw);
    free(gb);
}

static int64_t put_f16(Packer *k, const char *name, int64_t ne0, int64_t ne1)
{
    void *t;
    if (k->ret < 0)
        return -1;
    if ((k->ret = read_tensor(k->bt, name, ML_TYPE_F16, ne0, ne1, 1, 1, &t)) < 0)
        return -1;
    int64_t at = put(k, t, (size_t)(ne0 * ne1) * 2);
    free(t);
    return at;
}

/* A frontend block's convolution, [cout][cin][2][3] in the file, as
 * [cout][kf][kt][cin] for bt_im2col.comp's rows. */
static int64_t put_conv(Packer *k, const char *name, uint32_t cin, uint32_t cout)
{
    void *t;
    if (k->ret < 0)
        return -1;
    if ((k->ret = read_tensor(k->bt, name, ML_TYPE_F16, 3, 2, cin, cout, &t)) < 0)
        return -1;
    int64_t at = put(k, NULL, (size_t)cout * 6 * cin * 2);
    if (at >= 0) {
        const uint16_t *s = t;
        uint16_t *d = (uint16_t *)(k->p.data + at);
        for (uint32_t co = 0; co < cout; co++)
            for (uint32_t ci = 0; ci < cin; ci++)
                for (uint32_t kf = 0; kf < 2; kf++)
                    for (uint32_t kt = 0; kt < 3; kt++)
                        d[(size_t)co * 6 * cin + (kf * 3 + kt) * cin + ci] =
                            s[(((size_t)co * cin + ci) * 2 + kf) * 3 + kt];
    }
    free(t);
    return at;
}

/* The offsets of a Layer while packing; addresses after. */
typedef struct LayerOff {
    int64_t attn_norm, ff_norm, qkvg_w, qkvg_b, out_w, ff1_w, ff1_b, ff2_w, ff2_b;
} LayerOff;

static void put_layer(Packer *k, const char *attn, const char *ff, uint32_t dim, uint32_t heads,
                      uint32_t gpad, LayerOff *o)
{
    char name[128];
    snprintf(name, sizeof(name), "%s.norm.gamma", attn);
    o->attn_norm = put_f32(k, name, dim);
    put_qkvg(k, attn, dim, heads, gpad, &o->qkvg_w, &o->qkvg_b);
    snprintf(name, sizeof(name), "%s.to_out.0.weight", attn);
    o->out_w = put_f16(k, name, dim, dim);
    snprintf(name, sizeof(name), "%s.net.0.gamma", ff);
    o->ff_norm = put_f32(k, name, dim);
    snprintf(name, sizeof(name), "%s.net.1.weight", ff);
    o->ff1_w = put_f16(k, name, dim, FF_MULT * dim);
    snprintf(name, sizeof(name), "%s.net.1.bias", ff);
    o->ff1_b = put_f32(k, name, FF_MULT * dim);
    snprintf(name, sizeof(name), "%s.net.4.weight", ff);
    o->ff2_w = put_f16(k, name, FF_MULT * dim, dim);
    snprintf(name, sizeof(name), "%s.net.4.bias", ff);
    o->ff2_b = put_f32(k, name, dim);
}

static void set_layer(Layer *l, const LayerOff *o, VkDeviceAddress base, uint32_t dim,
                      uint32_t heads, uint32_t gpad)
{
    l->dim = dim;
    l->heads = heads;
    l->attn_norm = base + o->attn_norm;
    l->ff_norm = base + o->ff_norm;
    l->qkvg = (Lin) { base + o->qkvg_w, base + o->qkvg_b, 3 * dim + gpad, dim };
    l->out = (Lin) { base + o->out_w, 0, dim, dim };
    l->ff1 = (Lin) { base + o->ff1_w, base + o->ff1_b, FF_MULT * dim, dim };
    l->ff2 = (Lin) { base + o->ff2_w, base + o->ff2_b, dim, FF_MULT * dim };
}

/* Bytes from host memory into a device buffer, through a staging one. */
static int upload(CBBeatThis *bt, MLBuf *dst, const void *src, size_t size)
{
    MLBuf st = { 0 };
    int ret = ml_buf_create(&bt->g, &st, size, ML_MEM_UPLOAD);
    if (ret < 0)
        return ret;
    memcpy(st.map, src, size);
    if ((ret = ml_buf_flush(&bt->g, &st, size)) >= 0) {
        ml_gpu_copy(&bt->g, &st, 0, dst, 0, size);
        ret = ml_gpu_run(&bt->g);
    }
    ml_buf_free(&bt->g, &st);
    return ret;
}

static int load_weights(CBBeatThis *bt)
{
    Packer k = { .bt = bt };
    char a[96], f[96], name[128];

    int64_t bn_scale = put_f32(&k, "frontend.stem.bn1d.scale", MELS);
    int64_t bn_offset = put_f32(&k, "frontend.stem.bn1d.offset", MELS);
    int64_t stem_w = -1;
    {
        void *t;
        if (k.ret >= 0 && (k.ret = read_tensor(bt, "frontend.stem.conv2d.weight", ML_TYPE_F16,
                                               3, 4, 1, STEM, &t)) >= 0) {
            stem_w = put(&k, t, (size_t)STEM * 12 * 2);
            free(t);
        }
    }
    int64_t stem_b = put_f32(&k, "frontend.stem.conv2d.bias", STEM);

    LayerOff blk[BLOCKS][2], tr[LAYERS];
    int64_t conv_w[BLOCKS], conv_b[BLOCKS];
    for (int i = 0; i < BLOCKS; i++) {
        uint32_t dim = STEM << i, heads = dim / HD;
        for (int d = 0; d < 2; d++) {
            snprintf(a, sizeof(a), "frontend.blocks.%d.partial.attn%c", i, "FT"[d]);
            snprintf(f, sizeof(f), "frontend.blocks.%d.partial.ff%c", i, "FT"[d]);
            put_layer(&k, a, f, dim, heads, 4, &blk[i][d]);
        }
        snprintf(name, sizeof(name), "frontend.blocks.%d.conv2d.weight", i);
        conv_w[i] = put_conv(&k, name, dim, 2 * dim);
        snprintf(name, sizeof(name), "frontend.blocks.%d.conv2d.bias", i);
        conv_b[i] = put_f32(&k, name, 2 * dim);
    }

    /* The projection's input is "(c f)", channel major; the rows here are
     * [f][c]: its columns are permuted so. */
    const uint32_t PC = STEM << BLOCKS, PF = (MELS / 4) >> BLOCKS, PK = PC * PF;
    int64_t proj_w = -1;
    {
        void *t;
        if (k.ret >= 0 && (k.ret = read_tensor(bt, "frontend.linear.weight", ML_TYPE_F16,
                                               PK, DIM, 1, 1, &t)) >= 0) {
            proj_w = put(&k, NULL, (size_t)DIM * PK * 2);
            if (proj_w >= 0) {
                const uint16_t *s = t;
                uint16_t *d = (uint16_t *)(k.p.data + proj_w);
                for (uint32_t n = 0; n < DIM; n++)
                    for (uint32_t c = 0; c < PC; c++)
                        for (uint32_t fq = 0; fq < PF; fq++)
                            d[(size_t)n * PK + fq * PC + c] = s[(size_t)n * PK + c * PF + fq];
            }
            free(t);
        }
    }
    int64_t proj_b = put_f32(&k, "frontend.linear.bias", DIM);

    for (int l = 0; l < LAYERS; l++) {
        snprintf(a, sizeof(a), "transformer_blocks.layers.%d.0", l);
        snprintf(f, sizeof(f), "transformer_blocks.layers.%d.1", l);
        put_layer(&k, a, f, DIM, HEADS, HEADS, &tr[l]);
    }
    int64_t final_norm = put_f32(&k, "transformer_blocks.norm.gamma", DIM);

    /* The head's beat and downbeat rows, two of zeros after them. */
    int64_t head_w = -1, head_b = -1;
    {
        void *tw = NULL, *tb = NULL;
        if (k.ret >= 0 &&
            (k.ret = read_tensor(bt, "task_heads.beat_downbeat_lin.weight", ML_TYPE_F16, DIM, 2,
                                 1, 1, &tw)) >= 0 &&
            (k.ret = read_tensor(bt, "task_heads.beat_downbeat_lin.bias", ML_TYPE_F32, 2, 1, 1,
                                 1, &tb)) >= 0) {
            head_w = put(&k, NULL, (size_t)4 * DIM * 2);
            head_b = put(&k, NULL, 4 * 4);
            if (head_w >= 0 && head_b >= 0) {
                memcpy(k.p.data + head_w, tw, (size_t)2 * DIM * 2);
                memcpy(k.p.data + head_b, tb, 2 * 4);
            }
        }
        free(tw);
        free(tb);
    }

    int ret = k.ret;
    if (ret >= 0)
        ret = ml_buf_create(&bt->g, &bt->weights, CB_ALIGN(k.p.size, 256), ML_MEM_DEVICE);
    if (ret >= 0)
        ret = upload(bt, &bt->weights, k.p.data, k.p.size);
    free(k.p.data);
    if (ret < 0)
        return ret;

    VkDeviceAddress base = bt->weights.addr;
    bt->bn_scale = base + bn_scale;
    bt->bn_offset = base + bn_offset;
    bt->stem_w = base + stem_w;
    bt->stem_b = base + stem_b;
    for (int i = 0; i < BLOCKS; i++) {
        uint32_t dim = STEM << i;
        for (int d = 0; d < 2; d++)
            set_layer(&bt->blk[i][d], &blk[i][d], base, dim, dim / HD, 4);
        bt->conv[i] = (Lin) { base + conv_w[i], base + conv_b[i], 2 * dim, 6 * dim };
    }
    bt->proj = (Lin) { base + proj_w, base + proj_b, DIM, PK };
    for (int l = 0; l < LAYERS; l++)
        set_layer(&bt->tr[l], &tr[l], base, DIM, HEADS, HEADS);
    bt->final_norm = base + final_norm;
    bt->head = (Lin) { base + head_w, base + head_b, 4, DIM };
    return 0;
}

/* -------------------------------------------------------------- tables */

static int make_tables(CBBeatThis *bt)
{
    size_t nb_window = N_FFT, nb_basis = (size_t)N_FFT * BINS * 2, nb_filters = (size_t)BINS * MELS;
    size_t nb_rope = (size_t)CHUNK * (HD / 2) * 2;
    size_t total = nb_window + nb_basis + nb_filters + nb_rope;
    float *t = malloc(total * sizeof(*t));
    float *filters = NULL, *freqs = NULL;
    if (!t)
        return CB_ERROR(ENOMEM);
    int ret;
    if ((ret = read_tensor(bt, "aux.mel_filterbank", ML_TYPE_F32, MELS, BINS, 1, 1,
                           (void **)&filters)) < 0 ||
        (ret = read_tensor(bt, "transformer_blocks.layers.0.0.rotary_embed.freqs", ML_TYPE_F32,
                           HD / 2, 1, 1, 1, (void **)&freqs)) < 0)
        goto end;

    float *window = t, *basis = window + nb_window, *filt = basis + nb_basis;
    float *rope = filt + nb_filters;
    /* torch.hann_window(1024): periodic */
    for (int j = 0; j < N_FFT; j++)
        window[j] = (float)(0.5 - 0.5 * cos(2.0 * M_PI * j / N_FFT));
    for (int j = 0; j < N_FFT; j++)
        for (int b = 0; b < BINS; b++) {
            double a = 2.0 * M_PI * (double)((int64_t)j * b % N_FFT) / N_FFT;
            basis[2 * ((size_t)j * BINS + b)] = (float)cos(a);
            basis[2 * ((size_t)j * BINS + b) + 1] = (float)-sin(a);
        }
    memcpy(filt, filters, nb_filters * sizeof(*filt));
    /* rotary_embedding_torch: the angle is position times frequency in f32
     * (its einsum), its cosine and sine of that */
    for (int p = 0; p < CHUNK; p++)
        for (int i = 0; i < HD / 2; i++) {
            float a = (float)p * freqs[i];
            rope[2 * ((size_t)p * (HD / 2) + i)] = (float)cos((double)a);
            rope[2 * ((size_t)p * (HD / 2) + i) + 1] = (float)sin((double)a);
        }

    if ((ret = ml_buf_create(&bt->g, &bt->tables, total * sizeof(*t), ML_MEM_DEVICE)) < 0 ||
        (ret = upload(bt, &bt->tables, t, total * sizeof(*t))) < 0)
        goto end;
    bt->window = bt->tables.addr;
    bt->basis = bt->window + nb_window * 4;
    bt->filters = bt->basis + nb_basis * 4;
    bt->rope = bt->filters + nb_filters * 4;
end:
    free(t);
    free(filters);
    free(freqs);
    return ret;
}

/* --------------------------------------------------------------- open */

static int check_shape(CBBeatThis *bt)
{
    const MLValue *arch = ml_gguf_value(&bt->f, "general.architecture");
    if (!arch || arch->type != ML_GGUF_STRING || strcmp(arch->s, "beat-this")) {
        cb_err("beatthis: not a Beat This! model");
        return CB_ERROR(EINVAL);
    }
    static const struct { const char *key; int64_t v; } want[] = {
        { "beat-this.sample_rate", SR }, { "beat-this.n_fft", N_FFT },
        { "beat-this.hop_length", HOP }, { "beat-this.mel_bins", MELS },
        { "beat-this.transformer_dim", DIM }, { "beat-this.n_layers", LAYERS },
        { "beat-this.head_dim", HD }, { "beat-this.n_heads", HEADS },
        { "beat-this.stem_dim", STEM }, { "beat-this.ff_mult", FF_MULT },
        { "beat-this.chunk_size", CHUNK }, { "beat-this.border_size", BORDER },
    };
    for (size_t i = 0; i < CB_ARRAY_ELEMS(want); i++) {
        int64_t v = ml_gguf_int(&bt->f, want[i].key, -1);
        if (v != want[i].v) {
            cb_err("beatthis: %s is %lld, not %lld", want[i].key, (long long)v,
                   (long long)want[i].v);
            return CB_ERROR(EINVAL);
        }
    }
    const MLValue *ov = ml_gguf_value(&bt->f, "beat-this.overlap_mode");
    if (ov && (ov->type != ML_GGUF_STRING || strcmp(ov->s, "keep_first"))) {
        cb_err("beatthis: overlap mode %s, not keep_first",
               ov->type == ML_GGUF_STRING ? ov->s : "?");
        return CB_ERROR(EINVAL);
    }
    return 0;
}

void cb_beatthis_options_default(CBBeatThisOptions *o)
{
    *o = (CBBeatThisOptions) { 0 };
}

static int scratch(CBBeatThis *bt)
{
    size_t B = (size_t)bt->batch, rows = B * CHUNK * (MELS / 4);
    /* Every block keeps rows * dim the same (the frequencies halve as the
     * channels double), and so does the projection's input. */
    size_t act = rows * STEM * 4;
    size_t qkv = 0, u = 0, col = 0;
    for (int i = 0; i < BLOCKS; i++) {
        size_t r = rows >> i, dim = STEM << i;
        qkv = CB_MAX(qkv, r * (3 * dim + 4) * 4);
        u = CB_MAX(u, r * FF_MULT * dim * 4);
        col = CB_MAX(col, (r / 2) * 6 * dim * 4);
    }
    qkv = CB_MAX(qkv, B * CHUNK * (3 * DIM + HEADS) * 4);
    u = CB_MAX(u, B * CHUNK * FF_MULT * DIM * 4);
    size_t y = B * CHUNK * 4 * 4;
    int ret;
    if ((ret = ml_buf_create(&bt->g, &bt->xa, act, ML_MEM_DEVICE)) < 0 ||
        (ret = ml_buf_create(&bt->g, &bt->xb, act, ML_MEM_DEVICE)) < 0 ||
        (ret = ml_buf_create(&bt->g, &bt->h, act, ML_MEM_DEVICE)) < 0 ||
        (ret = ml_buf_create(&bt->g, &bt->o, act, ML_MEM_DEVICE)) < 0 ||
        (ret = ml_buf_create(&bt->g, &bt->qkv, qkv, ML_MEM_DEVICE)) < 0 ||
        (ret = ml_buf_create(&bt->g, &bt->u, u, ML_MEM_DEVICE)) < 0 ||
        (ret = ml_buf_create(&bt->g, &bt->col, col, ML_MEM_DEVICE)) < 0 ||
        (ret = ml_buf_create(&bt->g, &bt->y, y, ML_MEM_DEVICE)) < 0 ||
        (ret = ml_buf_create(&bt->g, &bt->down, y, ML_MEM_READBACK)) < 0 ||
        (ret = ml_buf_create(&bt->g, &bt->starts, 256, ML_MEM_DEVICE)) < 0)
        return ret;
    size_t pcm = ((size_t)MEL_SEG * HOP + N_FFT) * 4;
    if ((ret = ml_buf_create(&bt->g, &bt->pcm, pcm, ML_MEM_DEVICE)) < 0 ||
        (ret = ml_buf_create(&bt->g, &bt->up, CB_MAX(pcm, 256), ML_MEM_UPLOAD)) < 0)
        return ret;
    return 0;
}

int cb_beatthis_open(CBBeatThis **out, const char *path, const CBBeatThisOptions *opt)
{
    *out = NULL;
    CBBeatThisOptions defopt;
    if (!opt) {
        cb_beatthis_options_default(&defopt);
        opt = &defopt;
    }
    CBBeatThis *bt = calloc(1, sizeof(*bt));
    if (!bt)
        return CB_ERROR(ENOMEM);
    bt->f.fd = -1;
    /* a sequence a frame across frequency is a workgroup row of bt_attn.comp: at most 65535 */
    bt->batch = opt->batch > 0 ? CB_MIN(opt->batch, 32) : 4;

    int64_t t0 = cb_time_us();
    int ret;
    if ((ret = ml_gguf_open(&bt->f, path)) < 0 || (ret = check_shape(bt)) < 0)
        goto fail;
    if ((ret = ml_gpu_create(&bt->g, opt->device, opt->validation)) < 0)
        goto fail;
    cb_info("beatthis: %s on %s", path, bt->g.vk.props.deviceName);
    if ((ret = load_weights(bt)) < 0 || (ret = make_tables(bt)) < 0 || (ret = scratch(bt)) < 0)
        goto fail;
    cb_debug("beatthis: ready in %.1f s, %.0f MB on the GPU", (cb_time_us() - t0) * 1e-6,
             ml_gpu_allocated(&bt->g) / 1048576.0);
    *out = bt;
    return 0;

fail:
    cb_beatthis_close(&bt);
    return ret;
}

void cb_beatthis_close(CBBeatThis **pbt)
{
    CBBeatThis *bt = *pbt;
    if (!bt)
        return;
    if (bt->g.vk.dev) {
        ml_gpu_profile_print(&bt->g);
        vkDeviceWaitIdle(bt->g.vk.dev);
        MLBuf *all[] = {
            &bt->weights, &bt->tables, &bt->pcm, &bt->mel, &bt->starts, &bt->xa, &bt->xb,
            &bt->h, &bt->o, &bt->qkv, &bt->u, &bt->col, &bt->y, &bt->up, &bt->down,
        };
        for (size_t i = 0; i < CB_ARRAY_ELEMS(all); i++)
            ml_buf_free(&bt->g, all[i]);
    }
    ml_gguf_close(&bt->f, bt->g.vk.dev ? &bt->g : NULL);
    ml_gpu_destroy(&bt->g);
    free(bt);
    *pbt = NULL;
}

const char *cb_beatthis_device_name(const CBBeatThis *bt)
{
    return bt->g.vk.props.deviceName;
}

void cb_beatthis_times(const CBBeatThis *bt, double *mel_ms, double *net_ms)
{
    *mel_ms = bt->mel_ms;
    *net_ms = bt->net_ms;
}

/* ------------------------------------------------------------- kernels */

static void dispatch_elems(CBBeatThis *bt, enum MLKernel k, const void *push, size_t size,
                           uint64_t count)
{
    uint64_t groups = (count + 255) / 256;
    uint32_t x = (uint32_t)CB_MIN(groups, 65535);
    uint32_t y = (uint32_t)((groups + x - 1) / x);
    ml_gpu_dispatch(&bt->g, k, push, size, x, CB_MAX(y, 1), 1);
}

/* y[M][N] (ldy) = x[M][K] (ldx) w^T, with flags. */
static void gemm(CBBeatThis *bt, VkDeviceAddress x, uint32_t ldx, const Lin *w, VkDeviceAddress y,
                 uint32_t ldy, uint32_t M, uint32_t flags)
{
    GemmPush p = {
        .x = x, .w = w->w, .bias = w->b, .y = y,
        .M = M, .N = w->N, .K = w->K,
        .ldx = ldx, .ldy = ldy,
        .flags = flags | (w->b ? FLAG_BIAS : 0),
    };
    /* gemm_f16 takes K in tiles of 64 where the device has the shared
     * memory for them (gpu.c): the first block's 32 needs the 32s. */
    enum MLKernel k = w->K % 64 ? ML_K_GEMM_F16_BK32 : ML_K_GEMM_F16;
    ml_gpu_dispatch(&bt->g, k, &p, sizeof(p), (w->N + 127) / 128, (M + 127) / 128, 1);
}

static void rmsnorm(CBBeatThis *bt, VkDeviceAddress x, VkDeviceAddress w, VkDeviceAddress y,
                    uint32_t rows, uint32_t n)
{
    NormPush p = {
        .x = x, .weight = w, .y = y,
        .n = n, .ldx = n, .ldy = n,
        .eps = 1e-24f,
    };
    for (uint32_t r = 0; r < rows; r += 65535) {
        p.x = x + (VkDeviceAddress)r * n * 4;
        p.y = y + (VkDeviceAddress)r * n * 4;
        ml_gpu_dispatch(&bt->g, ML_K_RMSNORM, &p, sizeof(p), CB_MIN(rows - r, 65535), 1, 1);
    }
}

/* Sequences of tokens in the rows (bt_attn.comp says how). */
typedef struct Seqs {
    uint32_t nb, L, nb_inner, stride0, stride1, tok;
} Seqs;

/* An attention and its feed-forward on rows x (rows of dim), in place:
 * x += attention(norm(x)); x += ff(norm(x)). */
static void layer(CBBeatThis *bt, const Layer *l, VkDeviceAddress x, uint32_t rows, const Seqs *s)
{
    uint32_t dim = l->dim, ld = l->qkvg.N;
    rmsnorm(bt, x, l->attn_norm, bt->h.addr, rows, dim);
    ml_gpu_barrier(&bt->g);
    gemm(bt, bt->h.addr, dim, &l->qkvg, bt->qkv.addr, ld, rows, 0);
    ml_gpu_barrier(&bt->g);
    BtAttnPush ap = {
        .qkv = bt->qkv.addr, .o = bt->o.addr, .rope = bt->rope,
        .ld = ld, .ldo = dim,
        .L = s->L, .nb_inner = s->nb_inner, .stride0 = s->stride0, .stride1 = s->stride1,
        .tok = s->tok, .C = dim,
        .scale = 1.0f / sqrtf((float)HD),
    };
    ml_gpu_dispatch(&bt->g, ML_K_BT_ATTN, &ap, sizeof(ap), (s->L + 63) / 64, l->heads, s->nb);
    ml_gpu_barrier(&bt->g);
    gemm(bt, bt->o.addr, dim, &l->out, x, dim, rows, FLAG_ADD);
    ml_gpu_barrier(&bt->g);
    rmsnorm(bt, x, l->ff_norm, bt->h.addr, rows, dim);
    ml_gpu_barrier(&bt->g);
    gemm(bt, bt->h.addr, dim, &l->ff1, bt->u.addr, FF_MULT * dim, rows, FLAG_GELU);
    ml_gpu_barrier(&bt->g);
    gemm(bt, bt->u.addr, FF_MULT * dim, &l->ff2, x, dim, rows, FLAG_ADD);
    ml_gpu_barrier(&bt->g);
}

/* The network on chunks starting at frames starts[nb] of the spectrogram,
 * T frames each: their logits into y, [chunk][t][4]. */
static void network(CBBeatThis *bt, int nb, uint32_t T)
{
    uint32_t B = (uint32_t)nb;
    BtStemPush sp = {
        .mel = bt->mel.addr, .weight = bt->stem_w, .bias = bt->stem_b,
        .scale = bt->bn_scale, .offset = bt->bn_offset, .starts = bt->starts.addr,
        .dst = bt->xa.addr,
        .nb_frames = (uint32_t)bt->mel_frames, .T = T,
        .count = B * T * (MELS / 4) * STEM,
    };
    dispatch_elems(bt, ML_K_BT_STEM, &sp, sizeof(sp), sp.count);
    ml_gpu_barrier(&bt->g);

    MLBuf *x = &bt->xa, *nx = &bt->xb;
    uint32_t F = MELS / 4;
    for (int i = 0; i < BLOCKS; i++) {
        uint32_t dim = STEM << i, rows = B * T * F;
        /* across frequency: a sequence a frame, its F rows in a run */
        Seqs sf = { .nb = B * T, .L = F, .nb_inner = 1, .stride0 = F, .tok = 1 };
        layer(bt, &bt->blk[i][0], x->addr, rows, &sf);
        /* across time: a sequence a frequency of a chunk, a token every F rows */
        Seqs st = { .nb = B * F, .L = T, .nb_inner = F, .stride0 = T * F, .stride1 = 1, .tok = F };
        layer(bt, &bt->blk[i][1], x->addr, rows, &st);
        BtIm2colPush cp = {
            .src = x->addr, .dst = bt->col.addr,
            .T = T, .F = F, .C = dim,
            .count = B * T * (F / 2) * 6 * dim,
        };
        dispatch_elems(bt, ML_K_BT_IM2COL, &cp, sizeof(cp), cp.count);
        ml_gpu_barrier(&bt->g);
        gemm(bt, bt->col.addr, 6 * dim, &bt->conv[i], nx->addr, 2 * dim, B * T * (F / 2),
             FLAG_GELU);
        ml_gpu_barrier(&bt->g);
        MLBuf *t = x;
        x = nx;
        nx = t;
        F /= 2;
    }

    uint32_t rows = B * T;
    gemm(bt, x->addr, bt->proj.K, &bt->proj, nx->addr, DIM, rows, 0);
    ml_gpu_barrier(&bt->g);
    x = nx;
    Seqs s = { .nb = B, .L = T, .nb_inner = 1, .stride0 = T, .tok = 1 };
    for (int l = 0; l < LAYERS; l++)
        layer(bt, &bt->tr[l], x->addr, rows, &s);
    rmsnorm(bt, x->addr, bt->final_norm, bt->h.addr, rows, DIM);
    ml_gpu_barrier(&bt->g);
    gemm(bt, bt->h.addr, DIM, &bt->head, bt->y.addr, 4, rows, 0);
    ml_gpu_barrier(&bt->g);
    ml_gpu_copy(&bt->g, &bt->y, 0, &bt->down, 0, (VkDeviceSize)rows * 4 * 4);
}

/* LogMelSpect of the piece into `mel`, MEL_SEG frames a dispatch. */
static int spectrogram(CBBeatThis *bt, const float *pcm, int64_t n, int64_t frames)
{
    int ret;
    size_t need = (size_t)frames * MELS * 4;
    if (bt->mel.size < need) {
        ml_buf_free(&bt->g, &bt->mel);
        if ((ret = ml_buf_create(&bt->g, &bt->mel, CB_ALIGN(need + need / 2, 65536),
                                 ML_MEM_DEVICE)) < 0)
            return ret;
    }
    bt->mel_frames = frames;
    for (int64_t f0 = 0; f0 < frames; f0 += MEL_SEG) {
        int64_t nf = CB_MIN(MEL_SEG, frames - f0);
        /* the samples these frames read, reflections at the piece's ends included */
        int64_t s0 = CB_MAX(f0 * HOP - N_FFT / 2, 0);
        int64_t s1 = CB_MIN((f0 + nf - 1) * HOP + N_FFT / 2 + 1, n);
        if (f0 == 0)
            s0 = 0;
        if (f0 + nf == frames)
            s1 = n;
        size_t bytes = (size_t)(s1 - s0) * 4;
        if (bytes > bt->up.size)
            return CB_ERROR(EINVAL);
        memcpy(bt->up.map, pcm + s0, bytes);
        if ((ret = ml_buf_flush(&bt->g, &bt->up, bytes)) < 0)
            return ret;
        ml_gpu_copy(&bt->g, &bt->up, 0, &bt->pcm, 0, bytes);
        ml_gpu_barrier(&bt->g);
        BtMelPush p = {
            .pcm = bt->pcm.addr, .window = bt->window, .basis = bt->basis,
            .filters = bt->filters, .dst = bt->mel.addr + (VkDeviceAddress)f0 * MELS * 4,
            .nb_samples = (uint32_t)n, .pcm0 = (uint32_t)s0,
            .frame0 = (uint32_t)f0, .nb_frames = (uint32_t)nf,
        };
        ml_gpu_dispatch(&bt->g, ML_K_BT_MEL, &p, sizeof(p), (uint32_t)((nf + 7) / 8), 1, 1);
        /* the upload buffer is written again for the next window */
        if ((ret = ml_gpu_run(&bt->g)) < 0)
            return ret;
    }
    return 0;
}

int cb_beatthis_run(CBBeatThis *bt, const float *pcm, int64_t n, float **pbeat, float **pdown,
                    int *nb_frames)
{
    *pbeat = *pdown = NULL;
    *nb_frames = 0;
    if (n < 1 || n > (int64_t)INT32_MAX / 2)
        return CB_ERROR(EINVAL);
    int64_t frames = 1 + n / HOP;
    float *beat = malloc((size_t)frames * sizeof(*beat));
    float *down = malloc((size_t)frames * sizeof(*down));
    int *starts = NULL;
    int ret = 0;
    if (!beat || !down) {
        ret = CB_ERROR(ENOMEM);
        goto fail;
    }
    for (int64_t i = 0; i < frames; i++)
        beat[i] = down[i] = -1000.0f;

    int64_t t0 = cb_time_us();
    if ((ret = spectrogram(bt, pcm, n, frames)) < 0)
        goto fail;
    int64_t t1 = cb_time_us();

    /* split_piece: starts every CHUNK - 2 BORDER from -BORDER, the last moved
     * to end at the piece's end; a piece no longer than that one chunk of
     * its own length and the borders */
    int nb = 0;
    int64_t step = CHUNK - 2 * BORDER;
    for (int64_t s = -BORDER; s < frames - BORDER; s += step)
        nb++;
    starts = malloc((size_t)nb * sizeof(*starts));
    if (!starts) {
        ret = CB_ERROR(ENOMEM);
        goto fail;
    }
    for (int i = 0; i < nb; i++)
        starts[i] = (int)(-BORDER + i * step);
    uint32_t T = CHUNK;
    if (frames > step)
        starts[nb - 1] = (int)(frames - (CHUNK - BORDER));
    else
        T = (uint32_t)(frames + 2 * BORDER);

    /* keep_first: a chunk's frames go where no earlier chunk's went */
    int64_t done_to = 0;
    for (int c0 = 0; c0 < nb; c0 += bt->batch) {
        int nc = CB_MIN(bt->batch, nb - c0);
        memcpy(bt->up.map, starts + c0, (size_t)nc * sizeof(*starts));
        if ((ret = ml_buf_flush(&bt->g, &bt->up, (size_t)nc * sizeof(*starts))) < 0)
            goto fail;
        ml_gpu_copy(&bt->g, &bt->up, 0, &bt->starts, 0, (size_t)nc * sizeof(*starts));
        ml_gpu_barrier(&bt->g);
        network(bt, nc, T);
        if ((ret = ml_gpu_run(&bt->g)) < 0 || (ret = ml_buf_invalidate(&bt->g, &bt->down)) < 0)
            goto fail;
        const float *y = bt->down.map;
        for (int c = 0; c < nc; c++) {
            int64_t s = starts[c0 + c];
            for (uint32_t t = BORDER; t + BORDER < T; t++) {
                int64_t pos = s + t;
                if (pos < done_to || pos >= frames)
                    continue;
                const float *r = y + ((size_t)c * T + t) * 4;
                /* SumHead: the beat's is beat plus downbeat, in f32 */
                beat[pos] = r[0] + r[1];
                down[pos] = r[1];
            }
            done_to = CB_MAX(done_to, s + T - BORDER);
        }
    }
    int64_t t2 = cb_time_us();
    bt->mel_ms = (t1 - t0) / 1000.0;
    bt->net_ms = (t2 - t1) / 1000.0;
    free(starts);
    *pbeat = beat;
    *pdown = down;
    *nb_frames = (int)frames;
    return 0;

fail:
    ml_gpu_discard(&bt->g);
    free(starts);
    free(beat);
    free(down);
    return ret;
}
