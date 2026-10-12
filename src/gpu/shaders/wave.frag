// Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md.

// The waveform renderer: every pixel of the rectangle reads the ANLZ column
// bytes it covers straight from the column buffer (a buffer device address
// in the push constants, no descriptors) and works out its own colour, so
// the picture is exact at any zoom and nothing is drawn on the CPU.
//
// The rules are rekordbox's as measured by rbxport (see include/rxbox/gpu.h
// and docs/OVERVIEW.md section 6); the facts are rbxport's, the code is
// not. Per device-pixel column:
//
//   - The columns the pixel covers are [floor(c0), max(floor(c0) + 1,
//     floor(c1))) where c0, c1 are the column positions of its left and
//     right edges. Each band takes the maximum over them (the single-colour
//     kinds take the column of greatest height, and its colour), so a
//     transient survives many columns to a pixel.
//   - When columns are wider than a pixel and the draw is a detail view,
//     the value is interpolated linearly between the column under the left
//     edge and the next one, except at a sharp attack - the next column
//     rising from silence, or by 16 or more and to at least twice the
//     level - which stays vertical, since ramping it turns a kick into a
//     diamond.
//   - All bands silent: a 1 px line (one logical pixel) in #D2DCFA, on the
//     centre line, or along the floor in the half modes. Past the ends of
//     the data nothing is drawn.
//   - 3-band centred: each band reaches value / 127 of half the usable
//     height either way of the centre (at least half a pixel), and each
//     pixel is in the colour of exactly the bands that reach it: the outer
//     envelope in the colour of the furthest band alone, the core in the
//     combination of all. Overlaid half: the same, from the floor, over
//     the whole usable height. Stacked half: low, then mid on it, then high
//     on that, in blue, amber and cream, scaled by 128, 256 and 128.
//   - Blue: height / 31, coloured from #0055E1 to #F5EBD7 by whiteness / 7.
//   - RGB: the strongest channel at full, the others in proportion.
//   - Insets keep rows clear above and below; `dim` darkens the columns
//     before `dim_off` (the played part); `alpha` fades the whole.
//
// Coverage is exact area: a pixel row partly inside a span takes the
// fraction it covers (nested spans as differences of their coverages), so
// edges are anti-aliased the way a canvas fillRect is, with no seams
// between abutting spans. The output is premultiplied.

#version 460
#extension GL_EXT_buffer_reference : require

layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer Columns {
    uint words[];
};

layout(push_constant, std430) uniform Push {
    Columns cols;
    vec4    rect;           // x0, y0, x1, y1 in framebuffer pixels
    vec2    fb_size;
    float   col_off;        // column at rect.x, minus col_base
    float   col_per_px;     // columns per framebuffer pixel
    int     col_base;       // integer part of the first column, for precision
    int     nb_columns;
    int     kind;           // enum RxWaveKind
    int     mode;           // enum RxWaveMode
    float   inset_top;      // framebuffer pixels
    float   inset_bottom;
    float   unit;           // framebuffer pixels per logical pixel
    float   alpha;
    float   dim;
    float   dim_off;        // dim_until minus col_base
    uint    flags;          // bit 0: detail
} pc;

layout(location = 0) out vec4 out_colour;

const int KIND_3BAND       = 0;
const int KIND_BLUE        = 1;
const int KIND_RGB_DETAIL  = 2;
const int KIND_RGB_PREVIEW = 3;

const int MODE_CENTRED  = 0;
const int MODE_STACKED  = 1;
const int MODE_OVERLAID = 2;

const uint FLAG_DETAIL = 1u;

// A pixel never reads more columns than this; beyond it the span is
// sampled at a stride (a whole long track squeezed into a few pixels).
const int MAX_SPAN = 4096;

// rekordbox's 3-band palette, one colour per combination of bands
// (bit 0 low, bit 1 mid, bit 2 high), sampled by rbxport.
const vec3 LOW      = vec3(0x00, 0x55, 0xE1) / 255.0;
const vec3 MID      = vec3(0xFF, 0xA6, 0x00) / 255.0;
const vec3 HIGH     = vec3(0xFF, 0xFF, 0xFF) / 255.0;
const vec3 LOW_MID  = vec3(0xB4, 0x69, 0x0A) / 255.0;
const vec3 LOW_HIGH = vec3(0xD2, 0xDC, 0xFA) / 255.0;
const vec3 MID_HIGH = vec3(0xFF, 0xF0, 0xD7) / 255.0;
const vec3 ALL      = vec3(0xF5, 0xEB, 0xD7) / 255.0;

