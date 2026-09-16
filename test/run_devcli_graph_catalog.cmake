# AG-0: the shipped CLI prints the module catalog. Discovery goes through the
# production entry point, not through a helper the test calls itself.
if(NOT DEFINED CLI OR NOT EXISTS "${CLI}")
  message(FATAL_ERROR "pelican_cli is missing: ${CLI}")
endif()

execute_process(
  COMMAND "${CLI}" graph-catalog
  RESULT_VARIABLE run_result
  OUTPUT_VARIABLE catalog
  ERROR_VARIABLE run_error)
if(NOT run_result EQUAL 0)
  message(FATAL_ERROR "graph-catalog failed (${run_result}): ${run_error}")
endif()

string(JSON schema GET "${catalog}" "schema")
if(NOT schema STREQUAL "pelican.graph_catalog")
  message(FATAL_ERROR "unexpected catalog schema: ${schema}")
endif()

string(JSON module_count LENGTH "${catalog}" "modules")
if(NOT module_count EQUAL 3)
  message(FATAL_ERROR "expected three built-in modules, got ${module_count}")
endif()

set(seen_namespaces "")
set(seen_operations "")
set(trigger_clock_evaluation "")
set(render_buffer_change "")
math(EXPR last_module "${module_count} - 1")
foreach(module_index RANGE ${last_module})
  string(JSON module GET "${catalog}" "modules" ${module_index})
  string(JSON name_space GET "${module}" "namespace")
  string(JSON version GET "${module}" "version")
  string(JSON description GET "${module}" "description")
  if(NOT version EQUAL 1 OR description STREQUAL "")
    message(FATAL_ERROR "module ${name_space} lacks a version or a description")
  endif()
  list(APPEND seen_namespaces "${name_space}")
  string(JSON operation_count LENGTH "${module}" "operations")
  if(operation_count EQUAL 0)
    message(FATAL_ERROR "module ${name_space} registered no operation")
  endif()
  math(EXPR last_operation "${operation_count} - 1")
  foreach(operation_index RANGE ${last_operation})
    string(JSON operation GET "${module}" "operations" ${operation_index})
    string(JSON op GET "${operation}" "op")
    string(JSON domain GET "${operation}" "domain")
    string(JSON evaluation GET "${operation}" "evaluation")
    string(JSON change GET "${operation}" "change" "default")
    string(JSON sink GET "${operation}" "sink")
    if(domain STREQUAL "" OR evaluation STREQUAL "" OR change STREQUAL "" OR sink STREQUAL "")
      message(FATAL_ERROR "operation ${op} lacks a decision 5 field")
    endif()
    list(APPEND seen_operations "${op}")
    if(op STREQUAL "value.trigger_clock")
      set(trigger_clock_evaluation "${evaluation}")
    endif()
    if(op STREQUAL "render.buffer")
      set(render_buffer_change "${change}")
    endif()
    if(op STREQUAL "audio.volume" AND NOT sink)
      message(FATAL_ERROR "audio.volume is registered as a sink")
    endif()
  endforeach()
endforeach()

foreach(expected IN ITEMS pelican.value pelican.audio pelican.render)
  if(NOT expected IN_LIST seen_namespaces)
    message(FATAL_ERROR "catalog is missing ${expected}")
  endif()
endforeach()

foreach(expected IN ITEMS value.input value.constant value.parameter value.morph_weight
                          value.trigger_clock value.event_clock value.add value.multiply
                          value.curve value.playback event.playback
                          audio.playback audio.volume audio.pitch
                          render.scalar render.buffer render.attribute render.image
                          render.compute render.fullscreen render.raster)
  if(NOT expected IN_LIST seen_operations)
    message(FATAL_ERROR "catalog is missing operation ${expected}")
  endif()
endforeach()

if(NOT trigger_clock_evaluation STREQUAL "replay")
  message(FATAL_ERROR
    "value.trigger_clock keeps state between ticks: ${trigger_clock_evaluation}")
endif()
if(NOT render_buffer_change STREQUAL "rebuild")
  message(FATAL_ERROR "render.buffer parameters rebuild: ${render_buffer_change}")
endif()
