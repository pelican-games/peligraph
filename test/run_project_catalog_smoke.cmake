if(NOT DEFINED PROJECTS_DIR OR "${PROJECTS_DIR}" STREQUAL "")
    message(STATUS
        "PELICAN_PROJECT_CATALOG_SMOKE_SKIP_NO_PROJECTS_DIR: "
        "configure PELICAN_TEST_PROJECTS_DIR to select a smoke-test corpus")
    return()
endif()

if(NOT DEFINED PLAYER)
    message(FATAL_ERROR "PLAYER is required")
endif()
if(NOT DEFINED PNG_NONUNIFORM_CHECK)
    message(FATAL_ERROR "PNG_NONUNIFORM_CHECK is required")
endif()
if(NOT DEFINED OUT_DIR)
    message(FATAL_ERROR "OUT_DIR is required")
endif()

# Discover at test execution time so a newly added project.json is covered
# without maintaining a second, hand-written project catalog in CMake.
file(GLOB_RECURSE project_manifests LIST_DIRECTORIES false
    "${PROJECTS_DIR}/*/project.json")
list(SORT project_manifests)
list(LENGTH project_manifests project_count)
if(project_count EQUAL 0)
    message(FATAL_ERROR "no projects were discovered below ${PROJECTS_DIR}")
endif()

file(REMOVE_RECURSE "${OUT_DIR}")
file(MAKE_DIRECTORY "${OUT_DIR}")

foreach(project_manifest IN LISTS project_manifests)
    get_filename_component(project_dir "${project_manifest}" DIRECTORY)
    file(RELATIVE_PATH project_relative "${PROJECTS_DIR}" "${project_dir}")
    string(REPLACE "/" "_" output_stem "${project_relative}")
    string(REPLACE "\\" "_" output_stem "${output_stem}")
    set(output_png "${OUT_DIR}/${output_stem}.png")

    execute_process(
        COMMAND "${PLAYER}"
            --headless
            --project "${project_dir}"
            --frames 2
            --size 160x90
            --fps 30
            --render-out "${output_png}"
        RESULT_VARIABLE player_result
        OUTPUT_VARIABLE player_stdout
        ERROR_VARIABLE player_stderr
        TIMEOUT 180
    )
    if(NOT player_result EQUAL 0)
        message(FATAL_ERROR
            "project '${project_relative}' failed in pelican_player with ${player_result}\n"
            "stdout:\n${player_stdout}\nstderr:\n${player_stderr}")
    endif()
    if(player_stdout MATCHES "Validation Error|VUID-" OR
       player_stderr MATCHES "Validation Error|VUID-")
        message(FATAL_ERROR
            "project '${project_relative}' emitted Vulkan validation errors\n"
            "stdout:\n${player_stdout}\nstderr:\n${player_stderr}")
    endif()
    if(NOT EXISTS "${output_png}")
        message(FATAL_ERROR
            "project '${project_relative}' did not render ${output_png}")
    endif()

    execute_process(
        COMMAND "${PNG_NONUNIFORM_CHECK}" "${output_png}"
        RESULT_VARIABLE image_result
        OUTPUT_VARIABLE image_stdout
        ERROR_VARIABLE image_stderr
    )
    if(NOT image_result EQUAL 0)
        message(FATAL_ERROR
            "project '${project_relative}' rendered an invalid smoke image\n"
            "stdout:\n${image_stdout}\nstderr:\n${image_stderr}")
    endif()
    string(STRIP "${image_stdout}" image_summary)
    message(STATUS "project '${project_relative}' passed: ${image_summary}")
endforeach()

message(STATUS "all ${project_count} discovered projects passed headless smoke")
