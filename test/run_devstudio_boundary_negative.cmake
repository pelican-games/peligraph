if(NOT DEFINED PELICAN_SOURCE_DIR OR NOT DEFINED TEST_BINARY_DIR)
  message(FATAL_ERROR "PELICAN_SOURCE_DIR and TEST_BINARY_DIR are required")
endif()

execute_process(
  COMMAND ${CMAKE_COMMAND}
    -S ${PELICAN_SOURCE_DIR}/test/fixtures/devstudio_boundary
    -B ${TEST_BINARY_DIR}
    -DPELICAN_SOURCE_DIR=${PELICAN_SOURCE_DIR}
  RESULT_VARIABLE configure_result
  OUTPUT_VARIABLE configure_output
  ERROR_VARIABLE configure_error
)

if(configure_result EQUAL 0)
  message(FATAL_ERROR
    "The D0 boundary fixture unexpectedly accepted a transitive pelican_core link")
endif()

set(combined_output "${configure_output}\n${configure_error}")
if(NOT combined_output MATCHES "D0 link boundary violation")
  message(FATAL_ERROR
    "The fixture failed for an unexpected reason:\n${combined_output}")
endif()

message(STATUS "D0 boundary rejected the transitive pelican_core link as expected")
