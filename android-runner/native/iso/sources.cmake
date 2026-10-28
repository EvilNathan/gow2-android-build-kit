# ISO9660 reader + SHA1 for the boot-ELF extraction (part of ps2_runtime).
#
# Included from android-runner/native/CMakeLists.txt after add_subdirectory()
# has created the engine targets. The Lead wires this file into ps2_runtime so
# the JNI host layer and the app can call ps2x::iso::ExtractBootDataFromFd().
target_sources(ps2_runtime PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/iso9660.cpp
    ${CMAKE_CURRENT_LIST_DIR}/sha1.cpp)

# Consumers include "iso/iso9660.h" from the native root (android-runner/native).
target_include_directories(ps2_runtime PUBLIC "${CMAKE_CURRENT_LIST_DIR}/..")
