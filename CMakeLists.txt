cmake_minimum_required(VERSION 3.20)
project(brscan_mac LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

# The scanner protocol core. Front ends (CLI, ICA module, SANE backend) link
# against this and never talk to a socket or the ICA framework directly.
add_library(brscan
  libbrscan/version.cpp
  libbrscan/session.cpp
  libbrscan/transport_tcp.cpp
  libbrscan/command.cpp
  libbrscan/response.cpp
  libbrscan/decode_jpeg.cpp
  libbrscan/decode_rlength.cpp
  libbrscan/scanner.cpp
)
target_include_directories(brscan PUBLIC libbrscan/include)
target_include_directories(brscan PRIVATE libbrscan)

# libturbojpeg, for DecodeJpeg. Installed under MacPorts; located via
# pkg-config rather than assuming a fixed prefix.
find_package(PkgConfig REQUIRED)
pkg_check_modules(TURBOJPEG REQUIRED libturbojpeg)
target_include_directories(brscan PRIVATE ${TURBOJPEG_INCLUDE_DIRS})
target_link_libraries(brscan PRIVATE ${TURBOJPEG_LIBRARIES})
target_link_directories(brscan PRIVATE ${TURBOJPEG_LIBRARY_DIRS})

# The end-to-end scanning CLI. Links against the brscan library for the
# scan protocol itself, plus daemon/output_writer.mm (Task 1c.2b's
# --format/--tiff-compression/--separate flags) for turning the scanned
# pages into the requested file(s). output_writer.mm's PDF/TIFF paths
# decode each page via daemon/action_ocr.mm's CreateCGImageFromScanResult
# and compose PDFs via its WriteSearchablePdf (with searchable always
# false from the CLI -- OCR is a daemon destination concern), so that
# source and its Vision/CoreGraphics framework dependencies are pulled in
# here too, even though the CLI itself never requests a text layer.
add_executable(brscan-cli
  tools/brscan-cli.cpp
  tools/scan_output.cpp
  daemon/action_ocr.mm
  daemon/output_writer.mm
)
target_include_directories(brscan-cli PRIVATE daemon tools)
target_link_libraries(brscan-cli PRIVATE brscan)
target_link_libraries(brscan-cli PRIVATE ${TURBOJPEG_LIBRARIES})
target_link_directories(brscan-cli PRIVATE ${TURBOJPEG_LIBRARY_DIRS})

# Frameworks for the OCR destination action (daemon/action_ocr.mm): Vision
# does the text recognition; the rest are the CoreGraphics/CoreText/ImageIO
# plumbing used to load the saved scan and compose the searchable PDF.
# PDFKit and AppKit aren't used by action_ocr.mm itself, but PDFKit is
# what tests/action_ocr_test.mm uses to read the produced PDF back for
# verification (see CMakeLists.txt's brscan_tests target below), and
# PDFKit pulls in AppKit.
find_library(VISION_FRAMEWORK Vision REQUIRED)
find_library(CORETEXT_FRAMEWORK CoreText REQUIRED)
find_library(COREGRAPHICS_FRAMEWORK CoreGraphics REQUIRED)
find_library(COREFOUNDATION_FRAMEWORK CoreFoundation REQUIRED)
find_library(FOUNDATION_FRAMEWORK Foundation REQUIRED)
find_library(IMAGEIO_FRAMEWORK ImageIO REQUIRED)
find_library(PDFKIT_FRAMEWORK PDFKit REQUIRED)
find_library(APPKIT_FRAMEWORK AppKit REQUIRED)
set(OCR_FRAMEWORKS
  ${VISION_FRAMEWORK}
  ${CORETEXT_FRAMEWORK}
  ${COREGRAPHICS_FRAMEWORK}
  ${COREFOUNDATION_FRAMEWORK}
  ${FOUNDATION_FRAMEWORK}
  ${IMAGEIO_FRAMEWORK}
  ${PDFKIT_FRAMEWORK}
  ${APPKIT_FRAMEWORK}
)
target_link_libraries(brscan-cli PRIVATE ${OCR_FRAMEWORKS})

# action_ocr.mm (and its test) use ARC for their Objective-C objects
# (NSData, VN*, NS*); Core Foundation/CoreGraphics/CoreText opaque types
# are unaffected by this flag and stay manually retained/released either
# way. CMake infers Objective-C++ compilation for these from the .mm
# extension on its own.
set_source_files_properties(
  daemon/action_ocr.mm
  daemon/output_writer.mm
  tests/action_ocr_test.mm
  tests/output_writer_test.mm
  PROPERTIES COMPILE_OPTIONS "-fobjc-arc"
)

# The Scan-button daemon: registers with the printer, listens for button
# presses on UDP 54925, pulls each scan, and saves it (Plan 1b). Its own
# protocol pieces (SNMP registration, the UDP listener, config, and FUNC
# dispatch) live under daemon/ rather than in the brscan library, since
# they're specific to this daemon and not part of the core scan protocol.
add_executable(brscan-scand
  tools/brscan-scand.cpp
  tools/scan_output.cpp
  daemon/snmp_register.cpp
  daemon/button_listener.cpp
  daemon/button_config.cpp
  daemon/paper_size.cpp
  daemon/config.cpp
  daemon/actions.cpp
  daemon/action_ocr.mm
  daemon/output_writer.mm
  daemon/button_plan.cpp
  daemon/handle_event.cpp
  daemon/sender_check.cpp
  daemon/notification_deduper.cpp
)
target_include_directories(brscan-scand PRIVATE daemon tools)
target_link_libraries(brscan-scand PRIVATE brscan)
target_link_libraries(brscan-scand PRIVATE ${TURBOJPEG_LIBRARIES})
target_link_directories(brscan-scand PRIVATE ${TURBOJPEG_LIBRARY_DIRS})
target_link_libraries(brscan-scand PRIVATE ${OCR_FRAMEWORKS})

# Plan 2 Task 1 — ICA device-module load spike.
#
# A separate, self-contained target: a minimal background-only .app bundle that
# the Image Capture discovery daemon (icdd) could load and match to the
# synthetic device. It does NOT link libbrscan and adds no scan logic; its whole
# job is the go/no-go finding "does a third-party ICA module still load, and
# under what signing" (docs/RUNBOOK-plan-2-loadspike.md). Guarded by an option
# so it never blocks the core build, but ON by default on Apple so the default
# `cmake --build build` keeps it compiling.
option(BRSCAN_BUILD_ICA_MODULE "Build the Plan 2 ICA load-spike module" ON)
if(APPLE AND BRSCAN_BUILD_ICA_MODULE)
  set(MODULE_NAME "BrscanICALoadSpike")
  set(MODULE_MIN_MACOS "13.0")

  # Fill the bundle name / min-OS placeholders into a concrete Info.plist.
  configure_file(
    ${CMAKE_SOURCE_DIR}/ica-module/Info.plist.in
    ${CMAKE_BINARY_DIR}/ica-module/Info.plist
    @ONLY)

  # Plan 2 Task 2 adds the session/open callbacks (module_main.mm) plus the
  # scanner-parameter enumeration (scan_parameters.mm), which reuses the
  # captured paper geometry in daemon/paper_size.cpp as its single source of
  # truth (PLAN-2-DESIGN.md decision E). paper_size.cpp is self-contained
  # (brscan::Area is a header POD in libbrscan/include), so it compiles into the
  # module with only the include dirs below — no libbrscan link needed here.
  # Plan 2 Task 7 adds scan execution: module_main.mm now wires ICD_ScannerStart
  # to brscan::RunScan (over TcpTransport), decodes each page (buffer_descriptor
  # + brscan::DecodeJpeg), and hands the pixels back via the ICADevices band
  # notification API. That pulls in libbrscan (RunScan / TcpTransport /
  # DecodeJpeg) and the pure params/buffer helpers, so the bundle now links
  # libbrscan (and, transitively, libturbojpeg) -- the load spike did not.
  add_executable(brscan-ica-loadspike MACOSX_BUNDLE
    ica-module/module_main.mm
    ica-module/scan_parameters.mm
    ica-module/scan_translate.cpp
    ica-module/buffer_descriptor.cpp
    daemon/paper_size.cpp)
  set_target_properties(brscan-ica-loadspike PROPERTIES
    OUTPUT_NAME ${MODULE_NAME}
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_BINARY_DIR}/ica-module/Info.plist)
  target_include_directories(brscan-ica-loadspike PRIVATE
    ${CMAKE_SOURCE_DIR}/libbrscan/include
    ${CMAKE_SOURCE_DIR}/libbrscan
    ${CMAKE_SOURCE_DIR}/ica-module
    ${CMAKE_SOURCE_DIR}/daemon)

  # ICADevices supplies ICD_ScannerMain + gICDScannerCallbackFunctions and the
  # band/notification hand-back API; Foundation/CoreGraphics are pulled in by the
  # umbrella header. libbrscan supplies the scan protocol; libturbojpeg is its
  # private dependency but must be on the final link line since libbrscan is a
  # static library.
  find_library(ICADEVICES_FRAMEWORK ICADevices REQUIRED)
  find_library(ICA_FOUNDATION_FRAMEWORK Foundation REQUIRED)
  find_library(ICA_COREGRAPHICS_FRAMEWORK CoreGraphics REQUIRED)
  target_link_libraries(brscan-ica-loadspike PRIVATE
    brscan
    ${ICADEVICES_FRAMEWORK}
    ${ICA_FOUNDATION_FRAMEWORK}
    ${ICA_COREGRAPHICS_FRAMEWORK}
    ${TURBOJPEG_LIBRARIES})
  target_link_directories(brscan-ica-loadspike PRIVATE ${TURBOJPEG_LIBRARY_DIRS})

  # icdd reads Contents/Resources/DeviceMatchingInfo.plist for the Bonjour
  # match and DeviceInfo.plist for the matched device's presentation metadata;
  # copy both into the bundle after link. (Task 1b: the first spike shipped
  # only DeviceMatchingInfo.plist, with the wrong schema, and no DeviceInfo.plist
  # at all — icdd matched nothing. Both are corrected here.)
  add_custom_command(TARGET brscan-ica-loadspike POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
      ${CMAKE_SOURCE_DIR}/ica-module/DeviceMatchingInfo.plist
      $<TARGET_BUNDLE_CONTENT_DIR:brscan-ica-loadspike>/Resources/DeviceMatchingInfo.plist
    COMMAND ${CMAKE_COMMAND} -E copy
      ${CMAKE_SOURCE_DIR}/ica-module/DeviceInfo.plist
      $<TARGET_BUNDLE_CONTENT_DIR:brscan-ica-loadspike>/Resources/DeviceInfo.plist
    COMMENT "Installing DeviceMatchingInfo.plist + DeviceInfo.plist into the ICA module bundle")

  # Write Contents/PkgInfo (APPL????). Apple's AirScanScanner.app ships one and
  # CMake's MACOSX_BUNDLE does not emit it; LaunchServices (icdd launches modules
  # via openApplicationAtURL:) reads it, so we add it for bundle parity. The file
  # is generated at configure time (no trailing newline) and copied in before
  # signing so PkgInfo is covered by the signature. (Task 1c.)
  file(WRITE ${CMAKE_BINARY_DIR}/ica-module/PkgInfo "APPL????")
  add_custom_command(TARGET brscan-ica-loadspike POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
      ${CMAKE_BINARY_DIR}/ica-module/PkgInfo
      $<TARGET_BUNDLE_CONTENT_DIR:brscan-ica-loadspike>/PkgInfo
    COMMENT "Writing Contents/PkgInfo (APPL????) into the ICA module bundle")

  # Ad-hoc code signature: the cheapest signing to try in the load spike. A
  # module in /Library/Image Capture/Devices/ is loaded into an Apple host, so
  # whether ad-hoc is enough or Developer-ID + notarization is required is
  # exactly the finding (see the runbook's Signing section). Ad-hoc costs
  # nothing and lets every local iteration be a plain rebuild.
  add_custom_command(TARGET brscan-ica-loadspike POST_BUILD
    COMMAND codesign --force --sign - --timestamp=none
      $<TARGET_BUNDLE_DIR:brscan-ica-loadspike>
    COMMENT "Ad-hoc signing the ICA module bundle")
