# IL-1 intro link tests. test/CMakeLists.txt includes this only with
# PELICAN_WITH_INTRO_LINK=ON; the OFF build has neither these targets nor the
# pelican_cli sources they exercise.

set(PELICAN_INTRO_LINK_HEADER_DIR "${CMAKE_SOURCE_DIR}/tools/intro_link")
set(PELICAN_INTRO_LINK_FIXTURE "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/intro_link")

# pil_eval against ScalarCurve::sample. curve.cpp is compiled in directly so
# the test needs only export.hpp's PELICAN_API and the standard library.
add_executable(pelican_test_intro_link_eval
  intro_link_eval_test.cpp
  ${CMAKE_SOURCE_DIR}/src/core/userpublic/animation/curve.cpp
)
set_target_properties(pelican_test_intro_link_eval PROPERTIES CXX_STANDARD 20)
target_include_directories(pelican_test_intro_link_eval PRIVATE
  ${CMAKE_SOURCE_DIR}/src/core/userpublic
  ${PELICAN_INTRO_LINK_HEADER_DIR}
)
add_test(NAME intro_link_eval_matches_engine COMMAND pelican_test_intro_link_eval)
set_tests_properties(intro_link_eval_matches_engine PROPERTIES LABELS "IL-1;cpu")

find_program(PELICAN_INTRO_LINK_GLSLC glslc
  HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin")
if(NOT PELICAN_INTRO_LINK_GLSLC)
  message(STATUS "IL-1: glslc not found; devcli_intro_link_bake and devcli_intro_link_serve are not registered")
  return()
endif()

file(GLOB_RECURSE pelican_intro_link_fixture_files CONFIGURE_DEPENDS
  "${PELICAN_INTRO_LINK_FIXTURE}/*")

# The development-form client (C, with the socket implementation).
add_executable(pelican_test_intro_link_client intro_link_test_client.c)
set_target_properties(pelican_test_intro_link_client PROPERTIES C_STANDARD 99)
target_include_directories(pelican_test_intro_link_client PRIVATE ${PELICAN_INTRO_LINK_HEADER_DIR})
if(WIN32)
  target_link_libraries(pelican_test_intro_link_client PRIVATE ws2_32)
endif()

# The release-form probe over the header bake writes at build time.
set(pelican_intro_link_baked_dir "${CMAKE_CURRENT_BINARY_DIR}/intro_link_baked")
set(pelican_intro_link_baked_header "${pelican_intro_link_baked_dir}/pelican_intro_link_fixture_baked.h")
add_custom_command(
  OUTPUT "${pelican_intro_link_baked_header}"
  COMMAND $<TARGET_FILE:pelican_cli> intro-link bake "${PELICAN_INTRO_LINK_FIXTURE}"
          --out "${pelican_intro_link_baked_header}" --glslc "${PELICAN_INTRO_LINK_GLSLC}"
  DEPENDS pelican_cli ${pelican_intro_link_fixture_files}
  COMMENT "IL-1: pelican_cli intro-link bake (fixture)"
  VERBATIM
)
add_executable(pelican_test_intro_link_baked intro_link_baked_probe.c "${pelican_intro_link_baked_header}")
set_target_properties(pelican_test_intro_link_baked PROPERTIES C_STANDARD 99)
target_include_directories(pelican_test_intro_link_baked PRIVATE
  ${pelican_intro_link_baked_dir}
  ${PELICAN_INTRO_LINK_HEADER_DIR}
)

add_test(
  NAME devcli_intro_link_bake
  COMMAND ${CMAKE_COMMAND}
    -DCLI=$<TARGET_FILE:pelican_cli>
    -DGLSLC=${PELICAN_INTRO_LINK_GLSLC}
    -DFIXTURE=${PELICAN_INTRO_LINK_FIXTURE}
    -DOUT_DIR=${CMAKE_BINARY_DIR}/test_artifacts/devcli_intro_link_bake
    -P ${CMAKE_CURRENT_SOURCE_DIR}/run_devcli_intro_link_bake.cmake
)
set_tests_properties(devcli_intro_link_bake PROPERTIES LABELS "IL-1;cpu")

add_test(
  NAME devcli_intro_link_serve
  COMMAND ${CMAKE_COMMAND}
    -DCLI=$<TARGET_FILE:pelican_cli>
    -DGLSLC=${PELICAN_INTRO_LINK_GLSLC}
    -DCLIENT=$<TARGET_FILE:pelican_test_intro_link_client>
    -DBAKED_PROBE=$<TARGET_FILE:pelican_test_intro_link_baked>
    -DFIXTURE=${PELICAN_INTRO_LINK_FIXTURE}
    -DOUT_DIR=${CMAKE_BINARY_DIR}/test_artifacts/devcli_intro_link_serve
    -P ${CMAKE_CURRENT_SOURCE_DIR}/run_devcli_intro_link_serve.cmake
)
set_tests_properties(devcli_intro_link_serve PROPERTIES LABELS "IL-1;cpu" TIMEOUT 120)
