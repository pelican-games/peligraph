cmake_minimum_required(VERSION 3.20)
project(wp358_schema_leaf_consumer LANGUAGES CXX)

if(NOT DEFINED PELICAN_SOURCE_DIR OR NOT DEFINED NLOHMANN_INCLUDE_DIR)
  message(FATAL_ERROR
    "PELICAN_SOURCE_DIR and NLOHMANN_INCLUDE_DIR are required")
endif()

# Deliberately provide only nlohmann. No Pelican core include or target is
# visible to this independent project.
add_library(nlohmann_json INTERFACE)
target_include_directories(nlohmann_json INTERFACE "${NLOHMANN_INCLUDE_DIR}")
add_subdirectory("${PELICAN_SOURCE_DIR}/src/schema" schema_leaf)

get_target_property(leaf_includes pelican_schema_leaf
  INTERFACE_INCLUDE_DIRECTORIES)
foreach(include_dir IN LISTS leaf_includes)
  if(include_dir MATCHES "[/\\\\]src[/\\\\]core([/\\\\]|$)")
    message(FATAL_ERROR "leaf exposes a core include directory: ${include_dir}")
  endif()
endforeach()

foreach(leaf_source IN ITEMS schema.hpp schema.cpp CMakeLists.txt)
  file(READ "${PELICAN_SOURCE_DIR}/src/schema/${leaf_source}" leaf_text)
  if(leaf_text MATCHES "(\\.\\./core|src/core|[/<\"]core[/>\"])")
    message(FATAL_ERROR "leaf source references core: ${leaf_source}")
  endif()
endforeach()

add_executable(schema_leaf_consumer main.cpp)
set_target_properties(schema_leaf_consumer PROPERTIES CXX_STANDARD 20)
target_link_libraries(schema_leaf_consumer PRIVATE pelican_schema_leaf)

