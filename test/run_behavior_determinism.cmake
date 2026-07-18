if(NOT DEFINED PROBE)
  message(FATAL_ERROR "PROBE is required")
endif()

function(run_probe label)
  execute_process(
    COMMAND "${PROBE}" ${ARGN}
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
  )
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "${label} failed with ${result}: ${error}")
  endif()
  set("${label}_OUTPUT" "${output}" PARENT_SCOPE)
endfunction()

run_probe(fresh_1)
run_probe(fresh_2)
run_probe(replay_1 --replay)
run_probe(replay_2 --replay)

if(NOT fresh_1_OUTPUT STREQUAL fresh_2_OUTPUT OR
   NOT fresh_1_OUTPUT STREQUAL replay_1_OUTPUT OR
   NOT fresh_1_OUTPUT STREQUAL replay_2_OUTPUT)
  message(FATAL_ERROR
    "behavior trace/RNG bytes differ across two fresh and two replay processes\n"
    "fresh_1:\n${fresh_1_OUTPUT}\n"
    "fresh_2:\n${fresh_2_OUTPUT}\n"
    "replay_1:\n${replay_1_OUTPUT}\n"
    "replay_2:\n${replay_2_OUTPUT}")
endif()

if(fresh_1_OUTPUT STREQUAL "")
  message(FATAL_ERROR "behavior determinism probe produced no trace")
endif()
