/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* The model file's header against crafted files: gguf.c must refuse every
 * one whose tensors the GPU would read out of bounds (a zero alignment, a
 * dimension of 0 or past 2^63, sizes that overflow, offsets that wrap,
 * tensors past the end, overlapping or off the alignment), and moss.c every
 * tensor whose shape or size is not the model's. The files are built in
 * memory (memfd, sparse where the tensors are large: nothing is written but
 * the header). Needs no GPU: rx_ml_open stops at the tensor table, before
 * the tokenizer and the device, and the full table's positive control is
 * told from a refusal by what is logged. */

#include <stdckdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

#include "ml_priv.h"

static int failures;

static void expect(const char *what, int got, int want)
{
    bool ok = got == want;
    printf("%-52s %s (%d)\n", what, ok ? "ok  " : "FAIL", got);
    failures += !ok;
}

/* ---------------------------------------------------------------- writing */

typedef struct Buf {
    uint8_t *p;
    size_t n, cap;
} Buf;

static void put(Buf *b, const void *d, size_t n)
{
    if (b->n + n > b->cap) {
        b->cap = RX_MAX(b->cap * 2, b->n + n + 4096);
        if (!(b->p = realloc(b->p, b->cap)))
            abort();
    }
    memcpy(b->p + b->n, d, n);
    b->n += n;
}

static void put_u32(Buf *b, uint32_t v) { put(b, &v, 4); }
static void put_u64(Buf *b, uint64_t v) { put(b, &v, 8); }

static void put_str(Buf *b, const char *s)
{
    put_u64(b, strlen(s));
    put(b, s, strlen(s));
}

enum { T_U32 = 4, T_STRING = 8, T_U64 = 10 };

typedef struct KV {
    const char *key;
    int type;                   /* T_U32, T_U64, T_STRING */
    uint64_t u;
    const char *s;
} KV;

typedef struct Tensor {
    char name[64];
    int nb_dims;
    uint64_t ne[4];
    uint32_t type;
    uint64_t offset;
} Tensor;

static uint64_t bytes_of(const Tensor *t)
{
    uint64_t n = 1;
    for (int i = 0; i < t->nb_dims; i++) {
        if (ckd_mul(&n, n, t->ne[i]))
            return 0;
    }
    return t->type == ML_TYPE_Q4_K ? n / ML_QK_K * ML_Q4K_BYTES :
           n * (t->type == ML_TYPE_F16 ? 2 : 4);
}

/* Lays the tensors out one after the other on `align` (offset UINT64_MAX:
 * the next free one), as a writer would. */
static uint64_t lay_out(Tensor *t, int nb, uint64_t align)
{
    uint64_t end = 0;
    for (int i = 0; i < nb; i++) {
        if (t[i].offset == UINT64_MAX)
            t[i].offset = RX_ALIGN(end, align);
        end = RX_MAX(end, t[i].offset + bytes_of(&t[i]));
    }
    return end;
}

/* The header, in a memfd `data_size` bytes past the data's start; returns
 * the fd, and the data's start in *data_offset. */
static int write_gguf(const KV *kv, int nb_kv, const Tensor *t, int nb, uint64_t data_size,
                      uint64_t align, uint64_t *data_offset)
{
    Buf b = { 0 };
    put(&b, "GGUF", 4);
    put_u32(&b, 3);
    put_u64(&b, (uint64_t)nb);
    put_u64(&b, (uint64_t)nb_kv);
    for (int i = 0; i < nb_kv; i++) {
        put_str(&b, kv[i].key);
        put_u32(&b, (uint32_t)kv[i].type);
        if (kv[i].type == T_U32)
            put_u32(&b, (uint32_t)kv[i].u);
        else if (kv[i].type == T_U64)
            put_u64(&b, kv[i].u);
        else
            put_str(&b, kv[i].s);
    }
    for (int i = 0; i < nb; i++) {
        put_str(&b, t[i].name);
        put_u32(&b, (uint32_t)t[i].nb_dims);
        for (int d = 0; d < t[i].nb_dims; d++)
            put_u64(&b, t[i].ne[d]);
        put_u32(&b, t[i].type);
        put_u64(&b, t[i].offset);
    }
    uint64_t start = align ? RX_ALIGN((uint64_t)b.n, align) : b.n;
    if (data_offset)
        *data_offset = start;

    int fd = memfd_create("gguf", MFD_CLOEXEC);
    if (fd < 0 || write(fd, b.p, b.n) != (ssize_t)b.n ||
        ftruncate(fd, (off_t)(start + data_size))) {
        perror("memfd");
        exit(1);
    }
    free(b.p);
    return fd;
}

