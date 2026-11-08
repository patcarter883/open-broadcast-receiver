# CMake generated Testfile for 
# Source directory: /home/pat/workspace/open-broadcast/open-broadcast-receiver/test
# Build directory: /home/pat/workspace/open-broadcast/open-broadcast-receiver/build-verify/test
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test("ring_test" "/home/pat/workspace/open-broadcast/open-broadcast-receiver/build-verify/test/ring_test")
set_tests_properties("ring_test" PROPERTIES  _BACKTRACE_TRIPLES "/home/pat/workspace/open-broadcast/open-broadcast-receiver/test/CMakeLists.txt;34;add_test;/home/pat/workspace/open-broadcast/open-broadcast-receiver/test/CMakeLists.txt;0;")
add_test("frame_ring_test" "/home/pat/workspace/open-broadcast/open-broadcast-receiver/build-verify/test/frame_ring_test")
set_tests_properties("frame_ring_test" PROPERTIES  _BACKTRACE_TRIPLES "/home/pat/workspace/open-broadcast/open-broadcast-receiver/test/CMakeLists.txt;35;add_test;/home/pat/workspace/open-broadcast/open-broadcast-receiver/test/CMakeLists.txt;0;")
add_test("validate_test" "/home/pat/workspace/open-broadcast/open-broadcast-receiver/build-verify/test/validate_test")
set_tests_properties("validate_test" PROPERTIES  _BACKTRACE_TRIPLES "/home/pat/workspace/open-broadcast/open-broadcast-receiver/test/CMakeLists.txt;36;add_test;/home/pat/workspace/open-broadcast/open-broadcast-receiver/test/CMakeLists.txt;0;")
add_test("start_parse_test" "/home/pat/workspace/open-broadcast/open-broadcast-receiver/build-verify/test/start_parse_test")
set_tests_properties("start_parse_test" PROPERTIES  _BACKTRACE_TRIPLES "/home/pat/workspace/open-broadcast/open-broadcast-receiver/test/CMakeLists.txt;37;add_test;/home/pat/workspace/open-broadcast/open-broadcast-receiver/test/CMakeLists.txt;0;")
add_test("transcode_select_test" "/home/pat/workspace/open-broadcast/open-broadcast-receiver/build-verify/test/transcode_select_test")
set_tests_properties("transcode_select_test" PROPERTIES  _BACKTRACE_TRIPLES "/home/pat/workspace/open-broadcast/open-broadcast-receiver/test/CMakeLists.txt;38;add_test;/home/pat/workspace/open-broadcast/open-broadcast-receiver/test/CMakeLists.txt;0;")
add_test("control_json_test" "/home/pat/workspace/open-broadcast/open-broadcast-receiver/build-verify/test/control_json_test")
set_tests_properties("control_json_test" PROPERTIES  _BACKTRACE_TRIPLES "/home/pat/workspace/open-broadcast/open-broadcast-receiver/test/CMakeLists.txt;39;add_test;/home/pat/workspace/open-broadcast/open-broadcast-receiver/test/CMakeLists.txt;0;")
add_test("pipeline_build_test" "/home/pat/workspace/open-broadcast/open-broadcast-receiver/build-verify/test/pipeline_build_test")
set_tests_properties("pipeline_build_test" PROPERTIES  _BACKTRACE_TRIPLES "/home/pat/workspace/open-broadcast/open-broadcast-receiver/test/CMakeLists.txt;50;add_test;/home/pat/workspace/open-broadcast/open-broadcast-receiver/test/CMakeLists.txt;0;")
add_test("decode_fallback_test" "/home/pat/workspace/open-broadcast/open-broadcast-receiver/build-verify/test/decode_fallback_test")
set_tests_properties("decode_fallback_test" PROPERTIES  _BACKTRACE_TRIPLES "/home/pat/workspace/open-broadcast/open-broadcast-receiver/test/CMakeLists.txt;63;add_test;/home/pat/workspace/open-broadcast/open-broadcast-receiver/test/CMakeLists.txt;0;")
add_test("decode_fixture_test" "/home/pat/workspace/open-broadcast/open-broadcast-receiver/test/tools/run_decode_fixture.sh" "/home/pat/workspace/open-broadcast/open-broadcast-receiver/build-verify/test/decode_file" "/home/pat/workspace/open-broadcast/open-broadcast-receiver/build-verify/test/av1_fixture.ts")
set_tests_properties("decode_fixture_test" PROPERTIES  SKIP_RETURN_CODE "77" TIMEOUT "180" _BACKTRACE_TRIPLES "/home/pat/workspace/open-broadcast/open-broadcast-receiver/test/CMakeLists.txt;78;add_test;/home/pat/workspace/open-broadcast/open-broadcast-receiver/test/CMakeLists.txt;0;")
