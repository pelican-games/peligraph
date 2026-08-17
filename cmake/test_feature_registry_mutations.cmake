cmake_minimum_required(VERSION 3.25)

get_filename_component(PELICAN_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(feature_ledger "${PELICAN_SOURCE_DIR}/docs/implementation_plan.md")
set(feature_verifier "${PELICAN_SOURCE_DIR}/cmake/verify_feature_registry.cmake")
include("${PELICAN_SOURCE_DIR}/cmake/pelican_feature_registry.cmake")

if(NOT DEFINED PELICAN_MUTATION_DIR)
  set(PELICAN_MUTATION_DIR
      "${CMAKE_CURRENT_BINARY_DIR}/feature-registry-mutations")
endif()
file(MAKE_DIRECTORY "${PELICAN_MUTATION_DIR}")

file(READ "${feature_ledger}" original_ledger)

function(expect_named_failure mutation_name ledger_contents error_id detail)
  set(mutated_ledger "${PELICAN_MUTATION_DIR}/${mutation_name}.md")
  file(WRITE "${mutated_ledger}" "${ledger_contents}")

  execute_process(
    COMMAND "${CMAKE_COMMAND}"
      "-DPELICAN_FEATURE_LEDGER=${mutated_ledger}"
      -P "${feature_verifier}"
    RESULT_VARIABLE verify_result
    OUTPUT_VARIABLE verify_stdout
    ERROR_VARIABLE verify_stderr
  )
  string(CONCAT verify_log "${verify_stdout}" "${verify_stderr}")

  if(verify_result EQUAL 0)
    message(FATAL_ERROR
      "pelican.feature_registry.mutation_unexpected_pass@1: ${mutation_name}")
  endif()
  string(FIND "${verify_log}" "${error_id}" error_position)
  string(FIND "${verify_log}" "${detail}" detail_position)
  if(error_position EQUAL -1 OR detail_position EQUAL -1)
    message(FATAL_ERROR
      "pelican.feature_registry.mutation_wrong_error@1: ${mutation_name}\n"
      "expected ${error_id} and ${detail}\n${verify_log}")
  endif()

  message(STATUS "${mutation_name}: ${error_id}: ${detail}")
endfunction()

set(missing_feature PELICAN_WITH_AUDIO)
pelican_render_feature_ledger_row("${missing_feature}" audio_row)
string(FIND "${original_ledger}" "${audio_row}" audio_row_position)
if(audio_row_position EQUAL -1)
  message(FATAL_ERROR
    "pelican.feature_registry.mutation_fixture_missing@1: ${audio_row}")
endif()
string(REPLACE "${audio_row}" "" missing_row_ledger "${original_ledger}")
expect_named_failure(
  missing_row
  "${missing_row_ledger}"
  pelican.feature_registry.ledger_missing@1
  "${missing_feature}"
)

set(fake_row "| `PELICAN_FAKE_FEATURE` | `ON` | `OFF` | — |")
string(REPLACE
  "<!-- PELICAN_FEATURE_REGISTRY_END -->"
  "${fake_row}\n<!-- PELICAN_FEATURE_REGISTRY_END -->"
  unknown_row_ledger
  "${original_ledger}"
)
expect_named_failure(
  unknown_row
  "${unknown_row_ledger}"
  pelican.feature_registry.ledger_unknown@1
  PELICAN_FAKE_FEATURE
)

set(default_mutation_feature PELICAN_WITH_JOLT_PHYSICS)
pelican_render_feature_ledger_row("${default_mutation_feature}" jolt_row)
set(jolt_baseline
    "${PELICAN_FEATURE_${default_mutation_feature}_BASELINE}")
if(jolt_baseline STREQUAL "ON")
  set(jolt_wrong_baseline OFF)
else()
  set(jolt_wrong_baseline ON)
endif()
pelican_feature_dependency_markdown(
  "${default_mutation_feature}" jolt_dependencies)
set(jolt_wrong_baseline_row
    "| `${default_mutation_feature}` | `${jolt_wrong_baseline}` | "
    "`${PELICAN_FEATURE_${default_mutation_feature}_CONTRAST}` | "
    "${jolt_dependencies} |")
string(JOIN "" jolt_wrong_baseline_row ${jolt_wrong_baseline_row})
string(FIND "${original_ledger}" "${jolt_row}" jolt_row_position)
if(jolt_row_position EQUAL -1)
  message(FATAL_ERROR
    "pelican.feature_registry.mutation_fixture_missing@1: ${jolt_row}")
endif()
string(REPLACE
  "${jolt_row}"
  "${jolt_wrong_baseline_row}"
  wrong_baseline_ledger
  "${original_ledger}"
)
expect_named_failure(
  wrong_baseline
  "${wrong_baseline_ledger}"
  pelican.feature_registry.ledger_baseline_mismatch@1
  "${default_mutation_feature} expected ${jolt_baseline}, found ${jolt_wrong_baseline}"
)