static int open_fd(int fd, MLGguf *f)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
    return ml_gguf_open(f, path);
}

/* ml_gguf_open on these tensors; the file holds `data_size` bytes of data
 * (0: exactly what the layout needs). */
static int try_open(const KV *kv, int nb_kv, Tensor *t, int nb, uint64_t data_size,
                    uint64_t align)
{
    uint64_t need = lay_out(t, nb, align ? align : 32);
    int fd = write_gguf(kv, nb_kv, t, nb, data_size ? data_size : need, align ? align : 32, NULL);
    MLGguf f;
    int ret = open_fd(fd, &f);
    if (ret >= 0)
        ml_gguf_close(&f, NULL);
    close(fd);
    return ret;
}

#define AUTO UINT64_MAX

static Tensor tensor1(const char *name, uint32_t type, uint64_t ne0, uint64_t offset)
{
    Tensor t = { .nb_dims = 1, .ne = { ne0, 1, 1, 1 }, .type = type, .offset = offset };
    snprintf(t.name, sizeof(t.name), "%s", name);
    return t;
}

/* ------------------------------------------------------------ the header */

static void test_header(void)
{
    /* A good file first: the rest differ from it in one thing each. */
    {
        Tensor t[2] = { tensor1("a", ML_TYPE_F32, 4, AUTO), tensor1("b", ML_TYPE_F16, 32, AUTO) };
        lay_out(t, 2, 32);
        int fd = write_gguf(NULL, 0, t, 2, 128, 32, NULL);
        MLGguf f;
        int ret = open_fd(fd, &f);
        expect("a good file is read", ret, 0);
        if (ret >= 0) {
            expect("  its sizes", (int)(f.tensors[0].size + f.tensors[1].size), 16 + 64);
            ml_gguf_close(&f, NULL);
        }
        close(fd);
    }

    static const uint64_t aligns[] = { 0, 3, 48, UINT64_C(1) << 40 };
    for (size_t i = 0; i < RX_ARRAY_ELEMS(aligns); i++) {
        KV kv = { "general.alignment", aligns[i] >> 32 ? T_U64 : T_U32, aligns[i], NULL };
        Tensor t = tensor1("a", ML_TYPE_F32, 4, 0);
        char what[64];
        snprintf(what, sizeof(what), "alignment %llu is refused", (unsigned long long)aligns[i]);
        expect(what, try_open(&kv, 1, &t, 1, 4096, 32), RX_ERROR(EINVAL));
    }

    static const uint64_t dims[] = { 0, UINT64_C(1) << 63, UINT64_MAX };
    for (size_t i = 0; i < RX_ARRAY_ELEMS(dims); i++) {
        Tensor t = tensor1("a", ML_TYPE_F32, dims[i], 0);
        char what[64];
        snprintf(what, sizeof(what), "a dimension of %llu is refused", (unsigned long long)dims[i]);
        expect(what, try_open(NULL, 0, &t, 1, 4096, 0), RX_ERROR(EINVAL));
    }

    {
        /* 2^31 cubed elements: 2^93, which wraps in 64 bits. */
        Tensor t = { .name = "a", .nb_dims = 3, .ne = { 1u << 31, 1u << 31, 1u << 31, 1 },
                     .type = ML_TYPE_F32, .offset = 0 };
        expect("a size that overflows is refused", try_open(NULL, 0, &t, 1, 4096, 0),
               RX_ERROR(EINVAL));
        /* 2^32 F16 halves: 8 GB, in range but past a segment's 1 GB. */
        Tensor u = { .name = "a", .nb_dims = 2, .ne = { 1u << 16, 1u << 16, 1, 1 },
                     .type = ML_TYPE_F16, .offset = 0 };
        expect("a tensor of more than 1 GB is refused", try_open(NULL, 0, &u, 1, 0, 0),
               RX_ERROR(EINVAL));
        /* A Q4_K row shorter than a block. */
        Tensor q = { .name = "a", .nb_dims = 2, .ne = { 128, 4, 1, 1 }, .type = ML_TYPE_Q4_K };
        expect("a Q4_K row of 128 is refused", try_open(NULL, 0, &q, 1, 4096, 0),
               RX_ERROR(EINVAL));
    }

    {
        /* An offset that, added to the data's start, wraps around to just
         * past it: the old check (start + offset + size > end) let it in. */
        Tensor t = tensor1("a", ML_TYPE_F32, 4, 0);
        uint64_t start;
        int fd = write_gguf(NULL, 0, &t, 1, 4096, 32, &start);
        close(fd);
        t.offset = (uint64_t)0 - start;
        expect("an offset that wraps is refused", try_open(NULL, 0, &t, 1, 4096, 32),
               RX_ERROR(EINVAL));
    }

    {
        Tensor t = tensor1("a", ML_TYPE_F32, 64, 0);
        expect("a tensor past the end is refused", try_open(NULL, 0, &t, 1, 128, 32),
               RX_ERROR(EINVAL));
    }

    {
        /* b inside a: uploaded, the smaller would have cut a's segment. */
        Tensor t[2] = { tensor1("a", ML_TYPE_F32, 64, 0), tensor1("b", ML_TYPE_F32, 8, 64) };
        expect("overlapping tensors are refused", try_open(NULL, 0, t, 2, 4096, 32),
               RX_ERROR(EINVAL));
    }

    {
        Tensor t[2] = { tensor1("a", ML_TYPE_F32, 4, 0), tensor1("b", ML_TYPE_F32, 4, 16) };
        expect("an offset off the alignment is refused", try_open(NULL, 0, t, 2, 4096, 32),
               RX_ERROR(EINVAL));
        KV kv = { "general.alignment", T_U32, 8, NULL };
        Tensor u[2] = { tensor1("a", ML_TYPE_F32, 2, 0), tensor1("b", ML_TYPE_F32, 2, 8) };
        expect("an offset off the GPU's 16 bytes is refused", try_open(&kv, 1, u, 2, 4096, 8),
               RX_ERROR(EINVAL));
    }
}

