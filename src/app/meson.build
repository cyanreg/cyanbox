# app: background jobs and the application state shared by the CLI and the
# interface. Tests: suite 'app'.

# MPRIS (rxbox/mpris.h) only with sd-bus (the mpris option); without it the
# header's stand-ins do nothing.
rx_app_sources = files('jobs.c', 'app.c')
rx_app_deps = [ rx_export_dep, rx_link_dep, rx_player_dep, rx_ml_dep ]
if sdbus_dep.found()
    rx_app_sources += files('mpris.c')
    rx_app_deps += sdbus_dep
endif
rx_app_lib = static_library('rx_app',
    rx_app_sources,
    include_directories: rx_inc,
    dependencies: rx_app_deps,
)
rx_app_dep = declare_dependency(link_with: rx_app_lib, dependencies: rx_app_deps)

# rxbox-cli: the command line without src/gpu and src/ui (headless.c stands
# in for them), for the CLI test: it needs no display and does not wait for
# the interface to build. The real executable is `rxbox` (top level).
rxbox_cli = executable('rxbox-cli',
    files('../cli.c', '../main.c', 'headless.c'),
    include_directories: rx_inc,
    dependencies: [ rx_app_dep ],
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
        include_directories: rx_inc, dependencies: [ rx_app_dep ])
    test('jobs', test_jobs, args: [ app_click_wav ], suite: 'app', timeout: 300)

    test_app = executable('test_app', files('../../tests/app/test_app.c', 'headless.c'),
        include_directories: rx_inc, dependencies: [ rx_app_dep ])
    test('app', test_app,
        args: [ app_click_wav, meson.current_build_dir() / 'test-app', app_long_wav ],
        suite: 'app', timeout: 300)

    # MPRIS against a private bus of the test's own (a dbus-daemon, else a
    # dbus-broker; skipped without either), never the session's; its client
    # is sd-bus too.
    if sdbus_dep.found()
        test_mpris = executable('test_mpris', files('../../tests/app/test_mpris.c', 'headless.c'),
            include_directories: rx_inc, dependencies: [ rx_app_dep ])
        test('mpris', test_mpris,
            args: [ app_click_wav, meson.current_build_dir() / 'test-mpris' ],
            suite: 'app', timeout: 300)
    endif

    # Link through the CLI on a private two-host network of user and
    # network namespaces (tests/link/netns.sh): rekordbox's own ports,
    # nothing outside the namespaces; skipped where they are not allowed.
    test('netns', find_program('../../tests/link/netns_test.sh'),
        args: [ rxbox_cli, app_click_wav, meson.current_build_dir() / 'test-netns' ],
        depends: [ rxbox_cli ], suite: 'link', timeout: 300)

    test('cli', find_program('../../tests/app/cli_test.sh'),
        args: [ rxbox_cli, meson.current_build_dir() / 'test-cli' ],
        depends: [ rxbox_cli ], suite: 'app', timeout: 900)
endif