const vec3 COMBO[8] = vec3[8](LOW, LOW, MID, LOW_MID, HIGH, LOW_HIGH, MID_HIGH, ALL);

// The stacked preview's slabs and their full scales (rbxport: measured
// against a 2x capture of rekordbox, 0.542 / 0.270 / 0.567 px per unit).
const vec3  STACK_COLOUR[3] = vec3[3](LOW, MID, ALL);
const float STACK_SCALE[3]  = float[3](128.0, 256.0, 128.0);

// The pixel row being shaded, [row, row + 1).
float row;

uint byte_at(int i)
{
    uint u = uint(i);
    return (pc.cols.words[u >> 2] >> ((u & 3u) * 8u)) & 0xFFu;
}

// Fraction of the pixel row inside [a, b].
float cover(float a, float b)
{
    return clamp(min(row + 1.0, b) - max(row, a), 0.0, 1.0);
}

// rbxport's detail rule, on a 0..127 scale.
float interp(float a, float b, float t)
{
    bool sharp = (a == 0.0 && b > 0.0) || (b - a >= 16.0 && b >= 2.0 * a);
    return sharp ? a : mix(a, b, t);
}

vec3 saturated(vec3 c)
{
    float m = max(c.r, max(c.g, c.b));
    return m > 0.0 ? c / m : vec3(0.0);
}

// A column of the single-colour kinds: colour, and height 0..1.
vec4 column(int i)
{
    if (pc.kind == KIND_BLUE) {
        uint b = byte_at(i);
        float white = float(b >> 5) / 7.0;
        return vec4(mix(LOW, ALL, white), float(b & 31u) / 31.0);
    }

    if (pc.kind == KIND_RGB_DETAIL) {
        // Big-endian rrrgggbbbhhhhh00 (PWV5).
        uint word = (byte_at(2 * i) << 8) | byte_at(2 * i + 1);
        vec3 c = vec3(float((word >> 13) & 7u), float((word >> 10) & 7u),
                      float((word >> 7) & 7u));
        return vec4(saturated(c), float((word >> 2) & 31u) / 31.0);
    }

    // PWV4: height, two unknown bytes, then red, green, blue.
    int at = 6 * i;
    vec3 c = vec3(float(byte_at(at + 3)), float(byte_at(at + 4)), float(byte_at(at + 5)));
    return vec4(saturated(c), min(float(byte_at(at)), 127.0) / 127.0);
}

vec3 bands(int i)
{
    int at = 3 * i;
    return vec3(float(byte_at(at)), float(byte_at(at + 1)), float(byte_at(at + 2)));
}