/* --------------------------------------------------------- the model's table */

/* Every tensor MOSS-Audio-4B has, with its shape in the real file
 * (models/moss-audio-4b-instruct-q4_k.gguf). */
static int moss_table(Tensor *t)
{
    int nb = 0;
#define ADD(type_, n0, n1, n2, n3, ...) do { \
        Tensor *x = &t[nb++]; \
        *x = (Tensor) { .type = type_, .ne = { n0, n1, n2, n3 }, .offset = AUTO }; \
        x->nb_dims = n3 > 1 ? 4 : n2 > 1 ? 3 : n1 > 1 ? 2 : 1; \
        snprintf(x->name, sizeof(x->name), __VA_ARGS__); \
    } while (0)
    const uint32_t F32 = ML_TYPE_F32, F16 = ML_TYPE_F16, Q4K = ML_TYPE_Q4_K;
    ADD(F32, 128, 201, 1, 1, "audio.mel_filters");
    ADD(F32, 400, 1, 1, 1, "audio.mel_window");
    ADD(F16, 3, 3, 1, 480, "enc.conv1.weight");
    for (int i = 1; i <= 3; i++)
        ADD(F32, 480, 1, 1, 1, "enc.conv%d.bias", i);
    for (int i = 2; i <= 3; i++)
        ADD(F16, 3, 3, 480, 480, "enc.conv%d.weight", i);
    ADD(F16, 7680, 1280, 1, 1, "enc.stem_proj.weight");
    ADD(F32, 1280, 1, 1, 1, "enc.stem_proj.bias");
    ADD(F32, 1280, 1, 1, 1, "enc.norm.weight");
    ADD(F32, 1280, 1, 1, 1, "enc.norm.bias");
    for (int i = 0; i < 32; i++) {
        static const char *const w[] = { "attn.q", "attn.k", "attn.v", "attn.o" };
        for (int k = 0; k < 4; k++)
            ADD(F16, 1280, 1280, 1, 1, "enc.blk.%d.%s.weight", i, w[k]);
        ADD(F32, 1280, 1, 1, 1, "enc.blk.%d.attn.q.bias", i);
        ADD(F32, 1280, 1, 1, 1, "enc.blk.%d.attn.v.bias", i);
        ADD(F32, 1280, 1, 1, 1, "enc.blk.%d.attn.o.bias", i);
        ADD(F32, 1280, 1, 1, 1, "enc.blk.%d.attn_norm.weight", i);
        ADD(F32, 1280, 1, 1, 1, "enc.blk.%d.attn_norm.bias", i);
        ADD(F32, 1280, 1, 1, 1, "enc.blk.%d.ffn_norm.weight", i);
        ADD(F32, 1280, 1, 1, 1, "enc.blk.%d.ffn_norm.bias", i);
        ADD(F16, 1280, 5120, 1, 1, "enc.blk.%d.ffn.fc1.weight", i);
        ADD(F32, 5120, 1, 1, 1, "enc.blk.%d.ffn.fc1.bias", i);
        ADD(F16, 5120, 1280, 1, 1, "enc.blk.%d.ffn.fc2.weight", i);
        ADD(F32, 1280, 1, 1, 1, "enc.blk.%d.ffn.fc2.bias", i);
    }
    for (int i = -1; i < 3; i++) {
        char pre[32];
        snprintf(pre, sizeof(pre), i < 0 ? "adapter" : "deepstack.%d", i);
        ADD(F16, 1280, 8192, 1, 1, "%s.gate.weight", pre);
        ADD(F16, 1280, 8192, 1, 1, "%s.up.weight", pre);
        ADD(F16, 8192, 2560, 1, 1, "%s.down.weight", pre);
    }
    ADD(Q4K, 2560, 151936, 1, 1, "llm.embed.weight");
    ADD(F32, 2560, 1, 1, 1, "llm.final_norm.weight");
    ADD(Q4K, 2560, 151936, 1, 1, "llm.lm_head.weight");
    for (int i = 0; i < 36; i++) {
        ADD(F32, 2560, 1, 1, 1, "llm.blk.%d.attn_norm.weight", i);
        ADD(Q4K, 2560, 4096, 1, 1, "llm.blk.%d.attn.q.weight", i);
        ADD(Q4K, 2560, 1024, 1, 1, "llm.blk.%d.attn.k.weight", i);
        ADD(Q4K, 2560, 1024, 1, 1, "llm.blk.%d.attn.v.weight", i);
        ADD(Q4K, 4096, 2560, 1, 1, "llm.blk.%d.attn.o.weight", i);
        ADD(F32, 128, 1, 1, 1, "llm.blk.%d.attn.q_norm.weight", i);
        ADD(F32, 128, 1, 1, 1, "llm.blk.%d.attn.k_norm.weight", i);
        ADD(F32, 2560, 1, 1, 1, "llm.blk.%d.ffn_norm.weight", i);
        ADD(Q4K, 2560, 9728, 1, 1, "llm.blk.%d.ffn.gate.weight", i);
        ADD(Q4K, 2560, 9728, 1, 1, "llm.blk.%d.ffn.up.weight", i);
        ADD(Q4K, 9728, 2560, 1, 1, "llm.blk.%d.ffn.down.weight", i);
    }
