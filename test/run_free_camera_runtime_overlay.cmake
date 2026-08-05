if(NOT DEFINED PLAYER)
    message(FATAL_ERROR "PLAYER is required")
endif()
if(NOT DEFINED PLAYER_DIR)
    message(FATAL_ERROR "PLAYER_DIR is required")
endif()
if(NOT DEFINED EXAMPLE_PASS)
    message(FATAL_ERROR "EXAMPLE_PASS is required")
endif()
if(NOT DEFINED OUT_DIR)
    message(FATAL_ERROR "OUT_DIR is required")
endif()

set(project_dir "${OUT_DIR}/project")
file(REMOVE_RECURSE "${OUT_DIR}")
file(MAKE_DIRECTORY
    "${project_dir}/assets"
    "${project_dir}/passes"
    "${project_dir}/scenes"
    "${project_dir}/ui"
)
configure_file("${EXAMPLE_PASS}" "${project_dir}/passes/main.json" COPYONLY)

file(WRITE "${project_dir}/project.json" [=[
{
  "schema": "pelican.project",
  "version": 1,
  "name": "free-camera-runtime-overlay",
  "engine_min_version": "0.1.0",
  "basic_config": {
    "window_title": "Free Camera Runtime Overlay",
    "window_size": {"width": 64, "height": 64},
    "fullscreen": false,
    "framerate": 30,
    "camera": {"yfov": 0.7853981633974483, "znear": 0.1, "zfar": 100.0, "up": [0.0, 1.0, 0.0]},
    "default_scene_id": "default_scene",
    "scene_data_json": "scenes/main.scene.json",
    "asset_data_json": "assets/asset_data.json",
    "rendering_config_json": "passes/main.json",
    "default_rendering_pass": "main_render",
    "ui_config_json": "ui/empty.json"
  }
}
]=])
file(WRITE "${project_dir}/assets/asset_data.json"
    "{\"schema\":\"pelican.asset_data\",\"version\":1,\"models\":[]}\n")
file(WRITE "${project_dir}/ui/empty.json"
    "{\"schema\":\"pelican.ui\",\"version\":1,\"key\":\"empty\",\"root\":{\"id\":\"root\",\"type\":\"panel\"}}\n")
file(WRITE "${project_dir}/scenes/main.scene.json" [=[
{
  "schema": "pelican.scene",
  "version": 1,
  "scenes": {
    "default_scene": {
      "objects": [
        {
          "name": "AuthoredCamera",
          "components": [
            {"name": "transform", "pos": [1.0, 2.0, -3.0], "rotation": [0.0, 0.0, 0.0, 1.0], "scale": [1.0, 1.0, 1.0]},
            {"name": "camera"}
          ]
        }
      ]
    }
  }
}
]=])

set(replay "${OUT_DIR}/free_camera.input_seq.jsonl")
file(WRITE "${replay}"
    "{\"schema\":\"pelican.input_seq\",\"version\":1,\"fps\":30}\n"
    "{\"frame\":0}\n"
    "{\"event_seq\":0,\"type\":\"button\",\"code\":\"W\",\"pressed\":true}\n"
    "{\"event_seq\":1,\"type\":\"button\",\"code\":\"ArrowRight\",\"pressed\":true}\n"
    "{\"frame\":1}\n"
    "{\"frame\":2}\n"
    "{\"event_seq\":2,\"type\":\"button\",\"code\":\"W\",\"pressed\":false}\n"
    "{\"event_seq\":3,\"type\":\"button\",\"code\":\"ArrowRight\",\"pressed\":false}\n"
)

function(project_digest output)
    file(GLOB_RECURSE files LIST_DIRECTORIES false RELATIVE "${project_dir}"
        "${project_dir}/*")
    list(SORT files)
    set(manifest "")
    foreach(relative IN LISTS files)
        file(SHA256 "${project_dir}/${relative}" hash)
        string(APPEND manifest "${relative}:${hash}\n")
    endforeach()
    string(SHA256 digest "${manifest}")
    set(${output} "${digest}" PARENT_SCOPE)
