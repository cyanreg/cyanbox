/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* Private to the model (rxbox/ml.h): the GPU it runs on (gpu.c), the GGUF
 * file its weights come from (gguf.c), the kernels (shaders/, their push
 * constants in kernels.h), the tokenizer (tokenizer.c), the network
 * (moss.c) and the tags made from its answers (tag.c).
 *
 * The model owns its VkDevice rather than share rxbox/compute.h's: that
 * context is made for the beat analysis' STFT alone and torn down after
 * it, where these weights (4 GB) stay on the GPU for as long as the model
 * is open, and the shaders need 16-bit floats and 8-bit storage, which the
 * compute context does not enable. */

#ifndef RXBOX_ML_PRIV_H
#define RXBOX_ML_PRIV_H

#include "rxbox/ml.h"

#include <stdatomic.h>

#include <vulkan/vulkan.h>

/* VMA tests extension macros that only exist on Windows with #if. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wundef"
#include "vk_mem_alloc.h"
#pragma GCC diagnostic pop

/* ------------------------------------------------------------------ the GPU */

/* Every kernel takes its buffers as device addresses in its push
 * constants, at most 128 bytes: what every device has. */
#define ML_PUSH_SIZE 128

enum MLKernel {
    ML_K_MEL,                   /* shaders/mel.comp */
    ML_K_MEL_NORM,
    ML_K_CONV1,                 /* shaders/conv1.comp */
    ML_K_CONV_PERMUTE,
    ML_K_STEM_PERMUTE,
    ML_K_GEMM_F16,              /* shaders/gemm.comp, F16 weights */
    ML_K_GEMM_F16_CONV,         /*   the same, an im2col gather of a 3x3 convolution */
    ML_K_GEMM_Q4K,              /*   Q4_K weights */
    ML_K_GEMM_Q4K_SMALL,        /*   the same in tiles of 32 rows, for a few tokens */
    ML_K_GEMV_Q4K,              /* shaders/gemv.comp: a few rows of Q4_K weights */
    ML_K_LAYERNORM,             /* shaders/norm.comp */
    ML_K_RMSNORM,
    ML_K_ACT,                   /* shaders/act.comp: GELU, SwiGLU, additions */
    ML_K_ENC_ATTN,              /* shaders/enc_attn.comp */
    ML_K_QK_ROPE,               /* shaders/rope.comp */
    ML_K_ATTN,                  /* shaders/attn.comp: causal, KV cache */
    ML_K_ATTN_FCOMB,
    ML_K_ATTN_DEC,              /* shaders/attn_dec.comp: one or two tokens */
    ML_K_ATTN_COMB,
    ML_K_EMBED,                 /* shaders/embed.comp */
    ML_K_ARGMAX,                /* shaders/argmax.comp */
    ML_NB_KERNELS,
};

/* A buffer with its device address; mapped when host visible. */
typedef struct MLBuf {
    VkBuffer        buf;
    VmaAllocation   alloc;
    VkDeviceSize    size;
    VkDeviceAddress addr;
    void           *map;
} MLBuf;

enum MLMem {
    ML_MEM_DEVICE,              /* device local, never mapped */
    ML_MEM_UPLOAD,              /* host memory written sequentially by the CPU */
    ML_MEM_READBACK,            /* host memory, cached, that the GPU writes */
};

typedef struct MLGpu {
    VkInstance inst;
    VkDebugUtilsMessengerEXT messenger;
    atomic_int validation_errors;
    VkPhysicalDevice phys;
    VkPhysicalDeviceProperties props;
    VkDevice dev;
    uint32_t qfam;
    VkQueue queue;
    VmaAllocator vma;
    uint64_t ts_mask;           /* the queue's valid timestamp bits, 0 = none */

    VkSemaphore timeline;
    uint64_t submitted;

    VkPipelineLayout layout;
    VkPipeline pipes[ML_NB_KERNELS];

    /* One command buffer at a time: the model is used by one thread. */
    VkCommandPool pool;
    VkCommandBuffer cmd;
    bool recording;

    /* Per-kernel GPU time, when RXBOX_ML_PROFILE=1: a timestamp after
     * every dispatch, read back at each wait. */
    bool profile;
    VkQueryPool times;
    int nb_times;
    enum MLKernel time_kernel[4096];
    double kernel_ms[ML_NB_KERNELS];
    int64_t kernel_calls[ML_NB_KERNELS];
} MLGpu;

int  ml_gpu_create(MLGpu *g, const char *device, bool validation);
void ml_gpu_destroy(MLGpu *g);

