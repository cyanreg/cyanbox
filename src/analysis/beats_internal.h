/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* Private interface between the stages of the beat-grid analyser
 * (spectrum.c, onset.c, tempo.c, grid_fit.c, downbeat.c, beats.c) and
 * analysis.c. Nothing here is part of cyanbox/analysis.h; the stages are
 * split into files so that each can be read, tested and replaced on its
 * own. The spectral front end's transforms run on the GPU when the options
 * carry a compute context (spectrum.c); nothing after it knows.
 *
 * Time conventions: every time is in seconds of the analysed signal, which
 * is the input resampled to 44.1 kHz with no delay, so it is also seconds
 * of the players' timeline. A spectral frame or onset value is stamped at
 * the centre of its window. */

#ifndef CYANBOX_ANALYSIS_BEATS_INTERNAL_H
#define CYANBOX_ANALYSIS_BEATS_INTERNAL_H

#include <stdint.h>

#include "cyanbox/analysis.h"

/* ---------------------------------------------------------------- spectrum */

#define CB_BEAT_RATE  44100
#define CB_BEAT_NFFT  2048          /* 46 ms: 21.5 Hz bins, enough for a kick band */
#define CB_BEAT_HOP   294           /* 150 frames a second, the waveform column rate */
#define CB_BEAT_NMELS 64

#define CB_BEAT_GROUP 4             /* mel bands per downbeat profile band */
#define CB_BEAT_NPROF (CB_BEAT_NMELS / CB_BEAT_GROUP)

/* What the later stages read of the spectrum. The mel frames themselves
 * are not kept (an hour of them is 138 MB): the flux is taken frame by
 * frame as they are computed, and the downbeat's profile bands are summed
 * from them on the way. */
typedef struct CBSpectrum {
    int    nb_frames;               /* frame f is centred on sample f * CB_BEAT_HOP */
    double fps;
    float *prof;                    /* [nb_frames][CB_BEAT_NPROF]: sums of 4 log-mel bands */
    float *kick;                    /* [nb_frames], log-compressed amplitude 30..150 Hz */
    float *flux_full;               /* [nb_frames] SuperFlux over all mel bands, at the window */
    float *flux_mid;                /* centre, before the onset stage's corrections */
    float *flux_high;
    double mel_hz[CB_BEAT_NMELS];   /* centre of each band */
} CBSpectrum;

/* The version of what cb_spectrum_compute and the envelopes produce: bump
 * it whenever their output changes, so that stored front-end results
 * (cyanbox-eval --cache) are made again. The GPU's output is the same
 * version: it agrees with the CPU's to float rounding. */
#define CB_FRONTEND_VERSION 1

/* Centred Hann STFT, triangular mel bands (30 Hz .. 16 kHz, like dj) on
 * the power spectrum, square-rooted back to an amplitude per band and
 * compressed as dj's log1p(1000 * amplitude / (N/4)). With `gpu`, the
 * transforms and the band energies run there (cyanbox/compute.h) and only
 * the profile sums and the flux here; the CPU takes over if it fails. */
int  cb_spectrum_compute(const float *x, int64_t n, CBCompute *gpu, CBSpectrum *s);
void cb_spectrum_free(CBSpectrum *s);

/* Resamples mono float with libswresample, time-aligned (output sample 0
 * is input time 0). *out is malloc'd. */
int  cb_resample_mono(const float *in, int64_t n, int in_rate, int out_rate,
                      float **out, int64_t *out_n);

/* Fine band envelopes around chosen instants, for attack timing. Rather
 * than band-pass the whole track (three zero-phase IIR filters over 16 M
 * samples cost more than the rest of the analyser together), a window
 * around each beat is transformed once and the bands are cut in the
 * frequency domain: zero phase by construction, and only the ~15 % of the
 * track near beats is ever filtered. */
