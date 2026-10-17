# Developer tools. Each is owned by the module it exercises; the file list
# is fixed here so no tool needs to edit this file.

# cyanbox-eval: runs the analyser over files and prints/compares results
# (analysis module).
executable('cyanbox-eval', 'cyanbox-eval.c',
    include_directories: cb_inc,
    dependencies: [ cb_analysis_dep, cb_audio_dep, cb_core_dep ],
)

# cyanbox-gpu-demo: opens the window, draws ImGui and a synthetic waveform,
# runs N frames and screenshots (gpu module).
executable('cyanbox-gpu-demo', 'cyanbox-gpu-demo.c', gpu_sources,
    include_directories: cb_inc,
    dependencies: [ cb_common_dep, vulkan_dep, sdl3_dep, wayland_client, imgui_dep, vma_dep ] + gpu_deps,
)

# cyanbox-beatthis: Beat This!'s frame activations for files, as .npy, for
# working on what to make of them (ml module).
executable('cyanbox-beatthis', 'cyanbox-beatthis.c',
    include_directories: cb_inc,
    dependencies: [ cb_ml_dep, cb_audio_dep, m_dep ],
)

# cyanbox-ml: listens to files with the model and prints its answers to
# questions, for trying prompts out (ml module).
executable('cyanbox-ml', 'cyanbox-ml.c',
    include_directories: cb_inc,
    dependencies: [ cb_ml_dep, cb_audio_dep, m_dep ],
)
