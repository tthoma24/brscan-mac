#!/bin/bash
# Install (or remove) the Plan 2 Task 1 ICA load-spike module into the system
# Image Capture devices directory. This is an OPT-IN, privileged step, kept out
# of the CMake build on purpose: /Library/Image Capture/Devices/ needs admin
# rights, and loading a module there is exactly what the load spike is testing.
#
# Usage, from the repo root after `cmake --build build`:
#   sudo ica-module/install-loadspike.sh install
#   sudo ica-module/install-loadspike.sh uninstall
#
# See docs/RUNBOOK-plan-2-loadspike.md for what to check once it is installed.
set -euo pipefail

BUNDLE="build/BrscanICALoadSpike.app"
DEST_DIR="/Library/Image Capture/Devices"
DEST="${DEST_DIR}/BrscanICALoadSpike.app"

case "${1:-}" in
  install)
    if [[ ! -d "${BUNDLE}" ]]; then
      echo "error: ${BUNDLE} not found. Build it first: cmake --build build" >&2
      exit 1
    fi
    mkdir -p "${DEST_DIR}"
    rm -rf "${DEST}"
    cp -R "${BUNDLE}" "${DEST}"
    echo "Installed ${DEST}"
    echo "Restart icdd so it rescans:"
    echo "  killall icdd 2>/dev/null || launchctl kickstart -k gui/\$(id -u)/com.apple.icdd"
    ;;
  uninstall)
    rm -rf "${DEST}"
    echo "Removed ${DEST}"
    echo "Restart icdd: killall icdd 2>/dev/null || true"
    ;;
  *)
    echo "usage: sudo $0 {install|uninstall}" >&2
    exit 2
    ;;
esac
