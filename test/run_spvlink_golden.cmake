if(NOT DEFINED GLSLANG OR NOT DEFINED LINKER OR NOT DEFINED SOURCE_DIR OR
   NOT DEFINED OUT_DIR OR NOT DEFINED TARGET_ENV)
    message(FATAL_ERROR
        "spvlink golden requires GLSLANG, LINKER, SOURCE_DIR, OUT_DIR, and TARGET_ENV")
endif()

file(REMOVE_RECURSE "${OUT_DIR}")
file(MAKE_DIRECTORY "${OUT_DIR}")

execute_process(
    COMMAND "${GLSLANG}" -V --target-env "${TARGET_ENV}" -S frag
        "${SOURCE_DIR}/experiments/spvlink/shaders/template.frag.glsl"
        -o "${OUT_DIR}/template.spv"
    RESULT_VARIABLE template_result
)
if(NOT template_result EQUAL 0)
    message(FATAL_ERROR "glslang template compilation failed: ${template_result}")
endif()

# --keep-uncalled is the production convention for a GLSL library module.
execute_process(
    COMMAND "${GLSLANG}" -V --target-env "${TARGET_ENV}" -S frag --keep-uncalled
        -DPELICAN_VARIANT_WARM
        "${SOURCE_DIR}/experiments/spvlink/shaders/surface.glsl"
        -o "${OUT_DIR}/user.spv"
    RESULT_VARIABLE user_result
)
if(NOT user_result EQUAL 0)
    message(FATAL_ERROR "glslang user compilation failed: ${user_result}")
endif()

execute_process(
    COMMAND "${LINKER}"
        --template "${OUT_DIR}/template.spv"
        --user "${OUT_DIR}/user.spv"
        --user-export pelican_surface
        --cache-salt PELICAN_VARIANT_WARM
        --output "${OUT_DIR}/final.spv"
        --bindings "${OUT_DIR}/bindings.json"
    RESULT_VARIABLE link_result
)
if(NOT link_result EQUAL 0)
    message(FATAL_ERROR "pelican-spv-link failed: ${link_result}")
endif()

if(DEFINED SPIRV_VAL AND EXISTS "${SPIRV_VAL}")
    execute_process(
        COMMAND "${SPIRV_VAL}" --target-env "${TARGET_ENV}" "${OUT_DIR}/final.spv"
        RESULT_VARIABLE val_result
    )
    if(NOT val_result EQUAL 0)
        message(FATAL_ERROR "external spirv-val rejected linked golden: ${val_result}")
    endif()
endif()

file(SHA256 "${OUT_DIR}/final.spv" actual_hash)
file(READ "${SOURCE_DIR}/test/fixtures/spvlink/linked_golden.sha256" expected_hash)
string(STRIP "${expected_hash}" expected_hash)
if(NOT actual_hash STREQUAL expected_hash)
    message(FATAL_ERROR "linked SPIR-V golden mismatch: expected ${expected_hash}, got ${actual_hash}")
endif()

file(READ "${OUT_DIR}/bindings.json" bindings_json)
string(JSON descriptor_type GET "${bindings_json}" bindings 0 descriptor_type)
string(JSON remapped_binding GET "${bindings_json}" bindings 0 remapped binding)
if(NOT descriptor_type STREQUAL "combined_image_sampler" OR NOT remapped_binding EQUAL 8)
    message(FATAL_ERROR "binding table golden mismatch")
endif()
