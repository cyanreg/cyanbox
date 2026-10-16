# analysis: beat grid, key, waveforms. Depends on common, audio, core (the model).
# Two agents share this module: waveform.c/key.c (+ test_waveform, test_key)
# and the rest (+ test_beats). The file list is fixed here so neither needs
# to edit this file; add new files only by appending to your own group.
cb_analysis_sources = files(
    # beat grid and orchestration
    'spectrum.c',
    'onset.c',
    'tempo.c',
    'grid_fit.c',
    'downbeat.c',
    'beats.c',
    'start.c',
    'walk.c',
    'analysis.c',
    # waveforms and key
    'waveform.c',
    'key.c',
)
cb_analysis_lib = static_library('cb_analysis',
    cb_analysis_sources,
    include_directories: cb_inc,
    dependencies: [ cb_core_dep, cb_compute_dep, libavutil, libswresample ],
)
cb_analysis_dep = declare_dependency(link_with: cb_analysis_lib,
                                     dependencies: [ cb_core_dep, cb_compute_dep ])

test_waveform = executable('test_waveform', files('../../tests/analysis/test_waveform.c'),
    include_directories: cb_inc, dependencies: [ cb_analysis_dep ])
test('waveform', test_waveform, suite: 'analysis', workdir: meson.project_source_root(), timeout: 600)
test_key = executable('test_key', files('../../tests/analysis/test_key.c'),
    include_directories: cb_inc, dependencies: [ cb_analysis_dep ])
test('key', test_key, suite: 'analysis', workdir: meson.project_source_root(), timeout: 600)
test_beats = executable('test_beats', files('../../tests/analysis/test_beats.c'),
    include_directories: cb_inc, dependencies: [ cb_analysis_dep ])
test('beats', test_beats, suite: 'analysis', workdir: meson.project_source_root(), timeout: 1200)
# cb_analyse's edges: NaN in a float file, the onsets of a few frames, the
# first loud onset, the automatic cue without a length. Writes its WAVs
# under the build directory.
test_analysis = executable('test_analysis', files('../../tests/analysis/test_analysis.c'),
    include_directories: cb_inc, dependencies: [ cb_analysis_dep ])
test('analysis', test_analysis, suite: 'analysis', workdir: meson.project_source_root(),
     env: [ 'CB_TEST_DIR=' + meson.current_build_dir() / 'test-media' ], timeout: 300)
