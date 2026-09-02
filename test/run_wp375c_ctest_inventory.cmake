cmake_minimum_required(VERSION 3.25)

foreach(required IN ITEMS PELICAN_CTEST_COMMAND PELICAN_BINARY_DIR
                          PELICAN_TEST_CONFIG PELICAN_EXPECT_RPC)
  if(NOT DEFINED ${required})
    message(FATAL_ERROR "WP375c inventory contract requires ${required}")
  endif()
endforeach()

execute_process(
  COMMAND "${PELICAN_CTEST_COMMAND}"
          --test-dir "${PELICAN_BINARY_DIR}"
          -C "${PELICAN_TEST_CONFIG}"
          --show-only=json-v1
  RESULT_VARIABLE inventory_result
  OUTPUT_VARIABLE inventory_json
  ERROR_VARIABLE inventory_error
)
if(NOT inventory_result EQUAL 0)
  message(FATAL_ERROR
    "WP375c could not read the CTest inventory (${inventory_result}): "
    "${inventory_error}")
endif()

string(JSON test_count LENGTH "${inventory_json}" tests)
set(all_names)
set(gpu_names)
if(test_count GREATER 0)
  math(EXPR last_test "${test_count} - 1")
  foreach(test_index RANGE 0 ${last_test})
    string(JSON test_name GET "${inventory_json}" tests ${test_index} name)
    list(APPEND all_names "${test_name}")

    string(JSON property_count LENGTH
           "${inventory_json}" tests ${test_index} properties)
    if(property_count GREATER 0)
      math(EXPR last_property "${property_count} - 1")
      foreach(property_index RANGE 0 ${last_property})
        string(JSON property_name GET
               "${inventory_json}" tests ${test_index} properties
               ${property_index} name)
        if(property_name STREQUAL "LABELS")
          string(JSON label_count LENGTH
                 "${inventory_json}" tests ${test_index} properties
                 ${property_index} value)
          if(label_count GREATER 0)
            math(EXPR last_label "${label_count} - 1")
            foreach(label_index RANGE 0 ${last_label})
              string(JSON label GET
                     "${inventory_json}" tests ${test_index} properties
                     ${property_index} value ${label_index})
              if(label STREQUAL "gpu")
                list(APPEND gpu_names "${test_name}")
              endif()
            endforeach()
          endif()
        endif()
      endforeach()
    endif()
  endforeach()
endif()

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
    message(FATAL_ERROR
      "WP375c CTest inventory contains synthetic discovery failure: "
      "'${test_name}'")
  endif()
endforeach()

function(wp375c_require_test test_name require_gpu)
  set(matches 0)
  foreach(candidate IN LISTS all_names)
    if(candidate STREQUAL test_name)
      math(EXPR matches "${matches} + 1")
    endif()
  endforeach()
  if(NOT matches EQUAL 1)
    message(FATAL_ERROR
      "WP375c expected exactly one '${test_name}', found ${matches}")
  endif()
  if(require_gpu)
    list(FIND gpu_names "${test_name}" gpu_index)
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
  list(FIND all_names "${rpc_test}" rpc_index)
  if(NOT rpc_index EQUAL -1)
    message(FATAL_ERROR
      "WP375c RPC-OFF inventory unexpectedly contains '${rpc_test}'")
  endif()
endif()

message(STATUS
  "WP375C_CTEST_INVENTORY rpc=${PELICAN_EXPECT_RPC} tests=${all_count} "
  "duplicates=0 gpu_contract=3")
