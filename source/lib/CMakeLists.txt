cmake_minimum_required(VERSION 3.28)

project(
    open-broadcast-receiver-lib
    LANGUAGES CXX
)

# ---- Declare library ----

add_library(receiver_lib STATIC)

target_sources(receiver_lib ${warning_guard}
  PUBLIC
    lib.h
    lib.cpp
)

target_include_directories(receiver_lib PUBLIC
    ${CMAKE_SOURCE_DIR}/source
)

target_compile_features(receiver_lib PUBLIC cxx_std_20)
