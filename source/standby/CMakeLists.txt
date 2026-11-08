cmake_minimum_required(VERSION 3.28)

project(
    open-broadcast-receiver-standby
    LANGUAGES CXX
)

# Standby slate (plan B1/B2): one-shot ES renderer + the loop that replays it.
#
# The AU splitter in es_looper.h is pure, so the fast tests-only lane builds the
# headers alone -- es_looper_test then proves the splice boundaries without a
# media stack. Only the full lane compiles the GStreamer-facing parts.
if(OBR_TESTS_ONLY)
    # Only the pure half of es_looper.cpp compiles here: the replay class is
    # inside #ifdef OBR_HAVE_GSTREAMER, so no media stack is needed.
    add_library(standby_lib STATIC)

    target_sources(standby_lib ${warning_guard}
        PRIVATE
            es_looper.h
            es_looper.cpp
            stream_switch.h
            stream_switch.cpp
    )

    target_include_directories(standby_lib PUBLIC
        ${CMAKE_SOURCE_DIR}/source
    )

    target_compile_features(standby_lib PUBLIC cxx_std_20)
    return()
endif()

add_library(standby_lib STATIC)

target_sources(standby_lib ${warning_guard}
    PRIVATE
        render.h
        render.cpp
        es_looper.h
        es_looper.cpp
        stream_switch.h
        stream_switch.cpp
)

target_include_directories(standby_lib PUBLIC
    ${CMAKE_SOURCE_DIR}/source
)

# The pure half of es_looper.h (the AU splitter) is always available; the
# replays-into-an-appsrc class is only declared when GStreamer is (see the
# header). Defining this PUBLIC keeps the two halves consistent for every
# consumer of the library.
target_compile_definitions(standby_lib PUBLIC OBR_HAVE_GSTREAMER)

target_compile_features(standby_lib PUBLIC cxx_std_20)

# GStreamer is a PUBLIC dependency, not a private one: es_looper.h includes
# gst/gst.h when OBR_HAVE_GSTREAMER is defined, so every consumer of the header
# needs both the definition and the include path.
target_link_libraries(standby_lib PUBLIC
    receiver_lib
    PkgConfig::gstreamer
    PkgConfig::gstreamer-app
)

target_include_directories(standby_lib PUBLIC SYSTEM
    ${GSTREAMER_INCLUDE_DIRS}
)

# The shared restartable stage (plan C4): one demux, two selectors, one
# re-stamped timeline, and the ES rings every output consumes. GStreamer-only,
# so it exists only in the full lane; the tests-only lane stays pure.
target_sources(standby_lib ${warning_guard}
    PRIVATE
        stage.h
        stage.cpp
)

# stage.cpp publishes into es_ring and reads the session's ts_ring; both live in
# ring_lib.
target_link_libraries(standby_lib PUBLIC ring_lib)

# The re-stamper the stage's probes drive. It is pure arithmetic, but the stage
# is its only runtime caller, so it joins the library in the full lane.
target_sources(standby_lib ${warning_guard}
    PRIVATE
        timeline.h
        timeline.cpp
)

# The mode controller (plan D1/D3): WHEN the session is on the slate. Pure C++
# (no GStreamer) but it joins the library so the receiver's /start path and
# /status read one implementation. controller_test compiles the .cpp directly,
# so the tests-only lane needs no extra target.
target_sources(standby_lib ${warning_guard}
    PRIVATE
        controller.h
        controller.cpp
)

# The receiver executable builds the shared stage on /start (plan D2/D3) and so
# needs these symbols. The executable target is declared in the root
# CMakeLists; fanout_lib is the library every receiver binary already links, so
# attaching the standby library to it PUBLIC is how main.cpp's `standby/...`
# includes resolve at link time without a second place to keep in step.
target_link_libraries(fanout_lib PUBLIC standby_lib)