endfunction()

function(authored_digest output)
    file(GLOB_RECURSE files LIST_DIRECTORIES false RELATIVE "${project_dir}"
        "${project_dir}/*")
    list(SORT files)
    set(manifest "")
    foreach(relative IN LISTS files)
        if(relative MATCHES "^\\.pelican/")
            continue()
        endif()
        file(SHA256 "${project_dir}/${relative}" hash)
        string(APPEND manifest "${relative}:${hash}\n")
    endforeach()
    string(SHA256 digest "${manifest}")
    set(${output} "${digest}" PARENT_SCOPE)
endfunction()

function(run_bake output)
    execute_process(
        COMMAND "${PLAYER}"
            --headless
            --project "${project_dir}"
            --size 64x64
            --replay "${replay}"
            --bake-camera-output "${output}"
            ${ARGN}
        WORKING_DIRECTORY "${PLAYER_DIR}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE stdout
        ERROR_VARIABLE stderr
    )
    if(NOT result EQUAL 0)
        message(FATAL_ERROR
            "camera runtime overlay player failed: ${result}\nstdout:\n${stdout}\nstderr:\n${stderr}")
    endif()
    if(NOT EXISTS "${output}")
        message(FATAL_ERROR "camera runtime overlay player did not write ${output}")
    endif()
endfunction()

authored_digest(authored_before_digest)

set(authored_bake "${OUT_DIR}/authored.transform_seq.jsonl")
run_bake("${authored_bake}")
file(STRINGS "${authored_bake}" authored_lines)
list(GET authored_lines 0 authored_header)
list(GET authored_lines 1 authored_first)
list(GET authored_lines 3 authored_last)
string(JSON authored_name GET "${authored_header}" objects 0)
string(JSON authored_first_pos GET "${authored_first}" transforms 0 pos)
string(JSON authored_last_pos GET "${authored_last}" transforms 0 pos)
string(JSON authored_first_rot GET "${authored_first}" transforms 0 rot)
string(JSON authored_last_rot GET "${authored_last}" transforms 0 rot)
if(NOT authored_name STREQUAL "camera" OR
   NOT authored_first_pos STREQUAL authored_last_pos OR
   NOT authored_first_rot STREQUAL authored_last_rot)
    message(FATAL_ERROR
        "ordinary launch changed the existing scene camera behavior:\n${authored_lines}")
endif()

# The renderer's pre-existing shader cache is a normal player side effect.
# Establish that ordinary baseline first, then hash every byte under the
# project so the free-camera operation itself must be completely read-only.
project_digest(before_digest)

set(free_bake "${OUT_DIR}/free.transform_seq.jsonl")
run_bake("${free_bake}" --free-camera)
file(STRINGS "${free_bake}" free_lines)
list(GET free_lines 0 free_header)
list(GET free_lines 1 free_first)
list(GET free_lines 3 free_last)
string(JSON free_name GET "${free_header}" objects 0)
string(JSON free_first_pos GET "${free_first}" transforms 0 pos)
string(JSON free_last_pos GET "${free_last}" transforms 0 pos)
string(JSON free_first_rot GET "${free_first}" transforms 0 rot)
string(JSON free_last_rot GET "${free_last}" transforms 0 rot)
if(NOT free_name MATCHES "^__pelican_runtime_free_camera" OR
   free_first_pos STREQUAL free_last_pos OR
   free_first_rot STREQUAL free_last_rot)
    message(FATAL_ERROR
        "runtime free camera did not move and look from embedded input:\n${free_lines}")
endif()

project_digest(after_digest)
if(NOT before_digest STREQUAL after_digest)
    message(FATAL_ERROR
        "runtime free camera changed bytes under the user project: ${before_digest} -> ${after_digest}")
endif()
authored_digest(authored_after_digest)
if(NOT authored_before_digest STREQUAL authored_after_digest)
    message(FATAL_ERROR
        "player runs changed user-authored project bytes: ${authored_before_digest} -> ${authored_after_digest}")
endif()
