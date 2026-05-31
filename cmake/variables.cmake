# ---- Developer mode ----

# Developer mode enables targets and code paths in the CMake scripts that are
# only relevant for the developer(s) of open-broadcast-receiver.
if(PROJECT_IS_TOP_LEVEL)
  option(open-broadcast-receiver_DEVELOPER_MODE "Enable developer mode" OFF)
endif()

# ---- Warning guard ----

# target_include_directories with the SYSTEM modifier will request the compiler
# to omit warnings from the provided paths, if the compiler supports that.
set(warning_guard "")
if(NOT PROJECT_IS_TOP_LEVEL)
  option(
      open-broadcast-receiver_INCLUDES_WITH_SYSTEM
      "Use SYSTEM modifier for open-broadcast-receiver's includes, disabling warnings"
      ON
  )
  mark_as_advanced(open-broadcast-receiver_INCLUDES_WITH_SYSTEM)
  if(open-broadcast-receiver_INCLUDES_WITH_SYSTEM)
    set(warning_guard SYSTEM)
  endif()
endif()
