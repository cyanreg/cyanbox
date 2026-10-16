# app: background jobs and the application state shared by the CLI and the
# interface. Tests: suite 'app'.

# MPRIS (cyanbox/mpris.h) only with sd-bus (the mpris option); without it the
# header's stand-ins do nothing.
cb_app_sources = files('jobs.c', 'app.c', 'link.c')
cb_app_deps = [ cb_export_dep, cb_link_dep, cb_player_dep, cb_ml_dep ]
if sdbus_dep.found()
    cb_app_sources += files('mpris.c')
    cb_app_deps += sdbus_dep
endif
cb_app_lib = static_library('cb_app',
    cb_app_sources,
    include_directories: cb_inc,
    dependencies: cb_app_deps,
)
cb_app_dep = declare_dependency(link_with: cb_app_lib, dependencies: cb_app_deps)

# cyanbox-cli: the command line without src/gpu and src/ui (headless.c stands
# in for them), for the CLI test: it needs no display and does not wait for
# the interface to build. The real executable is `cyanbox` (top level).
cyanbox_cli = executable('cyanbox-cli',
    files('../cli.c', '../main.c', 'headless.c'),
    include_directories: cb_inc,
    dependencies: [ cb_app_dep ],
)

# Test audio, made by the ffmpeg command line: 12 s of a 128 BPM kick with a
# click on top, 48 kHz stereo.
ffmpeg_prog = find_program('ffmpeg', required: false)
if ffmpeg_prog.found()
    app_click_wav = custom_target('app_click_wav',
        output: 'click128.wav',
        build_by_default: true,
        command: [ ffmpeg_prog, '-v', 'error', '-nostdin', '-y', '-f', 'lavfi', '-i',
                   'aevalsrc=exprs=\'0.8*sin(2*PI*55*mod(t,0.46875))*exp(-mod(t,0.46875)*25)' +
                   '+0.2*sin(2*PI*2000*t)*exp(-mod(t,0.46875)*200)\':s=48000:d=12',
                   '-ac', '2', '-c:a', 'pcm_s16le', '@OUTPUT@' ],
    )

    # The same, 40 s long, mono: long enough to count as a play (30 s).
    app_long_wav = custom_target('app_long_wav',
        output: 'click128-40s.wav',
        build_by_default: true,
        command: [ ffmpeg_prog, '-v', 'error', '-nostdin', '-y', '-f', 'lavfi', '-i',
                   'aevalsrc=exprs=\'0.8*sin(2*PI*55*mod(t,0.46875))*exp(-mod(t,0.46875)*25)' +
                   '+0.2*sin(2*PI*2000*t)*exp(-mod(t,0.46875)*200)\':s=48000:d=40',
                   '-ac', '1', '-c:a', 'pcm_s16le', '@OUTPUT@' ],
    )

    test_jobs = executable('test_jobs', files('../../tests/app/test_jobs.c'),
        include_directories: cb_inc, dependencies: [ cb_app_dep ])
    test('jobs', test_jobs, args: [ app_click_wav ], suite: 'app', timeout: 300)
    # The tag batch with the model ($CYANBOX_ML_MODEL; skipped without), on
    # its own: two models on one GPU have made the driver reset it.
    test('jobs_tag', test_jobs, args: [ '--tag', app_click_wav, app_long_wav ], suite: 'app',
         timeout: 600, is_parallel: false)

    test_app = executable('test_app', files('../../tests/app/test_app.c', 'headless.c'),
        include_directories: cb_inc, dependencies: [ cb_app_dep ])
    test('app', test_app,
        args: [ app_click_wav, meson.current_build_dir() / 'test-app', app_long_wav ],
        suite: 'app', timeout: 300)

    # MPRIS against a private bus of the test's own (a dbus-daemon, else a
    # dbus-broker; skipped without either), never the session's; its client
    # is sd-bus too.
    if sdbus_dep.found()
        test_mpris = executable('test_mpris', files('../../tests/app/test_mpris.c', 'headless.c'),
            include_directories: cb_inc, dependencies: [ cb_app_dep ])
        test('mpris', test_mpris,
            args: [ app_click_wav, meson.current_build_dir() / 'test-mpris' ],
            suite: 'app', timeout: 300)
    endif

    # Link in the app on the loopback: started with it, the fake player's load
    # and its play counted (30 s of it).
    test_link = executable('test_link', files('../../tests/app/test_link.c', 'headless.c'),
        include_directories: cb_inc, dependencies: [ cb_app_dep ])
    test('app-link', test_link,
        args: [ app_click_wav, meson.current_build_dir() / 'test-link' ],
        suite: 'link', timeout: 300)

    # Link through the CLI on a private two-host network of user and
    # network namespaces (tests/link/netns.sh): rekordbox's own ports,
    # nothing outside the namespaces; skipped where they are not allowed.
    test('netns', find_program('../../tests/link/netns_test.sh'),
        args: [ cyanbox_cli, app_click_wav, meson.current_build_dir() / 'test-netns' ],
        depends: [ cyanbox_cli ], suite: 'link', timeout: 300)

    test('cli', find_program('../../tests/app/cli_test.sh'),
        args: [ cyanbox_cli, meson.current_build_dir() / 'test-cli' ],
        depends: [ cyanbox_cli ], suite: 'app', timeout: 900)
endif