enum {
    CB_BAND_LOW,                    /* < 150 Hz: kick body */
    CB_BAND_MID,                    /* 150 .. 2500 Hz: snare, clap, kick knock */
    CB_BAND_HIGH,                   /* > 2500 Hz: kick click, hats */
    CB_NB_BANDS,
};

#define CB_ENV_DECIM   16                               /* 2756 Hz, 0.36 ms */
#define CB_ENV_RATE    ((double)CB_BEAT_RATE / CB_ENV_DECIM)
#define CB_ENV_HALF    160                              /* +-58 ms around the centre */
#define CB_ENV_LEN     (2 * CB_ENV_HALF + 1)

typedef struct CBEnvCtx CBEnvCtx;
/* With `gpu`, the windows are transformed there (in batches: call
 * cb_env_windows with every window needed at once); the CPU takes over if
 * it fails. */
int  cb_env_alloc(CBEnvCtx **ctx, const float *x, int64_t n, CBCompute *gpu);
void cb_env_free(CBEnvCtx **ctx);
/* env[b * CB_ENV_LEN + i] is band b at time centre + (i - CB_ENV_HALF) / CB_ENV_RATE,
 * where centre is `t` rounded to a multiple of 1 / CB_ENV_RATE (returned). */
int  cb_env_window(CBEnvCtx *ctx, double t, float *env, double *centre);
/* The same for t[0 .. nb): window i at env + i * CB_NB_BANDS * CB_ENV_LEN. */
int  cb_env_windows(CBEnvCtx *ctx, const double *t, int nb, float *env, double *centre);

/* The bass's pitch class around each of `nb` instants: a 12-bin chroma of
 * 30..250 Hz from a 0.74 s window starting a sixteenth of a second after
 * the instant (past the kick's thump), on the signal decimated to 2756 Hz,
 * where a 2048-point transform resolves 1.35 Hz: enough for bass notes,
 * which the 21.5 Hz bins of the main STFT cannot tell apart. Each chroma
 * sums to 1 (all zero in silence). out is [nb][12]. */
int cb_bass_chroma(const float *x, int64_t n, const double *times, int nb, float *out);

/* ------------------------------------------------------------- music start */

/* Where the music starts, from the level of 6.7 ms frames of the 44.1 kHz
 * signal (start.c). */
typedef struct CBMusicStart {
    enum CBStartKind kind;
    double start;                   /* seconds: a hit's first sample, where a rise leaves
                                     * the floor; 0 when at once */
    double intro;                   /* where a fade before a hit left the floor (= start
                                     * when the start is not a hit after a fade) */
    double audible;                 /* where the level first crosses the start threshold */
    double floor_db;                /* level of the silence or floor before it (-200: digital) */
    double loud_db;                 /* the track's loud level (90th percentile of the frames) */
    double jump_db;                 /* level two frames after the start over its threshold */
    double rise;                    /* seconds from the start to within 15 dB of the loud level */
} CBMusicStart;

int cb_music_start(const float *x, int64_t n, CBMusicStart *out);

/* ------------------------------------------------------------------ onsets */

typedef struct CBOnsets {
    int    nb;                      /* = nb_frames; value i is at i / fps seconds */
    double fps;
    float *full;                    /* SuperFlux over all mel bands */
    float *low;                     /* flux of the kick band */
    float *mid;                     /* flux of 150 .. 2500 Hz (snare, clap) */
    float *high;                    /* flux above 5 kHz (hats) */
    float *accent;                  /* what the beat tracker follows: kick-weighted */
    double kick_share;              /* periodic energy in the kick band vs the full band */
} CBOnsets;

/* The SuperFlux rises of mel frame `cur` against frame `ref` (two frames
 * earlier), summed over all bands, bands [mid0, mid1) and bands from
 * high0. Called by the STFT for every frame. */
void cb_superflux_frame(const float *ref, const float *cur, int mid0, int mid1, int high0,
                        float *full, float *mid, float *high);
