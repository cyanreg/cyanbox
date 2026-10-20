/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* How a track is read: the file mapped read-only (cb_audio_map), the MP3
 * and FLAC scanners walking the mapping, libavformat reading it through
 * an AVIOContext of ours (cb_audio_open).
 *
 * A file cut short while it is mapped makes the pages past its new end
 * raise SIGBUS on the first touch. Every read of a mapping runs under a
 * guard (cb_audio_guarded): a SIGBUS at an address inside the mapping the
 * thread is reading jumps back out of the read, which fails with -EIO, as
 * a read() past the new end would have come back short. Any other SIGBUS
 * goes to whatever handled it before (by default, the process dies). The
 * handler is installed once, the first time a mapping is read.
 *
 * libavformat reads in direct mode: a read the size of a packet or more is
 * copied from the mapping into the packet itself, not through the
 * AVIOContext's buffer, so a packet costs one copy, as with read(). */

#include "internal.h"

#include <fcntl.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

/* The AVIOContext's buffer, for the reads smaller than it. */
#define IO_BUFFER 32768

/* ---------------------------------------------------------------- mapping */

int cb_audio_map(const char *path, CBMap *m)
{
    *m = (CBMap) { 0 };
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return CB_ERROR(errno);
    struct stat st;
    if (fstat(fd, &st) < 0) {
        int err = errno;
        close(fd);
        return CB_ERROR(err);
    }
    if (!S_ISREG(st.st_mode)) {
        close(fd);
        return CB_ERROR(EINVAL);
    }
    if (st.st_size > 0) {
        void *p = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (p == MAP_FAILED) {
            int err = errno;
            close(fd);
            return CB_ERROR(err);
        }
        m->data = p;
        m->size = (size_t)st.st_size;
    }
    close(fd);
    return 0;
}

void cb_audio_unmap(CBMap *m)
{
    if (m->data)
        munmap((void *)m->data, m->size);
    *m = (CBMap) { 0 };
}

/* ------------------------------------------------------------------ guard */

typedef struct Guard {
    sigjmp_buf     jump;
    const uint8_t *lo, *hi;
} Guard;

static _Thread_local Guard *volatile guard;
static struct sigaction old_bus;
static pthread_once_t guard_once = PTHREAD_ONCE_INIT;

static void on_sigbus(int sig, siginfo_t *si, void *uc)
{
    Guard *g = guard;
    const uint8_t *at = si->si_addr;
    if (g && at >= g->lo && at < g->hi)
        siglongjmp(g->jump, 1);
    /* Not a read of ours: the handler before, or the default, which the
     * access faulting again then meets */
    if (old_bus.sa_flags & SA_SIGINFO) {
        old_bus.sa_sigaction(sig, si, uc);
    } else if (old_bus.sa_handler != SIG_DFL && old_bus.sa_handler != SIG_IGN) {
        old_bus.sa_handler(sig);
    } else {
        struct sigaction dfl = { .sa_handler = SIG_DFL };
        sigaction(SIGBUS, &dfl, NULL);
    }
}

static void install(void)
{
    struct sigaction sa = { .sa_sigaction = on_sigbus, .sa_flags = SA_SIGINFO | SA_NODEFER };
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGBUS, &sa, &old_bus) < 0)
        cb_warn("audio: no SIGBUS handler, a file cut short while read stops the program: %s",
                strerror(errno));
}

/* SA_NODEFER leaves SIGBUS unblocked in the handler, so the jump out of it
 * needs no mask restored: sigsetjmp saves none (no system call per read). */
int cb_audio_guarded(const CBMap *m, int (*fn)(void *opaque), void *opaque)
{
    pthread_once(&guard_once, install);
    Guard g = { .lo = m->data, .hi = m->data + m->size };
    Guard *outer = guard;
    if (sigsetjmp(g.jump, 0)) {
        guard = outer;
        return CB_ERROR(EIO);
    }
    guard = &g;
    int ret = fn(opaque);
    guard = outer;
    return ret;
}

/* ------------------------------------------------------------------ input */

