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
 * depend on it either way.
 *
 * Each stage stands on its own: one that fails (no memory for an hour's
 * waveforms, say) leaves the others to run, and the first error is
 * returned with what was done in `done`. The samples are taken to be
 * finite: rx_decode decodes NaN and infinities as silence (src/audio/
 * decode.c), since one of them takes the key and the waveforms with it. */

#include "rxbox/analysis.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "beats_internal.h"

/* A stage's result: done, or the call's error if it is the first. */
static void stage_done(RXAnalysis *out, unsigned stage, int ret, int *first_err)
{
    if (ret >= 0)
        out->done |= stage;
    else if (!*first_err)
        *first_err = ret;
}

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

    int first_err = 0;
    RXAudio mono = { 0 };
    const float *x = audio->samples;
    if (audio->channels != 1) {
        int ret = rx_audio_to_mono(audio, &mono);
        if (ret < 0)
            return ret;
        x = mono.samples;
    }
    const int64_t n = audio->nb_frames;

    if (what & RX_ANALYSE_WAVES)
        stage_done(out, RX_ANALYSE_WAVES, rx_waveforms_compute(x, n, audio->rate, &out->waves),
                   &first_err);

    if (what & (RX_ANALYSE_BEATS | RX_ANALYSE_KEY)) {
        /* At 44.1 kHz beats and key read the mono signal itself. Otherwise
         * one resampled copy, and our own mono mix goes before they run:
         * an hour of mono float is 635 MB at 44.1 kHz, and the library
         * analyses up to eleven tracks at once. */
        const float *x44 = x;
        float *own44 = NULL;
        int64_t n44 = n;
        int ret = 0;
        if (audio->rate != RX_BEAT_RATE) {
            ret = rx_resample_mono(x, n, audio->rate, RX_BEAT_RATE, &own44, &n44);
            x44 = own44;
            rx_audio_free(&mono);
        }
        if (ret < 0) {
            if (!first_err)
                first_err = ret;
        } else {
            if (what & RX_ANALYSE_BEATS)
                stage_done(out, RX_ANALYSE_BEATS,
                           rx_analyse_beats(x44, n44, RX_BEAT_RATE, opt, &out->beats), &first_err);
            if (what & RX_ANALYSE_KEY)
                stage_done(out, RX_ANALYSE_KEY, rx_analyse_key(x44, n44, RX_BEAT_RATE, &out->key),
                           &first_err);
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
     * after the start of the music (50 ms of slack, so a downbeat that the
     * grid places a hair before a hit still counts; dj took its first loud
     * onset, which in a fade-in is anywhere along the rise). With the
     * players' default AUTO CUE LEVEL = MEMORY, loading the track jumps
     * there. Musical (on the grid), so it follows a corrected grid. Only
     * for a track that has no cues yet, as dj did it: a cue the DJ set is
     * theirs to keep. */
    if (!t || !beats || t->nb_cues > 0)
        return 0;
    const RXGrid *g = rx_grid_valid(&t->grid) ? &t->grid : &beats->grid;
    if (!rx_grid_valid(g))
        return 0;

    /* The beats up to the end of the track. With its length not known yet
     * (0), up to a bar and a beat past the start or past the last section,
     * whichever is later: the last section's count runs on, so the
     * downbeat is within that bar. (It was a million seconds: 2.1 M beats
     * and 50 MB at 128 BPM.) */
    const double from = beats->music_start - 0.05;
    double duration = t->duration;
    if (!(duration > 0.0)) {
        const RXTempoSection *last = &g->sections[g->nb_sections - 1];
        int bpb = g->beats_per_bar > 0 ? g->beats_per_bar : 4;      /* 0 is 4, as in grid.c */
        duration = fmax(from, last->start) + (bpb + 1) * 60.0 / last->bpm;
    }

    RXBeat *b = NULL;
    int nb = 0;
    int ret = rx_grid_beats(g, duration, &b, &nb);
    if (ret < 0)
        return ret;
    double at = NAN;
    for (int i = 0; i < nb; i++) {
        if (b[i].number == 1 && b[i].time >= from) {
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
