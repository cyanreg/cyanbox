# link: PRO DJ LINK, the library served to players (docs/LINK.md). Tests:
# suite 'link'.
#
# Three codecs, each with its own sources, tests and fuzzer: presence (the
# DJ Link packets and the join), the database server (messages, catalog,
# sessions) and NFS (the virtual tree, portmap, mount, NFS version 2). The
# service thread and the fake player put them on sockets. Each codec's
# tests build that codec's sources in rather than the library, so each
# builds and runs on its own.

link_wire_src     = files('wire.c')
link_presence_src = files('packet.c', 'presence.c')
link_db_src       = files('dbmsg.c', 'catalog.c', 'dbserver.c', 'dbclient.c')
link_nfs_src      = files('vtree.c', 'nfs.c', 'nfsclient.c')

rx_link_lib = static_library('rx_link',
    link_wire_src + link_presence_src + link_db_src + link_nfs_src +
    files('net.c', 'service.c', 'client.c'),
    include_directories: rx_inc,
    dependencies: [ rx_export_dep, rx_analysis_dep, threads_dep ],
)
rx_link_dep = declare_dependency(link_with: rx_link_lib,
                                 dependencies: [ rx_export_dep, rx_analysis_dep, threads_dep ])

link_test_dir = meson.current_build_dir() / 'test-link'
# The fuzzers run the codecs under AddressSanitizer and
# UndefinedBehaviorSanitizer, which abort on the first finding.
link_fuzz_args = cc.get_supported_arguments([
    '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-omit-frame-pointer',
])
link_asan = 'detect_leaks=1:abort_on_error=1'
link_ubsan = 'print_stacktrace=1'

# ---------------------------------------------------------------- presence
test_link_packets = executable('test_link_packets',
    files('../../tests/link/test_packets.c') + link_wire_src + link_presence_src,
    include_directories: rx_inc, dependencies: [ rx_common_dep ])
test('packets', test_link_packets, suite: 'link', timeout: 60)
fuzz_link_presence = executable('fuzz_link_presence',
    files('../../tests/link/fuzz_presence.c') + link_wire_src + link_presence_src,
    include_directories: rx_inc, dependencies: [ rx_common_dep ],
    c_args: link_fuzz_args, link_args: link_fuzz_args)
test('fuzz-presence', fuzz_link_presence, suite: 'link', timeout: 300,
     env: { 'ASAN_OPTIONS': link_asan, 'UBSAN_OPTIONS': link_ubsan })

# ---------------------------------------------------------------- database
# The catalog reads the library's waveforms, which core's wavestore.c
# unpacks with the analysis module's packer (src/core/meson.build): the
# executables link it. The tests make their libraries with the ffmpeg CLI.
db_test_env = environment()
db_test_env.set('RX_TEST_DIR', link_test_dir / 'db')
test_link_db = executable('test_link_db',
    files('../../tests/link/test_db.c') + link_wire_src + link_db_src,
    include_directories: rx_inc, dependencies: [ rx_export_dep, rx_analysis_dep ])
test('db', test_link_db, suite: 'link', env: db_test_env, timeout: 300)
fuzz_link_db = executable('fuzz_link_db',
    files('../../tests/link/fuzz_db.c') + link_wire_src + link_db_src,
    include_directories: rx_inc, dependencies: [ rx_export_dep, rx_analysis_dep ],
    c_args: link_fuzz_args, link_args: link_fuzz_args)
test('fuzz-db', fuzz_link_db, suite: 'link', timeout: 300,
     env: { 'ASAN_OPTIONS': link_asan, 'UBSAN_OPTIONS': link_ubsan,
            'RX_TEST_DIR': link_test_dir / 'fuzz-db' })

# --------------------------------------------------------------------- NFS
nfs_test_env = environment()
nfs_test_env.set('RX_TEST_DIR', link_test_dir / 'nfs')
test_link_nfs = executable('test_link_nfs',
    files('../../tests/link/test_nfs.c') + link_wire_src + link_nfs_src,
    include_directories: rx_inc, dependencies: [ rx_common_dep ])
test('nfs', test_link_nfs, suite: 'link', env: nfs_test_env, timeout: 60)
fuzz_link_nfs = executable('fuzz_link_nfs',
    files('../../tests/link/fuzz_nfs.c') + link_wire_src + link_nfs_src,
    include_directories: rx_inc, dependencies: [ rx_common_dep ],
    c_args: link_fuzz_args, link_args: link_fuzz_args)
test('fuzz-nfs', fuzz_link_nfs, suite: 'link', timeout: 300,
     env: { 'ASAN_OPTIONS': link_asan, 'UBSAN_OPTIONS': link_ubsan,
            'RX_TEST_DIR': link_test_dir / 'fuzz-nfs' })

# ---------------------------------------------------------------- loopback
# The service and the fake player on loopback, with ports of their own.
loop_test_env = environment()
loop_test_env.set('RX_TEST_DIR', link_test_dir / 'loopback')
test_link_loopback = executable('test_link_loopback',
    files('../../tests/link/test_loopback.c'),
    include_directories: rx_inc, dependencies: [ rx_link_dep, rx_analysis_dep ])
test('loopback', test_link_loopback, suite: 'link', env: loop_test_env, timeout: 300)
