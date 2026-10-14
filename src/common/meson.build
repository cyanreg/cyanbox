# common: helpers, logging, threads. Depends on nothing else in rxbox.
rx_common_lib = static_library('rx_common',
    files('common.c', 'log.c', 'thread.c', 'utf8.c', 'latin1.c'),
    include_directories: rx_inc,
    dependencies: [ m_dep, threads_dep, mecab_dep, icu_dep ],
)
rx_common_dep = declare_dependency(link_with: rx_common_lib, include_directories: rx_inc,
                                   dependencies: [ m_dep, threads_dep, mecab_dep, icu_dep ])

test('common_thread',
    executable('test_common_thread', '../../tests/common/test_thread.c',
        include_directories: rx_inc,
        dependencies: [ rx_common_dep ],
    ),
    suite: 'common',
    timeout: 60,
)

test('common_helpers',
    executable('test_common_helpers', '../../tests/common/test_common.c',
        include_directories: rx_inc,
        dependencies: [ rx_common_dep ],
    ),
    suite: 'common',
    timeout: 60,
)

# rx_utf8_latin1 under AddressSanitizer and UndefinedBehaviorSanitizer,
# which abort on the first finding.
common_fuzz_args = cc.get_supported_arguments([
    '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-omit-frame-pointer',
])
test('fuzz-latin1',
    executable('fuzz_common_latin1',
        files('../../tests/common/fuzz_latin1.c', 'common.c', 'log.c', 'thread.c', 'utf8.c',
              'latin1.c'),
        include_directories: rx_inc,
        dependencies: [ m_dep, threads_dep, mecab_dep, icu_dep ],
        c_args: common_fuzz_args, link_args: common_fuzz_args,
    ),
    suite: 'common',
    env: { 'ASAN_OPTIONS': 'detect_leaks=1:abort_on_error=1', 'UBSAN_OPTIONS': 'print_stacktrace=1' },
    timeout: 300,
)
