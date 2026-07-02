if(NOT DEFINED PLAYER)
    message(FATAL_ERROR "PLAYER is required")
endif()
if(NOT DEFINED PLAYER_DIR)
    message(FATAL_ERROR "PLAYER_DIR is required")
endif()
if(NOT DEFINED PROJECT_DIR)
    message(FATAL_ERROR "PROJECT_DIR is required")
endif()
if(NOT DEFINED SEQ_FILE)
    message(FATAL_ERROR "SEQ_FILE is required")
endif()
if(NOT DEFINED OUT_DIR)
    message(FATAL_ERROR "OUT_DIR is required")
endif()

file(REMOVE_RECURSE "${OUT_DIR}")
file(MAKE_DIRECTORY "${OUT_DIR}")

execute_process(
    COMMAND "${PLAYER}"
        --headless
        --project "${PROJECT_DIR}"
        --frames 3
        --size 160x90
        --fps 30
        --play-seq "${SEQ_FILE}"
        --seq-mesh builtin:sphere
        --camera "0,1,3,0,0.5,0,40"
        --render-out "${OUT_DIR}/%04d.png"
    WORKING_DIRECTORY "${PLAYER_DIR}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
)

if(NOT result EQUAL 0)
    message(FATAL_ERROR "seqplayer headless run failed with ${result}\nstdout:\n${stdout}\nstderr:\n${stderr}")
endif()

foreach(frame 0001 0002 0003)
    set(path "${OUT_DIR}/${frame}.png")
    if(NOT EXISTS "${path}")
        message(FATAL_ERROR "seqplayer headless output missing: ${path}")
    endif()
    file(SIZE "${path}" size)
    if(size EQUAL 0)
        message(FATAL_ERROR "seqplayer headless output is empty: ${path}")
    endif()
endforeach()

if(stdout MATCHES "Validation Error|VUID-" OR stderr MATCHES "Validation Error|VUID-")
    message(FATAL_ERROR "seqplayer headless run emitted Vulkan validation errors\nstdout:\n${stdout}\nstderr:\n${stderr}")
endif()
