cmake_minimum_required(VERSION 3.28)

project(
    control-lib
    LANGUAGES CXX
)

add_library(control_lib STATIC)

target_sources(control_lib ${warning_guard}
  PUBLIC
    control.h
    control.cpp
)

target_include_directories(control_lib PUBLIC
    ${CMAKE_SOURCE_DIR}/source
    ${CMAKE_SOURCE_DIR}/external
)

target_compile_features(control_lib PUBLIC cxx_std_20)

target_link_libraries(control_lib PUBLIC
    receiver_lib
    nlohmann_json::nlohmann_json
    Threads::Threads
)
