# export: the stick plan and writer, inspect, pull.
rx_export_lib = static_library('rx_export',
    files(),
    include_directories: rx_inc,
    dependencies: [ rx_formats_dep ],
)
rx_export_dep = declare_dependency(link_with: rx_export_lib, dependencies: [ rx_formats_dep ])
