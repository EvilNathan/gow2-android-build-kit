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

if(TARGET main)
    # Android 15+ devices may use 16 KB pages. Keep both the LOAD segments and
    # the compressed-lib alignment compatible with those devices.
    target_link_options(main PRIVATE
        "-Wl,-z,max-page-size=16384"
        "-Wl,-z,common-page-size=16384")
endif()
