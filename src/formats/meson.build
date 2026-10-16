# formats: export.pdb, ANLZ, OneLibrary (sqlite3 + SQLCipher), settings, colours.
cb_formats_lib = static_library('cb_formats',
    files(
        'anlz.c',
        'colors.c',
        'onelibrary.c',
        'pdb.c',
        'settings.c',
        'sqlcipher.c',
        'util.c',
    ),
    include_directories: cb_inc,
    dependencies: [ cb_analysis_dep, sqlite3_dep, libavutil, m_dep ],
)
cb_formats_dep = declare_dependency(link_with: cb_formats_lib,
                                    dependencies: [ cb_analysis_dep, sqlite3_dep, libavutil, m_dep ])

# Tests run from the source root so they find tests/data.
foreach t : [ 'colors', 'util', 'pdb', 'anlz', 'settings', 'onelibrary', 'fuzz' ]
    exe = executable('test_formats_' + t, files('../../tests/formats/test_' + t + '.c'),
        include_directories: cb_inc, dependencies: [ cb_formats_dep ])
    test('formats_' + t, exe, suite: 'formats', workdir: meson.project_source_root(), timeout: 300)
endforeach
