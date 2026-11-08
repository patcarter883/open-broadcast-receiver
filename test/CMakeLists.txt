cmake_minimum_required(VERSION 3.28)

project(open-broadcast-receiver-tests LANGUAGES CXX)

# Pure-logic tests: no GStreamer, no librist, no network. Framework-free
# executables — exit 0 is pass — registered with CTest.

find_package(Threads REQUIRED)

add_executable(ring_test source/ring_test.cpp)
target_link_libraries(ring_test PRIVATE ring_lib Threads::Threads)
target_compile_features(ring_test PRIVATE cxx_std_20)

add_executable(frame_ring_test source/frame_ring_test.cpp)
target_link_libraries(frame_ring_test PRIVATE ring_lib Threads::Threads)
target_compile_features(frame_ring_test PRIVATE cxx_std_20)

add_executable(validate_test source/validate_test.cpp)
target_link_libraries(validate_test PRIVATE receiver_lib Threads::Threads)
target_compile_features(validate_test PRIVATE cxx_std_20)

add_executable(start_parse_test source/start_parse_test.cpp)
target_link_libraries(start_parse_test PRIVATE control_lib Threads::Threads)
target_compile_features(start_parse_test PRIVATE cxx_std_20)

add_executable(transcode_select_test source/transcode_select_test.cpp)
target_link_libraries(transcode_select_test PRIVATE receiver_lib Threads::Threads)
target_compile_features(transcode_select_test PRIVATE cxx_std_20)

add_executable(control_json_test source/control_json_test.cpp)
target_link_libraries(control_json_test PRIVATE control_lib Threads::Threads)
target_compile_features(control_json_test PRIVATE cxx_std_20)

add_test(NAME ring_test COMMAND ring_test)
add_test(NAME frame_ring_test COMMAND frame_ring_test)
add_test(NAME validate_test COMMAND validate_test)
add_test(NAME start_parse_test COMMAND start_parse_test)
add_test(NAME transcode_select_test COMMAND transcode_select_test)
add_test(NAME control_json_test COMMAND control_json_test)

# GStreamer pipeline-string tests need the media stack, so they are only added
# outside the tests-only lane (OBR_TESTS_ONLY).
if(NOT OBR_TESTS_ONLY)
  add_executable(pipeline_build_test source/pipeline_build_test.cpp)
  target_link_libraries(pipeline_build_test PRIVATE
    fanout_lib
    PkgConfig::gstreamer
    Threads::Threads)
  target_compile_features(pipeline_build_test PRIVATE cxx_std_20)
  add_test(NAME pipeline_build_test COMMAND pipeline_build_test)

  # decode_file drives the receiver's real decode stage from a local MPEG-TS
  # fixture; run_decode_fixture.sh generates the fixture, runs it with
  # --expect-frames and exits 77 (skip) when the AV1 encoder or a VA-API render
  # node is absent.
  #   ./build/test/decode_file /tmp/av1.ts av1
  add_executable(decode_file tools/decode_file.cpp)
  target_link_libraries(decode_file PRIVATE
    fanout_lib
    PkgConfig::gstreamer
    PkgConfig::gstreamer-app
    Threads::Threads)
  target_compile_features(decode_file PRIVATE cxx_std_20)

  add_test(NAME decode_fixture_test
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tools/run_decode_fixture.sh
            $<TARGET_FILE:decode_file>
            ${CMAKE_CURRENT_BINARY_DIR}/av1_fixture.ts)
  set_tests_properties(decode_fixture_test PROPERTIES
    SKIP_RETURN_CODE 77
    TIMEOUT 180)
endif()
