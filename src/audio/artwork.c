/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* Cover art: the embedded picture or one next to the file, decoded to RGBA,
 * scaled, and encoded as the JPEGs that go on a stick. A port of dj's
 * audio.artwork, export._artwork_image and export._jpeg, with the folder
 * lookup of cyanbox/audio.h (front, cover, folder, album, art; numbered firsts;
 * a lone image; never back/obi/booklet scans; the parent directory below a
 * disc folder), where dj only tried cover, folder and front.
 *
 * dj's _jpeg (Pillow) converts to RGB (alpha dropped, not composited), crops
 * the centred square of side min(w, h) with integer halving, resizes it with
 * Lanczos to size x size and saves a baseline JPEG at quality 90 with 4:2:0
 * chroma. cb_image_jpeg_square does the same with libswscale and libavcodec's
 * MJPEG encoder. SWS_LANCZOS with accurate rounding and full chroma is
 * within one level of Pillow's resize (mean 0.09 on a test picture).
 *
 * Quality: Pillow hands the number to libjpeg's jpeg_set_quality, which
 * scales the standard tables of JPEG Annex K by s = 5000 / q below 50, else
 * 200 - 2q, as (t * s + 50) / 100 clamped to 1..255. Those tables are given
 * to the encoder as its intra matrices, with the quantiser fixed at 8, which
 * the encoder multiplies in as qscale / 8 = 1 (mpegvideo_enc.c, "for mjpeg,
 * we do include qscale in the matrix"), so every AC step is libjpeg's. The
 * DC step and the rounding differ: the MJPEG encoder always uses 8 for DC
 * where libjpeg at quality 90 uses 3, and rounds AC coefficients with a
 * bias of 3/8 instead of 1/2 (neither can be set). On a busy 80 px test
 * picture the result is 3.4 levels from Pillow's on average, while Pillow's
 * own JPEG is 6.4 levels from the picture it encoded.
 * Huffman tables are optimised (smaller files, same pixels). */

#include "cyanbox/audio.h"

#include <dirent.h>
#include <stdio.h>
#include <strings.h>

#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/mem.h>
#include <libswscale/swscale.h>

#include "internal.h"

/* JPEG Annex K, tables K.1 and K.2, in natural (row-major) order, as
 * libjpeg's jcparam.c has them. */
static const uint8_t std_luma[64] = {
    16, 11, 10, 16,  24,  40,  51,  61,
    12, 12, 14, 19,  26,  58,  60,  55,
    14, 13, 16, 24,  40,  57,  69,  56,
    14, 17, 22, 29,  51,  87,  80,  62,
    18, 22, 37, 56,  68, 109, 103,  77,
    24, 35, 55, 64,  81, 104, 113,  92,
    49, 64, 78, 87, 103, 121, 120, 101,
    72, 92, 95, 98, 112, 100, 103,  99,
};

static const uint8_t std_chroma[64] = {
    17, 18, 24, 47, 99, 99, 99, 99,
    18, 21, 26, 66, 99, 99, 99, 99,
    24, 26, 56, 99, 99, 99, 99, 99,
    47, 66, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
};

/* Cover files next to the audio, in order of preference (cyanbox/audio.h:
 * measured on the user's library, 200 of 232 album folders use front.*,
 * the rest cover.* or folder.*). dj looked for cover, folder, front. */
static const char *const cover_names[] = { "front", "cover", "folder", "album", "art" };
static const char *const cover_exts[] = { ".jpg", ".jpeg", ".png", ".webp" };
/* Words that mark the other scans of a release; such a file is never the
 * cover, even alone in its folder */
static const char *const never_words[] = {
    "back", "obi", "booklet", "spine", "scan", "cd", "inlay", "inside", "disc", "tray",
};

/* ---------------------------------------------------------------- embedded */

static int embedded_raw(const char *path, uint8_t **data, size_t *size, char codec[16])
{
    *data = NULL;
    *size = 0;
    codec[0] = '\0';

    AVFormatContext *fmt;
    int ret = cb_audio_open(path, &fmt, false);
    if (ret < 0)
        return ret;

    ret = CB_ERROR(ENOENT);
    for (unsigned i = 0; i < fmt->nb_streams; i++) {
        const AVStream *st = fmt->streams[i];
        if (!(st->disposition & AV_DISPOSITION_ATTACHED_PIC) || st->attached_pic.size <= 0)
            continue;
        uint8_t *d = malloc((size_t)st->attached_pic.size);
        if (!d) {
            ret = CB_ERROR(ENOMEM);
            break;
        }
        memcpy(d, st->attached_pic.data, (size_t)st->attached_pic.size);
        const AVCodecDescriptor *desc = avcodec_descriptor_get(st->codecpar->codec_id);
        snprintf(codec, 16, "%s", desc ? desc->name : "none");
        *data = d;
        *size = (size_t)st->attached_pic.size;
        ret = 0;
        break;
    }
    cb_audio_close(&fmt);
    return ret;
}

/* ------------------------------------------------------------------ frames */

static void no_free(void *opaque, uint8_t *data)
{
}

/* An AVFrame over caller memory (never freed by it). */
static int wrap_rgba(AVFrame *f, const uint8_t *rgba, int stride, int w, int h)
{
    f->format = AV_PIX_FMT_RGBA;
    f->width = w;
    f->height = h;
    f->colorspace = AVCOL_SPC_RGB;
    f->color_range = AVCOL_RANGE_JPEG;
    f->buf[0] = av_buffer_create((uint8_t *)rgba, (size_t)stride * h, no_free, NULL,
                                 AV_BUFFER_FLAG_READONLY);
    if (!f->buf[0])
        return CB_ERROR(ENOMEM);
    f->data[0] = (uint8_t *)rgba;
    f->linesize[0] = stride;
    return 0;
}

/* Converts and scales src into a new frame of the given format and size. */
static int convert(const AVFrame *src, enum AVPixelFormat format, int w, int h,
                   unsigned flags, AVFrame **out)
{
    *out = NULL;
    AVFrame *dst = av_frame_alloc();
    SwsContext *sws = sws_alloc_context();
    int ret = CB_ERROR(ENOMEM);
    if (!dst || !sws)
        goto fail;

    dst->format = format;
    dst->width = w;
    dst->height = h;
    if (format == AV_PIX_FMT_RGBA) {
        dst->colorspace = AVCOL_SPC_RGB;
    } else {
        /* JFIF: BT.601 matrix, full range, chroma sited between samples */
        dst->colorspace = AVCOL_SPC_BT470BG;
        dst->chroma_location = AVCHROMA_LOC_CENTER;
    }
    dst->color_range = AVCOL_RANGE_JPEG;
    ret = av_frame_get_buffer(dst, 0);
    if (ret < 0)
        goto fail;

    sws->flags = flags | SWS_ACCURATE_RND | SWS_FULL_CHR_H_INT | SWS_FULL_CHR_H_INP;
    sws->threads = 1;
    ret = sws_scale_frame(sws, dst, src);
    if (ret < 0)
        goto fail;
    sws_free_context(&sws);
    *out = dst;
    return 0;

fail:
    sws_free_context(&sws);
    av_frame_free(&dst);
    return ret < 0 ? cb_audio_error(ret) : ret;
}

/* RGBA scaled to w x h into a new tightly packed buffer. */
static int scale_rgba(const uint8_t *rgba, int stride, int sw, int sh, int w, int h,
                      unsigned flags, uint8_t **out)
{
    *out = NULL;
    uint8_t *px = malloc((size_t)w * h * 4);
    if (!px)
        return CB_ERROR(ENOMEM);
    if (sw == w && sh == h) {
        for (int y = 0; y < h; y++)
            memcpy(px + (size_t)y * w * 4, rgba + (size_t)y * stride, (size_t)w * 4);
        *out = px;
        return 0;
    }

    AVFrame *src = av_frame_alloc(), *dst = NULL;
    int ret = src ? wrap_rgba(src, rgba, stride, sw, sh) : CB_ERROR(ENOMEM);
    if (ret >= 0)
        ret = convert(src, AV_PIX_FMT_RGBA, w, h, flags, &dst);
    if (ret >= 0) {
        for (int y = 0; y < h; y++)
            memcpy(px + (size_t)y * w * 4, dst->data[0] + (size_t)y * dst->linesize[0],
                   (size_t)w * 4);
        *out = px;
        px = NULL;
    }
    free(px);
    av_frame_free(&dst);
    av_frame_free(&src);
    return ret;
}

/* ------------------------------------------------------------------ decode */

static enum AVCodecID sniff(const uint8_t *d, size_t n)
{
    if (n >= 3 && d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF)
        return AV_CODEC_ID_MJPEG;
    if (n >= 8 && !memcmp(d, "\x89PNG\r\n\x1a\n", 8))
        return AV_CODEC_ID_PNG;
    if (n >= 12 && !memcmp(d, "RIFF", 4) && !memcmp(d + 8, "WEBP", 4))
        return AV_CODEC_ID_WEBP;
    if (n >= 6 && (!memcmp(d, "GIF87a", 6) || !memcmp(d, "GIF89a", 6)))
        return AV_CODEC_ID_GIF;
    if (n >= 4 && (!memcmp(d, "II*\0", 4) || !memcmp(d, "MM\0*", 4)))
        return AV_CODEC_ID_TIFF;
    if (n >= 14 && d[0] == 'B' && d[1] == 'M')
        return AV_CODEC_ID_BMP;
    return AV_CODEC_ID_NONE;
}

int cb_image_decode(const uint8_t *data, size_t size, uint8_t **rgba, int *width, int *height)
{
    cb_audio_init_logging();
    *rgba = NULL;
    *width = *height = 0;

    enum AVCodecID id = sniff(data, size);
    const AVCodec *codec = id != AV_CODEC_ID_NONE ? avcodec_find_decoder(id) : NULL;
    if (!codec || size > INT32_MAX - AV_INPUT_BUFFER_PADDING_SIZE) {
        cb_debug("image of %zu bytes: unknown format", size);
        return CB_ERROR(ENOTSUP);
    }

    AVCodecContext *dec = avcodec_alloc_context3(codec);
    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc(), *out = NULL;
    int ret = CB_ERROR(ENOMEM);
    if (!dec || !pkt || !frame)
        goto end;
    dec->thread_count = 1;
    ret = avcodec_open2(dec, codec, NULL);
    if (ret < 0)
        goto end;
    ret = av_new_packet(pkt, (int)size);
    if (ret < 0)
        goto end;
    memcpy(pkt->data, data, size);
    pkt->flags |= AV_PKT_FLAG_KEY;

    ret = avcodec_send_packet(dec, pkt);
    if (ret >= 0) {
        ret = avcodec_receive_frame(dec, frame);
        if (ret == AVERROR(EAGAIN)) {
            avcodec_send_packet(dec, NULL);
            ret = avcodec_receive_frame(dec, frame);
        }
    }
    if (ret < 0)
        goto end;
    if (frame->width <= 0 || frame->height <= 0) {
        ret = AVERROR_INVALIDDATA;
        goto end;
    }

    /* The MJPEG decoder still says yuvj*; the same planes as full-range yuv*
     * spare libswscale's deprecation warning */
    static const enum AVPixelFormat yuvj[][2] = {
        { AV_PIX_FMT_YUVJ420P, AV_PIX_FMT_YUV420P },
        { AV_PIX_FMT_YUVJ422P, AV_PIX_FMT_YUV422P },
        { AV_PIX_FMT_YUVJ444P, AV_PIX_FMT_YUV444P },
        { AV_PIX_FMT_YUVJ440P, AV_PIX_FMT_YUV440P },
        { AV_PIX_FMT_YUVJ411P, AV_PIX_FMT_YUV411P },
    };
    for (size_t i = 0; i < CB_ARRAY_ELEMS(yuvj); i++) {
        if (frame->format == yuvj[i][0]) {
            frame->format = yuvj[i][1];
            frame->color_range = AVCOL_RANGE_JPEG;
        }
    }

    /* Chroma is upsampled bilinearly, as libjpeg's "fancy upsampling" */
    ret = convert(frame, AV_PIX_FMT_RGBA, frame->width, frame->height, SWS_BILINEAR, &out);
    if (ret < 0)
        goto end;
    int w = out->width, h = out->height;
    uint8_t *px = malloc((size_t)w * h * 4);
    if (!px) {
        ret = CB_ERROR(ENOMEM);
        goto end;
    }
    for (int y = 0; y < h; y++)
        memcpy(px + (size_t)y * w * 4, out->data[0] + (size_t)y * out->linesize[0], (size_t)w * 4);
    *rgba = px;
    *width = w;
    *height = h;
    ret = 0;

end:
    if (ret < 0)
        cb_debug("image decode: %s", av_err2str(ret));
    av_frame_free(&out);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&dec);
    return ret < 0 ? cb_audio_error(ret) : 0;
}

