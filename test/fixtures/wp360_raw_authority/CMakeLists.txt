cmake_minimum_required(VERSION 3.23)
project(pelican_wp360_raw_authority LANGUAGES CXX)

if(NOT PELICAN_SOURCE_DIR OR NOT NLOHMANN_INCLUDE_DIR)
  message(FATAL_ERROR "PELICAN_SOURCE_DIR and NLOHMANN_INCLUDE_DIR are required")
endif()

function(wp360_raw_target name source)
  add_library(${name} OBJECT ${source})
  set_target_properties(${name} PROPERTIES CXX_STANDARD 20)
  target_include_directories(${name} PRIVATE
    ${PELICAN_SOURCE_DIR}/src
    ${NLOHMANN_INCLUDE_DIR}
  )
endfunction()

# This target explicitly opts into the authoring authority interface.
wp360_raw_target(wp360_raw_authority_allowed allowed.cpp)

# These target names are the three forbidden runtime readers from the WP360
# contract.  They see AuthoringSceneDocument but no authority interface.
wp360_raw_target(wp360_raw_authority_rejected_camera rejected_camera.cpp)
wp360_raw_target(wp360_raw_authority_rejected_reload rejected_reload.cpp)
wp360_raw_target(wp360_raw_authority_rejected_query rejected_query.cpp)
