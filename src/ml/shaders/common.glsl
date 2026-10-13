// Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md.

// What every kernel of the model shares: the extensions, the buffer types
// its device addresses are read through, and the exact GELU.
//
// Buffers are passed as raw 64-bit addresses in the push constants and cast
// to whichever of these views a kernel reads them through; one buffer may be
// read through several.

#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference2 : require
#extension GL_EXT_shader_explicit_arithmetic_types : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_EXT_shader_explicit_arithmetic_types_float16 : require
#extension GL_EXT_shader_16bit_storage : require
#extension GL_EXT_shader_8bit_storage : require
#extension GL_KHR_shader_subgroup_basic : require
#extension GL_KHR_shader_subgroup_arithmetic : require
#extension GL_KHR_shader_subgroup_shuffle : require

layout(buffer_reference, std430, buffer_reference_align = 4) buffer F32 { float v[]; };
layout(buffer_reference, std430, buffer_reference_align = 16) buffer F32x4 { vec4 v[]; };
layout(buffer_reference, std430, buffer_reference_align = 2) buffer F16 { float16_t v[]; };
layout(buffer_reference, std430, buffer_reference_align = 8) buffer F16x4 { f16vec4 v[]; };
layout(buffer_reference, std430, buffer_reference_align = 4) buffer U32 { uint v[]; };
layout(buffer_reference, std430, buffer_reference_align = 16) buffer U32x4 { uvec4 v[]; };
layout(buffer_reference, std430, buffer_reference_align = 4) buffer I32 { int v[]; };
layout(buffer_reference, std430, buffer_reference_align = 1) buffer U8 { uint8_t v[]; };

// erf(x) from Numerical Recipes' erfc (fractional error below 1.2e-7 over
// the whole line): GLSL has none, and the encoder's GELU is the exact one
// (nn.GELU() and Whisper's "gelu"), not the tanh approximation.
float erf_nr(float x)
{
    float z = abs(x);
    float t = 1.0 / (1.0 + 0.5 * z);
    float r = t * exp(-z * z - 1.26551223 + t * (1.00002368 + t * (0.37409196 +
              t * (0.09678418 + t * (-0.18628806 + t * (0.27886807 + t * (-1.13520398 +
              t * (1.48851587 + t * (-0.82215223 + t * 0.17087277)))))))));
    return x >= 0.0 ? 1.0 - r : r - 1.0;
}

float gelu(float x)
{
    return 0.5 * x * (1.0 + erf_nr(x * 0.70710678118654752));
}

float silu(float x)
{
    return x / (1.0 + exp(-x));
}

// Q4_K (ggml's block_q4_K, 144 bytes for 256 weights): the 6-bit scale and
// min of sub-block s (0..7) from the block's first 16 bytes, words h.
//   h.x: d (low half), dmin (high half), as f16
//   h.y, h.z, h.w: the 12 bytes of scales and mins
uint q4k_byte(uvec4 h, uint b)      // b: byte 4..15 of the block
{
    uint w = b < 8u ? (b < 4u ? h.x : h.y) : (b < 12u ? h.z : h.w);
    return (w >> (8u * (b & 3u))) & 0xffu;
}

vec2 q4k_scale_min(uvec4 h, uint s)
{
    uint sc, m;
    if (s < 4u) {
        sc = q4k_byte(h, 4u + s) & 63u;
        m  = q4k_byte(h, 8u + s) & 63u;
    } else {
        sc = (q4k_byte(h, 8u + s) & 0xfu) | ((q4k_byte(h, s) >> 6) << 4);
        m  = (q4k_byte(h, 8u + s) >> 4) | ((q4k_byte(h, 4u + s) >> 6) << 4);
    }
    vec2 dm = unpackHalf2x16(h.x);
    return vec2(dm.x * float(sc), dm.y * float(m));
}
