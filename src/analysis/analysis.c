/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* cb_analyse: the whole analysis of one decoded track, and the automatic
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
 * The grid is made with Beat This!: its network runs on the mono signal
 * resampled to 22050 Hz (the decoder's resampler, as the research that
 * measured it decoded), before the 44.1 kHz copy is made, so that the two
 * copies are never alive together; its logits wait for cyanbox's own
 * analysis, which cb_beats_hybrid then makes the grid from. Without the
 * network the beats fail: cyanbox's analysis alone is not the grid.
 *
 * Each stage stands on its own: one that fails (no memory for an hour's
 * waveforms, say) leaves the others to run, and the first error is
 * returned with what was done in `done`. */

#include "cyanbox/analysis.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "beats_internal.h"

/* A stage's result: done, or the call's error if it is the first. */
static void stage_done(CBAnalysis *out, unsigned stage, int ret, int *first_err)
{
    if (ret >= 0)
        out->done |= stage;
    else if (!*first_err)
        *first_err = ret;
}

int cb_analyse(const CBAudio *audio, const CBAnalysisOptions *opt, unsigned what, CBAnalysis *out)
{
    memset(out, 0, sizeof(*out));
    if (!audio || !audio->samples || audio->channels <= 0 || audio->rate <= 0 ||
        audio->nb_frames < 0)
        return CB_ERROR(EINVAL);
    out->duration = (double)audio->nb_frames / audio->rate;

    CBAnalysisOptions defopt;
    if (!opt) {
        cb_analysis_options_default(&defopt);
        opt = &defopt;
    }

    int first_err = 0;
    CBAudio mono = { 0 };
    const float *x = audio->samples;
    if (audio->channels != 1) {
        int ret = cb_audio_to_mono(audio, &mono);
        if (ret < 0)
            return ret;
        x = mono.samples;
    }
    const int64_t n = audio->nb_frames;

    if (what & CB_ANALYSE_WAVES)
        stage_done(out, CB_ANALYSE_WAVES, cb_waveforms_compute(x, n, audio->rate, &out->waves),
                   &first_err);

    /* Beat This!'s logits, while the mono signal is at its own rate. */
    float *bt_beat = NULL, *bt_down = NULL;
    int bt_frames = 0, bt_err = 0;
    if (what & CB_ANALYSE_BEATS) {
        CBAudio m = { .samples = (float *)x, .nb_frames = n, .channels = 1, .rate = audio->rate };
        CBAudio m22 = { 0 };
        if (!opt->beat_net)
            bt_err = CB_ERROR(ENODEV);
        else if ((bt_err = cb_audio_resample(&m, CB_BEAT_NET_RATE, &m22)) >= 0)
            bt_err = opt->beat_net->run(opt->beat_net->opaque, m22.samples, m22.nb_frames,
                                        &bt_beat, &bt_down, &bt_frames);
        cb_audio_free(&m22);
        if (bt_err < 0) {
            free(bt_beat);
            free(bt_down);
            bt_beat = bt_down = NULL;
        }
    }

    if (what & (CB_ANALYSE_BEATS | CB_ANALYSE_KEY)) {
        /* At 44.1 kHz beats and key read the mono signal itself. Otherwise
         * one resampled copy, and our own mono mix goes before they run:
         * an hour of mono float is 635 MB at 44.1 kHz, and the library
         * analyses up to eleven tracks at once. */
        const float *x44 = x;
        float *own44 = NULL;
        int64_t n44 = n;
        int ret = 0;
        if (audio->rate != CB_BEAT_RATE) {
            ret = cb_resample_mono(x, n, audio->rate, CB_BEAT_RATE, &own44, &n44);
            x44 = own44;
            cb_audio_free(&mono);
        }
        if (ret < 0) {
            if (!first_err)
                first_err = ret;
        } else {
            if (what & CB_ANALYSE_BEATS) {
                int r = bt_err;
                if (r >= 0)
                    r = cb_analyse_beats(x44, n44, CB_BEAT_RATE, opt, &out->beats);
                if (r >= 0 && ((r = cb_beats_hybrid(bt_beat, bt_down, bt_frames, out->duration,
                                                    &out->beats)) < 0 ||
                               (r = cb_beats_drums(x44, n44, CB_BEAT_RATE, &out->beats)) < 0))
                    cb_beat_result_free(&out->beats);
                stage_done(out, CB_ANALYSE_BEATS, r, &first_err);
            }
            if (what & CB_ANALYSE_KEY)
                stage_done(out, CB_ANALYSE_KEY, cb_analyse_key(x44, n44, CB_BEAT_RATE, &out->key),
                           &first_err);
        }
        free(own44);
    }

    free(bt_beat);
    free(bt_down);
    cb_audio_free(&mono);
    return first_err;
}

