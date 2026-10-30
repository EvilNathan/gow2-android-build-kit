# Host sources for the God of War II Android port.
#
# Included by the Lead from android-runner/native/CMakeLists.txt, after the
# engine's add_subdirectory() created ps2EntryRunner:
#
#   include("${CMAKE_CURRENT_LIST_DIR}/host/sources.cmake")
#
# Paths are authored as "host/<file>" (relative to android-runner/native) but are
# resolved through CMAKE_CURRENT_LIST_DIR so the include works from any CMake
# directory.

if(NOT TARGET ps2EntryRunner)
    message(FATAL_ERROR "host/sources.cmake: ps2EntryRunner target is missing (include this after add_subdirectory(PS2Recomp))")
endif()

set(PS2X_HOST_DIR "${CMAKE_CURRENT_LIST_DIR}")

target_sources(ps2EntryRunner PRIVATE
    "${PS2X_HOST_DIR}/window.cpp"
    "${PS2X_HOST_DIR}/lifecycle.cpp"
    "${PS2X_HOST_DIR}/input.cpp"
    "${PS2X_HOST_DIR}/audio_android.cpp"
    "${PS2X_HOST_DIR}/log.cpp"
    "${PS2X_HOST_DIR}/crash.cpp"
    "${PS2X_HOST_DIR}/storage.cpp"
    "${PS2X_HOST_DIR}/disc.cpp"
    "${PS2X_HOST_DIR}/jni_bridge.cpp"
)

target_include_directories(ps2EntryRunner PRIVATE "${PS2X_HOST_DIR}")

# The engine patch (0004-android-host) includes ps2x_host.h from ps2_runtime
# (window/audio/pad/log/pacing), so that target needs the header directory too.
if(TARGET ps2_runtime)
    target_include_directories(ps2_runtime PRIVATE "${PS2X_HOST_DIR}")
endif()

# ISO9660 module (owned by the iso-tools teammate).  Added only when present so
# the host still compiles before that module lands; disc.cpp drops back to a
# reporting stub when iso9660.h cannot be found.
set(PS2X_ISO_DIR "${CMAKE_CURRENT_LIST_DIR}/../iso")
if(EXISTS "${PS2X_ISO_DIR}/iso9660.h")
    target_include_directories(ps2EntryRunner PRIVATE "${PS2X_ISO_DIR}")
    if(NOT DEFINED PS2X_ISO_SOURCES_ADDED)
        set(PS2X_ISO_SOURCES_ADDED TRUE)
        if(EXISTS "${PS2X_ISO_DIR}/sources.cmake")
            include("${PS2X_ISO_DIR}/sources.cmake")
        endif()
    endif()
endif()

target_link_libraries(ps2EntryRunner PRIVATE aaudio log android)
