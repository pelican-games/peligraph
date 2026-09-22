if(NOT DEFINED TEST_BINARY_DIR)
    message(FATAL_ERROR "TEST_BINARY_DIR is required")
endif()
include("${CMAKE_CURRENT_LIST_DIR}/catalog_game_logic.cmake")
file(MAKE_DIRECTORY "${TEST_BINARY_DIR}")
set(PELICAN_WITH_ANIMATION OFF)

if(DEFINED NEGATIVE_CASE)
    set(invalid_bad_json "{")
    set(invalid_bad_root "[]")
    set(invalid_not_array "{\"requires\": {}}")
    set(invalid_not_string "{\"requires\": [1]}")
    set(invalid_empty_name "{\"requires\": [\"\"]}")
    set(invalid_unknown_after_unavailable "{\"requires\": [\"animation\", \"future\"]}")
    file(WRITE "${TEST_BINARY_DIR}/invalid.json" "${invalid_${NEGATIVE_CASE}}")
    pelican_unavailable_project_requirement("${TEST_BINARY_DIR}/invalid.json" name option)
    message(FATAL_ERROR "negative fixture was accepted")
endif()

set(project "${TEST_BINARY_DIR}/corpus/needs_animation")
file(MAKE_DIRECTORY "${project}/code")
file(WRITE "${project}/project.json" "{\"requires\": [\"animation\"]}")
file(WRITE "${project}/code/CMakeLists.txt"
    "set_property(GLOBAL PROPERTY CATALOG_CODE_INCLUDED TRUE)\n")
pelican_add_catalog_game_logic("${project}" needs_animation target)
get_property(included GLOBAL PROPERTY CATALOG_CODE_INCLUDED)
if(target OR included)
    message(FATAL_ERROR "An unavailable project's code entered the build")
endif()
set(PELICAN_WITH_ANIMATION ON)
pelican_add_catalog_game_logic("${project}" needs_animation target)
get_property(included GLOBAL PROPERTY CATALOG_CODE_INCLUDED)
if(NOT target STREQUAL "pelican_catalog_needs_animation_game_logic" OR NOT included)
    message(FATAL_ERROR "An available project's code was omitted")
endif()
pelican_project_requirement_arguments(arguments)
list(FIND arguments "-DPELICAN_WITH_ANIMATION=ON" enabled_argument)
if(enabled_argument EQUAL -1)
    message(FATAL_ERROR "The runtime gate did not receive the build's requirement options")
endif()
file(WRITE "${TEST_BINARY_DIR}/without_requires.json" "{}")
pelican_unavailable_project_requirement("${TEST_BINARY_DIR}/without_requires.json" name option)
if(name OR option)
    message(FATAL_ERROR "A project without requires was rejected")
endif()

foreach(case IN ITEMS bad_json bad_root not_array not_string empty_name unknown_after_unavailable)
    execute_process(COMMAND "${CMAKE_COMMAND}"
        "-DTEST_BINARY_DIR=${TEST_BINARY_DIR}/${case}" "-DNEGATIVE_CASE=${case}"
        -P "${CMAKE_CURRENT_LIST_FILE}"
        RESULT_VARIABLE result OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
    if(result EQUAL 0 OR NOT "${stdout}${stderr}" MATCHES "project.requires_unknown@1")
        message(FATAL_ERROR "Invalid requirement fixture ${case} was not rejected by name: ${stdout}${stderr}")
    endif()
endforeach()

# The real runtime corpus walker must make the same selection as configure.
file(WRITE "${project}/catalog_smoke.json" "{\"editor\": {}}")
execute_process(COMMAND "${CMAKE_COMMAND}"
    "-DPROJECTS_DIR=${TEST_BINARY_DIR}/corpus" -DPELICAN_WITH_ANIMATION=OFF
    -DPLAYER=unused -DPNG_NONUNIFORM_CHECK=unused -DSTUDIO_ARGUMENTS_PROBE=unused
    -DGAME_LOGIC_DIR=unused -DGAME_LOGIC_PREFIX=unused -DGAME_LOGIC_SUFFIX=unused
    "-DOUT_DIR=${TEST_BINARY_DIR}/runtime-output"
    -P "${CMAKE_CURRENT_LIST_DIR}/run_project_catalog_smoke.cmake"
    RESULT_VARIABLE result OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
if(NOT result EQUAL 0 OR NOT "${stdout}${stderr}" MATCHES "skip needs_animation: requires animation" OR
   NOT "${stdout}${stderr}" MATCHES "skip editor lane: needs_animation requires animation")
    message(FATAL_ERROR "Configure/runtime corpus selection disagreed: ${stdout}${stderr}")
endif()