/* Takes the spectrum's flux signals (they move into the onsets). */
int  cb_onsets_compute(CBSpectrum *s, CBOnsets *o);
void cb_onsets_free(CBOnsets *o);
/* The first onset that is loud relative to the track (dj's rule; where
 * the music starts is start.c's). Allocates nothing. */
double cb_onsets_first(const CBOnsets *o);
/* Removes the local mean (one second), clips at zero, scales to unit
 * standard deviation. In place. */
int  cb_onset_normalise(float *o, int n, double fps);

/* ------------------------------------------------------------------- tempo */

#define CB_TEMPO_MIN 50.0
#define CB_TEMPO_MAX 220.0

/* The rhythm at one metrical level (cb_tempo_octave_decide). */
typedef struct CBOctaveEvidence {
    double balance;                 /* snare band: min/max of even vs odd beats */
    double subdiv;                  /* snare band: half beats vs beats */
    double kick_alt;                /* kick band: min/max of even vs odd beats (dj's alternation) */
    double kick_sub;                /* kick band: half beats vs beats */
    double kick_bar;                /* kick band: min/max over the four beats of a bar */
    double snare;                   /* the strongest beat of a bar by its snare (mid band above
                                     * its mean, low band not), 0 if none */
    double snare_lone;              /* 1 - the beat two beats from it by the same, over it:
                                     * 1 is a lone snare every four beats, 0 a backbeat */
    double score;                   /* the rhythm's likelihood of the level, without the prior */
} CBOctaveEvidence;

typedef struct CBTempoEstimate {
    double bpm;                     /* working tempo: the best family's octave nearest the prior */
    double strength;                /* its harmonic DFT strength */
    double second_bpm;              /* the runner-up family, near bpm, 0 if none */
    double second_ratio;            /* its strength / the winner's */
    double octave_bpm;              /* the working octave's runner-up, 0 if none */
    double octave_margin;           /* their prior ratio */
} CBTempoEstimate;

/* Whole-track tempo: the family by harmonic strength, the working octave by
 * the prior (the octave is decided on the grid). lo/hi: the allowed range
 * (0 = 50..220); prior: the prior's centre (0 = 125). */
int cb_tempo_estimate(const CBOnsets *o, double lo, double hi, double prior,
                      CBTempoEstimate *out);

/* How strongly the full band repeats at each of the nb tempos: its
 * whole-track autocorrelation at the beat, two-beat and bar lags (the
 * comb cb_tempo_estimate finds its peaks with). */
int cb_tempo_combs(const CBOnsets *o, const double *bpm, int nb, double *out);

/* Phase (seconds, in [0, 60/bpm)) of the onset signal's fundamental at
 * `bpm`, and its strength. */
double cb_tempo_phase(const float *o, int n, double fps, double bpm, double *strength);

/* The octave, decided on a fitted grid (beats at t0 + k * period in
 * [from, to)): its own level, half of it and twice it are judged by the
 * snares and claps of the mid band (too slow when they sit on the half
 * beats about as strongly as on the beats, unless the level above has a
 * lone snare, its half-time reading; too fast when every other beat
 * lacks them), by a four-on-the-floor kick (on each beat of the bar
 * alike, which marks its level), and by a log-normal prior around
 * `prior` where the rhythm leaves it open. Levels outside [lo, hi]
 * (when given, else 50..220) are not considered. */
typedef struct CBOctaveDecision {
    double factor;                  /* 0.5, 1 or 2 */
    double margin;                  /* best score / runner-up (>= 1) */
    double bpm[3];                  /* half, same, double */
    double t0[3];                   /* each level's beats: t0[l] + k * 60 / bpm[l] */
    double from, to;                /* the stretch the evidence was read on */
    CBOctaveEvidence ev[3];
} CBOctaveDecision;

void cb_tempo_octave_decide(const CBOnsets *o, double t0, double period, double from, double to,
                            double lo, double hi, double prior, CBOctaveDecision *out);

/* The local tempo along the track: one region per stretch where a tempo
 * other than an octave of the main one is settled (the first region's
 * tempo is the main one, corrected when most of the track agrees on
 * another). */
