# Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md.

project('cyanbox',
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
# 3.44 for sqlite3_set_clientdata (the stick database's step budget).
sqlite3_dep  = dependency('sqlite3', version: '>= 3.44')
vulkan_dep   = dependency('vulkan')
sdl3_dep     = dependency('sdl3')
pipewire_dep = dependency('libpipewire-0.3')
wayland_client = dependency('wayland-client')
glslang_prog = find_program('glslangValidator', 'glslang', required: true)
# MPRIS (src/app/mpris.c): sd-bus, the D-Bus library in libsystemd, else in
# libelogind or basu (the standalone sd-bus) on a system without systemd,
# whichever is found first (LGPL-2.1 or later, linked dynamically); only
# when one is found (-Dmpris=enabled requires one, disabled leaves it out).
sdbus_dep    = dependency('libsystemd', 'libelogind', 'basu', required: get_option('mpris'))
# Romaji for players that show Latin-1 only (src/common/latin1.c): MeCab
# (BSD, of its three licences; linked dynamically) reads kanji with its
# system dictionary, found at run time (IPADIC: Debian's mecab-ipadic-utf8,
# NAIST's licence); ICU (the Unicode licence) romanises other scripts. Each
# only when found (-Dmecab=enabled / -Dicu=enabled require it).
mecab_dep    = cc.find_library('mecab', has_headers: [ 'mecab.h' ], required: get_option('mecab'))
icu_dep      = dependency('icu-i18n', required: get_option('icu'))

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
conf.set_quoted('CYANBOX_VERSION', meson.project_version())
conf.set10('CYANBOX_MPRIS', sdbus_dep.found())
conf.set10('CYANBOX_MECAB', mecab_dep.found())
conf.set10('CYANBOX_ICU', icu_dep.found())
configure_file(output: 'config.h', configuration: conf)

cb_inc = include_directories('include', '.')

# Third party (vendored)
#============================================================================
subdir('third_party')

# Modules, in dependency order. Each defines cb_<module>_lib / cb_<module>_dep
# and registers its own tests (suite: '<module>').
#============================================================================
subdir('src/common')
# The compute context first (a library the analysis uses); the window's
# sources from the same directory go into the executables below.
subdir('src/gpu')
# The model that tags tracks on request (cyanbox/ml.h): a Vulkan device of its
# own, only for as long as a batch of tracks is being tagged.
subdir('src/ml')
subdir('src/audio')
subdir('src/core')
subdir('src/analysis')
subdir('src/formats')
subdir('src/export')
# link: PRO DJ LINK, the library served to players (docs/LINK.md).
subdir('src/link')
subdir('src/player')
subdir('src/app')

# Everything without a window, for the CLI, the tests and the tools.
cyanbox_dep = cb_app_dep

subdir('src/ui')

executable('cyanbox',
    'src/main.c',
    'src/cli.c',
    gpu_sources, ui_sources,
    include_directories: cb_inc,
    dependencies: [ cyanbox_dep, vulkan_dep, sdl3_dep, wayland_client, imgui_dep, vma_dep ] + gpu_deps,
    install: true,
)
# Beat This!'s weights (models/README.md, docs/BEATTHIS.md).
install_data('models/beat-this-f16.gguf',
             install_dir: get_option('datadir') / 'cyanbox' / 'models')

subdir('tools')
