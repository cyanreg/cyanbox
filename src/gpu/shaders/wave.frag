// Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md.

// The waveform renderer: every pixel of the rectangle reads the ANLZ column
// bytes it covers straight from the column buffer (a buffer device address
// in the push constants, no descriptors) and works out its own colour, so
// the picture is exact at any zoom and nothing is drawn on the CPU.
//
// The rules are rekordbox's as measured by rbxport (see include/cyanbox/gpu.h
// and docs/OVERVIEW.md section 6); the facts are rbxport's, the code is
// not. Per device-pixel column, with c0, c1 the column positions of its
// left and right edges:
//
//   - A detail draw is box-filtered horizontally: the pixel is the
//     picture averaged over [c0, c1). Up to 16 columns a pixel, every
//     column the pixel covers is drawn on its own (the rules below) and
//     the results are blended by the fraction of the pixel each covers, so
//     a column straddling two pixels is in both at those weights and a
//     one-column spike keeps the same total as it scrolls by fractions of
//     a pixel, instead of shimmering as the pixels' footprints shift.
//   - When columns are wider than a pixel and the draw is a detail view,
//     the value is interpolated linearly between a column and the next
//     one, at the pixel's centre, except at a sharp attack - the next
//     column rising from silence, or by 16 or more and to at least twice
//     the level - which stays vertical, since ramping it turns a kick into
//     a diamond. The pixel such an edge (or either end of the data) runs
//     through is split there: each side is drawn on its own value and the
//     two blended by width, so the edge moves smoothly as the view scrolls
//     rather than snapping from pixel to pixel.
//   - Past 16 columns a pixel, and in a draw that is not a detail view
//     (the overview and the row previews, which do not scroll) at any
//     zoom, the columns the pixel covers are [floor(c0), max(floor(c0) + 1,
//     floor(c1))). Each band takes the maximum over them (the single-colour
//     kinds take the column of greatest height, and its colour), so a
//     transient survives many columns to a pixel.
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
//   - `gain` multiplies every band's level before the full scale clamps
//     it (an overview of PWV6 at rekordbox's levels fills its height so).
//   - Insets keep rows clear above and below; `dim` darkens the pixels
//     whose centre is before `dim_off` (the played part); `alpha` fades
//     the whole.
//
// Coverage is exact area: a pixel row partly inside a span takes the
// fraction it covers (nested spans as differences of their coverages), so
// edges are anti-aliased the way a canvas fillRect is, with no seams
// between abutting spans; with the box filter the same holds across. The
// output is premultiplied.

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
    int     kind;           // enum CBWaveKind
    int     mode;           // enum CBWaveMode
    float   inset_top;      // framebuffer pixels
    float   inset_bottom;
    float   unit;           // framebuffer pixels per logical pixel
    float   alpha;
    float   dim;
    float   dim_off;        // dim_until minus col_base
    uint    flags;          // bit 0: detail
    float   gain;           // every band's level times this, before full scale clamps it
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

// Up to this many columns a pixel, a detail draw shades every column the
// pixel covers (the box filter); past it, it takes the maximum. A column
// costs about what a whole pixel did, so this bounds the work at 17
// columns' worth. The player's detail view on a 1900 px window is 1.3 to
// 1.8 columns a pixel at 12 bars of 128 to 175 BPM, 7 to 9.5 at 64 bars;
// only a slower track or a narrower window at 64 bars goes past 16.
const float BOX_SPAN = 16.0;

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

// The pixel row being shaded, [row, row + 1), and the band of rows the
// waveform draws in (main sets them once).
float row;
float ytop, yfloor, usable, centre;
bool  centred;

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

// rbxport's detail rule, on a 0..127 scale: an attack this sharp is not
// ramped.
bool sharp(float a, float b)
{
    return (a == 0.0 && b > 0.0) || (b - a >= 16.0 && b >= 2.0 * a);
}

