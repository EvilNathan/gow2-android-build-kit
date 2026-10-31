# gs_vk sources compiled into ps2_runtime on Android (see ../../native/CMakeLists.txt).
target_sources(ps2_runtime PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/gs_vk_backend.cpp
    ${CMAKE_CURRENT_LIST_DIR}/gs_vk_loader.cpp
)
target_include_directories(ps2_runtime PRIVATE ${CMAKE_CURRENT_LIST_DIR})
