/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* Waveforms in the library: the packed form of cb_waveforms_pack(), its
 * header and previews in `waveform_preview` and its detail in
 * `waveform_detail`, so the browser's rows read a few kB and not the
 * whole; and a check of their format version from the header alone, so
 * the app can find the stale ones of a whole library at start.
 *
 * This is the one place core calls into the analysis module, and it is a
 * file of its own on purpose: a static library only pulls in the objects
 * that are used, so the core tests (and anything else that never touches
 * waveforms) link without the analysis library, which itself depends on
 * core. The executable links both. */

#include "cyanbox/library.h"

#include "cyanbox/analysis.h"
#include "core.h"

int cb_library_waveforms_save(CBLibrary *lib, uint64_t id, const CBWaveforms *w)
{
    uint8_t *data;
    size_t size;
    int ret = cb_waveforms_pack(w, &data, &size);
    if (ret < 0)
        return ret;
    ret = cb_core_waveform_blob_put(lib, id, data, size, CB_WAVE_PACKED_PREVIEW);
    free(data);
    return ret;
}

int cb_library_waveforms_check(const CBLibrary *lib, uint64_t id)
{
    /* The blob starts with its magic and format version, 4 bytes each,
     * which are the analysis module's to keep (waveform.c). */
    uint8_t head[8];
    int ret = cb_core_waveform_blob_head(lib, id, head, sizeof(head));
    return ret < 0 ? ret : cb_waveforms_check_head(head, sizeof(head));
}

static int load(const CBLibrary *lib, uint64_t id, bool detail, CBWaveforms *w)
{
    *w = (CBWaveforms) { 0 };
    uint8_t *data;
    size_t size;
    int ret = cb_core_waveform_blob_get(lib, id, detail, &data, &size);
    if (ret < 0)
        return ret;
    ret = detail ? cb_waveforms_unpack(data, size, w) : cb_waveforms_unpack_previews(data, size, w);
    free(data);
    /* A blob of another format version (-EPROTO) is expected after a
     * calibration change and is recomputed quietly; damaged data is an
     * error worth hearing about. */
    if (ret == CB_ERROR(EPROTO))
        cb_debug("track %llu: stored waveforms are an older format; recomputing",
                 (unsigned long long)id);
    else if (ret < 0)
        cb_err("track %llu: stored waveforms do not unpack (%s); analyse it again",
               (unsigned long long)id, strerror(-ret));
    return ret;
}

int cb_library_waveforms_load(const CBLibrary *lib, uint64_t id, CBWaveforms *w)
{
    return load(lib, id, true, w);
}

int cb_library_waveforms_load_previews(const CBLibrary *lib, uint64_t id, CBWaveforms *w)
{
    return load(lib, id, false, w);
}
