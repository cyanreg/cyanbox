/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* Private interface between the stages of the beat-grid analyser
 * (spectrum.c, onset.c, tempo.c, grid_fit.c, downbeat.c, beats.c) and
 * analysis.c. Nothing here is part of rxbox/analysis.h; the stages are
 * split into files so that each can be read, tested and replaced on its
 * own (the spectral front end in particular could move to Vulkan compute).
 *
 * Time conventions: every time is in seconds of the analysed signal, which
 * is the input resampled to 44.1 kHz with no delay, so it is also seconds
 * of the players' timeline. A spectral frame or onset value is stamped at
 * the centre of its window. */

#ifndef RXBOX_ANALYSIS_BEATS_INTERNAL_H
#define RXBOX_ANALYSIS_BEATS_INTERNAL_H

#include <stdint.h>

#include "rxbox/analysis.h"

/* ---------------------------------------------------------------- spectrum */

#define RX_BEAT_RATE  44100
#define RX_BEAT_NFFT  2048          /* 46 ms: 21.5 Hz bins, enough for a kick band */
#define RX_BEAT_HOP   294           /* 150 frames a second, the waveform column rate */
#define RX_BEAT_NMELS 64

#define RX_BEAT_GROUP 4             /* mel bands per downbeat profile band */
#define RX_BEAT_NPROF (RX_BEAT_NMELS / RX_BEAT_GROUP)

/* What the later stages read of the spectrum. The mel frames themselves
 * are not kept (an hour of them is 138 MB): the flux is taken frame by
 * frame as they are computed, and the downbeat's profile bands are summed
 * from them on the way. */
typedef struct RxSpectrum {
    int    nb_frames;               /* frame f is centred on sample f * RX_BEAT_HOP */
    double fps;
    float *prof;                    /* [nb_frames][RX_BEAT_NPROF]: sums of 4 log-mel bands */
    float *kick;                    /* [nb_frames], log-compressed amplitude 30..150 Hz */
    float *flux_full;               /* [nb_frames] SuperFlux over all mel bands, at the window */
    float *flux_mid;                /* centre, before the onset stage's corrections */
    float *flux_high;
    double mel_hz[RX_BEAT_NMELS];   /* centre of each band */
} RxSpectrum;

/* Centred Hann STFT, triangular mel bands (30 Hz .. 16 kHz, like dj) on
 * the power spectrum, square-rooted back to an amplitude per band and
 * compressed as dj's log1p(1000 * amplitude / (N/4)). */
int  rx_spectrum_compute(const float *x, int64_t n, RxSpectrum *s);
void rx_spectrum_free(RxSpectrum *s);

/* Resamples mono float with libswresample, time-aligned (output sample 0
 * is input time 0). *out is malloc'd. */
int  rx_resample_mono(const float *in, int64_t n, int in_rate, int out_rate,
                      float **out, int64_t *out_n);

/* Fine band envelopes around chosen instants, for attack timing. Rather
 * than band-pass the whole track (three zero-phase IIR filters over 16 M
 * samples cost more than the rest of the analyser together), a window
 * around each beat is transformed once and the bands are cut in the
 * frequency domain: zero phase by construction, and only the ~15 % of the
 * track near beats is ever filtered. */
enum {
    RX_BAND_LOW,                    /* < 150 Hz: kick body */
    RX_BAND_MID,                    /* 150 .. 2500 Hz: snare, clap, kick knock */
    RX_BAND_HIGH,                   /* > 2500 Hz: kick click, hats */
    RX_NB_BANDS,
};

#define RX_ENV_DECIM   16                               /* 2756 Hz, 0.36 ms */
#define RX_ENV_RATE    ((double)RX_BEAT_RATE / RX_ENV_DECIM)
#define RX_ENV_HALF    160                              /* +-58 ms around the centre */
#define RX_ENV_LEN     (2 * RX_ENV_HALF + 1)

