cmake_minimum_required(VERSION 3.24)

# Thin wrapper so the kit never edits PS2Recomp's CMake. It adds:
#   compat/arm64    x86 intrinsics header names for the files that include them unconditionally
#   PS2X_FORCE_NULL_GS  no-op GS backend used by M1 (boot validation without a renderer)
project(gow2_android LANGUAGES C CXX)

set(PS2X_FORCE_NULL_GS OFF CACHE BOOL "Install the no-op GS backend")

add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../../PS2Recomp" "${CMAKE_CURRENT_BINARY_DIR}/ps2recomp")

if(ANDROID)
    set(GOW2_COMPAT_DIR "${CMAKE_CURRENT_LIST_DIR}/../../compat/arm64")

    if(TARGET ps2_runtime)
        target_include_directories(ps2_runtime PUBLIC "${GOW2_COMPAT_DIR}")
    endif()

    if(TARGET ps2EntryRunner)
        target_include_directories(ps2EntryRunner PRIVATE "${GOW2_COMPAT_DIR}")
        # 16 KB page size, required by Android 15+ devices for new apps.
        target_link_options(ps2EntryRunner PRIVATE "-Wl,-z,max-page-size=16384")
    endif()

    if(PS2X_FORCE_NULL_GS)
        if(TARGET ps2_runtime)
            target_compile_definitions(ps2_runtime PRIVATE PS2X_FORCE_NULL_GS=1)
        endif()
        if(TARGET ps2EntryRunner)
            target_compile_definitions(ps2EntryRunner PRIVATE PS2X_FORCE_NULL_GS=1)
        endif()
    endif()
endif()
