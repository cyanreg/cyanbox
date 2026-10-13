# analysis: beat grid, key, waveforms. Depends on common, audio, core (the model).
# Two agents share this module: waveform.c/key.c (+ test_waveform, test_key)
# and the rest (+ test_beats). The file list is fixed here so neither needs
# to edit this file; add new files only by appending to your own group.
rx_analysis_sources = files(
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
rx_analysis_lib = static_library('rx_analysis',
    rx_analysis_sources,
    include_directories: rx_inc,
    dependencies: [ rx_core_dep, rx_compute_dep, libavutil, libswresample ],
)
rx_analysis_dep = declare_dependency(link_with: rx_analysis_lib,
                                     dependencies: [ rx_core_dep, rx_compute_dep ])

test_waveform = executable('test_waveform', files('../../tests/analysis/test_waveform.c'),
    include_directories: rx_inc, dependencies: [ rx_analysis_dep ])
test('waveform', test_waveform, suite: 'analysis', workdir: meson.project_source_root(), timeout: 600)
test_key = executable('test_key', files('../../tests/analysis/test_key.c'),
    include_directories: rx_inc, dependencies: [ rx_analysis_dep ])
test('key', test_key, suite: 'analysis', workdir: meson.project_source_root(), timeout: 600)
test_beats = executable('test_beats', files('../../tests/analysis/test_beats.c'),
    include_directories: rx_inc, dependencies: [ rx_analysis_dep ])
test('beats', test_beats, suite: 'analysis', workdir: meson.project_source_root(), timeout: 1200)