typedef struct RxEnvCtx RxEnvCtx;
int  rx_env_alloc(RxEnvCtx **ctx, const float *x, int64_t n);
void rx_env_free(RxEnvCtx **ctx);
/* env[b * RX_ENV_LEN + i] is band b at time centre + (i - RX_ENV_HALF) / RX_ENV_RATE,
 * where centre is `t` rounded to a multiple of 1 / RX_ENV_RATE (returned). */
int  rx_env_window(RxEnvCtx *ctx, double t, float *env, double *centre);

/* The bass's pitch class around each of `nb` instants: a 12-bin chroma of
 * 30..250 Hz from a 0.74 s window starting a sixteenth of a second after
 * the instant (past the kick's thump), on the signal decimated to 2756 Hz,
 * where a 2048-point transform resolves 1.35 Hz: enough for bass notes,
 * which the 21.5 Hz bins of the main STFT cannot tell apart. Each chroma
 * sums to 1 (all zero in silence). out is [nb][12]. */
int rx_bass_chroma(const float *x, int64_t n, const double *times, int nb, float *out);

/* ------------------------------------------------------------------ onsets */

typedef struct RxOnsets {
    int    nb;                      /* = nb_frames; value i is at i / fps seconds */
    double fps;
    float *full;                    /* SuperFlux over all mel bands */
    float *low;                     /* flux of the kick band */
    float *mid;                     /* flux of 150 .. 2500 Hz (snare, clap) */
    float *high;                    /* flux above 5 kHz (hats) */
    float *accent;                  /* what the beat tracker follows: kick-weighted */
    double kick_share;              /* periodic energy in the kick band vs the full band */
} RxOnsets;

/* The SuperFlux rises of mel frame `cur` against frame `ref` (two frames
 * earlier), summed over all bands, bands [mid0, mid1) and bands from
 * high0. Called by the STFT for every frame. */
void rx_superflux_frame(const float *ref, const float *cur, int mid0, int mid1, int high0,
                        float *full, float *mid, float *high);
/* Takes the spectrum's flux signals (they move into the onsets). */
int  rx_onsets_compute(RxSpectrum *s, RxOnsets *o);
void rx_onsets_free(RxOnsets *o);
/* The first onset that is loud relative to the track (dj's rule). */
double rx_onsets_first(const RxOnsets *o);
/* Removes the local mean (one second), clips at zero, scales to unit
 * standard deviation. In place. */
void rx_onset_normalise(float *o, int n, double fps);

/* ------------------------------------------------------------------- tempo */

#define RX_TEMPO_MIN 50.0
#define RX_TEMPO_MAX 220.0

/* The rhythm at one metrical level (rx_tempo_octave_decide). */
typedef struct RxOctaveEvidence {
    double balance;                 /* snare band: min/max of even vs odd beats */
    double subdiv;                  /* snare band: half beats vs beats */
    double kick_alt;                /* kick band: min/max of even vs odd beats (dj's alternation) */
    double kick_sub;                /* kick band: half beats vs beats */
    double score;                   /* the rhythm's likelihood of the level, without the prior */
} RxOctaveEvidence;

typedef struct RxTempoEstimate {
    double bpm;                     /* working tempo: the best family's octave nearest the prior */
    double strength;                /* its harmonic DFT strength */
    double second_bpm;              /* the runner-up family, near bpm, 0 if none */
    double second_ratio;            /* its strength / the winner's */
    double octave_bpm;              /* the working octave's runner-up, 0 if none */
    double octave_margin;           /* their prior ratio */
} RxTempoEstimate;

/* Whole-track tempo: the family by harmonic strength, the working octave by
 * the prior (the octave is decided on the grid). lo/hi: the allowed range
 * (0 = 50..220); prior: the prior's centre (0 = 125). */
int rx_tempo_estimate(const RxOnsets *o, double lo, double hi, double prior,
                      RxTempoEstimate *out);

