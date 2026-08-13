if(NOT DEFINED TEST_FILE OR NOT DEFINED ORDER_FILE)
  message(FATAL_ERROR "TEST_FILE and ORDER_FILE are required")
endif()
if(NOT EXISTS "${TEST_FILE}")
  message(FATAL_ERROR "Catch discovery output does not exist: ${TEST_FILE}")
endif()

file(STRINGS "${ORDER_FILE}" test_order ENCODING UTF-8)
list(FILTER test_order EXCLUDE REGEX "^(#|$)")
file(STRINGS "${TEST_FILE}" generated_lines ENCODING UTF-8)

# CatchAddTests.cmake emits one add_test/set_tests_properties pair per case,
# followed by the <target>_TESTS variable. Catch2 3.11 randomizes list-tests
# with a random_device seed, so relinking alone otherwise changes ctest order.
set(test_blocks)
set(trailer_lines)
unset(pending_add_test)
foreach(generated_line IN LISTS generated_lines)
  if(generated_line MATCHES "^add_test\\(")
    if(DEFINED pending_add_test)
      message(FATAL_ERROR "Discovered add_test is missing its properties")
    endif()
    set(pending_add_test "${generated_line}")
  elseif(DEFINED pending_add_test)
    if(NOT generated_line MATCHES "^set_tests_properties\\(")
      message(FATAL_ERROR
        "Unexpected line after discovered add_test: ${generated_line}")
    endif()
    list(APPEND test_blocks
      "${pending_add_test}\n${generated_line}\n")
    unset(pending_add_test)
  else()
    list(APPEND trailer_lines "${generated_line}")
  endif()
endforeach()
if(NOT test_blocks)
  message(FATAL_ERROR "No discovered Catch tests found in ${TEST_FILE}")
endif()

set(keyed_blocks)
set(original_index 0)
foreach(test_block IN LISTS test_blocks)
  string(REGEX MATCH
    "^add_test\\( \\[==\\[([^]]*)\\]==\\]"
    name_match "${test_block}")
  if(NOT name_match)
    message(FATAL_ERROR "Cannot read discovered test name from: ${test_block}")
  endif()

  set(test_name "${CMAKE_MATCH_1}")
  list(FIND test_order "${test_name}" order_index)
  if(order_index LESS 0)
    # Preserve newly added tests instead of silently dropping them. They stay
    # in Catch's discovered order after the recorded compatibility sequence.
    math(EXPR order_index "1000000 + ${original_index}")
  endif()
  math(EXPR order_key "1000000 + ${order_index}")
  math(EXPR original_key "1000000 + ${original_index}")
  list(APPEND keyed_blocks
    "${order_key}|${original_key}|${test_block}")
  math(EXPR original_index "${original_index} + 1")
endforeach()

list(SORT keyed_blocks)
set(reordered_content)
foreach(keyed_block IN LISTS keyed_blocks)
  string(REGEX REPLACE "^[^|]*\\|[^|]*\\|" "" test_block "${keyed_block}")
  string(APPEND reordered_content "${test_block}")
endforeach()
foreach(trailer_line IN LISTS trailer_lines)
  string(APPEND reordered_content "${trailer_line}\n")
endforeach()
file(WRITE "${TEST_FILE}" "${reordered_content}")
