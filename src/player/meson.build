# player: PipeWire output, the deck, CDJ-style controls, null backend.
rx_player_lib = static_library('rx_player',
    files('player.c', 'deck.c', 'controls.c', 'nullout.c', 'pipewire.c'),
    include_directories: rx_inc,
    dependencies: [ rx_core_dep, pipewire_dep ],
)
rx_player_dep = declare_dependency(link_with: rx_player_lib, dependencies: [ rx_core_dep, pipewire_dep ])

# Engine and controls on the null backend in manual mode; the PipeWire smoke
# test skips (exit 77) unless RXBOX_PW_TEST=1 and RXBOX_PW_SINK name a sink.
foreach t : [ 'engine', 'controls', 'pipewire' ]
    exe = executable('test_player_' + t, files('../../tests/player/test_' + t + '.c'),
        include_directories: rx_inc, dependencies: [ rx_player_dep ])
    test(t, exe, suite: 'player', workdir: meson.project_source_root(), timeout: 120)
endforeach