float interp(float a, float b, float t)
{
    return sharp(a, b) ? a : mix(a, b, t);
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

// The three bytes are in the word of the first and the word of the last
// (which may be the same): two loads, not three, in the loops that read
// many columns a pixel. Neither is past the buffer, which ends at the
// word of the last byte.
vec3 bands(int i)
{
    uint at = uint(3 * i);
    uint lo = pc.cols.words[at >> 2], hi = pc.cols.words[(at + 2u) >> 2];
    uint s = (at & 3u) * 8u;
    uint mid = (at & 3u) == 3u ? hi : lo >> (s + 8u);
    return vec3(float((lo >> s) & 0xFFu), float(mid & 0xFFu),
                float((hi >> (((at + 2u) & 3u) * 8u)) & 0xFFu));
}

// ------------------------------------------------------------ one column

// Everything below returns the pixel row's colour for one value of the
// waveform, premultiplied; the pixel is a blend of one or more of them.

// All bands silent: the line, snapped to whole pixels so that it is crisp.
vec4 silence()
{
    float y = centred ? floor(centre - 0.5 * pc.unit + 0.5)
                      : floor(yfloor - pc.unit + 0.5);
    float cov = cover(y, y + max(1.0, floor(pc.unit + 0.5)));
    return vec4(LOW_HIGH * cov, cov);
}

// 3-band levels, 0..127 before the gain.
vec4 shade_bands(vec3 v)
{
    v = min(v * pc.gain, vec3(127.0));
    if (all(equal(v, vec3(0.0))))
        return silence();

    vec3  rgb = vec3(0.0);
    float a = 0.0;

    if (pc.mode == MODE_STACKED) {
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
        return vec4(rgb, a);
    }

    // A row beyond the furthest band gets nothing, as the stretches below
    // would work out; most rows of a pixel shading many columns are such.
    float full = centred ? usable * 0.5 : usable;
    float reach = max(0.5 * pc.unit, max(v.x, max(v.y, v.z)) / 127.0 * full);
    bool beyond = centred ? abs(row + 0.5 - centre) >= reach + 0.5
                          : (row + 1.0 <= yfloor - reach || row >= yfloor);
    if (beyond)
        return vec4(0.0);

    // Each band's reach, sorted furthest first, then the stretches between
    // consecutive reaches in the colour of every band that reaches that
    // far: the row's coverage by each reach, and each stretch the
    // difference of two. Equal reaches leave an empty stretch between
    // them, and an absent band reaches nowhere (its coverage is 0).
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

    float cov[3];
    for (int k = 0; k < 3; k++)
        cov[k] = centred ? cover(centre - r[k], centre + r[k]) : cover(yfloor - r[k], yfloor);
    rgb = COMBO[bit[0]] * (cov[0] - cov[1]) + COMBO[bit[0] | bit[1]] * (cov[1] - cov[2]) +
          COMBO[7] * cov[2];
    return vec4(rgb, cov[0]);
}

// A column of the single-colour kinds, as column() returns it.
vec4 shade_column(vec4 col)
{
    if (col.a <= 0.0)
        return silence();

    float h = min(col.a * pc.gain, 1.0);
    float cov = centred
        ? cover(centre - max(0.5 * pc.unit, h * usable * 0.5),
                centre + max(0.5 * pc.unit, h * usable * 0.5))
        : cover(yfloor - max(pc.unit, h * usable), yfloor);
    return vec4(col.rgb * cov, cov);
}

vec4 shade(int i)
{
    return pc.kind == KIND_3BAND ? shade_bands(bands(i)) : shade_column(column(i));
}

// ------------------------------------------------------------- the pixel

// Columns wider than a pixel: the detail interpolation at x (relative to
// col_base), between the column under it and the next.
vec4 shade_at(float x)
{
    float f = floor(x);
    int i = pc.col_base + int(f);
    if (i < 0 || i >= pc.nb_columns)
        return vec4(0.0);
    int next = min(i + 1, pc.nb_columns - 1);
    float t = x - f;

    if (pc.kind == KIND_3BAND) {
        vec3 a = bands(i), b = bands(next);
        return shade_bands(vec3(interp(a.x, b.x, t), interp(a.y, b.y, t), interp(a.z, b.z, t)));
    }
    vec4 a = column(i), b = column(next);
    return shade_column(sharp(a.a * 127.0, b.a * 127.0) ? a : mix(a, b, t));
}

// Whether the interpolated picture jumps where column k (absolute) starts:
// at either end of the data, and after a column that a sharp attack in any
// band left flat. Everywhere else it is continuous.
bool jumps(int k)
{
    if (k <= 0 || k >= pc.nb_columns)
        return true;
    if (pc.kind == KIND_3BAND) {
        vec3 a = bands(k - 1), b = bands(k);
        return sharp(a.x, b.x) || sharp(a.y, b.y) || sharp(a.z, b.z);
    }
    return sharp(column(k - 1).a * 127.0, column(k).a * 127.0);
}

vec4 interpolated(float c0, float c1)
{
    // At most one column boundary lies inside the pixel. Where the picture
    // jumps there, either side is drawn on its own value, weighted by its
    // width; elsewhere the value at the pixel's centre stands for it.
    float k = floor(c1);
    if (k > c0 && k < c1 && jumps(pc.col_base + int(k))) {
        float wl = (k - c0) / pc.col_per_px;
        return wl * shade_at(0.5 * (c0 + k)) + (1.0 - wl) * shade_at(0.5 * (k + c1));
    }
    return shade_at(0.5 * (c0 + c1));
}

vec4 box(float c0, float c1)
{
    // Every column the pixel covers, weighted by the part of [c0, c1) it
    // covers; columns past the data add nothing.
    float f0 = floor(c0);
    int n = min(int(ceil(c1) - f0), int(BOX_SPAN) + 1);
    vec4 sum = vec4(0.0);
    for (int j = 0; j < n; j++) {
        float x = f0 + float(j);
        int i = pc.col_base + int(x);
        if (i < 0 || i >= pc.nb_columns)
            continue;
        sum += (min(c1, x + 1.0) - max(c0, x)) * shade(i);
    }
    return sum / pc.col_per_px;
}

vec4 widest(float c0, float c1)
{
    float f0 = floor(c0);
    int first = pc.col_base + int(f0);
    int last  = pc.col_base + max(int(f0) + 1, int(floor(c1)));
    if (last <= 0 || first >= pc.nb_columns)
        return vec4(0.0);
    first = max(first, 0);
    last  = min(last, pc.nb_columns);
    int stride = (last - first + MAX_SPAN - 1) / MAX_SPAN;

    if (pc.kind == KIND_3BAND) {
        vec3 v = vec3(0.0);
        for (int i = first; i < last; i += stride)
            v = max(v, bands(i));
        return shade_bands(v);
    }

    vec4 col = column(first);
    for (int i = first + stride; i < last; i += stride) {
        vec4 c = column(i);
        if (c.a > col.a)
            col = c;
    }
    return shade_column(col);
}

void main()
{
    float px = floor(gl_FragCoord.x);
    row = floor(gl_FragCoord.y);

    float height = pc.rect.w - pc.rect.y;
    float top    = max(0.0, min(pc.inset_top, height * 0.5 - 1.0));
    float bottom = max(0.0, min(pc.inset_bottom, height * 0.5 - 1.0));
    usable  = max(1.0, height - top - bottom);
    ytop    = pc.rect.y + top;
    yfloor  = pc.rect.w - bottom;
    centre  = ytop + usable * 0.5;
    centred = pc.mode == MODE_CENTRED;

    // Column positions of the pixel's edges, relative to col_base.
    float c0 = pc.col_off + (px - pc.rect.x) * pc.col_per_px;
    float c1 = c0 + pc.col_per_px;

    bool detail = (pc.flags & FLAG_DETAIL) != 0u;
    vec4 res;
    if (detail && pc.col_per_px < 1.0)
        res = interpolated(c0, c1);
    else if (detail && pc.col_per_px <= BOX_SPAN)
        res = box(c0, c1);
    else
        res = widest(c0, c1);

    // Nothing is discarded: a transparent premultiplied texel blends to
    // nothing, and `discard` would need the demote-to-helper feature.
    out_colour = vec4(0.0);
    if (res.a <= 0.0)
        return;

    // The played part, judged at the pixel's centre.
    if (pc.dim > 0.0 && c0 + 0.5 * pc.col_per_px < pc.dim_off)
        res.rgb *= 1.0 - pc.dim;

    out_colour = res * pc.alpha;
}
