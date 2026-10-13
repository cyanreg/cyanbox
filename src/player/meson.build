# The MASTER TEMPO shifter, Signalsmith Stretch's templates behind a C
# interface: the only C++ in the player, so whatever links the player links
# as C++. Optimised in every build: unoptimised it took 64 % of a core and
# callbacks of up to 240 ms (measured at 48 kHz, 256 frames), so a debug
# build could not play it in real time. Debug info and sanitizers still
# apply.
rx_stretch_lib = static_library('rx_stretch',
    files('stretch.cpp'),
    include_directories: rx_inc,
    dependencies: [ stretch_dep ],
    override_options: [ 'optimization=2' ],
)

# player: PipeWire output, the deck and its feed thread, CDJ-style controls,
# null backend.
rx_player_lib = static_library('rx_player',
    files('player.c', 'deck.c', 'feed.c', 'controls.c', 'nullout.c', 'pipewire.c'),
    include_directories: rx_inc,
    dependencies: [ rx_core_dep, pipewire_dep ],
    link_whole: rx_stretch_lib,
)
rx_player_dep = declare_dependency(link_with: rx_player_lib, dependencies: [ rx_core_dep, pipewire_dep ])

# Engine and controls on the null backend in manual mode; the PipeWire smoke
# test skips (exit 77) unless RXBOX_PW_TEST=1 and RXBOX_PW_SINK name a sink.
foreach t : [ 'engine', 'controls', 'pipewire' ]
    exe = executable('test_player_' + t, files('../../tests/player/test_' + t + '.c'),
        include_directories: rx_inc, dependencies: [ rx_player_dep ])
    test(t, exe, suite: 'player', workdir: meson.project_source_root(), timeout: 120)
endforeach

# Decks playing packet stores, with the feed thread decoding for real: the
# test writes its WAV (and a FLAC with the ffmpeg CLI) under this directory.
player_test_env = environment()
player_test_env.set('RX_TEST_DIR', meson.current_build_dir() / 'test-media')
test('stream',
    executable('test_player_stream', files('../../tests/player/test_stream.c'),
        include_directories: rx_inc, dependencies: [ rx_player_dep ]),
    env: player_test_env, suite: 'player', workdir: meson.project_source_root(), timeout: 300)
