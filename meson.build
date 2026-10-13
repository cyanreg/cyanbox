# Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md.

project('rxbox',
    [ 'c', 'cpp' ],
    license: 'BSD-2-Clause',
    license_files: [ 'LICENSE.md' ],
    default_options: [
        'buildtype=debugoptimized',
        'c_std=c23',
        'cpp_std=c++17',
        'warning_level=1',
        'b_ndebug=if-release',
    ],
    version: '0.1.0',
    meson_version: '>=1.4.0',
)

cc = meson.get_compiler('c')
cxx = meson.get_compiler('cpp')

# Compiler
#============================================================================
build_args = [
    '-D_GNU_SOURCE',
    '-D_ISOC23_SOURCE',
    '-D_FILE_OFFSET_BITS=64',
    '-U__STRICT_ANSI__',

    # Warnings
    '-Wundef', '-Wmissing-prototypes', '-Wshadow', '-Wparentheses',
    '-Wpointer-arith', '-Wno-pointer-sign', '-Wno-unused-parameter',

    # Warnings to treat as errors
    '-Werror=implicit-function-declaration', '-Werror=missing-prototypes',
    '-Werror=vla', '-Werror=incompatible-pointer-types',
    '-Werror=int-conversion', '-Werror=return-type',
]
add_project_arguments(cc.get_supported_arguments(build_args), language: 'c')
add_project_arguments(cxx.get_supported_arguments([
    '-D_GNU_SOURCE', '-Wshadow', '-Wno-unused-parameter', '-Werror=return-type',
]), language: 'cpp')

if cc.has_header('stdckdint.h') == false
    error('Compiler lacks stdckdint.h, cannot compile!')
endif

# Dependencies
#============================================================================
m_dep        = cc.find_library('m', required: false)
threads_dep  = dependency('threads')
libavformat  = dependency('libavformat')
libavcodec   = dependency('libavcodec')
libavutil    = dependency('libavutil')
libswresample = dependency('libswresample')
libswscale   = dependency('libswscale')
sqlite3_dep  = dependency('sqlite3')
vulkan_dep   = dependency('vulkan')
sdl3_dep     = dependency('sdl3')
pipewire_dep = dependency('libpipewire-0.3')
wayland_client = dependency('wayland-client')
glslang_prog = find_program('glslangValidator', 'glslang', required: true)

# Signalsmith Stretch (MIT), the master tempo pitch shifter: header-only C++,
# a git submodule in subprojects/ read through its own CMakeLists.txt. That
# file would fetch signalsmith-linear (MIT, its FFT and STFT) over the network
# with FetchContent; signalsmith-linear is a submodule too, pinned at the tag
# Stretch asks for (0.6.4), and is its own CMake subproject here, because
# meson drops include directories one subproject hands out from another's
# tree. FetchContent is told to try find_package() first and finds the empty
# stand-in in subprojects/cmake/, so it never downloads anything. Both are
# INTERFACE targets: no library is built, the headers compile into the
# player, statically.
cmake = import('cmake')
linear_opts = cmake.subproject_options()
linear_opts.add_cmake_defines({ 'BUILD_SHARED_LIBS': false })
linear_opts.set_override_option('default_library', 'static')
linear_proj = cmake.subproject('signalsmith-linear', options: linear_opts)
stretch_opts = cmake.subproject_options()
stretch_opts.add_cmake_defines({
    'FETCHCONTENT_TRY_FIND_PACKAGE_MODE': 'ALWAYS',
    'FETCHCONTENT_FULLY_DISCONNECTED': true,
    'signalsmith-linear_DIR': meson.project_source_root() / 'subprojects' / 'cmake',
    'BUILD_SHARED_LIBS': false,
})
stretch_opts.set_override_option('default_library', 'static')
stretch_proj = cmake.subproject('signalsmith-stretch', options: stretch_opts)
stretch_dep = [
    stretch_proj.dependency('signalsmith-stretch', include_type: 'system'),
    linear_proj.dependency('signalsmith-linear', include_type: 'system'),
]

# Configure file
#============================================================================
conf = configuration_data()
conf.set_quoted('RXBOX_VERSION', meson.project_version())
configure_file(output: 'config.h', configuration: conf)

rx_inc = include_directories('include', '.')

# Third party (vendored)
#============================================================================
subdir('third_party')

# Modules, in dependency order. Each defines rx_<module>_lib / rx_<module>_dep
# and registers its own tests (suite: '<module>').
#============================================================================
subdir('src/common')
# The compute context first (a library the analysis uses); the window's
# sources from the same directory go into the executables below.
subdir('src/gpu')
subdir('src/audio')
subdir('src/core')
subdir('src/analysis')
subdir('src/formats')
subdir('src/export')
subdir('src/player')
subdir('src/app')

# Everything without a window, for the CLI, the tests and the tools.
rxbox_dep = rx_app_dep

subdir('src/ui')

executable('rxbox',
    'src/main.c',
    'src/cli.c',
    gpu_sources, ui_sources,
    include_directories: rx_inc,
    dependencies: [ rxbox_dep, vulkan_dep, sdl3_dep, wayland_client, imgui_dep, vma_dep ] + gpu_deps,
    install: true,
)

subdir('tools')
