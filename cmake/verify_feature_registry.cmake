get_filename_component(PELICAN_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
include("${PELICAN_SOURCE_DIR}/cmake/pelican_feature_registry.cmake")

if(NOT DEFINED PELICAN_FEATURE_LEDGER)
  set(PELICAN_FEATURE_LEDGER
      "${PELICAN_SOURCE_DIR}/docs/implementation_plan.md")
endif()

pelican_verify_feature_ledger("${PELICAN_FEATURE_LEDGER}")
message(STATUS "pelican feature registry and ledger agree")

