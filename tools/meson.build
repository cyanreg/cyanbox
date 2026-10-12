# Developer tools. Each is owned by the module it exercises; the file list
# is fixed here so no tool needs to edit this file.

# rxbox-eval: runs the analyser over files and prints/compares results
# (analysis module).
executable('rxbox-eval', 'rxbox-eval.c',
    include_directories: rx_inc,
    dependencies: [ rx_analysis_dep, rx_audio_dep, rx_core_dep ],
)

# rxbox-gpu-demo: opens the window, draws ImGui and a synthetic waveform,
# runs N frames and screenshots (gpu module).
executable('rxbox-gpu-demo', 'rxbox-gpu-demo.c', gpu_sources,
    include_directories: rx_inc,
    dependencies: [ rx_common_dep, rx_audio_dep, vulkan_dep, sdl3_dep, wayland_client, imgui_dep, vma_dep ] + gpu_deps,
)