/* -------------------------------------------------------------- next to it */

enum {
    RANK_NAMED    = 0,      /* front.jpg ... art.webp */
    RANK_NUMBERED = 100,    /* front1.jpg, cover_01.png ... */
    RANK_OTHER    = 1000,   /* any other image: used only when alone */
};

/* True when `stem` (after a cover name) is a separator and a number that
 * is 1: "1", "01", "_1", "-001", " 1". */
static bool first_number(const char *stem, size_t n)
{
    size_t i = 0;
    if (i < n && strchr(" _-.", stem[i]))
        i++;
    if (i == n)
        return false;
    int value = 0;
    for (; i < n; i++) {
        if (stem[i] < '0' || stem[i] > '9')
            return false;
        value = CB_MIN(value * 10 + (stem[i] - '0'), 1000);
    }
    return value == 1;
}

static bool is_letter(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

/* True when a run of letters in the stem is one of never_words ("Back
 * Cover", "scan03", "cd1", "booklet-2" ...) */
static bool never_cover(const char *stem, size_t n)
{
    size_t i = 0;
    while (i < n) {
        while (i < n && !is_letter(stem[i]))
            i++;
        size_t start = i;
        while (i < n && is_letter(stem[i]))
            i++;
        for (size_t w = 0; w < CB_ARRAY_ELEMS(never_words); w++) {
            size_t wl = strlen(never_words[w]);
            if (i - start == wl && !strncasecmp(stem + start, never_words[w], wl))
                return true;
        }
    }
    return false;
}

/* The rank of a file name (lower is better), or -1 when it is not an image.
 * Within a rank: the order of names, then of extensions, then an all
 * lower-case name first. */
static int cover_rank(const char *name, bool *never)
{
    *never = false;
    const char *dot = strrchr(name, '.');
    if (!dot || dot == name)
        return -1;
    int e = -1;
    for (size_t i = 0; i < CB_ARRAY_ELEMS(cover_exts) && e < 0; i++)
        if (!cb_strcasecmp(dot, cover_exts[i]))
            e = (int)i;
    if (e < 0)
        return -1;

    size_t stem = (size_t)(dot - name);
    int ne = (int)CB_ARRAY_ELEMS(cover_exts);
    for (size_t k = 0; k < CB_ARRAY_ELEMS(cover_names); k++) {
        size_t cl = strlen(cover_names[k]);
        if (stem < cl || strncasecmp(name, cover_names[k], cl))
            continue;
        bool lower = !strncmp(name, cover_names[k], cl) && !strcmp(dot, cover_exts[e]);
        int sub = ((int)k * ne + e) * 2 + !lower;
        if (stem == cl)
            return RANK_NAMED + sub;
        if (first_number(name + cl, stem - cl))
            return RANK_NUMBERED + sub;
    }
    *never = never_cover(name, stem);
    return RANK_OTHER;
}

typedef struct Candidate {
    int rank;
    char *name;
} Candidate;

static int cmp_candidate(const void *a, const void *b)
{
    const Candidate *x = a, *y = b;
    if (x->rank != y->rank)
        return x->rank - y->rank;
    return strcmp(x->name, y->name);
}

/* The usable cover files of a directory, best first: the named and
 * numbered ones, else a lone image that is not a back/obi/... scan. */
static int dir_candidates(const char *dir, Candidate **out, int *nb_out)
{
    *out = NULL;
    *nb_out = 0;
    DIR *d = opendir(dir);
    if (!d)
        return 0;

    Candidate *cand = NULL, lone = { 0 };
    int nb = 0, images = 0, ret = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        /* Hidden files, macOS "._front.jpg" resource forks among them */
        if (e->d_name[0] == '.' || e->d_type == DT_DIR)
            continue;
        bool never;
        int rank = cover_rank(e->d_name, &never);
        if (rank < 0)
            continue;
        images++;
        if (rank == RANK_OTHER) {
            if (!never && !lone.name) {
                lone = (Candidate) { .rank = rank, .name = cb_strdup(e->d_name) };
                if (!lone.name) {
                    ret = CB_ERROR(ENOMEM);
                    break;
                }
            }
            continue;
        }
        Candidate *nc = realloc(cand, ((size_t)nb + 1) * sizeof(*nc));
        if (nc)
            cand = nc;
        char *name = nc ? cb_strdup(e->d_name) : NULL;
        if (!name) {
            ret = CB_ERROR(ENOMEM);
            break;
        }
        cand[nb++] = (Candidate) { .rank = rank, .name = name };
    }
    closedir(d);

    if (ret >= 0 && !nb && images == 1 && lone.name) {
        cand = malloc(sizeof(*cand));
        if (cand) {
            cand[nb++] = lone;
            lone.name = NULL;
        } else {
            ret = CB_ERROR(ENOMEM);
        }
    }
    free(lone.name);
    if (ret < 0) {
        for (int i = 0; i < nb; i++)
            free(cand[i].name);
        free(cand);
        return ret;
    }
    if (nb)
        qsort(cand, (size_t)nb, sizeof(*cand), cmp_candidate);
    *out = cand;
    *nb_out = nb;
    return 0;
}

