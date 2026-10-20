/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* A pipeline (cyanbox/audio.h): a source (source.c) in a thread of its
 * own, which reads chunk after chunk and queues them (fifo.c) for whoever
 * pops them, `ahead` seconds' worth at most, then waits.
 *
 * A chunk is a refstruct object from a pool, its header and its samples
 * in one allocation, which libswresample writes into: taking, holding and
 * dropping a reference allocate nothing, and only the thread that drops a
 * chunk's last reference returns it to the pool (under the pool's mutex).
 *
 * A seek starts a new generation: the queue is drained, a push the thread
 * is blocked in is woken, and a chunk of an older generation is dropped
 * wherever it turns up (in the thread, or queued in the race, which pop
 * drops). */

#include "internal.h"

#include <pthread.h>

#include <libavutil/refstruct.h>

#include "fifo.h"

/* Bytes before a chunk's samples. */
#define HEADER 64

typedef struct Chunk {
    CBChunk  c;                 /* first: a CBChunk is its object */
    uint64_t gen;
} Chunk;

struct CBPipeline {
    CBSource *src;
    CBFifo *fifo;
    AVRefStructPool *pool;
    int chunk, rate, channels;
    size_t plane;               /* bytes of one plane */
    bool planar;
    int64_t length;

    pthread_t thread;
    bool started;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    bool stop, seek_pending, eof;
    int64_t seek_to, next;
    uint64_t gen;
};

static int chunk_init(AVRefStructOpaque opaque, void *obj)
{
    const CBPipeline *p = opaque.nc;
    CBChunk *c = obj;
    c->data[0] = (uint8_t *)obj + HEADER;
    c->data[1] = p->planar && p->channels > 1 ? c->data[0] + p->plane : NULL;
    return 0;
}

CBChunk *cb_chunk_ref(CBChunk *c)
{
    return c ? av_refstruct_ref(c) : NULL;
}

void cb_chunk_unref(CBChunk **c)
{
    av_refstruct_unref(c);
}

/* ------------------------------------------------------------- the thread */

static void *run(void *arg)
{
    CBPipeline *p = arg;
    pthread_mutex_lock(&p->lock);
    while (!p->stop) {
        if (p->seek_pending) {
            int64_t k = p->seek_to;
            uint64_t gen = p->gen;
            p->seek_pending = false;
            pthread_mutex_unlock(&p->lock);
            int ret = cb_source_seek(p->src, k * p->chunk);
            pthread_mutex_lock(&p->lock);
            if (p->gen != gen)
                continue;
            p->next = k;
            p->eof = false;
            if (ret < 0) {
                /* what failed, in a chunk of its own, and nothing after */
                Chunk *e = av_refstruct_pool_get(p->pool);
                if (e) {
                    e->gen = gen;
                    e->c.index = k;
                    e->c.nb = 0;
                    e->c.last = true;
                    e->c.err = ret;
                }
                p->eof = true;
                pthread_mutex_unlock(&p->lock);
                if (e)
                    cb_fifo_push(p->fifo, e);
                av_refstruct_unref(&e);
                pthread_mutex_lock(&p->lock);
            }
            continue;
        }
        if (p->eof) {
            pthread_cond_wait(&p->cond, &p->lock);
            continue;
        }
        uint64_t gen = p->gen;
        int64_t k = p->next;
        pthread_mutex_unlock(&p->lock);

        Chunk *c = av_refstruct_pool_get(p->pool);
        int n = c ? cb_source_read(p->src, c->c.data, p->chunk) : CB_ERROR(ENOMEM);

        pthread_mutex_lock(&p->lock);
        if (p->gen != gen || p->stop) {
            av_refstruct_unref(&c);
            continue;
        }
        if (c) {
            c->gen = gen;
            c->c.index = k;
            c->c.nb = CB_MAX(n, 0);
            c->c.err = CB_MIN(n, 0);
            c->c.last = n < p->chunk;
        }
        p->next = k + 1;
        p->eof = !c || c->c.last;
        pthread_mutex_unlock(&p->lock);
        if (c)
            cb_fifo_push(p->fifo, c);   /* -EAGAIN when a seek or the end woke it */
        av_refstruct_unref(&c);
        pthread_mutex_lock(&p->lock);
    }
    pthread_mutex_unlock(&p->lock);
    return NULL;
}

