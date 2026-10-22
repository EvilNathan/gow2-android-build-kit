cmake_minimum_required(VERSION 3.24)

project(gow2_android C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(
    "${CMAKE_CURRENT_LIST_DIR}/../../../engine/third_party/SDL3-3.4.18"
    "${CMAKE_BINARY_DIR}/SDL3"
    EXCLUDE_FROM_ALL)

set(PS2_GENERATED "${CMAKE_CURRENT_LIST_DIR}/../../../generated/gow2" CACHE PATH "Generated game code")
set(PS2_WGPU_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../engine/third_party/wgpu-native-android" CACHE PATH "Android wgpu-native")

add_subdirectory(
    "${CMAKE_CURRENT_LIST_DIR}/../../../engine"
    "${CMAKE_BINARY_DIR}/engine")

# Rootless Adreno driver replacement (turnip / patched Qualcomm blobs).
# Source is fetched by scripts/setup.ps1; without it the build stays on the
# stock Vulkan driver and Mali devices are unaffected.
set(PS2_ADRENOTOOLS_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../engine/third_party/libadrenotools")
if(ANDROID AND TARGET main)
    # Always compile the bridge: without adrenotools it forwards dlopen()
    # straight to the system loader, and ps2_android_vulkan_prepare() is a no-op.
    target_sources(main PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../../engine/host/android_vulkan_loader.cpp")
    # Make wgpu's dlopen("libvulkan.so.1") bind to the interposer in
    # android_vulkan_loader.cpp instead of going through libdl directly.
    target_link_options(main PRIVATE "-Wl,-Bsymbolic-functions")
endif()
if(ANDROID AND EXISTS "${PS2_ADRENOTOOLS_DIR}/CMakeLists.txt" AND TARGET main)
    add_subdirectory("${PS2_ADRENOTOOLS_DIR}" "${CMAKE_BINARY_DIR}/adrenotools")
    if(TARGET adrenotools)
        target_compile_definitions(main PRIVATE PS2_ADRENOTOOLS=1)
        target_link_libraries(main PRIVATE adrenotools)
        add_dependencies(main hook_impl main_hook file_redirect_hook gsl_alloc_hook)
    endif()
endif()

if(TARGET main)
    # Android 15+ devices may use 16 KB pages. Keep both the LOAD segments and
    # the compressed-lib alignment compatible with those devices.
    target_link_options(main PRIVATE
        "-Wl,-z,max-page-size=16384"
        "-Wl,-z,common-page-size=16384")
    # libadrenotools builds the hook .so files itself, so apply the same 16 KB
    # page contract to those targets after add_subdirectory().
    foreach(ps2_hook hook_impl main_hook file_redirect_hook gsl_alloc_hook)
        if(TARGET ${ps2_hook})
            target_link_options(${ps2_hook} PRIVATE
                "-Wl,-z,max-page-size=16384"
                "-Wl,-z,common-page-size=16384")
        endif()
    endforeach()
endif()
