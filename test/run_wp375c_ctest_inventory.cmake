cmake_minimum_required(VERSION 3.25)

foreach(required IN ITEMS PELICAN_CTEST_COMMAND PELICAN_BINARY_DIR
                          PELICAN_TEST_CONFIG PELICAN_EXPECT_RPC)
  if(NOT DEFINED ${required})
    message(FATAL_ERROR "WP375c inventory contract requires ${required}")
  endif()
endforeach()

# The inventory is read from the `ctest -N` listing (one "  Test  #12: <name>"
# line per test), once for every test and once for the tests carrying the gpu
# label.  Walking `--show-only=json-v1` with string(JSON) re-parsed the whole
# document on every lookup and grew with the square of the test count.
#
# A test name may hold ';', '[', ']' or '\', which a CMake list would split on
# or join across.  Names are therefore percent-encoded for as long as they live
# in a list and decoded only for messages.  The encoding is one-to-one, so
# comparing encoded names compares the names themselves.
function(wp375c_encode out_var text)
  string(REPLACE "%" "%25" text "${text}")
  string(REPLACE "\\" "%5C" text "${text}")
  string(REPLACE ";" "%3B" text "${text}")
  string(REPLACE "[" "%5B" text "${text}")
  string(REPLACE "]" "%5D" text "${text}")
  set(${out_var} "${text}" PARENT_SCOPE)
endfunction()

function(wp375c_decode out_var text)
  string(REPLACE "%5D" "]" text "${text}")
  string(REPLACE "%5B" "[" text "${text}")
  string(REPLACE "%3B" ";" text "${text}")
  string(REPLACE "%5C" "\\" text "${text}")
  string(REPLACE "%25" "%" text "${text}")
  set(${out_var} "${text}" PARENT_SCOPE)
endfunction()

set(wp375c_config_args)
if(NOT PELICAN_TEST_CONFIG STREQUAL "")
  list(APPEND wp375c_config_args -C "${PELICAN_TEST_CONFIG}")
endif()

# Sets <out_var> to the encoded names `ctest -N <filter...>` lists.
function(wp375c_read_names out_var)
  execute_process(
    COMMAND "${PELICAN_CTEST_COMMAND}"
            --test-dir "${PELICAN_BINARY_DIR}"
            ${wp375c_config_args}
            -N
            ${ARGN}
    RESULT_VARIABLE inventory_result
    OUTPUT_VARIABLE inventory_text
    ERROR_VARIABLE inventory_error
    ENCODING UTF-8
  )
  if(NOT inventory_result EQUAL 0)
    message(FATAL_ERROR
      "WP375c could not read the CTest inventory (${inventory_result}): "
      "${inventory_error}")
  endif()

  string(REPLACE "\r" "" inventory_text "${inventory_text}")
  wp375c_encode(inventory_text "${inventory_text}")
  string(REPLACE "\n" ";" inventory_lines "${inventory_text}")
  set(names)
  set(listed_count 0)
  set(reported_count "")
  foreach(line IN LISTS inventory_lines)
    if(line MATCHES "^ *Test +#[0-9]+: (.*)$")
      list(APPEND names "${CMAKE_MATCH_1}")
      math(EXPR listed_count "${listed_count} + 1")
    elseif(line MATCHES "^Total Tests: ([0-9]+)$")
      set(reported_count "${CMAKE_MATCH_1}")
    endif()
  endforeach()
  # A listing whose shape this reader does not know must not pass as an
  # inventory with fewer tests in it.
  if(NOT reported_count STREQUAL "${listed_count}")
    message(FATAL_ERROR
      "WP375c could not read the CTest inventory: read ${listed_count} test "
      "lines, CTest reported '${reported_count}' (filter: ${ARGN})")
  endif()
  set(${out_var} "${names}" PARENT_SCOPE)
endfunction()

wp375c_read_names(all_names)
wp375c_read_names(gpu_names -L "^gpu$")

set(unique_names ${all_names})
list(REMOVE_DUPLICATES unique_names)
list(LENGTH all_names all_count)
list(LENGTH unique_names unique_count)
if(NOT all_count EQUAL unique_count)
  message(FATAL_ERROR
    "WP375c CTest inventory contains duplicate test names: "
    "total=${all_count}, unique=${unique_count}")
endif()
foreach(test_name IN LISTS all_names)
  if(test_name MATCHES "_NOT_BUILT($|-)")
    wp375c_decode(test_name "${test_name}")
    message(FATAL_ERROR
      "WP375c CTest inventory contains synthetic discovery failure: "
      "'${test_name}'")
  endif()
endforeach()

function(wp375c_require_test test_name require_gpu)
  wp375c_encode(encoded_name "${test_name}")
  set(matches 0)
  foreach(candidate IN LISTS all_names)
    if(candidate STREQUAL encoded_name)
      math(EXPR matches "${matches} + 1")
    endif()
  endforeach()
  if(NOT matches EQUAL 1)
    message(FATAL_ERROR
      "WP375c expected exactly one '${test_name}', found ${matches}")
  endif()
  if(require_gpu)
    list(FIND gpu_names "${encoded_name}" gpu_index)
    if(gpu_index EQUAL -1)
      message(FATAL_ERROR
        "WP375c expected gpu label on '${test_name}'")
    endif()
  endif()
endfunction()

wp375c_require_test(
  "WP377 authored pelican_ray_instance force opaque reaches the material alpha profile row guard"
  TRUE)
wp375c_require_test(
  "WP374a all ten root layout AS component states cross real SceneLoader"
  TRUE)
wp375c_require_test(
  "WP375 production SceneLoader preflight rejects a non-renderable ray instance beside a valid scene"
  TRUE)
wp375c_require_test(
  "WP379 production bootstrap keeps external ray_instance beside the builtin canonical component"
  TRUE)

set(rpc_test
  "WP379 production scene RPC and snapshot boundaries share component_unknown")
if(PELICAN_EXPECT_RPC)
  wp375c_require_test("${rpc_test}" TRUE)
else()
  wp375c_encode(encoded_rpc_test "${rpc_test}")
  list(FIND all_names "${encoded_rpc_test}" rpc_index)
  if(NOT rpc_index EQUAL -1)
    message(FATAL_ERROR
      "WP375c RPC-OFF inventory unexpectedly contains '${rpc_test}'")
  endif()
endif()

message(STATUS
  "WP375C_CTEST_INVENTORY rpc=${PELICAN_EXPECT_RPC} tests=${all_count} "
  "duplicates=0 gpu_contract=3")
