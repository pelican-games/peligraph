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

# pelican.ui is always registered (GP-3: the UI has no build without it).
# pelican.pose and pelican.audio are registered only when their units are
# built (AG-0, PG-1), so the catalog is the OFF builds' observable difference.
set(expected_modules 3)
if(WITH_ANIMATION)
  math(EXPR expected_modules "${expected_modules} + 1")
endif()
if(WITH_AUDIO)
  math(EXPR expected_modules "${expected_modules} + 1")
endif()
string(JSON module_count LENGTH "${catalog}" "modules")
if(NOT module_count EQUAL ${expected_modules})
  message(FATAL_ERROR "expected ${expected_modules} built-in modules, got ${module_count}")
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

set(expected_namespaces pelican.value pelican.render pelican.ui)
set(expected_audio_operations "")
if(WITH_AUDIO)
  list(APPEND expected_namespaces pelican.audio)
  set(expected_audio_operations audio.playback audio.volume audio.pitch)
elseif("pelican.audio" IN_LIST seen_namespaces)
  message(FATAL_ERROR "PELICAN_WITH_AUDIO=OFF still registered pelican.audio")
endif()
set(expected_pose_operations "")
if(WITH_ANIMATION)
  list(APPEND expected_namespaces pelican.pose)
  set(expected_pose_operations pose.clip_sample pose.blend pose.state_machine
                               pose.retarget pose.skeletal pose.transform)
elseif("pelican.pose" IN_LIST seen_namespaces)
  message(FATAL_ERROR "PELICAN_WITH_ANIMATION=OFF still registered pelican.pose")
endif()
foreach(expected IN LISTS expected_namespaces)
  if(NOT expected IN_LIST seen_namespaces)
    message(FATAL_ERROR "catalog is missing ${expected}")
  endif()
endforeach()
foreach(expected IN LISTS expected_pose_operations)
  if(NOT expected IN_LIST seen_operations)
    message(FATAL_ERROR "catalog is missing operation ${expected}")
  endif()
endforeach()

foreach(expected IN ITEMS value.input value.constant value.parameter value.morph_weight
                          value.trigger_clock value.event_clock value.add value.multiply
                          value.curve value.playback event.playback
                          ${expected_audio_operations}
                          render.scalar render.buffer render.attribute render.image
                          render.compute render.fullscreen render.raster
                          ui.value ui.visible)
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

# R-3: docs/v0_freeze.md is the freeze list, and its graph operation table is a
# copy of this catalog.  A copy nobody compares is a second source of truth, so
# the same run that reads the catalog reads the table: the documented set is
# exactly what the ON build registers, and a superset of what the OFF build
# does -- the difference being the pose operations the disabled module owns.
# The table is delimited by comment markers rather than by a heading, so
# reformatting the prose around it cannot silently empty the comparison.
set(freeze_doc "${CMAKE_CURRENT_LIST_DIR}/../docs/v0_freeze.md")
if(NOT EXISTS "${freeze_doc}")
  message(FATAL_ERROR "the v0 freeze list is missing: ${freeze_doc}")
endif()
file(READ "${freeze_doc}" freeze_text)
set(table_begin_marker "<!-- graph-op-table:begin -->")
set(table_end_marker "<!-- graph-op-table:end -->")
string(FIND "${freeze_text}" "${table_begin_marker}" table_begin_index)
string(FIND "${freeze_text}" "${table_end_marker}" table_end_index)
if(table_begin_index LESS 0 OR table_end_index LESS 0 OR
   NOT table_end_index GREATER table_begin_index)
  message(FATAL_ERROR
    "docs/v0_freeze.md no longer delimits its operation table with "
    "${table_begin_marker} and ${table_end_marker}")
endif()
string(LENGTH "${table_begin_marker}" table_begin_length)
math(EXPR table_body_start "${table_begin_index} + ${table_begin_length}")
math(EXPR table_body_length "${table_end_index} - ${table_body_start}")
string(SUBSTRING "${freeze_text}" ${table_body_start} ${table_body_length} table_body)

# One row per operation, the op name backquoted in the first column.
string(REGEX MATCHALL "\n\\| `[a-z_]+\\.[a-z_]+` \\|" documented_rows "${table_body}")
set(documented_operations "")
foreach(documented_row IN LISTS documented_rows)
  string(REGEX REPLACE "^\n\\| `([a-z_]+\\.[a-z_]+)` \\|$" "\\1"
    documented_op "${documented_row}")
  list(APPEND documented_operations "${documented_op}")
endforeach()
list(LENGTH documented_operations documented_count)
if(documented_count EQUAL 0)
  message(FATAL_ERROR
    "docs/v0_freeze.md documents no graph operation between its table markers")
endif()
set(unique_documented_operations ${documented_operations})
list(REMOVE_DUPLICATES unique_documented_operations)
list(LENGTH unique_documented_operations unique_documented_count)
if(NOT unique_documented_count EQUAL documented_count)
  message(FATAL_ERROR
    "docs/v0_freeze.md lists the same operation twice: ${documented_operations}")
endif()

set(undocumented_operations "")
foreach(registered_op IN LISTS seen_operations)
  if(NOT registered_op IN_LIST documented_operations)
    list(APPEND undocumented_operations "${registered_op}")
  endif()
endforeach()
if(undocumented_operations)
  message(FATAL_ERROR
    "the catalog registers operations docs/v0_freeze.md does not list: "
    "${undocumented_operations}")
endif()

set(unregistered_operations "")
foreach(documented_op IN LISTS documented_operations)
  if(NOT documented_op IN_LIST seen_operations)
    list(APPEND unregistered_operations "${documented_op}")
  endif()
endforeach()
# The freeze table documents the ON build. A build without a unit may miss
# exactly that unit's operations and nothing else; anything more means the
# table drifted from the catalog in a way the ON run would not catch either.
set(absent_unit_patterns "")
set(absent_unit_operations "")
if(NOT WITH_ANIMATION)
  list(APPEND absent_unit_patterns "^pose\\.")
  list(APPEND absent_unit_operations pose.clip_sample pose.blend pose.state_machine
                                     pose.retarget pose.skeletal pose.transform)
endif()
if(NOT WITH_AUDIO)
  list(APPEND absent_unit_patterns "^audio\\.")
  list(APPEND absent_unit_operations audio.playback audio.volume audio.pitch)
endif()
foreach(documented_op IN LISTS unregistered_operations)
  set(attributed FALSE)
  foreach(pattern IN LISTS absent_unit_patterns)
    if(documented_op MATCHES "${pattern}")
      set(attributed TRUE)
    endif()
  endforeach()
  if(NOT attributed)
    message(FATAL_ERROR
      "docs/v0_freeze.md lists ${documented_op}, which this build does not "
      "register and no unit built out of it accounts for")
  endif()
endforeach()
foreach(expected IN LISTS absent_unit_operations)
  if(NOT expected IN_LIST documented_operations)
    message(FATAL_ERROR
      "docs/v0_freeze.md no longer documents ${expected}")
  endif()
endforeach()
