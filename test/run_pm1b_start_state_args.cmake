# PM-1b: `pelican_player --start-state edit` starts an engine that waits in the
# edit state for a client to start the game. Everything that drives a run from
# its first frame is refused with it, by name, in the shape --start-time uses
# for its own conflicts (offline.mode_conflict@1). All of these are decided
# while the command line is read, so none of the runs reaches the GPU.

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
# --replay names a file that has to exist before its conflict is looked at.
file(WRITE "${OUT_DIR}/replay.jsonl" "{}\n")

function(expect_refusal label pattern)
    execute_process(
        COMMAND "${PLAYER}" ${ARGN}
        WORKING_DIRECTORY "${PLAYER_DIR}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE stdout
        ERROR_VARIABLE stderr
        TIMEOUT 60
    )
    if(result EQUAL 0)
        message(FATAL_ERROR "${label} was accepted\nstdout:\n${stdout}\nstderr:\n${stderr}")
    endif()
    if(NOT stderr MATCHES "${pattern}")
        message(FATAL_ERROR "${label} was not refused by name (expected /${pattern}/):\n${stderr}")
    endif()
endfunction()

set(conflict "offline\\.mode_conflict@1: --start-state edit cannot be combined with")
expect_refusal("edit with --replay" "${conflict} --replay"
    --start-state edit --headless --rpc --replay "${OUT_DIR}/replay.jsonl")
expect_refusal("edit with --record-input" "${conflict} --record-input"
    --start-state edit --record-input "${OUT_DIR}/record.jsonl")
expect_refusal("edit with --bake-camera-output" "${conflict} --bake-camera-output"
    --start-state edit --headless --replay "${OUT_DIR}/replay.jsonl"
    --bake-camera-output "${OUT_DIR}/bake.jsonl")
expect_refusal("edit with --start-time" "${conflict} --start-time"
    --start-state edit --headless --rpc --start-time 1.0)
expect_refusal("edit headless without --rpc" "${conflict} --headless without --rpc"
    --start-state edit --headless)
# A value that is not a state is an argument error, not a mode conflict.
expect_refusal("an unknown state" "--start-state must be one of: edit, playing"
    --start-state stopped)
