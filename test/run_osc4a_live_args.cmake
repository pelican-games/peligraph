if(NOT DEFINED PLAYER)
    message(FATAL_ERROR "PLAYER is required")
endif()
if(NOT DEFINED PLAYER_DIR)
    message(FATAL_ERROR "PLAYER_DIR is required")
endif()
if(NOT DEFINED OUT_DIR)
    message(FATAL_ERROR "OUT_DIR is required")
endif()

file(REMOVE_RECURSE "${OUT_DIR}")
file(MAKE_DIRECTORY "${OUT_DIR}")
file(WRITE "${OUT_DIR}/replay.live_seq" "reserved for the command-line exclusion\n")
execute_process(
    COMMAND "${PLAYER}" --headless --rpc
        --record-live "${OUT_DIR}/record.live_seq"
        --replay-live "${OUT_DIR}/replay.live_seq"
    WORKING_DIRECTORY "${PLAYER_DIR}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
    TIMEOUT 30
)
if(result EQUAL 0)
    message(FATAL_ERROR "--record-live with --replay-live unexpectedly succeeded")
endif()
set(output "${stdout}\n${stderr}")
if(NOT output MATCHES "live_input[.]record_while_replaying@1")
    message(FATAL_ERROR "the combined live record/replay refusal was not named:\n${output}")
endif()