/* Phase (seconds, in [0, 60/bpm)) of the onset signal's fundamental at
 * `bpm`, and its strength. */
double rx_tempo_phase(const float *o, int n, double fps, double bpm, double *strength);

/* The octave, decided on a fitted grid (beats at t0 + k * period in
 * [from, to)): its own level, half of it and twice it are judged by the
 * snares and claps of the mid band (too slow when they sit on the half
 * beats about as strongly as on the beats, too fast when every other beat
 * lacks them), by a four-on-the-floor kick (which marks its level), and by
 * a log-normal prior around `prior` where the rhythm leaves it open.
 * Levels outside [lo, hi] (when given, else 50..220) are not considered. */
typedef struct RxOctaveDecision {
    double factor;                  /* 0.5, 1 or 2 */
    double margin;                  /* best score / runner-up (>= 1) */
    double bpm[3];                  /* half, same, double */
    RxOctaveEvidence ev[3];
} RxOctaveDecision;

void rx_tempo_octave_decide(const RxOnsets *o, double t0, double period, double from, double to,
                            double lo, double hi, double prior, RxOctaveDecision *out);

/* The local tempo along the track: one region per stretch where a tempo
 * other than an octave of the main one is settled (the first region's
 * tempo is the main one, corrected when most of the track agrees on
 * another). */
typedef struct RxTempoRegion {
    double start, end;              /* seconds */
    double bpm;
} RxTempoRegion;

int rx_tempo_regions(const RxOnsets *o, double bpm, double lo, double hi,
                     RxTempoRegion **regions, int *nb);

/* ---------------------------------------------------------------- grid fit */

/* One beat as tracked and measured. */
typedef struct RxBeatObs {
    double time;                    /* tracked time (s), on the onset frame grid */
    double attack;                  /* measured attack time (s), NAN if none */
    double weight;                  /* attack strength, 0 if none */
    double kick;                    /* kick-band onset at the beat */
    int    index;                   /* beat count from the first tracked beat */
} RxBeatObs;

/* A stretch of beats at one tempo: beat i of the section is at
 * t0 + (index - index0) * period. */
typedef struct RxFitSection {
    int    first, last;             /* beats [first, last] of the observation array */
    int    index0;                  /* beat count at t0 */
    double t0, period;
    double rms;                     /* weighted residual RMS of the supported beats (s) */
    double jitter;                  /* robust spread of the attacks around the line (s) */
    double scatter;                 /* robust beat-to-beat scatter (s), from second differences */
    int    supported;               /* beats with an attack */
} RxFitSection;

/* Dynamic-programming beat tracker (Ellis 2007) on the accent onsets,
 * following `period_s[f]` (the target period at every frame, seconds). */
int rx_track_beats(const RxOnsets *o, const float *period_s, double tightness,
                   RxBeatObs **beats, int *nb);

/* The fine envelopes of every tracked beat: a window of RX_NB_BANDS x
 * RX_ENV_LEN values around each beat's tracked time. */
typedef struct RxBeatEnvs {
    int     nb;
    float  *env;                    /* [nb][RX_NB_BANDS][RX_ENV_LEN] */
    double *centre;                 /* [nb] time of sample RX_ENV_HALF */
} RxBeatEnvs;

int  rx_beat_envs(RxEnvCtx *ctx, const RxBeatObs *beats, int nb, RxBeatEnvs *out);
void rx_beat_envs_free(RxBeatEnvs *e);

/* The predicted time of beat i: on the section's line when `s` is given,
 * else the tracked time. */
double rx_beat_predict(const RxBeatObs *beats, int i, const RxFitSection *s);

/* Measures the attack of beats [first, last]: the nearest strong rise of
 * the band's envelope within `tol` seconds of the prediction. */
void rx_measure_attacks(const RxBeatEnvs *e, RxBeatObs *beats, int first, int last,
                        int band, const RxFitSection *s, double tol);

