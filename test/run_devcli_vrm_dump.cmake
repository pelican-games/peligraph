if(NOT DEFINED CLI OR NOT DEFINED WRITER OR NOT DEFINED EXPECTED OR NOT DEFINED OUT_DIR)
    message(FATAL_ERROR "CLI, WRITER, EXPECTED, and OUT_DIR are required")
endif()

file(REMOVE_RECURSE "${OUT_DIR}")
file(MAKE_DIRECTORY "${OUT_DIR}")

execute_process(
    COMMAND "${WRITER}" full "${OUT_DIR}/full.vrm"
    RESULT_VARIABLE writer_result
    OUTPUT_VARIABLE writer_stdout
    ERROR_VARIABLE writer_stderr
)
if(NOT writer_result EQUAL 0)
    message(FATAL_ERROR "VRM fixture writer failed\n${writer_stdout}\n${writer_stderr}")
endif()

foreach(run IN ITEMS first second)
    execute_process(
        COMMAND "${CLI}" vrm dump "${OUT_DIR}/full.vrm"
        RESULT_VARIABLE dump_result
        OUTPUT_FILE "${OUT_DIR}/${run}.json"
        ERROR_VARIABLE dump_stderr
    )
    if(NOT dump_result EQUAL 0)
        message(FATAL_ERROR "vrm dump ${run} failed\n${dump_stderr}")
    endif()
    if(NOT dump_stderr STREQUAL "")
        message(FATAL_ERROR "valid vrm dump ${run} emitted diagnostics\n${dump_stderr}")
    endif()
endforeach()

execute_process(COMMAND "${CMAKE_COMMAND}" -E compare_files
    "${OUT_DIR}/first.json" "${OUT_DIR}/second.json"
    RESULT_VARIABLE repeat_compare)
if(NOT repeat_compare EQUAL 0)
    message(FATAL_ERROR "vrm dump is not deterministic")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" -E compare_files
    "${OUT_DIR}/first.json" "${EXPECTED}"
    RESULT_VARIABLE canonical_compare)
if(NOT canonical_compare EQUAL 0)
    message(FATAL_ERROR "vrm dump differs from canonical fixture")
endif()

execute_process(
    COMMAND "${WRITER}" vrm0 "${OUT_DIR}/vrm0.vrm"
    RESULT_VARIABLE vrm0_writer_result
)
if(NOT vrm0_writer_result EQUAL 0)
    message(FATAL_ERROR "VRM 0.x fixture writer failed")
endif()
execute_process(
    COMMAND "${CLI}" vrm dump "${OUT_DIR}/vrm0.vrm"
    RESULT_VARIABLE vrm0_result
    OUTPUT_VARIABLE vrm0_stdout
    ERROR_VARIABLE vrm0_stderr
)
if(NOT vrm0_result EQUAL 0 OR NOT vrm0_stdout STREQUAL "null\n")
    message(FATAL_ERROR "VRM 0.x did not remain semantic-free\n${vrm0_stdout}\n${vrm0_stderr}")
endif()
string(REGEX MATCHALL "INFO:.*VRM 0.x semantic is unsupported" vrm0_info "${vrm0_stderr}")
list(LENGTH vrm0_info vrm0_info_count)
if(NOT vrm0_info_count EQUAL 1)
    message(FATAL_ERROR "VRM 0.x did not emit exactly one INFO diagnostic\n${vrm0_stderr}")
endif()

if(DEFINED ALICIA AND EXISTS "${ALICIA}")
    execute_process(
        COMMAND "${CLI}" vrm dump "${ALICIA}"
        RESULT_VARIABLE alicia_result
        OUTPUT_VARIABLE alicia_stdout
        ERROR_VARIABLE alicia_stderr
    )
    if(NOT alicia_result EQUAL 0 OR NOT alicia_stdout STREQUAL "null\n")
        message(FATAL_ERROR "AliciaSolid.vrm did not remain semantic-free\n${alicia_stdout}\n${alicia_stderr}")
    endif()
    string(REGEX MATCHALL "INFO:.*VRM 0.x semantic is unsupported" alicia_info "${alicia_stderr}")
    list(LENGTH alicia_info alicia_info_count)
    if(NOT alicia_info_count EQUAL 1)
        message(FATAL_ERROR "AliciaSolid.vrm did not emit exactly one INFO diagnostic\n${alicia_stderr}")
    endif()
endif()
