/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* Waveforms in the library: the packed form of rx_waveforms_pack() in the
 * `waveform` table, and a check of their format version from the header
 * alone, so the app can find the stale ones of a whole library at start.
 *
 * This is the one place core calls into the analysis module, and it is a
 * file of its own on purpose: a static library only pulls in the objects
 * that are used, so the core tests (and anything else that never touches
 * waveforms) link without the analysis library, which itself depends on
 * core. The executable links both. */

#include "rxbox/library.h"

#include "rxbox/analysis.h"
#include "core.h"

int rx_library_waveforms_save(RXLibrary *lib, uint64_t id, const RXWaveforms *w)
{
    uint8_t *data;
    size_t size;
    int ret = rx_waveforms_pack(w, &data, &size);
    if (ret < 0)
        return ret;
    ret = rx_core_waveform_blob_put(lib, id, data, size);
    free(data);
    return ret;
}

int rx_library_waveforms_check(const RXLibrary *lib, uint64_t id)
{
    /* The blob starts with "RXWV" and its format version, 4 bytes each
     * (waveform.c). What the packer writes now comes from packing one
     * column: the format version is the analysis module's to keep. */
    uint8_t col[3] = { 0 };
    RXWaveforms w = { .nb_detail = 1, .pwv3 = col, .pwv5 = col, .pwv7 = col };
    uint8_t *now, head[8];
    size_t size;
    int ret = rx_waveforms_pack(&w, &now, &size);
    if (ret < 0)
        return ret;
    ret = rx_core_waveform_blob_head(lib, id, head, sizeof(head));
    if (ret >= 0 && memcmp(head, now, 4))
        ret = RX_ERROR(EINVAL);
    else if (ret >= 0 && memcmp(head + 4, now + 4, 4))
        ret = RX_ERROR(EPROTO);
    free(now);
    return ret;
}

int rx_library_waveforms_load(const RXLibrary *lib, uint64_t id, RXWaveforms *w)
{
    uint8_t *data;
    size_t size;
    int ret = rx_core_waveform_blob_get(lib, id, &data, &size);
    if (ret < 0)
        return ret;
    ret = rx_waveforms_unpack(data, size, w);
    free(data);
    /* A blob of another format version (-EPROTO) is expected after a
     * calibration change and is recomputed quietly; damaged data is an
     * error worth hearing about. */
    if (ret == RX_ERROR(EPROTO))
        rx_debug("track %llu: stored waveforms are an older format; recomputing",
                 (unsigned long long)id);
    else if (ret < 0)
        rx_err("track %llu: stored waveforms do not unpack (%s); analyse it again",
               (unsigned long long)id, strerror(-ret));
    return ret;
}