/* Weighted robust line fit of the attacks of beats [first, last], into s
 * (first, last, index0, t0, period, rms, jitter, supported). The beats'
 * counts are taken as they are. The fit starts from `start` when given,
 * else from a Theil-Sen estimate, then iterates Tukey's biweight. */
int rx_fit_line(const RxBeatObs *beats, int first, int last, const RxFitSection *start,
                RxFitSection *s);

/* Shifts the counts of beats [first, last] by the whole number of beats
 * that puts them on `line`, judged on the `near` beats of the range
 * closest to the line's own beats (where its extrapolation is exact). A
 * stretch the tracker counted from the off-beat then sits half a beat
 * from the line, an outlier, instead of tilting it. */
void rx_recount(RxBeatObs *beats, int first, int last, const RxFitSection *line, bool from_end);

/* Folds the band envelopes of beats [first, last] on their predictions
 * (the line, or the tracked times when line is NULL): the average beat.
 * Returns the band with the most contrast when band < 0 (else `band`),
 * its contrast, and the offset of its steepest rise from the prediction
 * within +-search seconds (0 if it does not rise). */
int rx_fold(const RxBeatEnvs *e, const RxBeatObs *beats, int first, int last,
            const RxFitSection *line, int band, double search,
            double *contrast, double *offset);

/* Piecewise-constant segmentation (docs/BEATS.md, "Sections"): a
 * change-point search over the attacks with a penalty per section, then
 * evidence tests that merge every boundary a DJ would not need. `support`
 * is the onset signal the support tests read (the kick band when there are
 * kicks); `regions` are the tempogram's. *secs is malloc'd; beats are
 * re-counted per section. */
int rx_segment(RxBeatObs *beats, int nb, const RxOnsets *o, const float *support,
               const RxTempoRegion *regions, int nb_regions, bool allow,
               RxFitSection **secs, int *nb_secs);

/* Integer or half BPM when the drift over the section stays under `tol`
 * seconds; re-phased through the same attacks. Returns true if snapped. */
bool rx_snap_section(const RxBeatObs *beats, RxFitSection *s, double tol);

/* ---------------------------------------------------------------- downbeat */

typedef struct RxDownbeat {
    int    position;                /* 0..7: winning half-beat position of the bar */
    double margin;                  /* winner / runner-up (>= 1) */
    double scores[8];
} RxDownbeat;

/* Votes for the bar position of the beats t0 + k * period in [start, end):
 * position 2j is beat j+1 of the bar that starts at t0; odd positions are
 * the half beats after them. */
int rx_downbeat_vote(const RxSpectrum *s, const float *x, int64_t n,
                     double t0, double period, double start, double end, double first_onset,
                     RxDownbeat *out);

/* ------------------------------------------------------------ diagnostics */

/* What rxbox-eval shows about one analysis. Filled by rx_analyse_beats_dbg
 * when non-NULL; every array is malloc'd and freed by rx_beat_debug_free. */
typedef struct RxBeatDebug {
    RxOnsets        ons;            /* the onset signals */
    RxBeatObs      *beats;          /* tracked beats with their attacks */
    int             nb_beats;
    RxFitSection   *secs;
    int             nb_secs;
    RxTempoEstimate tempo;
    RxOctaveDecision octave;        /* the octave decision on the fitted grid */
    RxDownbeat      downbeat;
    double          kick_share;
    double          jitter;         /* largest section jitter (s) */
    double          scatter;        /* largest section beat-to-beat scatter (s) */
    int             band;           /* envelope band of the first section */
    bool            half_shift;     /* the grid was moved by half a beat */
} RxBeatDebug;

int  rx_analyse_beats_dbg(const float *mono, int64_t nb_samples, int rate,
                          const RxAnalysisOptions *opt, RxBeatResult *out, RxBeatDebug *dbg);
void rx_beat_debug_free(RxBeatDebug *dbg);

#endif /* RXBOX_ANALYSIS_BEATS_INTERNAL_H */
