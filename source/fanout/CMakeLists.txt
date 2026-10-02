cmake_minimum_required(VERSION 3.28)

project(
    fanout-lib
    LANGUAGES CXX
)

# ---- Ring (pure: no GStreamer, unit-testable standalone) ----

add_library(ring_lib STATIC)

target_sources(ring_lib ${warning_guard}
  PUBLIC
    ring.h
    ring.cpp
    frame_ring.h
    frame_ring.cpp
)

target_include_directories(ring_lib PUBLIC
    ${CMAKE_SOURCE_DIR}/source
)

target_compile_features(ring_lib PUBLIC cxx_std_20)

# ---- Output objects + recorder (GStreamer-facing) ----

add_library(fanout_lib STATIC)

target_sources(fanout_lib ${warning_guard}
  PUBLIC
    output.h
    output.cpp
    recorder.h
    recorder.cpp
)

target_include_directories(fanout_lib PUBLIC
    ${CMAKE_SOURCE_DIR}/source
)

target_compile_features(fanout_lib PUBLIC cxx_std_20)

target_link_libraries(fanout_lib PUBLIC
    receiver_lib
    ring_lib
)

target_link_libraries(fanout_lib PRIVATE
    PkgConfig::gstreamer
    PkgConfig::gstreamer-app)

target_include_directories(fanout_lib PRIVATE SYSTEM
    ${GSTREAMER_INCLUDE_DIRS})
