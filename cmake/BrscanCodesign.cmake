# Shared ad-hoc code-signing helper, used by every macOS `.app` bundle this
# project builds: the Plan 2 ICA module (`brscan-ica`) and the Plan 1e config
# app (`brscan-config-app`). The two bundles are constructed differently — the
# ICA module is a CMake-native `MACOSX_BUNDLE` target, the config app wraps
# `swift build` and hand-assembles `Contents/` — so bundle *construction* is
# not shared, but the sign + verify step is identical and lives here so both
# call the same code (and so distribution docs, docs/DISTRIBUTION.md, describe
# one signing story).
#
# Ad-hoc signing (`codesign --sign -`) is sufficient for a local/dev install;
# Developer-ID signing + notarization for redistribution is a documented future
# step, not wired into the build (see docs/DISTRIBUTION.md).

# brscan_adhoc_sign(<bundle_dir> [DEPENDS <target>])
#
# Attach a POST_BUILD step that ad-hoc signs <bundle_dir> and then verifies the
# signature. <bundle_dir> is the assembled `.app` path (e.g. a
# `$<TARGET_BUNDLE_DIR:...>` generator expression for a CMake bundle target, or
# a plain path for a hand-assembled bundle). Pass `DEPENDS <target>` to name the
# target whose POST_BUILD event runs the commands; the caller must supply a
# target since a bundle is always produced by one.
#
#   - `codesign --force --sign - --timestamp=none <bundle>` applies an ad-hoc
#     signature (no cert, no timestamp server), the cheapest signature and the
#     one that makes every local iteration a plain rebuild.
#   - `codesign --verify --deep --strict --verbose=2 <bundle>` then confirms the
#     result is well-formed, failing the build if signing did not take.
function(brscan_adhoc_sign bundle_dir)
  cmake_parse_arguments(BRSCAN_SIGN "" "DEPENDS" "" ${ARGN})
  if(NOT BRSCAN_SIGN_DEPENDS)
    message(FATAL_ERROR "brscan_adhoc_sign requires DEPENDS <target>")
  endif()
  add_custom_command(TARGET ${BRSCAN_SIGN_DEPENDS} POST_BUILD
    COMMAND codesign --force --sign - --timestamp=none "${bundle_dir}"
    COMMAND codesign --verify --deep --strict --verbose=2 "${bundle_dir}"
    COMMENT "Ad-hoc signing + verifying ${bundle_dir}"
    VERBATIM)
endfunction()
