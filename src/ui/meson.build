# The Dear ImGui interface (C++). Compiled into the rxbox executable with
# the window module (gpu_sources); the executable carries the ImGui, SDL3,
# Vulkan and fontconfig dependencies.
ui_sources = files(
    'ui.cpp',
    'deck_view.cpp',
    'waveform_view.cpp',
    'browser.cpp',
    'tree.cpp',
    'dialogs.cpp',
    'finder.cpp',
    'keys.cpp',
    'link_view.cpp',
    'link_decks.cpp',
)

# The headless run: starts the interface on a private sway with the null
# audio backend, loads a synthetic track and saves a screenshot. Opt-in
# (RXBOX_UI_TEST=1 and a WAYLAND_DISPLAY that is not the user's session);
# otherwise it reports skipped (77).
test('ui-screenshot', find_program('../../tests/ui/screenshot.sh'),
    args: [ meson.project_build_root() / 'rxbox', meson.project_build_root() / 'ui-test' ],
    suite: 'ui',
    timeout: 300,
    is_parallel: false,
)
