# player: PipeWire output, decks, CDJ-style controls, null backend.
rx_player_lib = static_library('rx_player',
    files(),
    include_directories: rx_inc,
    dependencies: [ rx_core_dep, pipewire_dep ],
)
rx_player_dep = declare_dependency(link_with: rx_player_lib, dependencies: [ rx_core_dep, pipewire_dep ])
