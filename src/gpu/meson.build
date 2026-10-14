# Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md.
#
# Three things live here. The Vulkan set-up that the window, the compute
# context and the model share (vkdev.c) is a library, rx_vkdev, which the
# other two and src/ml link. The compute context (compute.c, compute_dsp.c
# and the compute shaders; rxbox/compute.h) is a library, rx_compute: the
# analysis runs its spectral front end on it, and the CLI, the tools and
# the tests use it without a window, so it is configured before the
# analysis. The window (Vulkan, VMA, ImGui glue and the waveform shader) is
# compiled into the executables that open one (rxbox, rxbox-gpu-demo), not
# into a library, so gpu_sources carries the generated shader headers too.
# Shaders are compiled to SPIR-V with glslang (Vulkan 1.3 target) and
# embedded as C arrays by shaders/spv_embed.py.
rx_vkdev_lib = static_library('rx_vkdev', files('vkdev.c'),
    include_directories: rx_inc,
    dependencies: [ rx_common_dep, vulkan_dep, vma_dep ],
)
rx_vkdev_dep = declare_dependency(link_with: rx_vkdev_lib,
                                  dependencies: [ rx_common_dep, vulkan_dep, vma_dep ])

fontconfig_dep = dependency('fontconfig')
gpu_deps = [ fontconfig_dep, libavcodec, libavutil, rx_vkdev_dep ]

gpu_python = find_program('python3')
gpu_spv_embed = files('shaders/spv_embed.py')

compute_shader_headers = []
foreach sh : [ 'stft', 'env' ]
    spv = custom_target(sh + '_comp_spv',
        input: 'shaders' / sh + '.comp',
        output: sh + '_comp.spv',
        depend_files: files('shaders/fft.glsl'),
        command: [ glslang_prog, '--target-env', 'vulkan1.3', '-V', '@INPUT@', '-o', '@OUTPUT@' ],
    )
    compute_shader_headers += custom_target(sh + '_comp_spv_h',
        input: spv,
        output: sh + '_comp.spv.h',
        command: [ gpu_python, gpu_spv_embed, '@INPUT@', '@OUTPUT@', sh + '_comp' ],
    )
endforeach

rx_compute_lib = static_library('rx_compute',
    files('compute.c', 'compute_dsp.c'), compute_shader_headers,
    include_directories: rx_inc,
    dependencies: [ rx_vkdev_dep ],
)
rx_compute_dep = declare_dependency(link_with: rx_compute_lib, dependencies: [ rx_vkdev_dep ])

# The kernels against av_tx in double precision at every size they take.
# Needs a Vulkan device but no window; skipped (77) without one.
test('compute',
    executable('test_compute', '../../tests/gpu/test_compute.c',
        include_directories: rx_inc,
        dependencies: [ rx_compute_dep, libavutil ],
    ),
    suite: 'gpu',
    timeout: 300,
)

gpu_shader_headers = []
foreach sh : [ [ 'wave.vert', 'wave_vert', 'vert' ], [ 'wave.frag', 'wave_frag', 'frag' ] ]
    spv = custom_target(sh[1] + '_spv',
        input: 'shaders' / sh[0],
        output: sh[1] + '.spv',
        command: [ glslang_prog, '--target-env', 'vulkan1.3', '-V', '-S', sh[2],
                   '@INPUT@', '-o', '@OUTPUT@' ],
    )
    gpu_shader_headers += custom_target(sh[1] + '_spv_h',
        input: spv,
        output: sh[1] + '.spv.h',
        command: [ gpu_python, gpu_spv_embed, '@INPUT@', '@OUTPUT@', sh[1] ],
    )
endforeach

gpu_sources = files(
    'window.c',
    'vk.c',
    'swapchain.c',
    'textures.c',
    'wave.c',
    'imgui_glue.cpp',
) + gpu_shader_headers

# The demo run as a test: a pixel check of the waveform colours and a
# check that the detail waveform scrolls smoothly (tests/gpu/run_demo.sh).
# It opens a window, so it only runs where the caller says a private
# compositor is there (RXBOX_GPU_TEST=1 and WAYLAND_DISPLAY set);
# otherwise it is skipped (exit 77). tools/ is configured after this file,
# so the demo is found by path; build it by name first
# (ninja -C build-gpu rxbox-gpu-demo).
test('gpu-demo', find_program('../../tests/gpu/run_demo.sh'),
    args: [ meson.project_build_root() / 'tools' / 'rxbox-gpu-demo',
            meson.project_build_root() / 'gpu-test.png' ],
    suite: 'gpu',
    timeout: 120,
    is_parallel: false,
)