int  ml_buf_create(MLGpu *g, MLBuf *b, VkDeviceSize size, enum MLMem mem);
void ml_buf_free(MLGpu *g, MLBuf *b);
int  ml_buf_invalidate(MLGpu *g, MLBuf *b);
int  ml_buf_flush(MLGpu *g, MLBuf *b, VkDeviceSize size);

/* Recording: dispatches go into the one command buffer, begun on first
 * use; ml_gpu_run submits it and waits. Between dispatches that depend on
 * each other, ml_gpu_barrier. */
void ml_gpu_dispatch(MLGpu *g, enum MLKernel k, const void *push, size_t size,
                     uint32_t x, uint32_t y, uint32_t z);
void ml_gpu_barrier(MLGpu *g);
void ml_gpu_copy(MLGpu *g, const MLBuf *src, VkDeviceSize src_off, const MLBuf *dst,
                 VkDeviceSize dst_off, VkDeviceSize size);
void ml_gpu_fill(MLGpu *g, const MLBuf *dst, VkDeviceSize off, VkDeviceSize size, uint32_t v);
int  ml_gpu_run(MLGpu *g);
void ml_gpu_profile_print(MLGpu *g);

/* ----------------------------------------------------------------- GGUF */

enum MLType {
    ML_TYPE_F32  = 0,
    ML_TYPE_F16  = 1,
    ML_TYPE_Q4_K = 12,
};

/* Q4_K: 256 weights in 144 bytes (d, dmin, 12 bytes of 6-bit scales and
 * mins, 128 bytes of nibbles); ggml's block_q4_K. */
#define ML_QK_K       256
#define ML_Q4K_BYTES  144

typedef struct MLTensor {
    char          *name;
    enum MLType    type;
    int            nb_dims;
    int64_t        ne[4];       /* ne[0] varies fastest: a row */
    uint64_t       offset;      /* in the file */
    uint64_t       size;        /* bytes */
    VkDeviceAddress addr;       /* where it is on the GPU, once uploaded */
} MLTensor;

typedef enum MLValueType {
    ML_GGUF_INT,
    ML_GGUF_FLOAT,
    ML_GGUF_STRING,
    ML_GGUF_STRINGS,
    ML_GGUF_OTHER,
} MLValueType;

typedef struct MLValue {
    char        *key;
    MLValueType  type;
    int64_t      i;
    double       f;
    char        *s;
    char       **strs;          /* ML_GGUF_STRINGS */
    int64_t      nb_strs;
} MLValue;

typedef struct MLGguf {
    int        fd;
    uint64_t   data_offset;     /* where the tensors' data starts */
    MLValue   *values;
    int        nb_values;
    MLTensor  *tensors;
    int        nb_tensors;
    /* The weights on the GPU: device buffers of at most 1 GB, each a copy
     * of a run of the file's tensors. */
    MLBuf     *segs;
    int        nb_segs;
} MLGguf;

/* Reads the header and the tensor table (the metadata, tokenizer
 * included, is kept; the weights are not read). -EINVAL for a table whose
 * tensors are not each inside the file, apart from the others and aligned,
 * or whose sizes overflow. */
int  ml_gguf_open(MLGguf *f, const char *path);
/* Streams every tensor onto the GPU through a small staging buffer: the
 * weights never sit in system memory. */
int  ml_gguf_upload(MLGguf *f, MLGpu *g);
void ml_gguf_close(MLGguf *f, MLGpu *g);
const MLValue *ml_gguf_value(const MLGguf *f, const char *key);
int64_t ml_gguf_int(const MLGguf *f, const char *key, int64_t def);
double ml_gguf_float(const MLGguf *f, const char *key, double def);
const MLTensor *ml_gguf_tensor(const MLGguf *f, const char *name);

/* ------------------------------------------------------------- tokenizer */

typedef struct MLTokenizer MLTokenizer;

/* From the GGUF's tokenizer.ggml.tokens and .merges (Qwen2's byte-level
 * BPE). */
int  ml_tok_create(MLTokenizer **t, const MLGguf *f);
void ml_tok_free(MLTokenizer **t);
/* Appends the tokens of `text` (no special tokens are parsed). */
int  ml_tok_encode(const MLTokenizer *t, const char *text, int32_t **ids, int *nb, int *cap);
/* The bytes of token `id`, appended to *out (NUL-terminated). */
int  ml_tok_decode(const MLTokenizer *t, int32_t id, char **out, size_t *len, size_t *cap);
int32_t ml_tok_id(const MLTokenizer *t, const char *token);

#endif /* RXBOX_ML_PRIV_H */
