execute_process(
    COMMAND "${CLI}" dump-lowered-material "${SURFACE}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "dump-lowered-material failed (${result}): ${error}")
endif()
foreach(expected
        "values: std140 size=64"
        "binding=6 type=storage_buffer name=MaterialBuffer"
        "binding=7 type=combined_image_sampler name=albedo_detail view=SRGB"
        "render_state: blend=additive cull=none")
    string(FIND "${output}" "${expected}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "dump output is missing '${expected}':\n${output}")
    endif()
endforeach()
