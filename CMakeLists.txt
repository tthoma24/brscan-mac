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

# The end-to-end scanning CLI. Links only against the brscan library --
# never talks to a socket or the protocol codecs directly.
add_executable(brscan-cli tools/brscan-cli.cpp tools/scan_output.cpp)
target_link_libraries(brscan-cli PRIVATE brscan)
target_link_libraries(brscan-cli PRIVATE ${TURBOJPEG_LIBRARIES})
target_link_directories(brscan-cli PRIVATE ${TURBOJPEG_LIBRARY_DIRS})

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
  )
  target_link_libraries(brscan_tests PRIVATE brscan GTest::gtest_main)
  target_include_directories(brscan_tests PRIVATE libbrscan tests daemon)
  target_include_directories(brscan_tests PRIVATE ${TURBOJPEG_INCLUDE_DIRS})
  target_link_libraries(brscan_tests PRIVATE ${TURBOJPEG_LIBRARIES})
  target_link_directories(brscan_tests PRIVATE ${TURBOJPEG_LIBRARY_DIRS})
  target_compile_definitions(brscan_tests PRIVATE
    BRSCAN_FIXTURES_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures")

  include(GoogleTest)
  gtest_discover_tests(brscan_tests)
endif()
