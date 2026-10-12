# common: helpers, logging, threads. Depends on nothing else in rxbox.
rx_common_lib = static_library('rx_common',
    files('common.c', 'log.c', 'thread.c'),
    include_directories: rx_inc,
    dependencies: [ m_dep, threads_dep ],
)
rx_common_dep = declare_dependency(link_with: rx_common_lib, include_directories: rx_inc,
                                   dependencies: [ m_dep, threads_dep ])

test('common_thread',
    executable('test_common_thread', '../../tests/common/test_thread.c',
        include_directories: rx_inc,
        dependencies: [ rx_common_dep ],
    ),
    suite: 'common',
    timeout: 60,
)
