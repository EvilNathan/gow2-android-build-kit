cmake_minimum_required(VERSION 3.24)

# Thin wrapper so the kit never edits PS2Recomp's CMake. Everything here is
# additive: include paths, compile definitions, extra sources and the Android
# link options. The engine keeps building unchanged on desktop.
#
# Switches (all overridable from Gradle through -D...):
#   PS2X_FORCE_NULL_GS        no-op GS backend (M1 boot validation; default ON)
#   PS2X_GS_VK                compile android-runner/native/gs_vk into ps2_runtime
#   PS2X_ANDROID_NATIVE_VU1_DIR  directory with the generated native VU1 .cpp files
project(gow2_android LANGUAGES C CXX)

set(PS2X_FORCE_NULL_GS OFF CACHE BOOL "Install the no-op GS backend")
set(PS2X_GS_VK ON CACHE BOOL "Compile the raw Vulkan GS backend (native/gs_vk)")
set(PS2X_ANDROID_NATIVE_VU1_DIR "" CACHE PATH "Generated native VU1 sources (scripts/gen-native.ps1)")

set(GOW2_NATIVE_DIR "${CMAKE_CURRENT_LIST_DIR}")
set(GOW2_REPO_DIR "${GOW2_NATIVE_DIR}/../..")

add_subdirectory("${GOW2_REPO_DIR}/PS2Recomp" "${CMAKE_CURRENT_BINARY_DIR}/ps2recomp")

if(ANDROID)
    # ------------------------------------------------------------------ includes
    # compat/arm64 maps the x86 intrinsic header names used by the VU1/EE code.
    set(GOW2_COMPAT_DIR "${GOW2_REPO_DIR}/compat/arm64")

    foreach(target ps2_runtime ps2EntryRunner)
        if(TARGET ${target})
            target_include_directories(${target} PRIVATE "${GOW2_COMPAT_DIR}")
        endif()
    endforeach()

    if(TARGET ps2EntryRunner)
        # 16 KB page size, required by Android 15+ devices for new apps.
        target_link_options(ps2EntryRunner PRIVATE "-Wl,-z,max-page-size=16384")
        target_link_libraries(ps2EntryRunner PRIVATE android log)
    endif()

    # ------------------------------------------------------------- GS selection
    if(PS2X_FORCE_NULL_GS)
        foreach(target ps2_runtime ps2EntryRunner)
            if(TARGET ${target})
                target_compile_definitions(${target} PRIVATE PS2X_FORCE_NULL_GS=1)
            endif()
        endforeach()
    endif()

    if(PS2X_GS_VK AND EXISTS "${GOW2_NATIVE_DIR}/gs_vk/sources.cmake")
        set(GOW2_GS_VK_DIR "${GOW2_NATIVE_DIR}/gs_vk")
        set(GOW2_SPV_DIR "${CMAKE_CURRENT_BINARY_DIR}/gs_vk_spv")
        set(GOW2_SHADER_CPP "${CMAKE_CURRENT_BINARY_DIR}/gs_vk_shaders_generated.cpp")
        set(GOW2_SHADER_H "${CMAKE_CURRENT_BINARY_DIR}/gs_vk_shaders_generated.h")

        # GLSL -> SPIR-V with the glslc that ships in the NDK shader-tools package.
        if(EXISTS "${GOW2_GS_VK_DIR}/compile_shaders.cmake")
            if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
                set(GOW2_NDK_HOST_TAG "windows-x86_64")
            elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
                set(GOW2_NDK_HOST_TAG "darwin-x86_64")
            else()
                set(GOW2_NDK_HOST_TAG "linux-x86_64")
            endif()
            set(GOW2_GLSLC "${ANDROID_NDK}/shader-tools/${GOW2_NDK_HOST_TAG}/glslc${CMAKE_EXECUTABLE_SUFFIX}")
            if(EXISTS "${GOW2_GLSLC}")
                execute_process(
                    COMMAND "${CMAKE_COMMAND}" -DGLSLC=${GOW2_GLSLC}
                            -DSRC_DIR=${GOW2_GS_VK_DIR}/shaders -DOUT_DIR=${GOW2_SPV_DIR}
                            -P "${GOW2_GS_VK_DIR}/compile_shaders.cmake"
                    RESULT_VARIABLE gow2_glslc_result)
                if(NOT gow2_glslc_result EQUAL 0)
                    message(FATAL_ERROR "gs_vk: shader compilation failed (glslc ${GOW2_GLSLC})")
                endif()
            else()
                message(WARNING "gs_vk: glslc not found at ${GOW2_GLSLC}; expecting pre-built .spv files")
            endif()
        endif()

        # Embed the SPIR-V blobs so nothing has to be packaged as an asset.
        execute_process(
            COMMAND "${CMAKE_COMMAND}" -DSPV_DIR=${GOW2_SPV_DIR} -DOUT_CPP=${GOW2_SHADER_CPP} -DOUT_H=${GOW2_SHADER_H}
                    -P "${GOW2_REPO_DIR}/cmake/embed_spirv.cmake"
            RESULT_VARIABLE gow2_spv_result)
        if(NOT gow2_spv_result EQUAL 0)
            message(FATAL_ERROR "gs_vk: SPIR-V embedding failed")
        endif()

        include("${GOW2_GS_VK_DIR}/sources.cmake")
        if(TARGET ps2_runtime)
            target_sources(ps2_runtime PRIVATE "${GOW2_SHADER_CPP}")
            target_include_directories(ps2_runtime PUBLIC "${GOW2_GS_VK_DIR}" "${CMAKE_CURRENT_BINARY_DIR}")
            target_compile_definitions(ps2_runtime PUBLIC PS2X_ANDROID_VULKAN_GS=1)
            # The Vulkan loader is dlopen()ed, so no link dependency (and no API level requirement).
            target_compile_definitions(ps2_runtime PRIVATE VK_USE_PLATFORM_ANDROID_KHR=1)
        endif()
        if(TARGET ps2EntryRunner)
            target_compile_definitions(ps2EntryRunner PRIVATE PS2X_ANDROID_VULKAN_GS=1)
        endif()
    endif()

    # ------------------------------------------------------------------- host layer
    if(EXISTS "${GOW2_NATIVE_DIR}/host/sources.cmake")
        include("${GOW2_NATIVE_DIR}/host/sources.cmake")
    endif()

    # ------------------------------------------------------------------- ISO reader
    if(EXISTS "${GOW2_NATIVE_DIR}/iso/sources.cmake")
        include("${GOW2_NATIVE_DIR}/iso/sources.cmake")
    endif()

    # ------------------------------------------------------- native VU1 (host codegen)
    if(PS2X_ANDROID_NATIVE_VU1_DIR)
        file(GLOB GOW2_VU1N_PARTS CONFIGURE_DEPENDS "${PS2X_ANDROID_NATIVE_VU1_DIR}/*.cpp")
        if(GOW2_VU1N_PARTS AND TARGET ps2EntryRunner)
            target_sources(ps2EntryRunner PRIVATE ${GOW2_VU1N_PARTS})
            target_include_directories(ps2EntryRunner PRIVATE "${GOW2_REPO_DIR}/PS2Recomp/ps2xRuntime/include")
            message(STATUS "native VU1: ${GOW2_VU1N_PARTS}")
        else()
            message(WARNING "PS2X_ANDROID_NATIVE_VU1_DIR has no .cpp files: ${PS2X_ANDROID_NATIVE_VU1_DIR}")
        endif()
    endif()
endif()
