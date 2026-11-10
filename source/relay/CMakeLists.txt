# The relay is two pieces:
#
#   relay_srp_lib  -- pure SRP-file parsing (base64 + the ristsrppasswd line
#                     format). No librist, so it builds in the tests-only lane
#                     and its parser is unit-tested there.
#   relay_lib      -- the packet pump itself, on the librist C API. Only built
#                     in the full lane, where librist is present.
#
# Splitting them this way is deliberate: the parser is the part with edge cases
# (padding, CRLF, the legacy/current hash pair) and the part a mistake in would
# refuse a real customer's credential, so it must be testable without a network.

add_library(relay_srp_lib STATIC
  ${CMAKE_CURRENT_LIST_DIR}/relay_srp.h
  ${CMAKE_CURRENT_LIST_DIR}/relay_srp.cpp)
target_include_directories(relay_srp_lib PUBLIC ${CMAKE_SOURCE_DIR}/source)
target_compile_features(relay_srp_lib PUBLIC cxx_std_20)

if(NOT OBR_TESTS_ONLY)
  add_library(relay_lib STATIC
    ${CMAKE_CURRENT_LIST_DIR}/relay.h
    ${CMAKE_CURRENT_LIST_DIR}/relay.cpp)
  target_include_directories(relay_lib PUBLIC
    ${CMAKE_SOURCE_DIR}/source
    ${RIST_INSTALL_DIR}/include
    ${RIST_INSTALL_DIR}/include/librist)
  target_compile_features(relay_lib PUBLIC cxx_std_20)
  target_link_libraries(relay_lib PUBLIC relay_srp_lib rist Threads::Threads)
endif()