/* The track's directory and its parent ("Album/CD1/x.flac" -> "Album/CD1",
 * "Album"), malloc'd; parent NULL at the root. */
static int track_dirs(const char *path, char **dir, char **parent)
{
    *dir = *parent = NULL;
    const char *base = cb_path_base(path);
    if (base == path)
        *dir = cb_strdup(".");
    else if (base == path + 1)
        *dir = cb_strdup("/");
    else
        *dir = cb_strndup(path, (size_t)(base - path - 1));
    if (!*dir)
        return CB_ERROR(ENOMEM);

    if (!strcmp(*dir, "/"))
        return 0;
    const char *slash = strrchr(*dir, '/');
    if (!slash)
        *parent = cb_strdup(!strcmp(*dir, ".") ? ".." : ".");
    else if (slash == *dir)
        *parent = cb_strdup("/");
    else
        *parent = cb_strndup(*dir, (size_t)(slash - *dir));
    return *parent ? 0 : CB_ERROR(ENOMEM);
}

/* "CD1", "Disc 2", "[DISK_03] title", "dvd1": a disc of a release whose
 * cover sits one level up. */
static bool disc_folder(const char *dir)
{
    const char *name = cb_path_base(dir);
    while (*name == '[' || *name == '(' || *name == ' ')
        name++;
    static const char *const words[] = { "cd", "disc", "disk", "dvd" };
    for (size_t i = 0; i < CB_ARRAY_ELEMS(words); i++) {
        size_t n = strlen(words[i]);
        if (strncasecmp(name, words[i], n))
            continue;
        const char *p = name + n;
        while (*p && strchr(" _-.#", *p))
            p++;
        return *p >= '0' && *p <= '9';
    }
    return false;
}

