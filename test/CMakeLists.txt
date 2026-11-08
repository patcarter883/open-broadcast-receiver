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

add_executable(appsrc_cap_test source/appsrc_cap_test.cpp)
target_include_directories(appsrc_cap_test PRIVATE ${CMAKE_SOURCE_DIR}/source)
target_compile_features(appsrc_cap_test PRIVATE cxx_std_20)

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
add_test(NAME appsrc_cap_test COMMAND appsrc_cap_test)

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

  # audio_branch_test runs the transcode template's audio branch against real
  # MPEG-TS sources it generates: an AAC-in-TS must negotiate the declared
  # AAC-LC 48 kHz stereo raw caps, and a video-only TS must run through without
  # exposing an audio pad or stalling.
  add_executable(audio_branch_test source/audio_branch_test.cpp)
  target_link_libraries(audio_branch_test PRIVATE
    fanout_lib
    PkgConfig::gstreamer
    Threads::Threads)
  target_compile_features(audio_branch_test PRIVATE cxx_std_20)
  add_test(NAME audio_branch_test COMMAND audio_branch_test)
  set_tests_properties(audio_branch_test PROPERTIES TIMEOUT 180)

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

# ---- Standby-stage pure logic (no GStreamer) --------------------------------
# es_ring is the compressed-access-unit sibling of frame_ring; timeline is the
# re-stamper's arithmetic. Both are compiled into their test executables
# directly, so the tests-only lane (which declares ring_lib in the top-level
# CMakeLists) needs no extra library target and the full lane links no duplicate
# definition.

add_executable(es_ring_test
  source/es_ring_test.cpp
  ${CMAKE_SOURCE_DIR}/source/fanout/es_ring.cpp)
target_include_directories(es_ring_test PRIVATE ${CMAKE_SOURCE_DIR}/source)
target_link_libraries(es_ring_test PRIVATE Threads::Threads)
target_compile_features(es_ring_test PRIVATE cxx_std_20)
add_test(NAME es_ring_test COMMAND es_ring_test)

add_executable(timeline_test
  source/timeline_test.cpp
  ${CMAKE_SOURCE_DIR}/source/standby/timeline.cpp)
target_include_directories(timeline_test PRIVATE ${CMAKE_SOURCE_DIR}/source)
target_compile_features(timeline_test PRIVATE cxx_std_20)
add_test(NAME timeline_test COMMAND timeline_test)

# Standby slate (plan B1/B2).
#
# es_looper_test is pure: it pins the access-unit boundaries both framings
# produce, and a mid-AU splice is the one slate failure that reaches air as
# corruption. It runs in both lanes -- in the tests-only lane standby_lib is the
# headers alone, and the GStreamer half of es_looper.h is not declared.
add_executable(es_looper_test source/es_looper_test.cpp)
target_link_libraries(es_looper_test PRIVATE standby_lib Threads::Threads)
target_compile_features(es_looper_test PRIVATE cxx_std_20)
add_test(NAME es_looper_test COMMAND es_looper_test)

# slate_pipeline_test asserts the built pipeline strings, and (when a software
# h264 encoder is installed) renders one real slate: it needs the media stack.
if(NOT OBR_TESTS_ONLY)
  add_executable(slate_pipeline_test source/slate_pipeline_test.cpp)
  target_link_libraries(slate_pipeline_test PRIVATE
    standby_lib
    PkgConfig::gstreamer
    Threads::Threads)
  target_compile_features(slate_pipeline_test PRIVATE cxx_std_20)
  add_test(NAME slate_pipeline_test COMMAND slate_pipeline_test)
  set_tests_properties(slate_pipeline_test PROPERTIES TIMEOUT 120)
endif()

# stream_switch_test (plan C2). The keyframe predicate is pure and runs in
# both lanes; the selector/pipeline half is GStreamer-only and is compiled in
# only when the media stack is (it drives a real input-selector from two appsrc
# branches of generated h264 AUs). In the full lane it exits 77 when no h264
# encoder/parser is installed.
add_executable(stream_switch_test source/stream_switch_test.cpp)
target_link_libraries(stream_switch_test PRIVATE standby_lib Threads::Threads)
target_compile_features(stream_switch_test PRIVATE cxx_std_20)
add_test(NAME stream_switch_test COMMAND stream_switch_test)
set_tests_properties(stream_switch_test PROPERTIES TIMEOUT 180)
if(NOT OBR_TESTS_ONLY)
  target_link_libraries(stream_switch_test PRIVATE
    PkgConfig::gstreamer
    PkgConfig::gstreamer-app)
  set_tests_properties(stream_switch_test PROPERTIES SKIP_RETURN_CODE 77)
endif()

# stage_test (plan C4). The shared restartable stage is GStreamer-only (one
# demux, two selectors, one re-stamped timeline into the ES rings), so the test
# lives in the full lane only; the tests-only lane is unaffected. It generates
# its own TS fixture and slate, feeds them at real time and exits 77 when no h264
# encoder is installed.
if(NOT OBR_TESTS_ONLY)
  add_executable(stage_test source/stage_test.cpp)
  target_link_libraries(stage_test PRIVATE
    standby_lib
    PkgConfig::gstreamer
    PkgConfig::gstreamer-app
    Threads::Threads)
  target_compile_features(stage_test PRIVATE cxx_std_20)
  add_test(NAME stage_test COMMAND stage_test)

  # es_audio_framing_test asserts the audio framing contract between the stage
  # (what it publishes into the audio es_ring) and a standby-enabled output (what
  # its ES audio appsrc declares). A mismatch starves aacparse and silently stops
  # every standby RTMP publish; this fails it as a unit test instead.
  add_executable(es_audio_framing_test source/es_audio_framing_test.cpp)
  target_link_libraries(es_audio_framing_test PRIVATE
    standby_lib
    fanout_lib
    PkgConfig::gstreamer
    PkgConfig::gstreamer-app
    Threads::Threads)
  target_compile_features(es_audio_framing_test PRIVATE cxx_std_20)
  add_test(NAME es_audio_framing_test COMMAND es_audio_framing_test)
  set_tests_properties(es_audio_framing_test PROPERTIES
    TIMEOUT 120
    SKIP_RETURN_CODE 77)
  set_tests_properties(stage_test PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
endif()

# controller_test (plan D1). The mode controller is pure C++ -- no GStreamer, no
# pipeline -- so the decision logic (hysteresis, the session-end policy) is
# asserted in the fast lane with an explicit clock. The .cpp is compiled into
# the test directly, exactly like es_ring_test and timeline_test, so the
# tests-only lane needs no extra library target.
add_executable(controller_test
  source/controller_test.cpp
  ${CMAKE_SOURCE_DIR}/source/standby/controller.cpp)
target_include_directories(controller_test PRIVATE ${CMAKE_SOURCE_DIR}/source)
target_link_libraries(controller_test PRIVATE Threads::Threads)
target_compile_features(controller_test PRIVATE cxx_std_20)
add_test(NAME controller_test COMMAND controller_test)
