cmake_minimum_required(VERSION 3.28)

project(open-broadcast-receiver-tests LANGUAGES CXX)

# Pure-logic tests: no GStreamer, no librist, no network. Framework-free
# executables — exit 0 is pass — registered with CTest.

find_package(Threads REQUIRED)

add_executable(ring_test source/ring_test.cpp)
target_link_libraries(ring_test PRIVATE ring_lib Threads::Threads)
target_compile_features(ring_test PRIVATE cxx_std_20)

add_executable(validate_test source/validate_test.cpp)
target_link_libraries(validate_test PRIVATE receiver_lib Threads::Threads)
target_compile_features(validate_test PRIVATE cxx_std_20)

add_executable(start_parse_test source/start_parse_test.cpp)
target_link_libraries(start_parse_test PRIVATE control_lib Threads::Threads)
target_compile_features(start_parse_test PRIVATE cxx_std_20)

add_test(NAME ring_test COMMAND ring_test)
add_test(NAME validate_test COMMAND validate_test)
add_test(NAME start_parse_test COMMAND start_parse_test)
