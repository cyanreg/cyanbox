# app: background jobs and the application state shared by the CLI and the interface.
rx_app_lib = static_library('rx_app',
    files(),
    include_directories: rx_inc,
    dependencies: [ rx_export_dep, rx_player_dep ],
)
rx_app_dep = declare_dependency(link_with: rx_app_lib, dependencies: [ rx_export_dep, rx_player_dep ])
