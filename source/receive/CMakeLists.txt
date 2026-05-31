cmake_minimum_required(VERSION 3.28)

project(
    receive-lib
    LANGUAGES CXX
)

add_library(receive_lib STATIC)

target_sources(receive_lib ${warning_guard}
  PUBLIC
    receive.h
    receive.cpp
)

target_include_directories(receive_lib PUBLIC
    ${CMAKE_SOURCE_DIR}/source
)

target_compile_features(receive_lib PUBLIC cxx_std_20)

# ristnet (PUBLIC: RISTNet.h is included from receive.h) brings the rist-cpp +
# librist include dirs and links librist + threads.
target_link_libraries(receive_lib PUBLIC
    ristnet
)

target_link_libraries(receive_lib PRIVATE
    receiver_lib
)
