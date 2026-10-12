# core: model, grid arithmetic, notation, the SQLite library (tracks, playlists,
# settings, waveforms, artwork). Depends on common and audio (probing);
# wavestore.c calls the analysis module's waveform packer and is only linked
# where waveforms are stored (see the comment there).
rx_core_lib = static_library('rx_core',
    files(
        'config.c',
        'db.c',
        'grid.c',
        'library.c',
        'model.c',
        'notation.c',
        'playlist.c',
        'wavestore.c',
    ),
    include_directories: rx_inc,
    dependencies: [ rx_common_dep, rx_audio_dep, sqlite3_dep, m_dep ],
)
rx_core_dep = declare_dependency(link_with: rx_core_lib,
    dependencies: [ rx_common_dep, rx_audio_dep, sqlite3_dep, m_dep ])

# Tests make their files (WAVs, m3u8s, the library) under this directory.
core_test_env = environment()
core_test_env.set('RX_TEST_DIR', meson.current_build_dir() / 'test-tmp')

foreach t : [ 'grid', 'cues', 'notation', 'library', 'playlist' ]
    test('core_' + t,
        executable('test_core_' + t,
            '../../tests/core/test_' + t + '.c',
            '../../tests/core/util.c',
            include_directories: rx_inc,
            dependencies: [ rx_core_dep, m_dep ],
        ),
        env: core_test_env,
        suite: 'core',
        timeout: 120,
    )
endforeach
