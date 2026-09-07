#!/bin/bash
# Install (or remove) "Brscan Config.app" into /Applications. This is an OPT-IN
# convenience step, kept out of the CMake build: `cmake --build build` produces
# and ad-hoc signs the bundle under build/gui/, and this script just copies it
# where you can launch it.
#
# Usage, from the repo root after `cmake --build build`:
#   gui/install.sh install
#   gui/install.sh uninstall
#
# See docs/DISTRIBUTION.md for the signing story and gui/README.md for what the
# app does.
set -euo pipefail

BUNDLE="build/gui/Brscan Config.app"
DEST_DIR="/Applications"
DEST="${DEST_DIR}/Brscan Config.app"

case "${1:-}" in
  install)
    if [[ ! -d "${BUNDLE}" ]]; then
      echo "error: ${BUNDLE} not found. Build it first: cmake --build build" >&2
      exit 1
    fi
    rm -rf "${DEST}"
    cp -R "${BUNDLE}" "${DEST}"
    echo "Installed ${DEST}"
    echo
    echo "The app is ad-hoc signed, so Gatekeeper will not open it on a"
    echo "double-click the first time. Either:"
    echo "  - right-click the app in /Applications and choose Open, then Open"
    echo "    again in the dialog, or"
    echo "  - clear the quarantine flag:"
    echo "      xattr -dr com.apple.quarantine \"${DEST}\""
    ;;
  uninstall)
    rm -rf "${DEST}"
    echo "Removed ${DEST}"
    ;;
  *)
    echo "usage: $0 {install|uninstall}" >&2
    exit 2
    ;;
esac
