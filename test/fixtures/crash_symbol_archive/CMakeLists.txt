cmake_minimum_required(VERSION 3.13)
project(pelican_crash_symbol_fixture LANGUAGES CXX)

foreach(required IN ITEMS BUILD_MARKER ARCHIVE_ENABLED ARCHIVE_ROOT
                          MODULE_PATH SOURCE_COMMIT MAX_GENERATIONS)
  if(NOT DEFINED ${required})
    message(FATAL_ERROR "Missing fixture variable: ${required}")
  endif()
endforeach()

configure_file(probe.cpp.in "${CMAKE_CURRENT_BINARY_DIR}/probe.cpp" @ONLY)
add_executable(pelican_symbol_probe "${CMAKE_CURRENT_BINARY_DIR}/probe.cpp")
set(fixture_dist "${CMAKE_CURRENT_BINARY_DIR}/dist_debug")
set_target_properties(pelican_symbol_probe PROPERTIES
    CXX_STANDARD 20
    RUNTIME_OUTPUT_DIRECTORY "${fixture_dist}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${fixture_dist}"
    PDB_OUTPUT_DIRECTORY "${fixture_dist}"
    PDB_OUTPUT_DIRECTORY_DEBUG "${fixture_dist}")
target_link_options(pelican_symbol_probe PRIVATE /DEBUG:FULL /INCREMENTAL:NO)

if(ARCHIVE_ENABLED)
  include("${MODULE_PATH}")
  pelican_preserve_crash_symbols(
      TARGET pelican_symbol_probe
      ARCHIVE_ROOT "${ARCHIVE_ROOT}"
      SOURCE_COMMIT "${SOURCE_COMMIT}"
      MAX_GENERATIONS "${MAX_GENERATIONS}")
endif()
