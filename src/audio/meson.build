# audio: decoding, probing, artwork, transcoding (libav*). Depends on common.
cb_audio_lib = static_library('cb_audio',
    files(
        'artwork.c',
        'decode.c',
        'encode.c',
        'fifo.c',
        'flac.c',
        'mp3.c',
        'packets.c',
        'pipeline.c',
        'probe.c',
        'source.c',
    ),
    include_directories: cb_inc,
    dependencies: [ cb_common_dep, libavformat, libavcodec, libavutil, libswresample, libswscale, m_dep ],
)
cb_audio_dep = declare_dependency(link_with: cb_audio_lib,
    dependencies: [ cb_common_dep, libavformat, libavcodec, libavutil, libswresample, libswscale ])

# Tests generate their media (WAV written by the test, FLAC/MP3/PNG made with
# the ffmpeg CLI) under this module's build directory.
audio_test_env = environment()
audio_test_env.set('CB_TEST_DIR', meson.current_build_dir() / 'test-media')

foreach t : [ 'mp3', 'decode', 'packets', 'pipeline', 'artwork', 'encode', 'dj' ]
    test('audio_' + t,
        executable('test_audio_' + t,
            '../../tests/audio/test_' + t + '.c',
            '../../tests/audio/util.c',
            include_directories: cb_inc,
            dependencies: [ cb_audio_dep, m_dep ],
        ),
        env: audio_test_env,
        suite: 'audio',
        timeout: 120,
    )
endforeach
