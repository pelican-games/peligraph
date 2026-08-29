if(NOT DEFINED PELICAN_SOURCE_DIR OR NOT DEFINED TEST_BINARY_DIR OR
   NOT DEFINED NLOHMANN_INCLUDE_DIR)
  message(FATAL_ERROR
    "PELICAN_SOURCE_DIR, TEST_BINARY_DIR, and NLOHMANN_INCLUDE_DIR are required")
endif()

file(REMOVE_RECURSE "${TEST_BINARY_DIR}")
set(configure_command
  "${CMAKE_COMMAND}"
  -S "${PELICAN_SOURCE_DIR}/test/fixtures/wp360_raw_authority"
  -B "${TEST_BINARY_DIR}"
  -G "${GENERATOR}"
  "-DPELICAN_SOURCE_DIR=${PELICAN_SOURCE_DIR}"
  "-DNLOHMANN_INCLUDE_DIR=${NLOHMANN_INCLUDE_DIR}"
)
if(DEFINED GENERATOR_PLATFORM AND NOT GENERATOR_PLATFORM STREQUAL "")
  list(APPEND configure_command -A "${GENERATOR_PLATFORM}")
endif()
if(DEFINED GENERATOR_TOOLSET AND NOT GENERATOR_TOOLSET STREQUAL "")
  list(APPEND configure_command -T "${GENERATOR_TOOLSET}")
endif()

execute_process(
  COMMAND ${configure_command}
  RESULT_VARIABLE configure_result
  OUTPUT_VARIABLE configure_output
  ERROR_VARIABLE configure_error)
if(NOT configure_result EQUAL 0)
  message(FATAL_ERROR
    "WP360 raw authority fixture configure failed:\n${configure_output}\n${configure_error}")
endif()

execute_process(
  COMMAND ${CMAKE_COMMAND} --build ${TEST_BINARY_DIR}
          --config ${TEST_CONFIG}
          --target wp360_raw_authority_allowed
  RESULT_VARIABLE allowed_result
  OUTPUT_VARIABLE allowed_output
  ERROR_VARIABLE allowed_error)
if(NOT allowed_result EQUAL 0)
  message(FATAL_ERROR
    "WP360 allowed authoring authority target failed:\n${allowed_output}\n${allowed_error}")
endif()

foreach(reader IN ITEMS camera reload query)
  execute_process(
    COMMAND ${CMAKE_COMMAND} --build ${TEST_BINARY_DIR}
            --config ${TEST_CONFIG}
            --target wp360_raw_authority_rejected_${reader}
    RESULT_VARIABLE rejected_result
    OUTPUT_VARIABLE rejected_output
    ERROR_VARIABLE rejected_error)
  set(rejected_combined "${rejected_output}\n${rejected_error}")
  if(rejected_result EQUAL 0)
    message(FATAL_ERROR
      "WP360 forbidden ${reader} target unexpectedly acquired raw JSON")
  endif()
  if(NOT rejected_combined MATCHES "rawJson")
    message(FATAL_ERROR
      "WP360 forbidden ${reader} failed for an unrelated reason:\n${rejected_combined}")
  endif()
  message(STATUS
    "WP360_RAW_AUTHORITY_REJECTED target=${reader} symbol=rawJson")
endforeach()
message(STATUS
  "WP360_RAW_AUTHORITY_ALLOWED target=authoring_authority type=AuthoringSceneRawView")