#undef ADD
    return nb;
}

#define NB_MOSS 903

typedef struct Log {
    char text[8192];
    size_t len;
} Log;

static void log_sink(void *opaque, enum RXLogLevel level, const char *line)
{
    Log *l = opaque;
    if (level == RX_LOG_ERROR) {
        int n = snprintf(l->text + l->len, sizeof(l->text) - l->len, "%s\n", line);
        if (n > 0)
            l->len = RX_MIN(l->len + (size_t)n, sizeof(l->text) - 1);
    }
}

/* rx_ml_open on the model's table with `edit` applied to one tensor;
 * returns what it returned and what it logged. */
static int try_model(const char *name, void (*edit)(Tensor *), Log *log)
{
    static Tensor t[NB_MOSS];
    int nb = moss_table(t);
    if (nb != NB_MOSS) {
        printf("the table has %d tensors, not %d\n", nb, NB_MOSS);
        exit(1);
    }
    for (int i = 0; i < nb && name; i++) {
        if (!strcmp(t[i].name, name))
            edit(&t[i]);
    }
    uint64_t need = lay_out(t, nb, 32);

    static const struct { const char *key; uint64_t v; } shape[] = {
        { "moss_audio.enc.num_mel_bins", 128 },
        { "moss_audio.enc.encoder_layers", 32 },
        { "moss_audio.enc.d_model", 1280 },
        { "moss_audio.enc.encoder_attention_heads", 20 },
        { "moss_audio.enc.encoder_ffn_dim", 5120 },
        { "moss_audio.enc.downsample_hidden_size", 480 },
        { "moss_audio.adapter.hidden_size", 8192 },
        { "moss_audio.llm.hidden_size", 2560 },
        { "moss_audio.llm.num_layers", 36 },
        { "moss_audio.llm.num_heads", 32 },
        { "moss_audio.llm.num_kv_heads", 8 },
        { "moss_audio.llm.head_dim", 128 },
        { "moss_audio.llm.intermediate_size", 9728 },
        { "moss_audio.llm.vocab_size", 151936 },
        { "moss_audio.deepstack.num_taps", 3 },
        { "moss_audio.deepstack.tap.0", 8 },
        { "moss_audio.deepstack.tap.1", 16 },
        { "moss_audio.deepstack.tap.2", 24 },
        { "moss_audio.audio_token_id", 151654 },
        { "moss_audio.audio_start_id", 151669 },
        { "moss_audio.audio_end_id", 151670 },
    };
    KV kv[RX_ARRAY_ELEMS(shape) + 1];
    kv[0] = (KV) { "general.architecture", T_STRING, 0, "moss_audio" };
    for (size_t i = 0; i < RX_ARRAY_ELEMS(shape); i++)
        kv[i + 1] = (KV) { shape[i].key, T_U32, shape[i].v, NULL };

    /* 4 GB of tensors, all of it a hole in the memfd. */
    int fd = write_gguf(kv, RX_ARRAY_ELEMS(kv), t, nb, need, 32, NULL);
    char path[64];
    snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
    *log = (Log) { 0 };
    rx_log_set_sink(log_sink, log);
    RXML *ml = NULL;
    int ret = rx_ml_open(&ml, path, NULL);
    rx_log_set_sink(NULL, NULL);
    rx_ml_close(&ml);
    close(fd);
    return ret;
}

