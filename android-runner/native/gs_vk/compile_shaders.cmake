# Compiles shaders/*.vert|frag to SPIR-V with the NDK glslc.
#   cmake -DGLSLC=<glslc> -DSRC_DIR=<shaders> -DOUT_DIR=<spv> -P compile_shaders.cmake
file(GLOB sources LIST_DIRECTORIES FALSE "${SRC_DIR}/*.vert" "${SRC_DIR}/*.frag")
if(NOT sources)
    message(FATAL_ERROR "compile_shaders.cmake: no shaders in ${SRC_DIR}")
endif()
file(MAKE_DIRECTORY "${OUT_DIR}")
foreach(src ${sources})
    get_filename_component(name "${src}" NAME)
    set(dst "${OUT_DIR}/${name}.spv")
    execute_process(
        COMMAND "${GLSLC}" -O "--target-env=vulkan1.1" -o "${dst}" "${src}"
        RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "compile_shaders.cmake: ${GLSLC} failed on ${src} (exit ${rc})")
    endif()
    message(STATUS "gs_vk shader: ${name} -> ${dst}")
endforeach()
