# export: the stick plan and writer, inspect, the sync (pull) of what players wrote.
cb_export_lib = static_library('cb_export',
    files(
        'export.c',
        'inspect.c',
        'plan.c',
        'stick.c',
    ),
    include_directories: cb_inc,
    dependencies: [ cb_formats_dep, cb_audio_dep, cb_core_dep ],
)
cb_export_dep = declare_dependency(link_with: cb_export_lib,
                                   dependencies: [ cb_formats_dep, cb_audio_dep, cb_core_dep ])

# The end-to-end test builds a small library (synthetic audio from the
# ffmpeg CLI) and a stick under the build directory.
export_test_env = environment()
export_test_env.set('CB_TEST_DIR', meson.current_build_dir() / 'test-stick')
test_export = executable('test_export', files('../../tests/export/test_export.c'),
    include_directories: cb_inc, dependencies: [ cb_export_dep, cb_analysis_dep ])
test('export', test_export, suite: 'export', env: export_test_env,
     workdir: meson.project_source_root(), timeout: 600)
# Each test its own directory: test_export removes its whole tree first.
safety_test_env = environment()
safety_test_env.set('CB_TEST_DIR', meson.current_build_dir() / 'test-safety')
test_safety = executable('test_safety', files('../../tests/export/test_safety.c'),
    include_directories: cb_inc, dependencies: [ cb_export_dep ])
test('safety', test_safety, suite: 'export', env: safety_test_env,
     workdir: meson.project_source_root(), timeout: 300)
plan_test_env = environment()
plan_test_env.set('CB_TEST_DIR', meson.current_build_dir() / 'test-plan')
test_plan = executable('test_plan', files('../../tests/export/test_plan.c'),
    include_directories: cb_inc, dependencies: [ cb_export_dep ])
test('plan', test_plan, suite: 'export', env: plan_test_env,
     workdir: meson.project_source_root(), timeout: 60)

# The port check's driver (tests/export/port_check.sh): exports a library
# with cb_export, no interface. Not a test.
executable('cb_port_export', files('../../tests/export/port_export.c'),
    include_directories: cb_inc, dependencies: [ cb_export_dep, cb_analysis_dep ])