typedef struct CBTempoRegion {
    double start, end;              /* seconds */
    double bpm;
} CBTempoRegion;

int cb_tempo_regions(const CBOnsets *o, double bpm, double lo, double hi,
                     CBTempoRegion **regions, int *nb);

/* ---------------------------------------------------------------- grid fit */

/* One beat as tracked and measured. */
typedef struct CBBeatObs {
    double time;                    /* tracked time (s), on the onset frame grid */
    double attack;                  /* measured attack time (s), NAN if none */
    double weight;                  /* attack strength, 0 if none */
    double kick;                    /* kick-band onset at the beat */
    int    index;                   /* beat count from the first tracked beat */
} CBBeatObs;

/* A stretch of beats at one tempo: beat i of the section is at
 * t0 + (index - index0) * period. */
typedef struct CBFitSection {
    int    first, last;             /* beats [first, last] of the observation array */
    int    index0;                  /* beat count at t0 */
    double t0, period;
    double rms;                     /* weighted residual RMS of the supported beats (s) */
    double jitter;                  /* robust spread of the attacks around the line (s) */
    double scatter;                 /* robust beat-to-beat scatter (s), from second differences */
    int    supported;               /* beats with an attack */
    /* The section's grid is its beats' own times (walk.c), not the line:
     * the line is kept as their straight approximation, for the
     * arithmetic that wants a period and a count. Attacks lighter than
     * wmin are noise there, not beats (a quarter of the median weight of
     * the section's attacks, the segmentation's rule). */
    bool   dynamic;
    double wmin;
    /* A dynamic section cut from a straight one, handing over to the
     * straight pieces of that line beside it on the same beat: 1 the
     * piece before, 2 the piece after. Its half of the beat is theirs. */
    unsigned joined;
} CBFitSection;

/* Dynamic-programming beat tracker (Ellis 2007) on the accent onsets,
 * following `period_s[f]` (the target period at every frame, seconds). */
int cb_track_beats(const CBOnsets *o, const float *period_s, double tightness,
                   CBBeatObs **beats, int *nb);
/* The same over the frames [f0, f1] only (times still from 0). */
int cb_track_beats_range(const CBOnsets *o, const float *period_s, double tightness,
                         int f0, int f1, CBBeatObs **beats, int *nb);

/* The fine envelopes of every tracked beat: a window of CB_NB_BANDS x
 * CB_ENV_LEN values around each beat's tracked time. */
typedef struct CBBeatEnvs {
    int     nb;
    float  *env;                    /* [nb][CB_NB_BANDS][CB_ENV_LEN] */
    double *centre;                 /* [nb] time of sample CB_ENV_HALF */
} CBBeatEnvs;

int  cb_beat_envs(CBEnvCtx *ctx, const CBBeatObs *beats, int nb, CBBeatEnvs *out);
void cb_beat_envs_free(CBBeatEnvs *e);

/* The predicted time of beat i: on the section's line when `s` is given,
 * else the tracked time. */
double cb_beat_predict(const CBBeatObs *beats, int i, const CBFitSection *s);

/* How many beats, every first interval, a measured passage that opens a
 * grid gets before its first beat, back to time 0: none when it starts at
 * or before 0 (it may: the beats are tracked from the onsets' frames) or
 * has fewer than two beats. The grid's size is counted with it and the
 * beats are made with it, so the two cannot disagree. */
int cb_beats_back_to_zero(const CBBeatObs *b, int n);

/* Measures the attack of beats [first, last]: the nearest strong rise of
 * the band's envelope within `tol` seconds of the prediction. */
void cb_measure_attacks(const CBBeatEnvs *e, CBBeatObs *beats, int first, int last,
                        int band, const CBFitSection *s, double tol);

/* Weighted robust line fit of the attacks of beats [first, last], into s
 * (first, last, index0, t0, period, rms, jitter, supported). The beats'
 * counts are taken as they are. The fit starts from `start` when given,
 * else from a Theil-Sen estimate, then iterates Tukey's biweight. */
