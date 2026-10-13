// Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md.

// A complex FFT of M = 2^LOG2M points in shared memory, by one workgroup of
// 256 invocations, and the two steps that turn it into a real FFT of 2M
// points and back. Included by stft.comp and env.comp.
//
// Stockham autosort (natural order in and out, no bit reversal): radix 4
// while four sub-transforms fit, then one radix-2 stage when LOG2M is odd.
// Each stage reads every point once into registers, waits for the whole
// workgroup, and writes every point once, so one shared array serves as
// both the source and the destination. An invocation holds at most 16
// points across that barrier: M <= 4096 for radix 4 (four butterflies of
// four), M <= 2048 for the radix-2 stage. The loops over an invocation's
// butterflies have a constant trip count with a bounds test inside, so the
// compiler unrolls them and the points stay in registers.
//
// Twiddles come from a table the CPU computed in double precision:
// tw.w[k] = exp(-2 pi i k / 2M), k in [0, 2M). The complex transform uses
// its even entries; the real transform's split step the others. GPU sin()
// and cos() may be off by 2^-11 (the GLSL spec's bound), far worse than the
// av_tx transforms the results are compared with.

#extension GL_EXT_buffer_reference : require

layout(constant_id = 0) const uint LOG2M = 10;
const uint M = 1u << LOG2M;
const uint THREADS = 256u;

layout(buffer_reference, std430, buffer_reference_align = 8) readonly buffer Twiddles {
    vec2 w[];
};

shared vec2 buf[M];

vec2 cmul(vec2 a, vec2 b)
{
    return vec2(a.x * b.x - a.y * b.y, a.x * b.y + a.y * b.x);
}

// exp(-2 pi i m / M)
vec2 tw_m(Twiddles tw, uint m)
{
    return tw.w[2u * m];
}

// Forward (e^-i) complex FFT of buf[0 .. M), in place. Ends with a barrier.
void fft_forward(Twiddles tw)
{
    uint tid = gl_LocalInvocationIndex;
    uint ns = 1u;

    while (ns * 4u <= M) {
        vec2 v[16];
        for (uint b = 0u; b < 4u; b++) {
            uint j = tid + b * THREADS;
            if (j < M / 4u) {
                uint k = j & (ns - 1u);
                uint step = M / (4u * ns);
                vec2 x0 = buf[j];
                vec2 x1 = cmul(buf[j + M / 4u], tw_m(tw, k * step));
                vec2 x2 = cmul(buf[j + M / 2u], tw_m(tw, 2u * k * step));
                vec2 x3 = cmul(buf[j + 3u * M / 4u], tw_m(tw, 3u * k * step));
                vec2 a0 = x0 + x2, a1 = x0 - x2, a2 = x1 + x3, a3 = x1 - x3;
                a3 = vec2(a3.y, -a3.x);     // times -i
                v[4u * b]      = a0 + a2;
                v[4u * b + 1u] = a1 + a3;
                v[4u * b + 2u] = a0 - a2;
                v[4u * b + 3u] = a1 - a3;
            }
        }
        memoryBarrierShared();
        barrier();
        for (uint b = 0u; b < 4u; b++) {
            uint j = tid + b * THREADS;
            if (j < M / 4u) {
                uint d = (j / ns) * ns * 4u + (j & (ns - 1u));
                buf[d]           = v[4u * b];
                buf[d + ns]      = v[4u * b + 1u];
                buf[d + 2u * ns] = v[4u * b + 2u];
                buf[d + 3u * ns] = v[4u * b + 3u];
            }
        }
        memoryBarrierShared();
        barrier();
        ns *= 4u;
    }

    if (ns < M) {
        // The radix-2 stage: ns = M / 2.
        vec2 v[16];
        for (uint b = 0u; b < 4u; b++) {
            uint j = tid + b * THREADS;
            if (j < M / 2u) {
                uint k = j & (ns - 1u);
                vec2 x0 = buf[j];
                vec2 x1 = cmul(buf[j + M / 2u], tw_m(tw, k * (M / (2u * ns))));
                v[2u * b]      = x0 + x1;
                v[2u * b + 1u] = x0 - x1;
            }
        }
        memoryBarrierShared();
        barrier();
        for (uint b = 0u; b < 4u; b++) {
            uint j = tid + b * THREADS;
            if (j < M / 2u) {
                uint d = (j / ns) * ns * 2u + (j & (ns - 1u));
                buf[d]      = v[2u * b];
                buf[d + ns] = v[2u * b + 1u];
            }
        }
        memoryBarrierShared();
        barrier();
    }
}

// The real FFT of 2M points x, from Z = FFT(z), z[m] = x[2m] + i x[2m + 1]:
// X[k] and X[M - k] for k in [0, M / 2] (k = 0 gives X[0] and X[M]).
//   X[k] = E + w_k O,  E = (Z[k] + conj Z[M - k]) / 2,  O = (Z[k] - conj Z[M - k]) / 2i
// and X[M - k] = conj E + w_(M-k) conj O.
void real_split(Twiddles tw, uint k, out vec2 xk, out vec2 xmk)
{
    vec2 zk = buf[k], zm = buf[(M - k) & (M - 1u)];
    vec2 e = 0.5 * vec2(zk.x + zm.x, zk.y - zm.y);
    vec2 d = vec2(zk.x - zm.x, zk.y + zm.y);
    vec2 o = 0.5 * vec2(d.y, -d.x);
    xk = e + cmul(tw.w[k], o);
    xmk = vec2(e.x, -e.y) + cmul(tw.w[M - k], vec2(o.x, -o.y));
}

// The inverse of real_split, as the input of a forward FFT: from X[k] and
// X[M - k] (the spectrum of 2M real points), the conjugates of
//   Z'[k] = (X[k] + conj X[M - k]) + i conj(w_k) (X[k] - conj X[M - k])
// and of Z'[M - k] alike, so that the forward FFT of buf, conjugated, is
// z[m] = y[2m] + i y[2m + 1] with y the unnormalised inverse transform
// (2M times the signal).
void real_merge(Twiddles tw, uint k, vec2 xk, vec2 xmk, out vec2 zk, out vec2 zmk)
{
    vec2 s = vec2(xk.x + xmk.x, xk.y - xmk.y);
    vec2 d = vec2(xk.x - xmk.x, xk.y + xmk.y);
    vec2 wk = tw.w[k];
    vec2 t = cmul(vec2(wk.x, -wk.y), d);
    zk = s + vec2(-t.y, t.x);
    s = vec2(xmk.x + xk.x, xmk.y - xk.y);
    d = vec2(xmk.x - xk.x, xmk.y + xk.y);
    wk = tw.w[M - k];
    t = cmul(vec2(wk.x, -wk.y), d);
    zmk = s + vec2(-t.y, t.x);
    zk = vec2(zk.x, -zk.y);
    zmk = vec2(zmk.x, -zmk.y);
}