/* Offers each candidate file of the track's directory, then of the parent
 * when the track's is a disc folder, to take(); the first it accepts
 * (returns 0) ends the lookup.
 *
 * The parent is only searched below a disc folder: on the user's library
 * (244 folders with audio, none of them disc folders) an unconditional
 * parent lookup gave seven albums the library root's own cover.jpg. */
typedef int (*TakeFn)(void *opaque, const char *file);

static int lookup(const char *audio_path, TakeFn take, void *opaque)
{
    char *dirs[2];
    int ret = track_dirs(audio_path, &dirs[0], &dirs[1]);
    if (ret < 0)
        return ret;

    if (dirs[1] && !disc_folder(dirs[0])) {
        free(dirs[1]);
        dirs[1] = NULL;
    }

    ret = CB_ERROR(ENOENT);
    for (int k = 0; k < 2 && dirs[k] && ret == CB_ERROR(ENOENT); k++) {
        Candidate *cand;
        int nb;
        int err = dir_candidates(dirs[k], &cand, &nb);
        if (err < 0) {
            ret = err;
            break;
        }
        for (int i = 0; i < nb && ret == CB_ERROR(ENOENT); i++) {
            char *file = cb_path_join(dirs[k], cand[i].name);
            if (!file) {
                ret = CB_ERROR(ENOMEM);
                break;
            }
            err = take(opaque, file);
            if (err == 0 || err == CB_ERROR(ENOMEM))
                ret = err;
            free(file);
        }
        for (int i = 0; i < nb; i++)
            free(cand[i].name);
        free(cand);
    }
    free(dirs[0]);
    free(dirs[1]);
    return ret;
}

