# formats: export.pdb, ANLZ, OneLibrary (sqlite3 + SQLCipher via OpenSSL), settings, colours.
rx_formats_lib = static_library('rx_formats',
    files(),
    include_directories: rx_inc,
    dependencies: [ rx_analysis_dep, sqlite3_dep, openssl_dep ],
)
rx_formats_dep = declare_dependency(link_with: rx_formats_lib, dependencies: [ rx_analysis_dep, sqlite3_dep, openssl_dep ])
