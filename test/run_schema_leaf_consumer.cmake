if(NOT DEFINED PELICAN_SOURCE_DIR OR NOT DEFINED TEST_BINARY_DIR OR
   NOT DEFINED NLOHMANN_INCLUDE_DIR OR NOT DEFINED GENERATOR OR
   NOT DEFINED TEST_CONFIG)
  message(FATAL_ERROR "schema leaf consumer arguments are incomplete")
endif()

file(REMOVE_RECURSE "${TEST_BINARY_DIR}")
set(configure_command
  "${CMAKE_COMMAND}"
  -S "${PELICAN_SOURCE_DIR}/test/schema_leaf_consumer"
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
  ERROR_VARIABLE configure_error
)
if(NOT configure_result EQUAL 0)
  message(FATAL_ERROR
    "independent leaf configure failed (${configure_result})\n"
    "${configure_output}\n${configure_error}")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" --build "${TEST_BINARY_DIR}"
          --config "${TEST_CONFIG}"
  RESULT_VARIABLE build_result
  OUTPUT_VARIABLE build_output
  ERROR_VARIABLE build_error
)
if(NOT build_result EQUAL 0)
  message(FATAL_ERROR
    "independent leaf build failed (${build_result})\n"
    "${build_output}\n${build_error}")
endif()

set(consumer "${TEST_BINARY_DIR}/${TEST_CONFIG}/schema_leaf_consumer.exe")
if(NOT EXISTS "${consumer}")
  set(consumer "${TEST_BINARY_DIR}/schema_leaf_consumer")
endif()
execute_process(
  COMMAND "${consumer}"
  RESULT_VARIABLE run_result
  OUTPUT_VARIABLE run_output
  ERROR_VARIABLE run_error
)
if(NOT run_result EQUAL 0)
  message(FATAL_ERROR
    "independent leaf consumer failed (${run_result})\n"
    "${run_output}\n${run_error}")
endif()
message(STATUS "${run_output}")

