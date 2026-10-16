# core: model, grid arithmetic, notation, the SQLite library (tracks, playlists,
# settings, waveforms, artwork). Depends on common and audio (probing);
# wavestore.c calls the analysis module's waveform packer and is only linked
# where waveforms are stored (see the comment there).
cb_core_lib = static_library('cb_core',
    files(
        'clean.c',
        'config.c',
        'db.c',
        'grid.c',
        'library.c',
        'model.c',
        'notation.c',
        'playlist.c',
        'wavestore.c',
    ),
    include_directories: cb_inc,
    dependencies: [ cb_common_dep, cb_audio_dep, sqlite3_dep, m_dep ],
)
cb_core_dep = declare_dependency(link_with: cb_core_lib,
    dependencies: [ cb_common_dep, cb_audio_dep, sqlite3_dep, m_dep ])

# Tests make their files (WAVs, m3u8s, the library) under this directory.
core_test_env = environment()
core_test_env.set('CB_TEST_DIR', meson.current_build_dir() / 'test-tmp')

# The library test checks the colours it keeps against the formats
# module's colour table, which core does not link: colors.c (plain
# arithmetic on its tables) is built into that test alone.
foreach t : [ 'grid', 'cues', 'notation', 'library', 'playlist' ]
    test('core_' + t,
        executable('test_core_' + t,
            '../../tests/core/test_' + t + '.c',
            '../../tests/core/util.c',
            t == 'library' ? [ '../formats/colors.c' ] : [],
            include_directories: cb_inc,
            dependencies: [ cb_core_dep, m_dep ],
        ),
        env: core_test_env,
        suite: 'core',
        timeout: 120,
    )
endforeach
