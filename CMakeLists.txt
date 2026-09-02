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
)
target_include_directories(brscan PUBLIC libbrscan/include)
target_include_directories(brscan PUBLIC libbrscan)

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
  )
  target_link_libraries(brscan_tests PRIVATE brscan GTest::gtest_main)
  target_include_directories(brscan_tests PRIVATE libbrscan tests)

  include(GoogleTest)
  gtest_discover_tests(brscan_tests)
endif()
