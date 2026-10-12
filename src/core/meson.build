# core: model, grid arithmetic, sidecars, library, playlists, config. Depends on common and audio (probing).
rx_core_lib = static_library('rx_core',
    files(),
    include_directories: rx_inc,
    dependencies: [ rx_common_dep, rx_audio_dep, tomlc17_dep ],
)
rx_core_dep = declare_dependency(link_with: rx_core_lib, dependencies: [ rx_common_dep, rx_audio_dep ])
