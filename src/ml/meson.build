# Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md.
#
# ml: MOSS-Audio on Vulkan compute (cyanbox/ml.h). Its own VkDevice
# (ml_priv.h says why); the shaders are GLSL compiled by glslang to SPIR-V
# at build time and embedded as C arrays (shaders/spv_embed.py), as
# src/gpu's are. A shader source may build several kernels, by defines.

ml_python = find_program('python3')
ml_spv_embed = files('shaders/spv_embed.py')
ml_shader_deps = files('shaders/common.glsl')

ml_shader_headers = []
foreach sh : [
    [ 'mel',           'mel.comp',          [] ],
    [ 'mel_norm',      'mel_norm.comp',     [] ],
    [ 'conv1',         'conv1.comp',        [] ],
    [ 'conv_permute',  'conv_permute.comp', [] ],
    [ 'stem_permute',  'stem_permute.comp', [] ],
    [ 'gemm_f16',      'gemm.comp',         [] ],
    [ 'gemm_f16_conv', 'gemm.comp',         [ '-DA_CONV' ] ],
    [ 'gemm_q4k',      'gemm.comp',         [ '-DW_Q4K' ] ],
    [ 'gemm_q4k_small', 'gemm.comp',        [ '-DW_Q4K', '-DSMALL_M' ] ],
    [ 'gemv_q4k',      'gemv.comp',         [] ],
    [ 'layernorm',     'norm.comp',         [] ],
    [ 'rmsnorm',       'norm.comp',         [ '-DRMS' ] ],
    [ 'act',           'act.comp',          [] ],
    [ 'enc_attn',      'enc_attn.comp',     [] ],
    [ 'qk_rope',       'qk_rope.comp',      [] ],
    [ 'attn',          'attn.comp',         [] ],
    [ 'attn_fcomb',    'attn.comp',         [ '-DCOMBINE' ] ],
    [ 'attn_dec',      'attn_dec.comp',     [] ],
    [ 'attn_comb',     'attn_dec.comp',     [ '-DCOMBINE' ] ],
    [ 'embed',         'embed.comp',        [] ],
    [ 'argmax',        'argmax.comp',       [] ],
]
    spv = custom_target(sh[0] + '_comp_spv',
        input: 'shaders' / sh[1],
        output: sh[0] + '_comp.spv',
        depend_files: ml_shader_deps,
        command: [ glslang_prog, '--target-env', 'vulkan1.3', '-V' ] + sh[2] +
                 [ '@INPUT@', '-o', '@OUTPUT@' ],
    )
    ml_shader_headers += custom_target(sh[0] + '_comp_spv_h',
        input: spv,
        output: sh[0] + '_comp.spv.h',
        command: [ ml_python, ml_spv_embed, '@INPUT@', '@OUTPUT@', sh[0] + '_comp' ],
    )
endforeach

cb_ml_lib = static_library('cb_ml',
    files('gpu.c', 'gguf.c', 'tokenizer.c', 'moss.c', 'tag.c', 'answers.c'), ml_shader_headers,
    include_directories: cb_inc,
    dependencies: [ cb_vkdev_dep, m_dep ],
)
cb_ml_dep = declare_dependency(link_with: cb_ml_lib,
                               dependencies: [ cb_vkdev_dep ])

# The kernels against plain C on small random inputs. Needs a Vulkan device
# that can run the model; skipped (77) without one.
test('ml_kernels',
    executable('test_ml_kernels', '../../tests/ml/test_kernels.c',
        include_directories: [ cb_inc, include_directories('.') ],
        dependencies: [ cb_ml_dep ],
    ),
    suite: 'ml',
    timeout: 300,
)

# Questions asked together against the same asked one at a time, bit for
# bit. Needs the model ($CYANBOX_ML_MODEL) and a GPU that runs it; skipped
# (77) without them. On its own: two models on one GPU have made the
# driver reset it.
test('ml_ask',
    executable('test_ml_ask', '../../tests/ml/test_ask.c',
        include_directories: [ cb_inc, include_directories('.') ],
        dependencies: [ cb_ml_dep ],
    ),
    suite: 'ml',
    timeout: 300,
    is_parallel: false,
)

# What the tags make of the model's answers in words, and that an answer
# cut short once settled makes the same. No GPU.
test('ml_answers',
    executable('test_ml_answers', '../../tests/ml/test_answers.c',
        include_directories: [ cb_inc, include_directories('.') ],
        dependencies: [ cb_ml_dep ],
    ),
    suite: 'ml',
    timeout: 60,
)

# The model file's header against crafted files that must be refused (a
# zero alignment, sizes that overflow, offsets that wrap, tensors of the
# wrong shape). No GPU: the refusals all come before the device.
test('ml_gguf',
    executable('test_ml_gguf', '../../tests/ml/test_gguf.c',
        include_directories: [ cb_inc, include_directories('.') ],
        dependencies: [ cb_ml_dep ],
    ),
    suite: 'ml',
    timeout: 60,
)
