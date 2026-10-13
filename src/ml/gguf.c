/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* GGUF (ggml's file format, version 3): the header, the metadata and the
 * tensor table are read into memory; the tensors' data (4 GB) is streamed
 * onto the GPU and never kept on the host.
 *
 * Choices, and why:
 *  - The data is read with O_DIRECT, in 64 MB pieces, into a page-aligned
 *    bounce buffer, and copied from there into the staging buffer the GPU
 *    copies from: a driver's mapping of GPU-visible memory cannot be the
 *    target of direct I/O (the read fails with EFAULT). O_DIRECT keeps the
 *    file out of the page cache: a buffered read would leave 4 GB of it
 *    there, on a machine that may not have it to spare. Where the file
 *    system refuses O_DIRECT, the reads go straight into the staging
 *    buffer and the pages are dropped after each piece
 *    (POSIX_FADV_DONTNEED). The GPU's copy of a piece takes a tenth of its
 *    read, so the two are not overlapped.
 *  - On the GPU the tensors keep the file's layout: buffers of at most
 *    1 GB (well under every device's maxBufferSize), each a copy of a run
 *    of consecutive tensors. A tensor's address is its segment's plus its
 *    offset in it; nothing is repacked, so a piece of the file is one
 *    copy.
 *  - The header is not trusted: the GPU reads a tensor wherever its offset
 *    and size say, with no bounds check of its own, so every size is
 *    checked for overflow, every tensor must lie inside the file, apart
 *    from the others and on the file's alignment (a power of two, as ggml's
 *    gguf.cpp requires), and moss.c checks each one's shape and size
 *    against the model's before it is used. */

#include "ml_priv.h"

#include <fcntl.h>
#include <stdckdint.h>
#include <stdio.h>
#include <unistd.h>

#define SEG_MAX    (UINT64_C(1) << 30)
#define PIECE      (UINT64_C(64) << 20)
#define DIRECT_ALIGN 4096
/* general.alignment: ggml writes 32. More than this is no alignment
 * anything needs, only a way to make the arithmetic overflow. */
#define MAX_ALIGN  (UINT64_C(1) << 20)
/* The kernels read the weights 16 bytes at a time (shaders/common.glsl,
 * U32x4 and F32x4), at the tensor's offset from its segment's start. */
#define GPU_ALIGN  16

enum {
    GGUF_U8 = 0, GGUF_I8, GGUF_U16, GGUF_I16, GGUF_U32, GGUF_I32, GGUF_F32, GGUF_BOOL,
    GGUF_STRING, GGUF_ARRAY, GGUF_U64, GGUF_I64, GGUF_F64,
};

/* ----------------------------------------------------------------- header */

/* The header is read through a small buffer, sequentially. */
typedef struct Reader {
    int fd;
    uint64_t pos;               /* file offset of buf[0] */
    uint8_t buf[1 << 16];
    size_t len, off;
} Reader;

static int rd(Reader *r, void *dst, size_t size)
{
    uint8_t *d = dst;
    while (size) {
        if (r->off == r->len) {
            r->pos += r->len;
            ssize_t got = pread(r->fd, r->buf, sizeof(r->buf), (off_t)r->pos);
            if (got <= 0)
                return RX_ERROR(EINVAL);
            r->len = (size_t)got;
            r->off = 0;
        }
        size_t n = RX_MIN(size, r->len - r->off);
        memcpy(d, r->buf + r->off, n);
        r->off += n;
        d += n;
        size -= n;
    }
    return 0;
}

static int rd_u32(Reader *r, uint32_t *v) { return rd(r, v, 4); }
static int rd_u64(Reader *r, uint64_t *v) { return rd(r, v, 8); }

static int rd_str(Reader *r, char **out)
{
    uint64_t n;
    int ret = rd_u64(r, &n);
    if (ret < 0)
        return ret;
    if (n > (UINT64_C(1) << 24))
        return RX_ERROR(EINVAL);
    char *s = malloc(n + 1);
    if (!s)
        return RX_ERROR(ENOMEM);
    if ((ret = rd(r, s, n)) < 0) {
        free(s);
        return ret;
    }
    s[n] = 0;
    *out = s;
    return 0;
}

static int scalar_size(uint32_t type)
{
    switch (type) {
    case GGUF_U8: case GGUF_I8: case GGUF_BOOL: return 1;
    case GGUF_U16: case GGUF_I16:               return 2;
    case GGUF_U32: case GGUF_I32: case GGUF_F32: return 4;
    case GGUF_U64: case GGUF_I64: case GGUF_F64: return 8;
    default:                                     return 0;
    }
}

static int rd_scalar(Reader *r, uint32_t type, MLValue *v)
{
    uint8_t b[8] = { 0 };
    int size = scalar_size(type);
    if (!size)
        return RX_ERROR(EINVAL);
    int ret = rd(r, b, size);
    if (ret < 0)
        return ret;

    v->type = ML_GGUF_INT;
    switch (type) {
    case GGUF_U8:   v->i = b[0]; break;
    case GGUF_I8:   v->i = (int8_t)b[0]; break;
    case GGUF_BOOL: v->i = b[0] != 0; break;
    case GGUF_U16:  { uint16_t x; memcpy(&x, b, 2); v->i = x; break; }
    case GGUF_I16:  { int16_t x;  memcpy(&x, b, 2); v->i = x; break; }
    case GGUF_U32:  { uint32_t x; memcpy(&x, b, 4); v->i = x; break; }
    case GGUF_I32:  { int32_t x;  memcpy(&x, b, 4); v->i = x; break; }
    case GGUF_U64:  { uint64_t x; memcpy(&x, b, 8); v->i = (int64_t)x; break; }
    case GGUF_I64:  { int64_t x;  memcpy(&x, b, 8); v->i = x; break; }
    case GGUF_F32:  { float x;    memcpy(&x, b, 4); v->f = x; v->type = ML_GGUF_FLOAT; break; }
    case GGUF_F64:  { double x;   memcpy(&x, b, 8); v->f = x; v->type = ML_GGUF_FLOAT; break; }
    }
    if (v->type == ML_GGUF_INT)
        v->f = (double)v->i;
    return 0;
}

static int rd_value(Reader *r, MLValue *v)
{
    uint32_t type;
    int ret = rd_u32(r, &type);
    if (ret < 0)
        return ret;

    if (type == GGUF_STRING) {
        v->type = ML_GGUF_STRING;
        return rd_str(r, &v->s);
    }
    if (type != GGUF_ARRAY)
        return rd_scalar(r, type, v);

    uint32_t et;
    uint64_t n;
    if ((ret = rd_u32(r, &et)) < 0 || (ret = rd_u64(r, &n)) < 0)
        return ret;
    if (et == GGUF_STRING) {
        if (n > (UINT64_C(1) << 24))
            return RX_ERROR(EINVAL);
        v->type = ML_GGUF_STRINGS;
        v->strs = calloc(RX_MAX(n, 1), sizeof(*v->strs));
        if (!v->strs)
            return RX_ERROR(ENOMEM);
        for (uint64_t i = 0; i < n; i++) {
            if ((ret = rd_str(r, &v->strs[i])) < 0)
                return ret;
            v->nb_strs = (int64_t)i + 1;
        }
        return 0;
    }

    /* Arrays of numbers: none the model needs; skipped. */
    int size = scalar_size(et);
    if (!size)
        return RX_ERROR(EINVAL);
    v->type = ML_GGUF_OTHER;
    for (uint64_t i = 0; i < n; i++) {
        uint8_t b[8];
        if ((ret = rd(r, b, size)) < 0)
            return ret;
    }
    return 0;
}

/* The tensor's bytes, or an error if they do not fit in 64 bits. */
static int tensor_size(const MLTensor *t, uint64_t *size)
{
    uint64_t n = 1;
    for (int i = 0; i < t->nb_dims; i++) {
        if (ckd_mul(&n, n, (uint64_t)t->ne[i]))
            return RX_ERROR(EINVAL);
    }
    uint64_t per = 4, block = 1;
    switch (t->type) {
    case ML_TYPE_F32:  per = 4; break;
    case ML_TYPE_F16:  per = 2; break;
    case ML_TYPE_Q4_K: per = ML_Q4K_BYTES; block = ML_QK_K; break;
    }
    return ckd_mul(size, n / block, per) ? RX_ERROR(EINVAL) : 0;
}

static int cmp_offset(const void *a, const void *b)
{
    const MLTensor *x = *(const MLTensor *const *)a, *y = *(const MLTensor *const *)b;
    return x->offset < y->offset ? -1 : x->offset > y->offset;
}

/* The tensors' place in the file: on the alignment, inside the file, apart
 * from each other. */
static int check_layout(MLGguf *f, const char *path, uint64_t align)
{
    off_t end = lseek(f->fd, 0, SEEK_END);
    if (end < 0 || f->data_offset > (uint64_t)end) {
        rx_err("ml: %s ends before its tensors' data", path);
        return RX_ERROR(EINVAL);
    }
    uint64_t room = (uint64_t)end - f->data_offset;
    for (int i = 0; i < f->nb_tensors; i++) {
        const MLTensor *t = &f->tensors[i];
        if (t->offset % align || t->offset % GPU_ALIGN) {
            rx_err("ml: %s: tensor %s at %llu is not aligned to %llu bytes", path, t->name,
                   (unsigned long long)t->offset,
                   (unsigned long long)RX_MAX(align, (uint64_t)GPU_ALIGN));
            return RX_ERROR(EINVAL);
        }
        /* Compared so that nothing can wrap around. */
        if (t->offset > room || t->size > room - t->offset) {
            rx_err("ml: %s: tensor %s runs past the end of the file", path, t->name);
            return RX_ERROR(EINVAL);
        }
    }

    const MLTensor **order = calloc(RX_MAX(f->nb_tensors, 1), sizeof(*order));
    if (!order)
        return RX_ERROR(ENOMEM);
    for (int i = 0; i < f->nb_tensors; i++)
        order[i] = &f->tensors[i];
    qsort(order, f->nb_tensors, sizeof(*order), cmp_offset);
    int ret = 0;
    for (int i = 1; i < f->nb_tensors && ret >= 0; i++) {
        if (order[i]->offset < order[i - 1]->offset + order[i - 1]->size) {
            rx_err("ml: %s: tensors %s and %s overlap", path, order[i - 1]->name, order[i]->name);
            ret = RX_ERROR(EINVAL);
        }
    }
    free(order);
    return ret;
}

int ml_gguf_open(MLGguf *f, const char *path)
{
    *f = (MLGguf) { .fd = -1 };
    Reader *r = calloc(1, sizeof(*r));
    if (!r)
        return RX_ERROR(ENOMEM);

    int ret = 0;
    f->fd = open(path, O_RDONLY | O_CLOEXEC);
    if (f->fd < 0) {
        ret = RX_ERROR(errno);
        goto fail;
    }
    r->fd = f->fd;

    char magic[4];
    uint32_t version;
    uint64_t nb_tensors, nb_values;
    if ((ret = rd(r, magic, 4)) < 0 || (ret = rd_u32(r, &version)) < 0 ||
        (ret = rd_u64(r, &nb_tensors)) < 0 || (ret = rd_u64(r, &nb_values)) < 0)
        goto fail;
    if (memcmp(magic, "GGUF", 4) || version < 2 || version > 3 ||
        nb_tensors > 65536 || nb_values > 65536) {
        rx_err("ml: %s is not a GGUF file this reads", path);
        ret = RX_ERROR(EINVAL);
        goto fail;
    }

    f->values = calloc(RX_MAX(nb_values, 1), sizeof(*f->values));
    f->tensors = calloc(RX_MAX(nb_tensors, 1), sizeof(*f->tensors));
    if (!f->values || !f->tensors) {
        ret = RX_ERROR(ENOMEM);
        goto fail;
    }

    for (uint64_t i = 0; i < nb_values; i++) {
        MLValue *v = &f->values[i];
        f->nb_values = (int)i + 1;
        if ((ret = rd_str(r, &v->key)) < 0 || (ret = rd_value(r, v)) < 0)
            goto fail;
    }

    for (uint64_t i = 0; i < nb_tensors; i++) {
        MLTensor *t = &f->tensors[i];
        f->nb_tensors = (int)i + 1;
        uint32_t nb_dims, type;
        if ((ret = rd_str(r, &t->name)) < 0 || (ret = rd_u32(r, &nb_dims)) < 0)
            goto fail;
        if (nb_dims < 1 || nb_dims > 4) {
            ret = RX_ERROR(EINVAL);
            goto fail;
        }
        t->nb_dims = (int)nb_dims;
        for (int d = 0; d < 4; d++)
            t->ne[d] = 1;
        for (uint32_t d = 0; d < nb_dims; d++) {
            uint64_t ne;
            if ((ret = rd_u64(r, &ne)) < 0)
                goto fail;
            if (!ne || ne > INT64_MAX) {
                rx_err("ml: %s: tensor %s has a dimension of %llu", path, t->name,
                       (unsigned long long)ne);
                ret = RX_ERROR(EINVAL);
                goto fail;
            }
            t->ne[d] = (int64_t)ne;
        }
        if ((ret = rd_u32(r, &type)) < 0 || (ret = rd_u64(r, &t->offset)) < 0)
            goto fail;
        if (type != ML_TYPE_F32 && type != ML_TYPE_F16 && type != ML_TYPE_Q4_K) {
            rx_err("ml: tensor %s has type %u, which this does not read", t->name, type);
            ret = RX_ERROR(ENOTSUP);
            goto fail;
        }
        t->type = (enum MLType)type;
        if (t->type == ML_TYPE_Q4_K && t->ne[0] % ML_QK_K) {
            ret = RX_ERROR(EINVAL);
            goto fail;
        }
        if ((ret = tensor_size(t, &t->size)) < 0 || t->size > SEG_MAX) {
            rx_err("ml: %s: tensor %s is too large", path, t->name);
            ret = RX_ERROR(EINVAL);
            goto fail;
        }
    }

    /* 0 would divide by zero in RX_ALIGN; ggml refuses anything but a
     * power of two. */
    int64_t align = ml_gguf_int(f, "general.alignment", 32);
    if (align < 1 || (uint64_t)align > MAX_ALIGN || (align & (align - 1))) {
        rx_err("ml: %s has an alignment of %lld", path, (long long)align);
        ret = RX_ERROR(EINVAL);
        goto fail;
    }
    uint64_t here = r->pos + r->off;
    f->data_offset = RX_ALIGN(here, (uint64_t)align);
    if ((ret = check_layout(f, path, (uint64_t)align)) < 0)
        goto fail;

    free(r);
    return 0;

fail:
    free(r);
    ml_gguf_close(f, NULL);
    return ret;
}

/* ----------------------------------------------------------------- upload */

/* Reads [off, off + size) of the file into dst (at least size rounded up to
 * DIRECT_ALIGN, page aligned); returns where in dst `off` landed. */
static int read_piece(int fd, bool direct, uint64_t off, uint64_t size, uint8_t *dst,
                      uint64_t *skip)
{
    uint64_t start = direct ? off / DIRECT_ALIGN * DIRECT_ALIGN : off;
    uint64_t want = direct ? RX_ALIGN(off + size - start, DIRECT_ALIGN) : size;
    uint64_t done = 0;
    while (done < want) {
        ssize_t got = pread(fd, dst + done, want - done, (off_t)(start + done));
        if (got < 0) {
            if (errno == EINTR)
                continue;
            return RX_ERROR(errno);
        }
        if (!got)
            break;          /* the end of the file, inside the last block */
        done += (uint64_t)got;
    }
    if (done < off - start + size)
        return RX_ERROR(EIO);
    if (!direct)
        posix_fadvise(fd, (off_t)start, (off_t)want, POSIX_FADV_DONTNEED);
    *skip = off - start;
    return 0;
}

int ml_gguf_upload(MLGguf *f, MLGpu *g)
{
    MLTensor **order = calloc(RX_MAX(f->nb_tensors, 1), sizeof(*order));
    if (!order)
        return RX_ERROR(ENOMEM);
    for (int i = 0; i < f->nb_tensors; i++)
        order[i] = &f->tensors[i];
    qsort(order, f->nb_tensors, sizeof(*order), cmp_offset);

    /* Segments: runs of consecutive tensors of at most SEG_MAX bytes. */
    f->segs = calloc(RX_MAX(f->nb_tensors, 1), sizeof(*f->segs));
    uint64_t *seg_start = calloc(RX_MAX(f->nb_tensors, 1), sizeof(*seg_start));
    uint64_t *seg_end = calloc(RX_MAX(f->nb_tensors, 1), sizeof(*seg_end));
    int *seg_of = calloc(RX_MAX(f->nb_tensors, 1), sizeof(*seg_of));
    MLBuf staging = { 0 };
    uint8_t *bounce = NULL;
    int ret = 0;
    if (!f->segs || !seg_start || !seg_end || !seg_of) {
        ret = RX_ERROR(ENOMEM);
        goto end;
    }
    for (int i = 0; i < f->nb_tensors; i++) {
        const MLTensor *t = order[i];
        int s = f->nb_segs - 1;
        if (s < 0 || t->offset + t->size - seg_start[s] > SEG_MAX) {
            s = f->nb_segs++;
            seg_start[s] = t->offset;
        }
        seg_end[s] = RX_MAX(seg_end[s], t->offset + t->size);
        seg_of[i] = s;
    }
    for (int s = 0; s < f->nb_segs; s++) {
        if ((ret = ml_buf_create(g, &f->segs[s], RX_ALIGN(seg_end[s] - seg_start[s], 256),
                                 ML_MEM_DEVICE)) < 0)
            goto end;
    }
    for (int i = 0; i < f->nb_tensors; i++)
        order[i]->addr = f->segs[seg_of[i]].addr + (order[i]->offset - seg_start[seg_of[i]]);

    if ((ret = ml_buf_create(g, &staging, PIECE + 2 * DIRECT_ALIGN, ML_MEM_UPLOAD)) < 0)
        goto end;

    char fdpath[64];
    snprintf(fdpath, sizeof(fdpath), "/proc/self/fd/%d", f->fd);
    int fd = open(fdpath, O_RDONLY | O_DIRECT | O_CLOEXEC);
    bool direct = fd >= 0 &&
                  !posix_memalign((void **)&bounce, DIRECT_ALIGN, PIECE + 2 * DIRECT_ALIGN);
    if (!direct) {
        if (fd >= 0)
            close(fd);
        fd = f->fd;
        rx_debug("ml: no O_DIRECT, reading the weights buffered");
    }

    /* Each segment, a piece at a time. */
    int64_t t0 = rx_time_us();
    uint64_t total = 0;
    for (int s = 0; s < f->nb_segs && ret >= 0; s++) {
        for (uint64_t off = seg_start[s]; off < seg_end[s]; off += PIECE) {
            uint64_t size = RX_MIN(PIECE, seg_end[s] - off);
            uint64_t skip = 0;
            ret = read_piece(fd, direct, f->data_offset + off, size,
                             direct ? bounce : staging.map, &skip);
            if (ret < 0)
                break;
            if (direct) {
                memcpy(staging.map, bounce + skip, size);
                skip = 0;
            }
            if ((ret = ml_buf_flush(g, &staging, skip + size)) < 0)
                break;
            ml_gpu_copy(g, &staging, skip, &f->segs[s], off - seg_start[s], size);
            if ((ret = ml_gpu_run(g)) < 0)
                break;
            total += size;
        }
    }
    if (fd != f->fd)
        close(fd);
    if (ret >= 0) {
        double sec = (rx_time_us() - t0) * 1e-6;
        rx_debug("ml: %.2f GB of weights onto the GPU in %.1f s%s", total * 1e-9, sec,
                 direct ? "" : " (buffered)");
    }

end:
    ml_buf_free(g, &staging);
    free(bounce);
    free(seg_of);
    free(seg_end);
    free(seg_start);
    free(order);
    return ret;
}

void ml_gguf_close(MLGguf *f, MLGpu *g)
{
    if (g) {
        for (int s = 0; s < f->nb_segs; s++)
            ml_buf_free(g, &f->segs[s]);
    }
    free(f->segs);
    for (int i = 0; i < f->nb_values; i++) {
        MLValue *v = &f->values[i];
        free(v->key);
        free(v->s);
        for (int64_t j = 0; j < v->nb_strs; j++)
            free(v->strs[j]);
        free(v->strs);
    }
    free(f->values);
    for (int i = 0; i < f->nb_tensors; i++)
        free(f->tensors[i].name);
    free(f->tensors);
    if (f->fd >= 0)
        close(f->fd);
    *f = (MLGguf) { .fd = -1 };
}

/* ----------------------------------------------------------------- lookups */

const MLValue *ml_gguf_value(const MLGguf *f, const char *key)
{
    for (int i = 0; i < f->nb_values; i++) {
        if (!strcmp(f->values[i].key, key))
            return &f->values[i];
    }
    return NULL;
}

int64_t ml_gguf_int(const MLGguf *f, const char *key, int64_t def)
{
    const MLValue *v = ml_gguf_value(f, key);
    return v && v->type == ML_GGUF_INT ? v->i : def;
}

double ml_gguf_float(const MLGguf *f, const char *key, double def)
{
    const MLValue *v = ml_gguf_value(f, key);
    return v && (v->type == ML_GGUF_FLOAT || v->type == ML_GGUF_INT) ? v->f : def;
}

const MLTensor *ml_gguf_tensor(const MLGguf *f, const char *name)
{
    for (int i = 0; i < f->nb_tensors; i++) {
        if (!strcmp(f->tensors[i].name, name))
            return &f->tensors[i];
    }
    return NULL;
}
