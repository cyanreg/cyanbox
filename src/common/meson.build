# common: helpers, logging, threads. Depends on nothing else in rxbox.
rx_common_lib = static_library('rx_common',
    files('common.c', 'log.c'),
    include_directories: rx_inc,
    dependencies: [ m_dep, threads_dep ],
)
rx_common_dep = declare_dependency(link_with: rx_common_lib, include_directories: rx_inc,
                                   dependencies: [ m_dep, threads_dep ])
