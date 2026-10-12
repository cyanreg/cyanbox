/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* Waveforms in the library: the packed form of rx_waveforms_pack() in the
 * `waveform` table.
 *
 * This is the one place core calls into the analysis module, and it is a
 * file of its own on purpose: a static library only pulls in the objects
 * that are used, so the core tests (and anything else that never touches
 * waveforms) link without the analysis library, which itself depends on
 * core. The executable links both. */

#include "rxbox/library.h"

#include "rxbox/analysis.h"
#include "core.h"

int rx_library_waveforms_save(RxLibrary *lib, uint64_t id, const RxWaveforms *w)
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

int rx_library_waveforms_load(const RxLibrary *lib, uint64_t id, RxWaveforms *w)
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
