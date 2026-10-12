# formats: export.pdb, ANLZ, OneLibrary (sqlite3 + SQLCipher via OpenSSL), settings, colours.
rx_formats_lib = static_library('rx_formats',
    files(
        'anlz.c',
        'colors.c',
        'onelibrary.c',
        'pdb.c',
        'settings.c',
        'sqlcipher.c',
        'util.c',
    ),
    include_directories: rx_inc,
    dependencies: [ rx_analysis_dep, sqlite3_dep, openssl_dep, m_dep ],
)
rx_formats_dep = declare_dependency(link_with: rx_formats_lib,
                                    dependencies: [ rx_analysis_dep, sqlite3_dep, openssl_dep, m_dep ])

# Tests run from the source root so they find tests/data.
foreach t : [ 'colors', 'util', 'pdb', 'anlz', 'settings', 'onelibrary', 'fuzz' ]
    exe = executable('test_formats_' + t, files('../../tests/formats/test_' + t + '.c'),
        include_directories: rx_inc, dependencies: [ rx_formats_dep ])
    test('formats_' + t, exe, suite: 'formats', workdir: meson.project_source_root(), timeout: 300)
endforeach
