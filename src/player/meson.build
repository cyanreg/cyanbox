# The MASTER TEMPO shifter, Signalsmith Stretch's templates behind a C
# interface: the only C++ in the player, so whatever links the player links
# as C++. Optimised in every build: unoptimised it took 64 % of a core and
# callbacks of up to 240 ms (measured at 48 kHz, 256 frames), so a debug
# build could not play it in real time. Debug info and sanitizers still
# apply.
cb_stretch_lib = static_library('cb_stretch',
    files('stretch.cpp'),
    include_directories: cb_inc,
    dependencies: [ stretch_dep ],
    override_options: [ 'optimization=2' ],
)

# player: PipeWire output, the deck and its feed thread, CDJ-style controls,
# null backend.
cb_player_lib = static_library('cb_player',
    files('player.c', 'deck.c', 'feed.c', 'controls.c', 'nullout.c', 'ringout.c', 'pipewire.c'),
    include_directories: cb_inc,
    dependencies: [ cb_core_dep, pipewire_dep ],
    link_whole: cb_stretch_lib,
)
cb_player_dep = declare_dependency(link_with: cb_player_lib, dependencies: [ cb_core_dep, pipewire_dep ])

# Engine and controls on the null backend in manual mode; the PipeWire smoke
# test skips (exit 77) unless CYANBOX_PW_TEST=1 and CYANBOX_PW_SINK name a sink.
foreach t : [ 'engine', 'controls', 'pipewire' ]
    exe = executable('test_player_' + t, files('../../tests/player/test_' + t + '.c'),
        include_directories: cb_inc, dependencies: [ cb_player_dep ])
    test(t, exe, suite: 'player', workdir: meson.project_source_root(), timeout: 120)
endforeach

# The ring backend (LINK CUE's) in real time: about four seconds.
test('ring',
    executable('test_player_ring', files('../../tests/player/test_ring.c'),
        include_directories: cb_inc, dependencies: [ cb_player_dep ]),
    suite: 'player', workdir: meson.project_source_root(), timeout: 60)

# The next track against the owner's changes of mind, on the null backend's
# own thread, in real time: about a minute.
test('next_race',
    executable('test_player_next_race', files('../../tests/player/test_next_race.c'),
        include_directories: cb_inc, dependencies: [ cb_player_dep ]),
    suite: 'player', workdir: meson.project_source_root(), timeout: 300)

# Decks playing packet stores, with the feed thread decoding for real: the
# test writes its WAV (and a FLAC with the ffmpeg CLI) under this directory.
player_test_env = environment()
player_test_env.set('CB_TEST_DIR', meson.current_build_dir() / 'test-media')
test('stream',
    executable('test_player_stream', files('../../tests/player/test_stream.c'),
        include_directories: cb_inc, dependencies: [ cb_player_dep ]),
    env: player_test_env, suite: 'player', workdir: meson.project_source_root(), timeout: 300)
