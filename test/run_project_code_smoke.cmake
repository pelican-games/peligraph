# Project-code smoke.
#
# Run from the repository root:
#   cmake -DPELICAN_PROJECT_CODE_SMOKE_CONFIG=Debug -P test/run_project_code_smoke.cmake
#
# This script intentionally is not registered with ctest. It configures and
# builds a player with -DPELICAN_PROJECT=projects/example, then runs a small
# headless project through that project-code-enabled binary and verifies PNG
# output.

if(NOT DEFINED PELICAN_PROJECT_CODE_SMOKE_CONFIG)
    set(PELICAN_PROJECT_CODE_SMOKE_CONFIG Debug)
endif()

get_filename_component(SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(BUILD_DIR "${SOURCE_DIR}/build-project-example")
set(ARTIFACT_ROOT "${SOURCE_DIR}/build-project-example-artifacts")

if(WIN32)
    set(EXE_SUFFIX ".exe")
else()
    set(EXE_SUFFIX "")
endif()

function(run_process label)
    execute_process(
        COMMAND ${ARGN}
        WORKING_DIRECTORY "${SOURCE_DIR}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE stdout
        ERROR_VARIABLE stderr
    )

    if(NOT result EQUAL 0)
        message(FATAL_ERROR "${label} failed with ${result}\nstdout:\n${stdout}\nstderr:\n${stderr}")
    endif()

    set("${label}_STDOUT" "${stdout}" PARENT_SCOPE)
    set("${label}_STDERR" "${stderr}" PARENT_SCOPE)
endfunction()

function(find_built_executable build_dir target_name out_path)
    file(GLOB_RECURSE candidates LIST_DIRECTORIES false "${build_dir}/*${target_name}${EXE_SUFFIX}")
    set(found "")
    foreach(candidate IN LISTS candidates)
        get_filename_component(filename "${candidate}" NAME)
        if(filename STREQUAL "${target_name}${EXE_SUFFIX}")
            set(found "${candidate}")
            break()
        endif()
    endforeach()

    if(found STREQUAL "")
        message(FATAL_ERROR "failed to find ${target_name}${EXE_SUFFIX} under ${build_dir}")
    endif()
    set("${out_path}" "${found}" PARENT_SCOPE)
endfunction()

file(REMOVE_RECURSE "${BUILD_DIR}" "${ARTIFACT_ROOT}")

run_process(
    configure_project_code
    "${CMAKE_COMMAND}"
        -S "${SOURCE_DIR}"
        -B "${BUILD_DIR}"
        -DSKIP_DEVSTUDIO=ON
        -DPELICAN_PROJECT=projects/example
        "-DCMAKE_BUILD_TYPE=${PELICAN_PROJECT_CODE_SMOKE_CONFIG}"
)

run_process(
    build_project_code
    "${CMAKE_COMMAND}"
        --build "${BUILD_DIR}"
        --config "${PELICAN_PROJECT_CODE_SMOKE_CONFIG}"
)

find_built_executable("${BUILD_DIR}" "pelican_player" player)

file(MAKE_DIRECTORY
    "${ARTIFACT_ROOT}/project/assets/models"
    "${ARTIFACT_ROOT}/project/input"
    "${ARTIFACT_ROOT}/project/passes"
    "${ARTIFACT_ROOT}/project/scenes"
    "${ARTIFACT_ROOT}/project/ui"
    "${ARTIFACT_ROOT}/frames"
)

file(COPY "${SOURCE_DIR}/test/fixtures/ground.glb" DESTINATION "${ARTIFACT_ROOT}/project/assets/models")
file(RENAME "${ARTIFACT_ROOT}/project/assets/models/ground.glb" "${ARTIFACT_ROOT}/project/assets/models/character.glb")
configure_file("${SOURCE_DIR}/projects/example/input/actions.json" "${ARTIFACT_ROOT}/project/input/actions.json" COPYONLY)
configure_file("${SOURCE_DIR}/projects/example/passes/main_rendering_config.json" "${ARTIFACT_ROOT}/project/passes/main_rendering_config.json" COPYONLY)

file(WRITE "${ARTIFACT_ROOT}/project/project.json" [=[
{
  "schema": "pelican.project",
  "version": 1,
  "name": "project-code-smoke",
  "engine_min_version": "0.1.0",
  "basic_config": {
    "window_title": "Project Code Smoke",
    "window_size": {"width": 160, "height": 90},
    "fullscreen": false,
    "framerate": 30,
    "camera": {"fov_y": 45.0, "near": 0.1, "far": 1000.0, "up": [0.0, 1.0, 0.0]},
    "default_scene_id": "default_scene",
    "scene_data_json": "scenes/main.scene.json",
    "asset_data_json": "assets/asset_data.json",
    "rendering_config_json": "passes/main_rendering_config.json",
    "default_rendering_pass": "main_render",
    "ui_config_json": "ui/ui_overlay.json",
    "input_actions_json": "input/actions.json"
  }
}
]=])

file(WRITE "${ARTIFACT_ROOT}/project/assets/asset_data.json" [=[
{
  "models": [
    {"name": "character", "path": "assets/models/character.glb"}
  ]
}
]=])

file(WRITE "${ARTIFACT_ROOT}/project/scenes/main.scene.json" [=[
{
  "schema": "pelican.scene",
  "version": 1,
  "scenes": {
    "default_scene": {
      "objects": [
        {
          "components": [
            {
              "name": "transform",
              "pos": [0.0, 0.0, -3.0],
              "rotation": [0.0, 0.0, 0.0, 1.0],
              "scale": [1.0, 1.0, 1.0]
            },
            {"name": "camera"}
          ]
        }
      ]
    }
  }
}
]=])

file(WRITE "${ARTIFACT_ROOT}/project/ui/ui_overlay.json" "{\"images\":[]}\n")

execute_process(
    COMMAND "${player}"
        --headless
        --project "${ARTIFACT_ROOT}/project"
        --frames 3
        --size 160x90
        --fps 30
        --render-out "${ARTIFACT_ROOT}/frames/out.png"
    WORKING_DIRECTORY "${SOURCE_DIR}"
    RESULT_VARIABLE run_result
    OUTPUT_VARIABLE run_stdout
    ERROR_VARIABLE run_stderr
)

if(NOT run_result EQUAL 0)
    message(FATAL_ERROR "project-code headless run failed with ${run_result}\nstdout:\n${run_stdout}\nstderr:\n${run_stderr}")
endif()
if(run_stdout MATCHES "Validation Error|VUID-" OR run_stderr MATCHES "Validation Error|VUID-")
    message(FATAL_ERROR "project-code headless run emitted Vulkan validation errors\nstdout:\n${run_stdout}\nstderr:\n${run_stderr}")
endif()

set(output_png "${ARTIFACT_ROOT}/frames/out.png")
if(NOT EXISTS "${output_png}")
    message(FATAL_ERROR "project-code smoke output missing: ${output_png}")
endif()
file(SIZE "${output_png}" output_png_size)
if(output_png_size EQUAL 0)
    message(FATAL_ERROR "project-code smoke output is empty: ${output_png}")
endif()

message(STATUS "project-code smoke passed: ${output_png}")
