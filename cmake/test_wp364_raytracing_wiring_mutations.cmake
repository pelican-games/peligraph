cmake_minimum_required(VERSION 3.25)

get_filename_component(PELICAN_SOURCE_DIR
  "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
file(READ
  "${PELICAN_SOURCE_DIR}/src/core/shader/pipelinefactory.cpp"
  pipeline_source)
file(READ
  "${PELICAN_SOURCE_DIR}/src/core/renderingpass/computetask.cpp"
  compute_source)

set(pipeline_build_wire "validateRayTracingPipelineBuild(")
set(initial_extent_wire "validateRayTracingInitialExtent(")
set(rebind_wire "validateRayTracingRebindExtent(")
set(final_command_wire "validateRayTracingFinalCommandExtent(")

function(wp364_wiring_is_complete pipeline compute result)
  foreach(needle IN ITEMS
      "${pipeline_build_wire}"
      "${final_command_wire}")
    string(FIND "${pipeline}" "${needle}" position)
    if(position EQUAL -1)
      set(${result} FALSE PARENT_SCOPE)
      return()
    endif()
  endforeach()
  foreach(needle IN ITEMS
      "${initial_extent_wire}"
      "${rebind_wire}")
    string(FIND "${compute}" "${needle}" position)
    if(position EQUAL -1)
      set(${result} FALSE PARENT_SCOPE)
      return()
    endif()
  endforeach()
  set(${result} TRUE PARENT_SCOPE)
endfunction()

wp364_wiring_is_complete("${pipeline_source}" "${compute_source}"
  baseline_complete)
if(NOT baseline_complete)
  message(FATAL_ERROR
    "pelican.ray_tracing.wiring_missing@1: the production RT validation wiring is incomplete")
endif()

foreach(mutation IN ITEMS
    pipeline_build_wire
    initial_extent_wire
    rebind_wire
    final_command_wire)
  set(mutated_pipeline "${pipeline_source}")
  set(mutated_compute "${compute_source}")
  if(mutation STREQUAL "pipeline_build_wire" OR
     mutation STREQUAL "final_command_wire")
    string(REPLACE "${${mutation}}" "WP364_DELETED_WIRE("
      mutated_pipeline "${mutated_pipeline}")
  else()
    string(REPLACE "${${mutation}}" "WP364_DELETED_WIRE("
      mutated_compute "${mutated_compute}")
  endif()
  wp364_wiring_is_complete("${mutated_pipeline}" "${mutated_compute}"
    mutation_passed)
  if(mutation_passed)
    message(FATAL_ERROR
      "pelican.ray_tracing.wiring_mutation_survived@1: ${mutation}")
  endif()
endforeach()

message(STATUS
  "WP364 four-site wiring contract and all four deletion mutations passed")
