set(args dump-lowered-material "${SURFACE}")
if(DEFINED MATERIAL)
    list(APPEND args "${MATERIAL}")
endif()
execute_process(
    COMMAND "${CLI}" ${args}
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "dump-lowered-material failed (${result}): ${error}")
endif()
if(DEFINED MATERIAL)
    set(expectations
        "material: imported_mask"
        "alpha_cutoff type=float"
        "name=opacity_map view=UNORM default=project://textures/leaf_opacity.png"
        "target_pass: forward_opaque"
        "routing: alpha_mode=mask double_sided=true variant=mask_double_sided discard=alpha<alpha_cutoff")
else()
    set(expectations
        "values: std140 size=64"
        "binding=6 type=storage_buffer name=MaterialBuffer"
        "binding=7 type=combined_image_sampler name=albedo_detail view=SRGB"
        "render_state: blend=additive cull=none")
endif()
foreach(expected IN LISTS expectations)
    string(FIND "${output}" "${expected}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "dump output is missing '${expected}':\n${output}")
    endif()
endforeach()
