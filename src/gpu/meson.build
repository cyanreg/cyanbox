# Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md.
#
# Window, Vulkan, VMA, ImGui glue and the waveform shader. The sources are
# compiled into the executables that open a window (rxbox, rxbox-gpu-demo),
# not into a library, so gpu_sources carries the generated shader headers
# too. Shaders are compiled to SPIR-V with glslang (Vulkan 1.3 target) and
# embedded as C arrays by shaders/spv_embed.py.
fontconfig_dep = dependency('fontconfig')
gpu_deps = [ fontconfig_dep, libavcodec, libavutil ]

gpu_python = find_program('python3')
gpu_spv_embed = files('shaders/spv_embed.py')

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

# The demo run as a test: 30 frames with a pixel check of the waveform
# colours. It opens a window, so it only runs where the caller says a
# private compositor is there (RXBOX_GPU_TEST=1 and WAYLAND_DISPLAY set);
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