typedef struct Take {
    char *path;
    uint8_t *data;
    size_t size;
    char codec[16];
    uint8_t *rgba;
    int width, height;
} Take;

/* Reads a candidate and accepts it when its bytes are an image we know */
static int take_bytes(Take *t, const char *file)
{
    uint8_t *d;
    size_t n;
    int ret = cb_file_read(file, &d, &n);
    if (ret < 0)
        return ret == CB_ERROR(ENOMEM) ? ret : CB_ERROR(ENOENT);
    enum AVCodecID id = sniff(d, n);
    if (id == AV_CODEC_ID_NONE) {
        cb_debug("%s: not an image", file);
        free(d);
        return CB_ERROR(ENOENT);
    }
    snprintf(t->codec, sizeof(t->codec), "%s", avcodec_get_name(id));
    t->data = d;
    t->size = n;
    return 0;
}

/* The same test as take_bytes on the first bytes only: cb_probe asks this
 * for every track it probes, and a cover can be megabytes. */
static int take_path(void *opaque, const char *file)
{
    Take *t = opaque;
    uint8_t head[16];
    FILE *f = fopen(file, "rbe");
    if (!f)
        return CB_ERROR(ENOENT);
    size_t n = fread(head, 1, sizeof(head), f);
    fclose(f);
    if (sniff(head, n) == AV_CODEC_ID_NONE) {
        cb_debug("%s: not an image", file);
        return CB_ERROR(ENOENT);
    }
    t->path = cb_strdup(file);
    return t->path ? 0 : CB_ERROR(ENOMEM);
}