void cb_analysis_free(CBAnalysis *a)
{
    cb_beat_result_free(&a->beats);
    cb_waveforms_free(&a->waves);
    memset(a, 0, sizeof(*a));
}

int cb_analysis_auto_cue_at(const CBGrid *g, const CBBeatResult *beats, double duration,
                            double *at)
{
    /* dj's export._auto_cues put it on the first downbeat at or after the
     * start of the music (50 ms of slack, so a downbeat that the grid
     * places a hair before a hit still counts; dj took its first loud
     * onset, which in a fade-in is anywhere along the rise). It waits for
     * the drums now (cb_beats_drums): past a fade-in and an intro without
     * them, where the user starts a track. */
    if (!g || !beats || !cb_grid_valid(g))
        return 0;
    const double from = fmax(beats->drums_start, beats->music_start) - 0.05;

    /* The beats up to the end of the track. With its length not known yet
     * (0), up to a bar and a beat past the start or past the last section,
     * whichever is later: the last section's count runs on, so the
     * downbeat is within that bar. (It was a million seconds: 2.1 M beats
     * and 50 MB at 128 BPM.) */
    if (!(duration > 0.0)) {
        const CBTempoSection *last = &g->sections[g->nb_sections - 1];
        int bpb = g->beats_per_bar > 0 ? g->beats_per_bar : 4;      /* 0 is 4, as in grid.c */
        duration = fmax(from, last->start) + (bpb + 1) * 60.0 / last->bpm;
    }

    CBBeat *b = NULL;
    int nb = 0;
    int ret = cb_grid_beats(g, duration, &b, &nb);
    if (ret < 0)
        return ret;
    ret = 0;
    for (int i = 0; i < nb; i++) {
        if (b[i].number == 1 && b[i].time >= from) {
            *at = b[i].time;
            ret = 1;
            break;
        }
    }
    free(b);
    return ret;
}

int cb_analysis_add_auto_cue(CBTrack *t, const CBBeatResult *beats)
{
    /* Musical (on the grid), so it follows a corrected grid. With the
     * players' default AUTO CUE LEVEL = MEMORY, loading the track jumps
     * there. */
    if (!t || !beats)
        return 0;
    const CBGrid *g = cb_grid_valid(&t->grid) ? &t->grid : &beats->grid;
    double at;
    int ret = cb_analysis_auto_cue_at(g, beats, t->duration, &at);
    if (ret <= 0)
        return ret;
    CBCue c;
    cb_cue_init(&c, at);
    cb_cue_attach_to_grid(&c, g);
    ret = cb_track_add_cue(t, &c);
    return ret < 0 ? ret : 0;
}

int cb_analysis_auto_cues(CBTrack *t, const CBBeatResult *beats)
{
    /* Only for a track that has no cues yet, as dj did it: a cue the DJ
     * set is theirs to keep. */
    if (!t || !beats || t->nb_cues > 0)
        return 0;
    return cb_analysis_add_auto_cue(t, beats);
}

int cb_analysis_take_auto_cue(CBTrack *t, const CBBeatResult *beats)
{
    /* The analyser's cue is a plain memory cue (no name, colour or loop) on
     * the grid, where an analysis put it: on the grid's first downbeat
     * (grid beat 0), where it went before it waited for the drums, or
     * where this analysis puts it on the same grid. A DJ's own plain cue
     * on either downbeat is taken for it: it would be placed there again. */
    if (!t || !beats || !cb_grid_valid(&t->grid))
        return 0;
    double at = NAN;
    int ret = cb_analysis_auto_cue_at(&t->grid, beats, t->duration, &at);
    if (ret < 0)
        return ret;
    for (int i = 0; i < t->nb_cues; i++) {
        const CBCue *c = &t->cues[i];
        if (cb_cue_is_hot(c) || cb_cue_is_loop(c) || c->name[0] || c->color[0] ||
            !cb_cue_is_musical(c))
            continue;
        if (fabs(c->grid_beat) < 1e-6 || fabs(c->time - at) < 1e-3) {
            cb_track_remove_cue(t, i);
            return 1;
        }
    }
    return 0;
}
