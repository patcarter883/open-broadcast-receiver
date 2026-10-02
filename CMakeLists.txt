cmake_minimum_required(VERSION 3.28)

include(cmake/prelude.cmake)

project(
    open-broadcast-receiver
    VERSION 0.1.0
    DESCRIPTION "Headless RIST receiver / restreamer (partner to open-broadcast-encoder)"
    HOMEPAGE_URL "https://example.com/"
    LANGUAGES C CXX
)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake/modules")
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_BUILD_PARALLEL_LEVEL 16)

include(cmake/variables.cmake)

# Tests-only mode: build just the pure-logic libraries (no GStreamer, no
# vendored librist, no network) plus their unit tests. Used by the fast CI
# lane and on dev boxes without the media stack installed.
option(OBR_TESTS_ONLY "Build only pure-logic libs + unit tests" OFF)

find_package(Threads REQUIRED)

if(OBR_TESTS_ONLY)
  add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/source/lib)

  # The /start body parser is pure logic (httplib + nlohmann only, no
  # GStreamer), so the control library is unit-testable in this fast lane.
  find_package(nlohmann_json 3.2 REQUIRED)
  add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/source/control)

  add_library(ring_lib STATIC
    ${CMAKE_CURRENT_LIST_DIR}/source/fanout/ring.h
    ${CMAKE_CURRENT_LIST_DIR}/source/fanout/ring.cpp)
  target_include_directories(ring_lib PUBLIC ${CMAKE_CURRENT_LIST_DIR}/source)
  target_compile_features(ring_lib PUBLIC cxx_std_20)

  include(CTest)
  add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/test)
  return()
endif()

include(cmake/ExternalBuilds.cmake)

find_package(nlohmann_json 3.2 REQUIRED)

find_package(PkgConfig REQUIRED)
    pkg_search_module(gstreamer       REQUIRED IMPORTED_TARGET gstreamer-1.0>=1.28)
    pkg_search_module(gstreamer-app   REQUIRED IMPORTED_TARGET gstreamer-app-1.0>=1.28)
    pkg_search_module(gstreamer-sdp   REQUIRED IMPORTED_TARGET gstreamer-sdp-1.0>=1.28)
    pkg_search_module(gstreamer-rtp   REQUIRED IMPORTED_TARGET gstreamer-rtp-1.0>=1.28)
    pkg_search_module(gstreamer-video REQUIRED IMPORTED_TARGET gstreamer-video-1.0>=1.28)

add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/source/lib)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/source/fanout)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/source/receive)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/source/control)

include(CTest)
if(BUILD_TESTING)
  add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/test)
endif()

# ---- Declare executable ----

add_executable(open-broadcast-receiver_exe ${CMAKE_CURRENT_LIST_DIR}/source/main.cpp)
add_executable(open-broadcast-receiver::exe ALIAS open-broadcast-receiver_exe)

set_property(TARGET open-broadcast-receiver_exe PROPERTY OUTPUT_NAME open-broadcast-receiver)

target_compile_features(open-broadcast-receiver_exe PRIVATE cxx_std_20)

target_link_libraries(open-broadcast-receiver_exe PRIVATE
  receiver_lib
  ring_lib
  fanout_lib
  receive_lib
  control_lib
  ristnet
  PkgConfig::gstreamer
  PkgConfig::gstreamer-app
  PkgConfig::gstreamer-sdp
  PkgConfig::gstreamer-rtp
  PkgConfig::gstreamer-video
  Threads::Threads
  nlohmann_json::nlohmann_json)

target_include_directories(open-broadcast-receiver_exe PUBLIC SYSTEM
  ${RIST_INSTALL_DIR}/include
  ${RIST_INSTALL_DIR}/include/rist-cpp
  ${GSTREAMER_INCLUDE_DIRS})

target_link_options(open-broadcast-receiver_exe PRIVATE -Wl,--as-needed)

target_include_directories(open-broadcast-receiver_exe PUBLIC
  ${CMAKE_CURRENT_LIST_DIR}/source
  ${CMAKE_CURRENT_LIST_DIR}/external)

install(TARGETS open-broadcast-receiver_exe RUNTIME DESTINATION bin)
