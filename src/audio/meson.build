# audio: decoding, probing, artwork, transcoding (libav*). Depends on common.
rx_audio_lib = static_library('rx_audio',
    files(
        'artwork.c',
        'decode.c',
        'flac.c',
        'mp3.c',
        'probe.c',
        'transcode.c',
    ),
    include_directories: rx_inc,
    dependencies: [ rx_common_dep, libavformat, libavcodec, libavutil, libswresample, libswscale, m_dep ],
)
rx_audio_dep = declare_dependency(link_with: rx_audio_lib,
    dependencies: [ rx_common_dep, libavformat, libavcodec, libavutil, libswresample, libswscale ])

# Tests generate their media (WAV written by the test, FLAC/MP3/PNG made with
# the ffmpeg CLI) under this module's build directory.
audio_test_env = environment()
audio_test_env.set('RX_TEST_DIR', meson.current_build_dir() / 'test-media')

foreach t : [ 'mp3', 'decode', 'artwork', 'transcode', 'dj' ]
    test('audio_' + t,
        executable('test_audio_' + t,
            '../../tests/audio/test_' + t + '.c',
            '../../tests/audio/util.c',
            include_directories: rx_inc,
            dependencies: [ rx_audio_dep, m_dep ],
        ),
        env: audio_test_env,
        suite: 'audio',
        timeout: 120,
    )
endforeach
