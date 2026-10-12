// Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md.

// The waveform quad: four vertices of a triangle strip, generated from the
// destination rectangle in the push constants. No vertex buffer; the
// fragment shader does all the work. The push constant block is shared
// with wave.frag and must match RXWavePush in wave.c.

#version 460
#extension GL_EXT_buffer_reference : require

layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer Columns {
    uint words[];
};

layout(push_constant, std430) uniform Push {
    Columns cols;
    vec4    rect;           // x0, y0, x1, y1 in framebuffer pixels
    vec2    fb_size;
    float   col_off;
    float   col_per_px;
    int     col_base;
    int     nb_columns;
    int     kind;
    int     mode;
    float   inset_top;
    float   inset_bottom;
    float   unit;
    float   alpha;
    float   dim;
    float   dim_off;
    uint    flags;
    float   gain;
} pc;

void main()
{
    // 0 (x0, y0), 1 (x1, y0), 2 (x0, y1), 3 (x1, y1)
    vec2 t = vec2(float(gl_VertexIndex & 1), float(gl_VertexIndex >> 1));
    vec2 p = mix(pc.rect.xy, pc.rect.zw, t);

    // Vulkan's clip space has y pointing down, like ImGui's coordinates.
    gl_Position = vec4(p / pc.fb_size * 2.0 - 1.0, 0.0, 1.0);
}
