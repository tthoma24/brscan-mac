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
  daemon/config.cpp
  daemon/actions.cpp
  daemon/action_ocr.mm
  daemon/output_writer.mm
  daemon/handle_event.cpp
  daemon/sender_check.cpp
  daemon/notification_deduper.cpp
)
target_include_directories(brscan-scand PRIVATE daemon tools)
target_link_libraries(brscan-scand PRIVATE brscan)
target_link_libraries(brscan-scand PRIVATE ${TURBOJPEG_LIBRARIES})
target_link_directories(brscan-scand PRIVATE ${TURBOJPEG_LIBRARY_DIRS})
target_link_libraries(brscan-scand PRIVATE ${OCR_FRAMEWORKS})

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
    daemon/config.cpp
    tests/config_test.cpp
    daemon/actions.cpp
    tests/actions_test.cpp
    daemon/action_ocr.mm
    tests/action_ocr_test.mm
    daemon/output_writer.mm
    tests/output_writer_test.mm
    daemon/handle_event.cpp
    tests/handle_event_test.cpp
    daemon/sender_check.cpp
    tests/sender_check_test.cpp
    daemon/notification_deduper.cpp
    tests/notification_deduper_test.cpp
    tools/scan_output.cpp
  )
  target_link_libraries(brscan_tests PRIVATE brscan GTest::gtest_main)
  target_include_directories(brscan_tests PRIVATE libbrscan tests daemon tools)
  target_include_directories(brscan_tests PRIVATE ${TURBOJPEG_INCLUDE_DIRS})
  target_link_libraries(brscan_tests PRIVATE ${TURBOJPEG_LIBRARIES})
  target_link_directories(brscan_tests PRIVATE ${TURBOJPEG_LIBRARY_DIRS})
  target_link_libraries(brscan_tests PRIVATE ${OCR_FRAMEWORKS})
  target_compile_definitions(brscan_tests PRIVATE
    BRSCAN_FIXTURES_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures")

  include(GoogleTest)
  gtest_discover_tests(brscan_tests)
endif()
