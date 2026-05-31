# ExternalBuilds.cmake
#
# Builds the rist-cpp dependency (which embeds librist + the RISTNet C++
# wrapper) in an isolated directory so that cleaning or removing the main build
# directory never forces a rebuild. This mirrors the open-broadcast-encoder
# pattern but drops the FLTK and sdp-tools-cpp blocks (the receiver is headless
# and has no GUI or SDP needs).
#
# Layout:
#   ${PROJECT_SOURCE_DIR}/build-external/rist-cpp/install/  -- installed artifacts
#
# Exposed to the main project through IMPORTED targets `rist` and `ristnet`.
#
# NOTE: this repo vendors a *patched* copy of rist-cpp under external/rist-cpp.
# The patch makes RISTNetReceiver::sendOOBData log+return false instead of
# tearing down the receiver on a transient OOB write failure (DECISIONS.md §6).

include(ExternalProject)

set(EXTERNAL_BUILD_DIR "${PROJECT_SOURCE_DIR}/build-external")

find_package(Threads REQUIRED)

# ===========================================================================
# rist-cpp  (includes librist via meson + ristnet wrapper)
# ===========================================================================

set(RIST_PREFIX "${EXTERNAL_BUILD_DIR}/rist-cpp")

ExternalProject_Add(
  external_rist_cpp
  SOURCE_DIR "${PROJECT_SOURCE_DIR}/external/rist-cpp"
  PREFIX     "${RIST_PREFIX}"
  INSTALL_DIR "${RIST_PREFIX}/install"

  CMAKE_ARGS
    -DCMAKE_INSTALL_PREFIX:PATH=<INSTALL_DIR>
    -DCMAKE_BUILD_TYPE:STRING=Release
    -DCMAKE_C_COMPILER:FILEPATH=${CMAKE_C_COMPILER}
    -DCMAKE_CXX_COMPILER:FILEPATH=${CMAKE_CXX_COMPILER}
    -DCMAKE_POSITION_INDEPENDENT_CODE:BOOL=ON
  BUILD_BYPRODUCTS
    "${RIST_PREFIX}/install/lib/librist.a"
    "${RIST_PREFIX}/install/lib/libristnet.a"
  BUILD_ALWAYS 0
)

ExternalProject_Get_Property(external_rist_cpp INSTALL_DIR)
set(RIST_INSTALL_DIR "${INSTALL_DIR}")

file(MAKE_DIRECTORY
  "${RIST_INSTALL_DIR}/include"
  "${RIST_INSTALL_DIR}/include/librist"
  "${RIST_INSTALL_DIR}/include/rist-cpp"
)

add_library(rist STATIC IMPORTED GLOBAL)
set_target_properties(rist PROPERTIES
  IMPORTED_LOCATION "${RIST_INSTALL_DIR}/lib/librist.a"
  INTERFACE_INCLUDE_DIRECTORIES
    "${RIST_INSTALL_DIR}/include;${RIST_INSTALL_DIR}/include/librist"
)
add_dependencies(rist external_rist_cpp)

add_library(ristnet STATIC IMPORTED GLOBAL)
set_target_properties(ristnet PROPERTIES
  IMPORTED_LOCATION "${RIST_INSTALL_DIR}/lib/libristnet.a"
  INTERFACE_INCLUDE_DIRECTORIES "${RIST_INSTALL_DIR}/include;${RIST_INSTALL_DIR}/include/rist-cpp"
  INTERFACE_LINK_LIBRARIES "rist;Threads::Threads"
)
add_dependencies(ristnet external_rist_cpp)
