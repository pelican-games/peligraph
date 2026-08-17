get_filename_component(PELICAN_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
include("${PELICAN_SOURCE_DIR}/cmake/pelican_feature_registry.cmake")
pelican_verify_feature_ledger(
  "${PELICAN_SOURCE_DIR}/docs/implementation_plan.md")

set(matrix_json "{\"include\":[")
set(first_entry TRUE)
foreach(feature_name IN LISTS PELICAN_FEATURE_REGISTRY_NAMES)
  if(first_entry)
    set(first_entry FALSE)
  else()
    string(APPEND matrix_json ",")
  endif()

  set(contrast "${PELICAN_FEATURE_${feature_name}_CONTRAST}")
  set(smoke_id "${PELICAN_FEATURE_${feature_name}_SMOKE_ID}")
  pelican_feature_contrast_overrides("${feature_name}" overrides)
  set(display_name "${feature_name}=${contrast}")
  if(overrides)
    list(JOIN overrides ", " rendered_overrides)
    string(APPEND display_name " / ${rendered_overrides}")
  endif()
  string(APPEND matrix_json
    "{\"name\":\"${display_name}\","
    "\"id\":\"${smoke_id}\",\"kind\":\"build-unit\"}")
endforeach()
string(APPEND matrix_json
  ",{\"name\":\"PELICAN_PROJECT project-code smoke\","
  "\"id\":\"project-code\",\"kind\":\"project-code\"}]}")

if(DEFINED PELICAN_FEATURE_SMOKE_MATRIX_OUTPUT AND
   NOT "${PELICAN_FEATURE_SMOKE_MATRIX_OUTPUT}" STREQUAL "")
  get_filename_component(
    matrix_output_path
    "${PELICAN_FEATURE_SMOKE_MATRIX_OUTPUT}"
    ABSOLUTE
    BASE_DIR "${PELICAN_SOURCE_DIR}"
  )
  file(WRITE "${matrix_output_path}" "${matrix_json}")
else()
  message(STATUS "${matrix_json}")
endif()
