execute_process(
    COMMAND "${PLAYER}" --project "${PROJECT}" --headless --frames 1 --size 8x8
    RESULT_VARIABLE result
    OUTPUT_FILE "${PROJECT}/startup.stdout.log"
    ERROR_FILE "${PROJECT}/startup.stderr.log"
    TIMEOUT 120)
file(READ "${PROJECT}/startup.stderr.log" error)
if(NOT result MATCHES "^[1-9][0-9]*$")
    message(FATAL_ERROR "CP1 startup must return nonzero, got '${result}': ${error}")
endif()
if(NOT error MATCHES "compute.descriptor_budget@1")
    message(FATAL_ERROR "CP1 startup stderr omitted descriptor refusal: ${error}")
endif()