void main()
{
    float px = floor(gl_FragCoord.x);
    row = floor(gl_FragCoord.y);

    // Column positions of the pixel's edges, relative to col_base.
    float c0 = pc.col_off + (px - pc.rect.x) * pc.col_per_px;
    float c1 = c0 + pc.col_per_px;
    float f0 = floor(c0);
    int first = pc.col_base + int(f0);
    int last  = pc.col_base + max(int(f0) + 1, int(floor(c1)));
    // Nothing is discarded: a transparent premultiplied texel blends to
    // nothing, and `discard` would need the demote-to-helper feature.
    out_colour = vec4(0.0);
    if (last <= 0 || first >= pc.nb_columns)
        return;
    first = max(first, 0);
    last  = min(last, pc.nb_columns);

    bool interpolate = (pc.flags & FLAG_DETAIL) != 0u && pc.col_per_px < 1.0;
    float t = c0 - f0;
    int next = min(first + 1, pc.nb_columns - 1);
    int span = last - first;
    int stride = (span + MAX_SPAN - 1) / MAX_SPAN;

    // The band the waveform draws in.
    float height = pc.rect.w - pc.rect.y;
    float top    = max(0.0, min(pc.inset_top, height * 0.5 - 1.0));
    float bottom = max(0.0, min(pc.inset_bottom, height * 0.5 - 1.0));
    float usable = max(1.0, height - top - bottom);
    float ytop   = pc.rect.y + top;
    float yfloor = pc.rect.w - bottom;
    float centre = ytop + usable * 0.5;
    bool  centred = pc.mode == MODE_CENTRED;

    vec3  rgb = vec3(0.0);
    float a = 0.0;
    bool  silent;

    if (pc.kind == KIND_3BAND) {
        vec3 v = vec3(0.0);
        if (interpolate) {
            vec3 va = bands(first), vb = bands(next);
            v = vec3(interp(va.x, vb.x, t), interp(va.y, vb.y, t), interp(va.z, vb.z, t));
        } else {
            for (int i = first; i < last; i += stride)
                v = max(v, bands(i));
        }
        v = min(v, vec3(127.0));
        silent = all(equal(v, vec3(0.0)));

        if (!silent && pc.mode == MODE_STACKED) {
            float base = yfloor;
            for (int b = 0; b < 3; b++) {
                if (v[b] <= 0.0)
                    continue;
                float tall = min(v[b] / STACK_SCALE[b], 1.0) * usable;
                float cov = cover(max(ytop, base - tall), base);
                rgb += STACK_COLOUR[b] * cov;
                a += cov;
                base -= tall;
                if (base <= ytop)
                    break;
            }
        } else if (!silent) {
            // Each band's reach, sorted furthest first, then the stretches
            // between consecutive reaches in the colour of every band that
            // reaches that far. Equal reaches share one stretch.
            float full = centred ? usable * 0.5 : usable;
            float r[3];
            uint bit[3] = uint[3](1u, 2u, 4u);
            for (int b = 0; b < 3; b++)
                r[b] = v[b] > 0.0 ? max(0.5 * pc.unit, v[b] / 127.0 * full) : 0.0;
            for (int i = 0; i < 2; i++) {
                for (int j = 0; j < 2 - i; j++) {
                    if (r[j] < r[j + 1]) {
                        float tr = r[j]; r[j] = r[j + 1]; r[j + 1] = tr;
                        uint tb = bit[j]; bit[j] = bit[j + 1]; bit[j + 1] = tb;
                    }
                }
            }

            uint combo = 0u;
            for (int k = 0; k < 3; k++) {
                if (r[k] <= 0.0)
                    break;
                combo |= bit[k];
                float inner = k < 2 ? r[k + 1] : 0.0;
                if (inner == r[k])
                    continue;
                float cov_out = centred ? cover(centre - r[k], centre + r[k])
                                        : cover(yfloor - r[k], yfloor);
                float cov_in = inner <= 0.0 ? 0.0 :
                               centred ? cover(centre - inner, centre + inner)
                                       : cover(yfloor - inner, yfloor);
                rgb += COMBO[combo] * (cov_out - cov_in);
                if (a == 0.0)
                    a = cov_out;
            }
        }
    } else {
        vec4 col;
        if (interpolate) {
            vec4 ca = column(first), cb = column(next);
            float ha = ca.a * 127.0, hb = cb.a * 127.0;
            bool sharp = (ha == 0.0 && hb > 0.0) || (hb - ha >= 16.0 && hb >= 2.0 * ha);
            col = sharp ? ca : mix(ca, cb, t);
        } else {
            col = column(first);
            for (int i = first + stride; i < last; i += stride) {
                vec4 c = column(i);
                if (c.a > col.a)
                    col = c;
            }
        }
        silent = col.a <= 0.0;

        if (!silent) {
            float h = min(col.a, 1.0);
            float cov = centred
                ? cover(centre - max(0.5 * pc.unit, h * usable * 0.5),
                        centre + max(0.5 * pc.unit, h * usable * 0.5))
                : cover(yfloor - max(pc.unit, h * usable), yfloor);
            rgb = col.rgb * cov;
            a = cov;
        }
    }

    if (silent) {
        // Snapped to whole pixels so the line is crisp.
        float y = centred ? floor(centre - 0.5 * pc.unit + 0.5)
                          : floor(yfloor - pc.unit + 0.5);
        float cov = cover(y, y + max(1.0, floor(pc.unit + 0.5)));
        rgb = LOW_HIGH * cov;
        a = cov;
    }

    if (a <= 0.0)
        return;

    // The played part, judged at the pixel's centre.
    if (pc.dim > 0.0 && c0 + 0.5 * pc.col_per_px < pc.dim_off)
        rgb *= 1.0 - pc.dim;

    out_colour = vec4(rgb, a) * pc.alpha;
}
