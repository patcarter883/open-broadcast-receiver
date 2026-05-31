cmake_minimum_required(VERSION 3.10)
project(rist-cpp)
set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(WIN32)
set(CMAKE_C_STANDARD_REQUIRED True)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
endif()

# if(UNIX)
# find_package(GTest REQUIRED)
# endif()

if(WIN32)
# Set compiler options.
set(variables
	CMAKE_C_FLAGS_DEBUG
	CMAKE_C_FLAGS_MINSIZEREL
	CMAKE_C_FLAGS_RELEASE
	CMAKE_C_FLAGS_RELWITHDEBINFO
	CMAKE_CXX_FLAGS_DEBUG
	CMAKE_CXX_FLAGS_MINSIZEREL
	CMAKE_CXX_FLAGS_RELEASE
	CMAKE_CXX_FLAGS_RELWITHDEBINFO
)

foreach(variable ${variables})
	if(${variable} MATCHES "/MD")
		string(REGEX REPLACE "/MD" "/MT" ${variable} "${${variable}}")
	endif()
endforeach()
endif()

#If no build type is set then force Release
IF( NOT CMAKE_BUILD_TYPE )
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING
            "Choose the type of build, options are: None Debug Release RelWithDebInfo MinSizeRel."
            FORCE)
ENDIF()

set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -DDEBUG")
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_package (Threads REQUIRED)
include(ExternalProject)

if(WIN32)
set(BUILD_DIR "${CMAKE_CURRENT_BINARY_DIR}/rist-build")
set(BUILD_CMD cd ${BUILD_DIR} && meson -Dbuiltin_mbedtls=true -Dbuiltin_cjson=true -Dbuiltin_lz4=true --default-library=static -Db_vscrt=mt ${CMAKE_CURRENT_SOURCE_DIR}/rist && ninja)
ExternalProject_Add(project_rist
        CONFIGURE_COMMAND ""
        GIT_REPOSITORY https://code.videolan.org/rist/librist.git
        GIT_TAG origin/master
        SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/rist
        BINARY_DIR ${BUILD_DIR}
        BUILD_COMMAND ${BUILD_CMD}
        GIT_PROGRESS 1
        STEP_TARGETS build
        EXCLUDE_FROM_ALL TRUE
        INSTALL_COMMAND ""
        BUILD_BYPRODUCTS ${BUILD_DIR}/librist.a
        )
else()
ExternalProject_Add(project_rist
        CONFIGURE_COMMAND ""
        GIT_REPOSITORY https://code.videolan.org/rist/librist.git
        GIT_TAG origin/master
        SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/rist
        BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR}/rist-build
        BUILD_COMMAND mkdir -p ${CMAKE_CURRENT_BINARY_DIR}/rist-build COMMAND cd ${CMAKE_CURRENT_BINARY_DIR}/rist-build COMMAND meson -Dbuiltin_mbedtls=true -Dbuiltin_cjson=true -Dbuiltin_lz4=true --default-library=static ${CMAKE_CURRENT_SOURCE_DIR}/rist COMMAND ninja
        GIT_PROGRESS 1
        STEP_TARGETS build
        EXCLUDE_FROM_ALL TRUE
        INSTALL_COMMAND ""
        BUILD_BYPRODUCTS ${CMAKE_CURRENT_BINARY_DIR}/rist-build/librist.a
        )
endif()

add_library(rist STATIC IMPORTED)
set_property(TARGET rist PROPERTY IMPORTED_LOCATION ${CMAKE_CURRENT_BINARY_DIR}/rist-build/librist.a)
add_dependencies(rist project_rist)

set_source_files_properties( ${CMAKE_CURRENT_SOURCE_DIR}/rist/contrib/lz4/lz4.c PROPERTIES GENERATED TRUE)
set_source_files_properties( ${CMAKE_CURRENT_SOURCE_DIR}/rist/contrib/lz4/lz4frame.c PROPERTIES GENERATED TRUE)
set_source_files_properties( ${CMAKE_CURRENT_SOURCE_DIR}/rist/contrib/lz4/lz4hc.c PROPERTIES GENERATED TRUE)
set_source_files_properties( ${CMAKE_CURRENT_SOURCE_DIR}/rist/contrib/lz4/xxhash.c PROPERTIES GENERATED TRUE)

add_library(ristnet STATIC
        RISTNet.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/rist/contrib/lz4/lz4.c
        ${CMAKE_CURRENT_SOURCE_DIR}/rist/contrib/lz4/lz4frame.c
        ${CMAKE_CURRENT_SOURCE_DIR}/rist/contrib/lz4/lz4hc.c
        ${CMAKE_CURRENT_SOURCE_DIR}/rist/contrib/lz4/xxhash.c
        )
target_link_libraries(ristnet PUBLIC ${warning_guard} rist)
target_link_libraries(ristnet PRIVATE Threads::Threads)

target_include_directories(ristnet ${warning_guard} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(ristnet ${warning_guard} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/rist/include/)
target_include_directories(ristnet ${warning_guard} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/rist/include/librist)
target_include_directories(ristnet ${warning_guard} PUBLIC ${CMAKE_CURRENT_BINARY_DIR}/rist-build/include/librist)



IF (WIN32)
        target_compile_definitions(ristnet PRIVATE _CRT_SECURE_NO_WARNINGS)
        target_link_libraries(ristnet PRIVATE iphlpapi wsock32 ws2_32)
ENDIF()

# ---- Install rules ----

include(GNUInstallDirs)

install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/rist-build/librist.a
        DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

install(DIRECTORY
        ${CMAKE_CURRENT_SOURCE_DIR}/rist/include/librist
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(DIRECTORY
        ${CMAKE_CURRENT_BINARY_DIR}/rist-build/include/librist
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        FILES_MATCHING PATTERN "*.h"
)

install(TARGETS ristnet
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

install(FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/RISTNet.h
        ${CMAKE_CURRENT_SOURCE_DIR}/RISTNetInternal.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/rist-cpp
)
