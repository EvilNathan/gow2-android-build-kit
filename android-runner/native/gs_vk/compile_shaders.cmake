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
    # single-output build of draw.frag for devices without dualSrcBlend
    if(name STREQUAL "draw.frag")
        set(dst2 "${OUT_DIR}/draw_nodual.frag.spv")
        execute_process(
            COMMAND "${GLSLC}" -O "--target-env=vulkan1.1" -DKS_VK_NO_DUAL_SRC -o "${dst2}" "${src}"
            RESULT_VARIABLE rc2)
        if(NOT rc2 EQUAL 0)
            message(FATAL_ERROR "compile_shaders.cmake: ${GLSLC} failed on draw.frag (nodual) (exit ${rc2})")
        endif()
        message(STATUS "gs_vk shader: draw_nodual.frag -> ${dst2}")
    endif()
endforeach()
