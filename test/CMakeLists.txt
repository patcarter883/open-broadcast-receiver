cmake_minimum_required(VERSION 3.28)

project(open-broadcast-receiver-tests LANGUAGES CXX)

# Pure-logic tests: no GStreamer, no librist, no network. Framework-free
# executables — exit 0 is pass — registered with CTest.

find_package(Threads REQUIRED)

add_executable(ring_test source/ring_test.cpp)
target_link_libraries(ring_test PRIVATE ring_lib Threads::Threads)
target_compile_features(ring_test PRIVATE cxx_std_20)

add_executable(pts_synth_test source/pts_synth_test.cpp)
target_link_libraries(pts_synth_test PRIVATE Threads::Threads)
target_include_directories(pts_synth_test PRIVATE ${CMAKE_SOURCE_DIR}/source)
target_compile_features(pts_synth_test PRIVATE cxx_std_20)

add_executable(frame_ring_test source/frame_ring_test.cpp)
target_link_libraries(frame_ring_test PRIVATE ring_lib Threads::Threads)
target_compile_features(frame_ring_test PRIVATE cxx_std_20)

add_executable(decode_health_test source/decode_health_test.cpp)
target_link_libraries(decode_health_test PRIVATE ring_lib Threads::Threads)
target_compile_features(decode_health_test PRIVATE cxx_std_20)

add_executable(validate_test source/validate_test.cpp)
target_link_libraries(validate_test PRIVATE receiver_lib Threads::Threads)
target_compile_features(validate_test PRIVATE cxx_std_20)

add_executable(bitrate_test source/bitrate_test.cpp)
target_link_libraries(bitrate_test PRIVATE receiver_lib Threads::Threads)
target_compile_features(bitrate_test PRIVATE cxx_std_20)

add_executable(start_parse_test source/start_parse_test.cpp)
target_link_libraries(start_parse_test PRIVATE control_lib Threads::Threads)
target_compile_features(start_parse_test PRIVATE cxx_std_20)

add_executable(transcode_select_test source/transcode_select_test.cpp)
target_link_libraries(transcode_select_test PRIVATE receiver_lib Threads::Threads)
target_compile_features(transcode_select_test PRIVATE cxx_std_20)

add_executable(source_codec_test source/source_codec_test.cpp)
target_link_libraries(source_codec_test PRIVATE receiver_lib Threads::Threads)
target_compile_features(source_codec_test PRIVATE cxx_std_20)

add_executable(control_json_test source/control_json_test.cpp)
target_link_libraries(control_json_test PRIVATE control_lib Threads::Threads)
target_compile_features(control_json_test PRIVATE cxx_std_20)

add_test(NAME ring_test COMMAND ring_test)
add_test(NAME pts_synth_test COMMAND pts_synth_test)
add_test(NAME frame_ring_test COMMAND frame_ring_test)
add_test(NAME decode_health_test COMMAND decode_health_test)
add_test(NAME validate_test COMMAND validate_test)
add_test(NAME bitrate_test COMMAND bitrate_test)
add_test(NAME start_parse_test COMMAND start_parse_test)
add_test(NAME transcode_select_test COMMAND transcode_select_test)
add_test(NAME source_codec_test COMMAND source_codec_test)
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

  # decode_fallback_test exercises decoder selection with real pipelines: it
  # generates AV1 fixtures in-process, forces a hardware refusal (4:4:4 against
  # vaav1dec's profile=main caps) and asserts the stage degrades to av1dec with
  # frames still flowing.
  add_executable(decode_fallback_test source/decode_fallback_test.cpp)
  target_link_libraries(decode_fallback_test PRIVATE
    fanout_lib
    PkgConfig::gstreamer
    PkgConfig::gstreamer-app
    Threads::Threads)
  target_compile_features(decode_fallback_test PRIVATE cxx_std_20)
  add_test(NAME decode_fallback_test COMMAND decode_fallback_test)

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

  # rist_feed reads an MPEG-TS byte stream on stdin and sends it to a RIST
  # listener in 7x188-byte datagrams (ADVANCED profile, SOURCE timing), so the
  # local e2e harness can drive the receiver over its real ingest instead of a
  # stub. Not a CTest by itself.
  add_executable(rist_feed tools/rist_feed.cpp)
  target_link_libraries(rist_feed PRIVATE ristnet Threads::Threads)
  target_compile_features(rist_feed PRIVATE cxx_std_20)

  # Local RTMP publish harness: drives the real output pipeline against a local
  # nginx-rtmp sink and judges it by the server's own byte count. It publishes,
  # so it is built with the full lane but only registered with CTest when a sink
  # is expected to be up (test/harness/rtmp_publish_harness.sh starts one).
  add_executable(rtmp_publish_harness source/rtmp_publish_harness.cpp)
  target_link_libraries(rtmp_publish_harness PRIVATE
    fanout_lib
    PkgConfig::gstreamer
    PkgConfig::gstreamer-app
    PkgConfig::gstreamer-video
    Threads::Threads)
  target_compile_features(rtmp_publish_harness PRIVATE cxx_std_20)
  target_include_directories(rtmp_publish_harness PRIVATE
    ${CMAKE_SOURCE_DIR}/external)

  # Stamp the binary with the commit it was linked against. A verdict is about
  # that commit only, and without the stamp a report cannot say which code it
  # measured. The run script refuses a binary older than its sources.
  execute_process(
    COMMAND git -C ${CMAKE_SOURCE_DIR} rev-parse --short HEAD
    OUTPUT_VARIABLE OBR_SOURCE_COMMIT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
  if(NOT OBR_SOURCE_COMMIT)
    set(OBR_SOURCE_COMMIT "unknown")
  endif()
  target_compile_definitions(rtmp_publish_harness PRIVATE
    OBR_SOURCE_COMMIT="${OBR_SOURCE_COMMIT}")

  option(OBR_RTMP_HARNESS_TESTS
    "Register the RTMP publish harness with CTest (needs a sink on 11935/18080)"
    OFF)
  if(OBR_RTMP_HARNESS_TESTS)
    add_test(NAME rtmp_publish_transcode_av
      COMMAND rtmp_publish_harness --mode transcode-av)
    add_test(NAME rtmp_publish_transcode_video_only
      COMMAND rtmp_publish_harness --mode transcode-video-only)
    set_tests_properties(rtmp_publish_transcode_av
      rtmp_publish_transcode_video_only
      PROPERTIES TIMEOUT 90)
  endif()
endif()