typedef struct Input {
    CBMap   map;
    int64_t pos;
} Input;

typedef struct Copy {
    uint8_t       *dst;
    const uint8_t *src;
    size_t         n;
} Copy;

static int copy(void *opaque)
{
    Copy *c = opaque;
    memcpy(c->dst, c->src, c->n);
    return 0;
}

static int read_packet(void *opaque, uint8_t *buf, int size)
{
    Input *in = opaque;
    int64_t left = (int64_t)in->map.size - in->pos;
    if (left <= 0 || size <= 0)
        return AVERROR_EOF;
    Copy c = { .dst = buf, .src = in->map.data + in->pos, .n = (size_t)CB_MIN(left, (int64_t)size) };
    if (cb_audio_guarded(&in->map, copy, &c) < 0)
        return AVERROR(EIO);
    in->pos += (int64_t)c.n;
    return (int)c.n;
}

static int64_t seek(void *opaque, int64_t offset, int whence)
{
    Input *in = opaque;
    int64_t size = (int64_t)in->map.size, to;
    switch (whence & ~AVSEEK_FORCE) {
    case AVSEEK_SIZE:
        return size;
    case SEEK_SET:
        to = offset;
        break;
    case SEEK_CUR:
        to = in->pos + offset;
        break;
    case SEEK_END:
        to = size + offset;
        break;
    default:
        return AVERROR(EINVAL);
    }
    if (to < 0)
        return AVERROR(EINVAL);
    in->pos = to;
    return to;
}

static void input_free(AVIOContext **pb)
{
    if (!*pb)
        return;
    Input *in = (*pb)->opaque;
    av_freep(&(*pb)->buffer);
    avio_context_free(pb);
    if (in) {
        cb_audio_unmap(&in->map);
        free(in);
    }
}

int cb_audio_open(const char *path, AVFormatContext **out, bool find_info)
{
    cb_audio_init_logging();
    *out = NULL;
    Input *in = calloc(1, sizeof(*in));
    if (!in)
        return CB_ERROR(ENOMEM);
    int ret = cb_audio_map(path, &in->map);
    if (ret < 0) {
        cb_debug("%s: %s", path, strerror(-ret));
        free(in);
        return ret;
    }
    uint8_t *buf = av_malloc(IO_BUFFER);
    AVIOContext *pb = buf ? avio_alloc_context(buf, IO_BUFFER, 0, in, read_packet, NULL, seek)
                          : NULL;
    AVFormatContext *fmt = pb ? avformat_alloc_context() : NULL;
    if (!fmt) {
        if (!pb) {
            av_free(buf);
            cb_audio_unmap(&in->map);
            free(in);
        }
        input_free(&pb);
        return CB_ERROR(ENOMEM);
    }
    pb->direct = 1;
    fmt->pb = pb;

    /* The name only for libavformat's guess by extension: no protocol */
    ret = avformat_open_input(&fmt, path, NULL, NULL);
    if (ret < 0) {
        cb_debug("%s: %s", path, av_err2str(ret));
        input_free(&pb);
        return cb_audio_error(ret);
    }
    if (cb_audio_is_container(fmt, "matroska")) {
        cb_debug("%s: Matroska is not supported (its times are milliseconds)", path);
        cb_audio_close(&fmt);
        return CB_ERROR(ENOTSUP);
    }
    if (find_info) {
        ret = avformat_find_stream_info(fmt, NULL);
        if (ret < 0) {
            cb_debug("%s: no stream info: %s", path, av_err2str(ret));
            cb_audio_close(&fmt);
            return cb_audio_error(ret);
        }
    }
    *out = fmt;
    return 0;
}

void cb_audio_close(AVFormatContext **fmt)
{
    if (!*fmt)
        return;
    AVIOContext *pb = (*fmt)->pb;
    avformat_close_input(fmt);
    input_free(&pb);
}

const CBMap *cb_audio_input_map(const AVFormatContext *fmt)
{
    const Input *in = fmt->pb->opaque;
    return &in->map;
}
