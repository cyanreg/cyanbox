#!/bin/sh
# Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md.
#
# Runs cyanbox-gpu-demo with its pixel check, its scroll check (which makes
# the run 47 frames) and a screenshot.
# It opens a window, so only where the caller has said a private compositor
# is there: CYANBOX_GPU_TEST=1 and WAYLAND_DISPLAY set (never the user's own
# session; see docs/HACKING.md). Otherwise the test is skipped.
#
# usage: run_demo.sh <path to cyanbox-gpu-demo> <screenshot.png>

if [ "$CYANBOX_GPU_TEST" != 1 ] || [ -z "$WAYLAND_DISPLAY" ]; then
    echo "skipped: set CYANBOX_GPU_TEST=1 and WAYLAND_DISPLAY to a private compositor"
    exit 77
fi

if [ ! -x "$1" ]; then
    echo "$1 is not built (ninja -C <builddir> cyanbox-gpu-demo)"
    exit 1
fi

exec "$1" --frames 30 --check --scroll --screenshot "$2" ${CYANBOX_GPU_VALIDATION:+--validation}
