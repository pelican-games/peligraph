if(NOT DEFINED TEST_EXECUTABLE OR NOT EXISTS "${TEST_EXECUTABLE}")
  message(FATAL_ERROR "WP381 test executable does not exist: ${TEST_EXECUTABLE}")
endif()

get_filename_component(test_working_directory "${TEST_EXECUTABLE}" DIRECTORY)
execute_process(
  COMMAND "${TEST_EXECUTABLE}" "${TEST_NAME}" -w NoAssertions
  WORKING_DIRECTORY "${test_working_directory}"
  RESULT_VARIABLE test_result)

if(NOT test_result EQUAL 0)
  message(FATAL_ERROR "WP381 OS-loader control failed with ${test_result}")
endif()
