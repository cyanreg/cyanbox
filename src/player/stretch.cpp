/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

// The C side of Signalsmith Stretch for MASTER TEMPO (see stretch.h). The
// only C++ in the player: the library is a header of templates.
//
// The configuration was chosen by measurement (48 kHz, a 30 s house track
// and a 30 s vocal track at tempo 1.08 and 0.94, varispeed by the deck's own
// cubic interpolator, then shifted back and compared with the original by
// the mean log-spectral distance of 4096-point frames, 60 Hz to 15 kHz):
//  - presetDefault, without split computation: 3.3 to 3.7 dB from the
//    original (the uncorrected varispeed is 9.2 to 9.3 dB), 1.3 % of one
//    core, and a delay of 5760 frames (120 ms) in to out. presetCheaper
//    measured 3.6 to 4.0 dB at 0.8 % of a core and 4800 frames, and missed
//    pure bass tones by more (55 Hz: +83 cents against +27; 110 Hz at
//    tempo 0.94: +36 against +13). The delay is hidden from transport
//    actions anyway (deck.c restarts the shifter ahead of the playhead), so
//    the better sound wins. Split computation spreads a block's work over
//    the following interval at the price of another interval of delay; a
//    block costs about 0.4 ms, which a 5.3 ms callback (256 frames) absorbs.
//  - No tonality limit. The limit maps frequencies above it by a constant
//    offset rather than by the factor, which suits a vocal pitched for
//    effect; here the factor has to undo a varispeed that scaled every
//    frequency by the same ratio, and with a limit at 8 kHz the distance
//    rose from 3.6 to 6.4 dB: cymbals and hi-hats stayed off pitch.
//  - A fixed seed, so a run is repeatable. The generator is only used for
//    time stretches beyond 2x, which a time ratio of one never asks for.

extern "C" {
#include "rxbox/common.h"
#include "stretch.h"
}

#include <new>

#include "signalsmith-stretch/signalsmith-stretch.h"

using Stretch = signalsmith::stretch::SignalsmithStretch<float>;

struct RXStretch {
    Stretch st;

    RXStretch() : st(1) {}
};

int rx_stretch_create(RXStretch **out, int channels, int rate)
{
    *out = nullptr;
    if (channels < 1 || rate <= 0)
        return RX_ERROR(EINVAL);

    RXStretch *s = nullptr;
    try {
        s = new RXStretch();
        s->st.presetDefault(channels, (float)rate, false);
    } catch (...) {
        delete s;
        return RX_ERROR(ENOMEM);
    }

    *out = s;
    return 0;
}

void rx_stretch_destroy(RXStretch *s)
{
    delete s;
}

int rx_stretch_latency(const RXStretch *s)
{
    return s->st.inputLatency() + s->st.outputLatency();
}

int rx_stretch_input_latency(const RXStretch *s)
{
    return s->st.inputLatency();
}

void rx_stretch_set_pitch(RXStretch *s, float factor)
{
    s->st.setTransposeFactor(factor);
}

void rx_stretch_reset(RXStretch *s)
{
    s->st.reset();
}

// outputSeek() resets, takes inputLatency() frames as the context before
// the start and turns the remaining outputLatency() frames into the output
// that precedes it, folded back so the start is clean. At a time ratio of
// one it wants exactly the latency in frames (outputSeekLength(1.0)).
void rx_stretch_seek(RXStretch *s, const float *const *in, int frames)
{
    s->st.outputSeek(in, frames);
}

void rx_stretch_process(RXStretch *s, const float *const *in, float *const *out, int frames)
{
    s->st.process(in, frames, out, frames);
}