endif()

# Tests.
include(CTest)
if(BUILD_TESTING)
  include(FetchContent)
  FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.15.2
  )
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)

  add_executable(brscan_tests
    tests/version_test.cpp
    tests/fake_transport_test.cpp
    tests/session_test.cpp
    tests/command_test.cpp
    tests/response_test.cpp
    tests/decode_test.cpp
    tests/decode_rlength_test.cpp
    tests/scanner_test.cpp
    daemon/snmp_register.cpp
    tests/snmp_register_test.cpp
    daemon/button_listener.cpp
    tests/button_listener_test.cpp
    daemon/button_config.cpp
    tests/button_config_test.cpp
    daemon/paper_size.cpp
    tests/paper_size_test.cpp
    daemon/config.cpp
    tests/config_test.cpp
    tests/config_reload_test.cpp
    daemon/actions.cpp
    tests/actions_test.cpp
    daemon/action_ocr.mm
    tests/action_ocr_test.mm
    daemon/output_writer.mm
    tests/output_writer_test.mm
    daemon/button_plan.cpp
    tests/button_plan_test.cpp
    daemon/handle_event.cpp
    tests/handle_event_test.cpp
    daemon/sender_check.cpp
    tests/sender_check_test.cpp
    daemon/notification_deduper.cpp
    tests/notification_deduper_test.cpp
    tools/scan_output.cpp
    tests/option_sets_test.cpp
    ica-module/buffer_descriptor.cpp
    tests/buffer_descriptor_test.cpp
    ica-module/scan_translate.cpp
    tests/scan_translate_test.cpp
  )
  target_link_libraries(brscan_tests PRIVATE brscan GTest::gtest_main)
  target_include_directories(brscan_tests PRIVATE libbrscan tests daemon tools
                             ica-module)
  target_include_directories(brscan_tests PRIVATE ${TURBOJPEG_INCLUDE_DIRS})
  target_link_libraries(brscan_tests PRIVATE ${TURBOJPEG_LIBRARIES})
  target_link_directories(brscan_tests PRIVATE ${TURBOJPEG_LIBRARY_DIRS})
  target_link_libraries(brscan_tests PRIVATE ${OCR_FRAMEWORKS})
  target_compile_definitions(brscan_tests PRIVATE
    BRSCAN_FIXTURES_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures"
    BRSCAN_OPTION_SETS_JSON="${CMAKE_SOURCE_DIR}/config/option-sets.json")

  include(GoogleTest)
  gtest_discover_tests(brscan_tests)

  # Task 1e.1's up-to-date check: config/option-sets.json is the single
  # source of truth for the GUI's Swift option-set constants (see
  # scripts/gen-option-sets.py); this test regenerates them into a temp
  # location and diffs against the committed
  # gui/BrscanConfigCore/Sources/BrscanConfigCore/GeneratedOptionSets.swift,
  # failing if a JSON edit landed without a matching regenerate-and-commit.
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  add_test(
    NAME OptionSetsSwiftConstantsUpToDate
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/gen-option-sets.py --check
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  )
endif()