static int take_raw(void *opaque, const char *file)
{
    return take_bytes(opaque, file);
}

static int take_rgba(void *opaque, const char *file)
{
    Take *t = opaque;
    int ret = take_bytes(t, file);
    if (ret < 0)
        return ret;
    ret = cb_image_decode(t->data, t->size, &t->rgba, &t->width, &t->height);
    free(t->data);
    t->data = NULL;
    if (ret == CB_ERROR(ENOMEM))
        return ret;
    if (ret < 0) {
        cb_debug("%s: unreadable image", file);
        return CB_ERROR(ENOENT);
    }
    return 0;
}

int cb_artwork_find_file(const char *audio_path, char **image_path)
{
    Take t = { 0 };
    int ret = lookup(audio_path, take_path, &t);
    *image_path = ret < 0 ? NULL : t.path;
    return ret;
}

int cb_artwork_raw(const char *path, uint8_t **data, size_t *size, char codec[16])
{
    /* Embedded, else next to the file (also when libavformat cannot read
     * the file: its folder's cover is still its cover) */
    int ret = embedded_raw(path, data, size, codec);
    if (ret >= 0 || ret == CB_ERROR(ENOMEM))
        return ret;

    Take t = { 0 };
    ret = lookup(path, take_raw, &t);
    if (ret < 0)
        return ret;
    *data = t.data;
    *size = t.size;
    memcpy(codec, t.codec, sizeof(t.codec));
    return 0;
}

int cb_artwork_rgba(const char *path, uint8_t **rgba, int *width, int *height)
{
    *rgba = NULL;
    *width = *height = 0;

    uint8_t *raw;
    size_t size;
    char codec[16];
    int ret = embedded_raw(path, &raw, &size, codec);
    if (ret >= 0) {
        ret = cb_image_decode(raw, size, rgba, width, height);
        free(raw);
        if (ret >= 0)
            return 0;
        cb_debug("%s: embedded %s cover unreadable, looking next to it", path, codec);
    } else if (ret == CB_ERROR(ENOMEM)) {
        return ret;
    }

    Take t = { 0 };
    ret = lookup(path, take_rgba, &t);
    if (ret < 0)
        return ret;
    *rgba = t.rgba;
    *width = t.width;
    *height = t.height;
    return 0;
}

/* ------------------------------------------------------------------- scale */

int cb_image_scale(const uint8_t *rgba, int width, int height, int max_w, int max_h,
                   uint8_t **out, int *out_w, int *out_h)
{
    cb_audio_init_logging();
    *out = NULL;
    *out_w = *out_h = 0;
    if (width <= 0 || height <= 0 || max_w <= 0 || max_h <= 0)
        return CB_ERROR(EINVAL);

    double s = CB_MIN(CB_MIN((double)max_w / width, (double)max_h / height), 1.0);
    int w = CB_CLAMP((int)(width * s + 0.5), 1, max_w);
    int h = CB_CLAMP((int)(height * s + 0.5), 1, max_h);
    int ret = scale_rgba(rgba, width * 4, width, height, w, h, SWS_AREA, out);
    if (ret < 0)
        return ret;
    *out_w = w;
    *out_h = h;
    return 0;
}

