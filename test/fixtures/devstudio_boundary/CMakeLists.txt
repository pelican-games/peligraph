cmake_minimum_required(VERSION 3.13)
project(pelican_devstudio_boundary_fixture LANGUAGES NONE)

if(NOT PELICAN_SOURCE_DIR)
  message(FATAL_ERROR "PELICAN_SOURCE_DIR is required")
endif()

include(${PELICAN_SOURCE_DIR}/cmake/devstudio_link_boundary.cmake)

add_library(pelican_core INTERFACE)
add_library(engine_wrapper INTERFACE)
target_link_libraries(engine_wrapper INTERFACE pelican_core)

add_library(pelican_project INTERFACE)
add_library(pelican_studio INTERFACE)
target_link_libraries(pelican_studio INTERFACE pelican_project engine_wrapper)

pelican_assert_link_boundary(
  TARGET pelican_studio
  REQUIRED_DIRECT_TARGETS pelican_project
  FORBIDDEN_TARGETS pelican_core
)