int cb_fit_line(const CBBeatObs *beats, int first, int last, const CBFitSection *start,
                CBFitSection *s);

/* Shifts the counts of beats [first, last] by the whole number of beats
 * that puts them on `line`, judged on the `near` beats of the range
 * closest to the line's own beats (where its extrapolation is exact). A
 * stretch the tracker counted from the off-beat then sits half a beat
 * from the line, an outlier, instead of tilting it. */
void cb_recount(CBBeatObs *beats, int first, int last, const CBFitSection *line, bool from_end);

/* Folds the band envelopes of beats [first, last] on their predictions
 * (the line, or the tracked times when line is NULL): the average beat.
 * Returns the band with the most contrast when band < 0 (else `band`),
 * its contrast, the offset of its steepest rise from the prediction
 * within +-search seconds (0 if it does not rise), and the height of that
 * rise, from the lowest level within the search before it to the highest
 * within the search after it, in the contrast's units (0 if it does not
 * rise; any output may be NULL). The contrast is the whole fold's, +-50
 * ms: a hit just outside the search, its decaying tail inside, has a
 * contrast and no rise. */
int cb_fold(const CBBeatEnvs *e, const CBBeatObs *beats, int first, int last,
            const CBFitSection *line, int band, double search,
            double *contrast, double *offset, double *rise);

/* Piecewise-constant segmentation (docs/BEATS.md, "Sections"): a
 * change-point search over the attacks with a penalty per section, then
 * evidence tests that merge every boundary a DJ would not need. `support`
 * is the onset signal the support tests read (the kick band when there are
 * kicks); `regions` are the tempogram's. *secs is malloc'd; beats are
 * re-counted per section. */
int cb_segment(CBBeatObs *beats, int nb, const CBOnsets *o, const float *support,
               const CBTempoRegion *regions, int nb_regions, bool allow,
               CBFitSection **secs, int *nb_secs);

/* Integer or half BPM when the drift over the section stays under `tol`
 * seconds; re-phased through the same attacks. Returns true if snapped. */
bool cb_snap_section(const CBBeatObs *beats, CBFitSection *s, double tol);

/* ------------------------------------------------------------------ walk */

/* How far the attacks of section s stray from its line (walk.c): dev[i]
 * (one per beat of the section, NAN without an attack) is the running
 * median of the smoothed residuals, *drift the largest of them, *bend
 * how much a quadratic through the attacks rises over the line between
 * the section's middle and its ends, and *bend_noise what the section's
 * own scatter would bend by. */
int cb_line_strain(const CBBeatObs *beats, const CBFitSection *s, double *dev,
                   double *drift, double *bend, double *bend_noise);

/* The walk: beats [first, last], whose counts must be consecutive, get
 * the times a player would follow through their attacks (a robust local
 * line a bar either side, gaps interpolated, the ends extrapolated),
 * written to beats[i].time. `scatter` is the section's beat-to-beat
 * scatter, the floor of the robust scale. -EINVAL with fewer than four
 * attacks or a gap in the counts. */
int cb_walk_beats(CBBeatObs *beats, int first, int last, double scatter);

/* ---------------------------------------------------------------- downbeat */

typedef struct CBDownbeat {
    int    position;                /* 0..7: winning half-beat position of the bar */
    double margin;                  /* winner / runner-up (>= 1) */
    double scores[8];
} CBDownbeat;

/* Votes for the bar position of the beats t0 + k * period in [start, end):
 * position 2j is beat j+1 of the bar that starts at t0; odd positions are
 * the half beats after them. `start_vote` (NAN: none) is where the music
 * starts, a vote of one scale for the beat or half beat there. */
int cb_downbeat_vote(const CBSpectrum *s, const float *x, int64_t n,
                     double t0, double period, double start, double end, double start_vote,
                     CBDownbeat *out);
