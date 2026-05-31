cmake_minimum_required(VERSION 3.28)

project(
    restream-lib
    LANGUAGES CXX
)

add_library(restream_lib STATIC)

target_sources(restream_lib ${warning_guard}
  PUBLIC
    restream.h
    restream.cpp
)

target_include_directories(restream_lib PUBLIC
    ${CMAKE_SOURCE_DIR}/source
)

target_compile_features(restream_lib PUBLIC cxx_std_20)

target_link_libraries(restream_lib PUBLIC
    receiver_lib
)

target_link_libraries(restream_lib PRIVATE
    PkgConfig::gstreamer
    PkgConfig::gstreamer-app
    PkgConfig::gstreamer-video)

target_include_directories(restream_lib PRIVATE SYSTEM
    ${GSTREAMER_INCLUDE_DIRS})