static void conv1_in_2(Tensor *t) { t->ne[2] = 2; t->nb_dims = 4; }
static void lm_head_3d(Tensor *t) { t->ne[2] = 2; t->nb_dims = 3; }
static void down_f16(Tensor *t) { t->type = ML_TYPE_F16; }
static void q_norm_short(Tensor *t) { t->ne[0] = 64; }

static void test_model(void)
{
    /* The table as it is: refused only for the tokenizer it does not have
     * (silently, ml_tok_create), after every tensor passed. */
    Log log;
    int ret = try_model(NULL, NULL, &log);
    expect("the model's table passes its checks", ret == RX_ERROR(EINVAL) && !log.len, true);
    if (log.len)
        printf("%s", log.text);

    static const struct {
        const char *name;
        void (*edit)(Tensor *);
    } bad[] = {
        { "enc.conv1.weight", conv1_in_2 },
        { "llm.lm_head.weight", lm_head_3d },
        { "llm.blk.35.ffn.down.weight", down_f16 },
        { "llm.blk.7.attn.q_norm.weight", q_norm_short },
    };
    for (size_t i = 0; i < RX_ARRAY_ELEMS(bad); i++) {
        ret = try_model(bad[i].name, bad[i].edit, &log);
        char what[96];
        snprintf(what, sizeof(what), "a wrong %s is refused", bad[i].name);
        bool named = strstr(log.text, bad[i].name) != NULL;
        expect(what, ret == RX_ERROR(EINVAL) && named, true);
        if (!named)
            printf("%s", log.text);
    }
}

int main(void)
{
    test_header();
    test_model();
    printf("%s\n", failures ? "FAILED" : "all ok");
    return failures ? 1 : 0;
}
