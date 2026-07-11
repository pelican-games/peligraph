if(NOT DEFINED CLI OR NOT DEFINED WRITER OR NOT DEFINED OUT_DIR)
    message(FATAL_ERROR "CLI, WRITER, and OUT_DIR are required")
endif()

file(REMOVE_RECURSE "${OUT_DIR}")
file(MAKE_DIRECTORY "${OUT_DIR}")
set(GLB "${OUT_DIR}/fixture.glb")
execute_process(COMMAND "${WRITER}" "${GLB}" RESULT_VARIABLE writer_result)
if(NOT writer_result EQUAL 0)
    message(FATAL_ERROR "failed to create glTF scene fixture")
endif()

foreach(run RANGE 1 2)
    execute_process(
        COMMAND "${CLI}" import gltf --extract-scene "${GLB}"
        RESULT_VARIABLE result
        OUTPUT_FILE "${OUT_DIR}/scene_${run}.json"
        ERROR_VARIABLE stderr
    )
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "gltf extract run ${run} failed (${result})\n${stderr}")
    endif()
endforeach()

file(SHA256 "${OUT_DIR}/scene_1.json" hash_1)
file(SHA256 "${OUT_DIR}/scene_2.json" hash_2)
if(NOT hash_1 STREQUAL hash_2)
    message(FATAL_ERROR "gltf extract output is not byte deterministic")
endif()
file(READ "${OUT_DIR}/scene_1.json" scene)
foreach(expected
        "\"schema\": \"pelican.scene\""
        "\"parent\": \"Root\""
        "#node/Root/MeshNode"
        "\"name\": \"camera\""
        "\"name\": \"light\"")
    string(FIND "${scene}" "${expected}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "extracted scene is missing ${expected}")
    endif()
endforeach()
