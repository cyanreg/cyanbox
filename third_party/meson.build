# Vendored third-party code. Each is built once here and handed on as a
# dependency; nothing else includes these directories directly.

# tomlc17 (MIT): the TOML reader for rxbox.toml and the sidecars.
tomlc17_lib = static_library('tomlc17', 'tomlc17/tomlc17.c',
    c_args: [ '-w' ],
)
tomlc17_dep = declare_dependency(
    link_with: tomlc17_lib,
    include_directories: include_directories('tomlc17'),
)

# AMD Vulkan Memory Allocator (MIT). Dynamic entry points only: the C side
# hands vmaCreateAllocator() the two loader functions.
vma_lib = static_library('vma', 'vma/vma.cpp',
    cpp_args: [ '-w', '-DVMA_STATIC_VULKAN_FUNCTIONS=0', '-DVMA_DYNAMIC_VULKAN_FUNCTIONS=1' ],
    dependencies: [ vulkan_dep.partial_dependency(compile_args: true, includes: true) ],
)
vma_dep = declare_dependency(
    link_with: vma_lib,
    include_directories: include_directories('vma'),
    compile_args: [ '-DVMA_STATIC_VULKAN_FUNCTIONS=0', '-DVMA_DYNAMIC_VULKAN_FUNCTIONS=1' ],
)

# Dear ImGui (MIT) with its SDL3 and Vulkan backends.
imgui_lib = static_library('imgui',
    'imgui/imgui.cpp',
    'imgui/imgui_draw.cpp',
    'imgui/imgui_tables.cpp',
    'imgui/imgui_widgets.cpp',
    'imgui/imgui_demo.cpp',
    'imgui/backends/imgui_impl_sdl3.cpp',
    'imgui/backends/imgui_impl_vulkan.cpp',
    cpp_args: [ '-w', '-DIMGUI_DEFINE_MATH_OPERATORS' ],
    include_directories: include_directories('imgui', 'imgui/backends'),
    dependencies: [ sdl3_dep.partial_dependency(compile_args: true, includes: true),
                    vulkan_dep.partial_dependency(compile_args: true, includes: true) ],
)
imgui_dep = declare_dependency(
    link_with: imgui_lib,
    include_directories: include_directories('imgui', 'imgui/backends'),
    compile_args: [ '-DIMGUI_DEFINE_MATH_OPERATORS' ],
)
