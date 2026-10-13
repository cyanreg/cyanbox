# Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md.
#
# The package signalsmith-stretch's CMakeLists.txt finds instead of fetching
# signalsmith-linear over the network (FETCHCONTENT_TRY_FIND_PACKAGE_MODE,
# see the top-level meson.build). The target is empty on purpose: the
# headers come from the signalsmith-linear submodule, which meson reads as a
# CMake subproject of its own, because meson ignores an include directory
# that one subproject hands out from another's tree.
if(NOT TARGET signalsmith-linear)
    add_library(signalsmith-linear INTERFACE)
endif()
set(signalsmith-linear_FOUND TRUE)
