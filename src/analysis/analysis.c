/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* rx_analyse: the whole analysis of one decoded track, and the automatic
 * cues dj added after it.
 *
 * The audio is mixed to mono once (the decoder's mean of the channels, so
 * that the analyser and the deck agree on what mono is); the app decodes
 * mono, so usually there is nothing to mix. The waveforms are cut from it
 * at the file's own rate, because their columns are counted on the
 * players' timeline at that rate. Beats and key both work at 44.1 kHz:
 * they read the mono signal itself when it is at 44.1 kHz, else one copy
 * resampled after the waveforms are done (time-aligned, libswresample),
 * with our own mono mix freed first. No more than one full-length signal
 * of ours is alive at any time.
 *
 * Inside one call everything runs on the calling thread: the library
 * analyses many tracks at once on its pool, and a call that spawned its
 * own threads would only compete with its siblings. The result does not
 * depend on it either way. */

#include "rxbox/analysis.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "beats_internal.h"

int rx_analyse(const RXAudio *audio, const RXAnalysisOptions *opt, unsigned what, RXAnalysis *out)
{
    memset(out, 0, sizeof(*out));
    if (!audio || !audio->samples || audio->channels <= 0 || audio->rate <= 0 ||
        audio->nb_frames < 0)
        return RX_ERROR(EINVAL);
    out->duration = (double)audio->nb_frames / audio->rate;

    RXAnalysisOptions defopt;
    if (!opt) {
        rx_analysis_options_default(&defopt);
        opt = &defopt;
    }

    int ret = 0, first_err = 0;
    RXAudio mono = { 0 };
    const float *x = audio->samples;
    if (audio->channels != 1) {
        if ((ret = rx_audio_to_mono(audio, &mono)) < 0)
            return ret;
        x = mono.samples;
    }
    const int64_t n = audio->nb_frames;

    if (what & RX_ANALYSE_WAVES) {
        ret = rx_waveforms_compute(x, n, audio->rate, &out->waves);
        if (ret >= 0)
            out->done |= RX_ANALYSE_WAVES;
        else if (!first_err)
            first_err = ret;
    }

    if (what & (RX_ANALYSE_BEATS | RX_ANALYSE_KEY)) {
        /* At 44.1 kHz beats and key read the mono signal itself. Otherwise
         * one resampled copy, and our own mono mix goes before they run:
         * an hour of mono float is 635 MB at 44.1 kHz, and the library
         * analyses up to eleven tracks at once. */
        const float *x44 = x;
        float *own44 = NULL;
        int64_t n44 = n;
        if (audio->rate != RX_BEAT_RATE) {
            ret = rx_resample_mono(x, n, audio->rate, RX_BEAT_RATE, &own44, &n44);
            x44 = own44;
            rx_audio_free(&mono);
        }
        if (ret < 0) {
            if (!first_err)
                first_err = ret;
        } else {
            if (what & RX_ANALYSE_BEATS) {
                ret = rx_analyse_beats(x44, n44, RX_BEAT_RATE, opt, &out->beats);
                if (ret >= 0)
                    out->done |= RX_ANALYSE_BEATS;
                else if (!first_err)
                    first_err = ret;
            }
            if (what & RX_ANALYSE_KEY) {
                ret = rx_analyse_key(x44, n44, RX_BEAT_RATE, &out->key);
                if (ret >= 0)
                    out->done |= RX_ANALYSE_KEY;
                else if (!first_err)
                    first_err = ret;
            }
        }
        free(own44);
    }

    rx_audio_free(&mono);
    return first_err;
}

void rx_analysis_free(RXAnalysis *a)
{
    rx_beat_result_free(&a->beats);
    rx_waveforms_free(&a->waves);
    memset(a, 0, sizeof(*a));
}

int rx_analysis_auto_cues(RXTrack *t, const RXBeatResult *beats)
{
    /* dj's export._auto_cues: one memory cue, on the first downbeat at or
     * after the first onset (50 ms of slack, so a downbeat that the onset
     * detector places a hair late still counts). With the players' default
     * AUTO CUE LEVEL = MEMORY, loading the track jumps there. Musical (on
     * the grid), so it follows a corrected grid. Only for a track that has
     * no cues yet, as dj did it: a cue the DJ set is theirs to keep. */
    if (!t || !beats || t->nb_cues > 0)
        return 0;
    const RXGrid *g = rx_grid_valid(&t->grid) ? &t->grid : &beats->grid;
    if (!rx_grid_valid(g))
        return 0;
    double duration = t->duration > 0.0 ? t->duration : 1e6;

    RXBeat *b = NULL;
    int nb = 0;
    int ret = rx_grid_beats(g, duration, &b, &nb);
    if (ret < 0)
        return ret;
    double at = NAN;
    for (int i = 0; i < nb; i++) {
        if (b[i].number == 1 && b[i].time >= beats->first_onset - 0.05) {
            at = b[i].time;
            break;
        }
    }
    free(b);
    if (isnan(at))
        return 0;

    RXCue c;
    rx_cue_init(&c, at);
    rx_cue_attach_to_grid(&c, g);
    ret = rx_track_add_cue(t, &c);
    return ret < 0 ? ret : 0;
}
