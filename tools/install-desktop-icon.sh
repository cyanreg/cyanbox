#!/bin/sh
set -eu
SRC="${MESON_INSTALL_DESTDIR_PREFIX}/share/applications/cyanbox.desktop"
if command -v update-desktop-database >/dev/null 2>&1; then
  update-desktop-database "${MESON_INSTALL_DESTDIR_PREFIX}/share/applications" || true
fi
[ -n "${DESTDIR:-}" ] && exit 0
TARGET_USER="${SUDO_USER:-${USER:-}}"
TARGET_HOME="$(getent passwd "$TARGET_USER" 2>/dev/null | cut -d: -f6)"
[ -z "$TARGET_HOME" ] && TARGET_HOME="${HOME:-}"
if [ -n "$TARGET_HOME" ] && [ -d "$TARGET_HOME/Desktop" ] && [ -f "$SRC" ]; then
  install -m 0755 "$SRC" "$TARGET_HOME/Desktop/cyanbox.desktop"
  if command -v gio >/dev/null 2>&1 && [ -z "${SUDO_USER:-}" ]; then
    gio set "$TARGET_HOME/Desktop/cyanbox.desktop" metadata::trusted true 2>/dev/null || true
  fi
fi