/* -------------------------------------------------------------------- jpeg */

static int quant_scale(int quality)
{
    quality = CB_CLAMP(quality, 1, 100);
    return quality < 50 ? 5000 / quality : 200 - quality * 2;
}

static uint16_t *quant_table(const uint8_t *std, int scale)
{
    uint16_t *t = av_malloc(64 * sizeof(*t));
    if (!t)
        return NULL;
    for (int i = 0; i < 64; i++)
        t[i] = (uint16_t)CB_CLAMP((std[i] * scale + 50) / 100, 1, 255);
    return t;
}

static int encode_jpeg(AVFrame *yuv, int quality, uint8_t **jpeg, size_t *jpeg_size)
{
    const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
    if (!codec)
        return CB_ERROR(ENOTSUP);
    AVCodecContext *enc = avcodec_alloc_context3(codec);
    AVPacket *pkt = av_packet_alloc();
    int ret = CB_ERROR(ENOMEM);
    if (!enc || !pkt)
        goto end;

    enc->width = yuv->width;
    enc->height = yuv->height;
    enc->pix_fmt = yuv->format;
    enc->color_range = AVCOL_RANGE_JPEG;
    enc->colorspace = AVCOL_SPC_BT470BG;
    enc->chroma_sample_location = AVCHROMA_LOC_CENTER;
    enc->time_base = (AVRational) { 1, 1 };
    enc->thread_count = 1;
    /* Fixed quantiser 8: the matrices below are used as they are */
    enc->flags |= AV_CODEC_FLAG_QSCALE | AV_CODEC_FLAG_BITEXACT;
    enc->global_quality = 8 * FF_QP2LAMBDA;
    int scale = quant_scale(quality);
    enc->intra_matrix = quant_table(std_luma, scale);
    enc->chroma_intra_matrix = quant_table(std_chroma, scale);
    if (!enc->intra_matrix || !enc->chroma_intra_matrix)
        goto end;
    ret = avcodec_open2(enc, codec, NULL);
    if (ret < 0)
        goto end;

    yuv->quality = enc->global_quality;
    yuv->pts = 0;
    ret = avcodec_send_frame(enc, yuv);
    if (ret >= 0)
        ret = avcodec_send_frame(enc, NULL);
    if (ret >= 0)
        ret = avcodec_receive_packet(enc, pkt);
    if (ret < 0)
        goto end;

    *jpeg = malloc((size_t)pkt->size);
    if (!*jpeg) {
        ret = CB_ERROR(ENOMEM);
        goto end;
    }
    memcpy(*jpeg, pkt->data, (size_t)pkt->size);
    *jpeg_size = (size_t)pkt->size;
    ret = 0;

end:
    av_packet_free(&pkt);
    avcodec_free_context(&enc);
    return ret < 0 ? cb_audio_error(ret) : 0;
}

int cb_image_jpeg_square(const uint8_t *rgba, int width, int height, int size, int quality,
                         uint8_t **jpeg, size_t *jpeg_size)
{
    cb_audio_init_logging();
    *jpeg = NULL;
    *jpeg_size = 0;
    if (width <= 0 || height <= 0 || size <= 0)
        return CB_ERROR(EINVAL);

    /* Pillow: crop((w - s) // 2, (h - s) // 2, ...), then resize(LANCZOS) */
    int s = CB_MIN(width, height);
    const uint8_t *crop = rgba + ((size_t)((height - s) / 2) * width + (width - s) / 2) * 4;
    uint8_t *square;
    int ret = scale_rgba(crop, width * 4, s, s, size, size, SWS_LANCZOS, &square);
    if (ret < 0)
        return ret;

    AVFrame *src = av_frame_alloc(), *yuv = NULL;
    ret = src ? wrap_rgba(src, square, size * 4, size, size) : CB_ERROR(ENOMEM);
    if (ret >= 0)
        ret = convert(src, AV_PIX_FMT_YUV420P, size, size, SWS_AREA, &yuv);
    if (ret >= 0)
        ret = encode_jpeg(yuv, quality, jpeg, jpeg_size);
    av_frame_free(&yuv);
    av_frame_free(&src);
    free(square);
    return ret;
}
