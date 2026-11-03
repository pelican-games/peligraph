# A -P script starts with every policy unset; before CMake 4 that means OLD
# (IN_LIST is then not an operator). LX-1: Linux use CMake 3.28.
cmake_minimum_required(VERSION 3.20)
get_filename_component(PELICAN_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
include("${PELICAN_SOURCE_DIR}/cmake/pelican_feature_registry.cmake")

if(NOT DEFINED PELICAN_FEATURE_LEDGER)
  set(PELICAN_FEATURE_LEDGER
      "${PELICAN_FEATURE_LEDGER_DEFAULT}")
endif()

pelican_verify_feature_ledger("${PELICAN_FEATURE_LEDGER}")
message(STATUS "pelican feature registry and ledger agree")