/* The same vote over beats given by their times (a dynamic section): the
 * J half beats span [from[j], to[j]), the first at position pos0 of the
 * bar (0..7); the bass vote reads the K beats `beat`, the first at the
 * even position bpos0; start_pos is the position the music's start votes
 * for, -1 for none. */
int cb_downbeat_vote_times(const CBSpectrum *s, const float *x, int64_t n,
                           const double *from, const double *to, int J, int pos0,
                           const double *beat, int K, int bpos0, int start_pos,
                           CBDownbeat *out);

/* ------------------------------------------------------------ diagnostics */

/* What cyanbox-eval shows about one analysis. Filled by cb_analyse_beats_dbg
 * when non-NULL; every array is malloc'd and freed by cb_beat_debug_free. */
typedef struct CBBeatDebug {
    CBOnsets        ons;            /* the onset signals */
    CBBeatObs      *beats;          /* tracked beats with their attacks */
    int             nb_beats;
    CBFitSection   *secs;
    int             nb_secs;
    CBTempoEstimate tempo;
    CBOctaveDecision octave;        /* the octave decision on the fitted grid */
    CBDownbeat      downbeat;
    double          kick_share;
    double          jitter;         /* largest section jitter (s) */
    double          scatter;        /* largest section beat-to-beat scatter (s) */
    int             band;           /* envelope band of the first section */
    bool            half_shift;     /* the grid was moved by half a beat */
    /* Where the music starts, and the evidence of the section holding it
     * (before its on/off-beat decision). */
    CBMusicStart    start;
    int             start_section;  /* -1: the music starts at once */
    double          start_kick;     /* log evidence of the kicks for the line as it was */
    double          start_struct;   /* of the structure (without the start's votes) */
    double          start_hit;      /* of the hit after silence */
    long            start_half_beat;/* the start's nearest half beat from t0 (even: a beat) */
    double          start_off;      /* and its distance from it (s) */
    double          start_t0, start_period;
    double          start_scores[8];/* the structure's scores by half-beat position from t0 */
    double          start_kick_ratio;/* a hit's kick band against the kick beats' */
} CBBeatDebug;

/* The grid's first beat may lie this much before the music's start (a
 * line a few ms early on the hit; a fade's start is a frame's estimate). */
#define CB_BEAT_START_SLACK 0.050

/* ------------------------------------------------------- with Beat This! */

/* What the grid with Beat This! (hybrid.c) takes of cyanbox's analysis. */
typedef struct CBHybridHints {
    double        bpm;              /* its tempo (cb_grid_bpm_median), 0: none */
    const double *beats;            /* its beats (s), in order */
    int           nb_beats;
    bool          unsure;           /* of its phase: a flag, or a phase margin under 10 */
    bool          steady;           /* one straight section */
    bool          measured;         /* a passage measured beat by beat */
} CBHybridHints;

/* The beats from Beat This!'s logits (50 a second) and the hints: time,
 * number in the bar of 4, tempo. *out is malloc'd; none (*nb 0) when too
 * few beats are heard. */
int cb_hybrid_beats(const float *beat, const float *downbeat, int nb_frames,
                    const CBHybridHints *h, CBBeat **out, int *nb);

int  cb_analyse_beats_dbg(const float *mono, int64_t nb_samples, int rate,
                          const CBAnalysisOptions *opt, CBBeatResult *out, CBBeatDebug *dbg);
/* The same on a 44.1 kHz signal whose spectrum is already computed
 * (cb_spectrum_compute on x; cyanbox-eval's cache): `spec` is taken over
 * and freed. Everything after the front end, exactly as above. */
int  cb_analyse_beats_spec(const float *x, int64_t n, CBSpectrum *spec,
                           const CBAnalysisOptions *opt, CBBeatResult *out, CBBeatDebug *dbg);
void cb_beat_debug_free(CBBeatDebug *dbg);

#endif /* CYANBOX_ANALYSIS_BEATS_INTERNAL_H */
