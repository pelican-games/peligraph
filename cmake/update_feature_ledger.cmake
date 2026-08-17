get_filename_component(PELICAN_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
include("${PELICAN_SOURCE_DIR}/cmake/pelican_feature_registry.cmake")

if(NOT DEFINED PELICAN_FEATURE_LEDGER)
  set(PELICAN_FEATURE_LEDGER
      "${PELICAN_SOURCE_DIR}/docs/implementation_plan.md")
endif()

file(READ "${PELICAN_FEATURE_LEDGER}" ledger_contents)
set(begin_marker "<!-- PELICAN_FEATURE_REGISTRY_BEGIN -->")
set(end_marker "<!-- PELICAN_FEATURE_REGISTRY_END -->")
string(FIND "${ledger_contents}" "${begin_marker}" begin_offset)
string(FIND "${ledger_contents}" "${end_marker}" end_offset)
if(begin_offset EQUAL -1)
  message(FATAL_ERROR
    "pelican.feature_registry.ledger_begin_missing@1: ${PELICAN_FEATURE_LEDGER}")
endif()
if(end_offset EQUAL -1 OR end_offset LESS begin_offset)
  message(FATAL_ERROR
    "pelican.feature_registry.ledger_end_missing@1: ${PELICAN_FEATURE_LEDGER}")
endif()

string(SUBSTRING "${ledger_contents}" 0 ${begin_offset} prefix)
string(LENGTH "${end_marker}" end_marker_length)
math(EXPR suffix_offset "${end_offset} + ${end_marker_length}")
string(SUBSTRING "${ledger_contents}" ${suffix_offset} -1 suffix)
pelican_render_feature_ledger(rendered_registry)
file(WRITE "${PELICAN_FEATURE_LEDGER}"
     "${prefix}${rendered_registry}${suffix}")
pelican_verify_feature_ledger("${PELICAN_FEATURE_LEDGER}")
message(STATUS
  "updated feature ledger from registry: ${PELICAN_FEATURE_LEDGER}")

