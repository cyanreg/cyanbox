# audio: decoding, probing, artwork, transcoding (libav*). Depends on common.
rx_audio_lib = static_library('rx_audio',
    files(),
    include_directories: rx_inc,
    dependencies: [ rx_common_dep, libavformat, libavcodec, libavutil, libswresample, libswscale ],
)
rx_audio_dep = declare_dependency(link_with: rx_audio_lib,
    dependencies: [ rx_common_dep, libavformat, libavcodec, libavutil, libswresample, libswscale ])