/* ------------------------------------------------------------------- calls */

int cb_pipeline_open(CBPipeline **out, const char *path, const CBPipelineOptions *o)
{
    *out = NULL;
    if (!o || o->chunk <= 0 || o->ahead < 0 || o->start < 0)
        return CB_ERROR(EINVAL);
    CBPipeline *p = calloc(1, sizeof(*p));
    if (!p)
        return CB_ERROR(ENOMEM);
    pthread_mutex_init(&p->lock, NULL);
    pthread_cond_init(&p->cond, NULL);
    int ret = cb_source_open(&p->src, path, &o->format);
    if (ret < 0)
        goto fail;
    p->chunk = o->chunk;
    p->rate = cb_source_rate(p->src);
    p->channels = cb_source_channels(p->src);
    p->length = cb_source_length(p->src);
    enum AVSampleFormat fmt = cb_source_format(p->src);
    p->planar = av_sample_fmt_is_planar(fmt);
    p->plane = (size_t)o->chunk * av_get_bytes_per_sample(fmt) * (p->planar ? 1 : p->channels);
    p->plane = (p->plane + 63) & ~(size_t)63;

    int queue = (int)CB_MAX(o->ahead * p->rate / o->chunk, 2.0);
    p->fifo = cb_fifo_create(queue, CB_FIFO_BLOCK_MAX_OUTPUT, true);
    p->pool = av_refstruct_pool_alloc_ext(HEADER + p->plane * (p->planar ? p->channels : 1),
                                          AV_REFSTRUCT_POOL_FLAG_NO_ZEROING, p, chunk_init,
                                          NULL, NULL, NULL);
    ret = CB_ERROR(ENOMEM);
    if (!p->fifo || !p->pool)
        goto fail;
    if (o->start && (ret = cb_source_seek(p->src, o->start * o->chunk)) < 0)
        goto fail;
    p->next = o->start;
    if ((ret = pthread_create(&p->thread, NULL, run, p))) {
        ret = CB_ERROR(ret);
        goto fail;
    }
    p->started = true;
    *out = p;
    return 0;

fail:
    cb_pipeline_free(&p);
    return ret;
}

void cb_pipeline_free(CBPipeline **pp)
{
    CBPipeline *p = *pp;
    if (!p)
        return;
    if (p->started) {
        pthread_mutex_lock(&p->lock);
        p->stop = true;
        pthread_cond_signal(&p->cond);
        pthread_mutex_unlock(&p->lock);
        cb_fifo_drain(p->fifo);
        cb_fifo_wake(p->fifo);
        pthread_join(p->thread, NULL);
    }
    cb_fifo_free(&p->fifo);
    av_refstruct_pool_uninit(&p->pool);
    cb_source_free(&p->src);
    pthread_cond_destroy(&p->cond);
    pthread_mutex_destroy(&p->lock);
    free(p);
    *pp = NULL;
}

int cb_pipeline_rate(const CBPipeline *p)
{
    return p->rate;
}

int cb_pipeline_channels(const CBPipeline *p)
{
    return p->channels;
}

int cb_pipeline_chunk(const CBPipeline *p)
{
    return p->chunk;
}

int64_t cb_pipeline_length(const CBPipeline *p)
{
    return p->length;
}

int cb_pipeline_seek(CBPipeline *p, int64_t chunk)
{
    if (chunk < 0)
        return CB_ERROR(EINVAL);
    pthread_mutex_lock(&p->lock);
    p->gen++;
    p->seek_to = chunk;
    p->seek_pending = true;
    pthread_cond_signal(&p->cond);
    pthread_mutex_unlock(&p->lock);
    cb_fifo_drain(p->fifo);
    cb_fifo_wake(p->fifo);
    return 0;
}

CBChunk *cb_pipeline_pop(CBPipeline *p)
{
    while (1) {
        Chunk *c = cb_fifo_pop(p->fifo);
        if (!c)
            return NULL;
        pthread_mutex_lock(&p->lock);
        bool stale = c->gen != p->gen;
        pthread_mutex_unlock(&p->lock);
        if (!stale)
            return &c->c;
        av_refstruct_unref(&c);
    }
}

int cb_pipeline_fd(const CBPipeline *p)
{
    return cb_fifo_fd(p->fifo);
}
