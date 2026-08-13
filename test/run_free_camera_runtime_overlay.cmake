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
    "${project_dir}/input/profiles"
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
    "ui_config_json": "ui/empty.json",
    "input_actions_json": "input/actions.json",
    "input_profiles": {"keyboard": "input/profiles/keyboard.json"},
    "input_profile": "keyboard"
  }
}
]=])
file(WRITE "${project_dir}/assets/asset_data.json"
    "{\"schema\":\"pelican.asset_data\",\"version\":1,\"models\":[]}\n")
file(WRITE "${project_dir}/ui/empty.json"
    "{\"schema\":\"pelican.ui\",\"version\":1,\"key\":\"empty\",\"root\":{\"id\":\"root\",\"type\":\"panel\"}}\n")
file(WRITE "${project_dir}/input/actions.json" [=[
{
  "schema": "pelican.input_actions",
  "version": 1,
  "action_sets": [{
    "name": "gameplay",
    "actions": [
      {"name": "move", "type": "axis2"},
      {"name": "jump", "type": "button"}
    ]
  }]
}
]=])
file(WRITE "${project_dir}/input/profiles/keyboard.json" [=[
{
  "schema": "pelican.input_profile",
  "version": 1,
  "name": "keyboard",
  "bindings": [
    {"action": "move", "binding": "kbd:wasd"},
    {"action": "jump", "binding": "kbd:space"}
  ]
}
]=])
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

set(blender_replay "${OUT_DIR}/blender.input_seq.jsonl")
file(WRITE "${blender_replay}"
    "{\"schema\":\"pelican.input_seq\",\"version\":1,\"fps\":30}\n"
    "{\"frame\":0}\n"
    "{\"event_seq\":0,\"type\":\"cursor_move\",\"x\":0,\"y\":0}\n"
    "{\"frame\":1}\n"
    "{\"event_seq\":1,\"type\":\"button\",\"code\":\"MouseMiddle\",\"pressed\":true}\n"
    "{\"event_seq\":2,\"type\":\"cursor_move\",\"x\":10,\"y\":0}\n"
    "{\"frame\":2}\n"
    "{\"event_seq\":3,\"type\":\"button\",\"code\":\"MouseMiddle\",\"pressed\":false}\n"
    "{\"frame\":3}\n"
    "{\"event_seq\":4,\"type\":\"button\",\"code\":\"LeftShift\",\"pressed\":true}\n"
    "{\"event_seq\":5,\"type\":\"button\",\"code\":\"MouseMiddle\",\"pressed\":true}\n"
    "{\"event_seq\":6,\"type\":\"cursor_move\",\"x\":20,\"y\":0}\n"
    "{\"frame\":4}\n"
    "{\"event_seq\":7,\"type\":\"button\",\"code\":\"MouseMiddle\",\"pressed\":false}\n"
    "{\"event_seq\":8,\"type\":\"button\",\"code\":\"LeftShift\",\"pressed\":false}\n"
    "{\"frame\":5}\n"
    "{\"event_seq\":9,\"type\":\"button\",\"code\":\"LeftControl\",\"pressed\":true}\n"
    "{\"event_seq\":10,\"type\":\"button\",\"code\":\"MouseMiddle\",\"pressed\":true}\n"
    "{\"event_seq\":11,\"type\":\"cursor_move\",\"x\":20,\"y\":-10}\n"
    "{\"frame\":6}\n"
    "{\"event_seq\":12,\"type\":\"button\",\"code\":\"MouseMiddle\",\"pressed\":false}\n"
    "{\"event_seq\":13,\"type\":\"button\",\"code\":\"LeftControl\",\"pressed\":false}\n"
    "{\"frame\":7}\n"
    "{\"event_seq\":14,\"type\":\"scroll\",\"x\":0,\"y\":1}\n"
    "{\"frame\":8}\n"
)

set(unity_replay "${OUT_DIR}/unity.input_seq.jsonl")
file(WRITE "${unity_replay}"
    "{\"schema\":\"pelican.input_seq\",\"version\":1,\"fps\":30}\n"
    "{\"frame\":0}\n"
    "{\"event_seq\":0,\"type\":\"cursor_move\",\"x\":0,\"y\":0}\n"
    "{\"frame\":1}\n"
    "{\"event_seq\":1,\"type\":\"button\",\"code\":\"LeftAlt\",\"pressed\":true}\n"
    "{\"event_seq\":2,\"type\":\"button\",\"code\":\"MouseLeft\",\"pressed\":true}\n"
    "{\"event_seq\":3,\"type\":\"cursor_move\",\"x\":10,\"y\":0}\n"
    "{\"frame\":2}\n"
    "{\"event_seq\":4,\"type\":\"button\",\"code\":\"MouseLeft\",\"pressed\":false}\n"
    "{\"event_seq\":5,\"type\":\"button\",\"code\":\"LeftAlt\",\"pressed\":false}\n"
    "{\"frame\":3}\n"
    "{\"event_seq\":6,\"type\":\"button\",\"code\":\"MouseMiddle\",\"pressed\":true}\n"
    "{\"event_seq\":7,\"type\":\"cursor_move\",\"x\":20,\"y\":0}\n"
    "{\"frame\":4}\n"
    "{\"event_seq\":8,\"type\":\"button\",\"code\":\"MouseMiddle\",\"pressed\":false}\n"
    "{\"frame\":5}\n"
    "{\"event_seq\":9,\"type\":\"button\",\"code\":\"LeftAlt\",\"pressed\":true}\n"
    "{\"event_seq\":10,\"type\":\"button\",\"code\":\"MouseRight\",\"pressed\":true}\n"
    "{\"event_seq\":11,\"type\":\"cursor_move\",\"x\":20,\"y\":-10}\n"
    "{\"frame\":6}\n"
    "{\"event_seq\":12,\"type\":\"button\",\"code\":\"MouseRight\",\"pressed\":false}\n"
    "{\"event_seq\":13,\"type\":\"button\",\"code\":\"LeftAlt\",\"pressed\":false}\n"
    "{\"frame\":7}\n"
    "{\"event_seq\":14,\"type\":\"scroll\",\"x\":0,\"y\":1}\n"
    "{\"frame\":8}\n"
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

function(run_bake output replay)
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

function(assert_navigation output preset)
    file(STRINGS "${output}" lines)
    list(GET lines 0 header)
    string(JSON camera_name GET "${header}" objects 0)
    if(NOT camera_name MATCHES "^__pelican_runtime_free_camera")
        message(FATAL_ERROR "${preset} did not select the runtime camera: ${header}")
    endif()

    foreach(frame RANGE 0 7)
        math(EXPR line_index "${frame} + 1")
        list(GET lines ${line_index} sample)
        string(JSON frame_${frame}_pos GET "${sample}" transforms 0 pos)
        string(JSON frame_${frame}_pos_y GET "${sample}" transforms 0 pos 1)
        string(JSON frame_${frame}_rot GET "${sample}" transforms 0 rot)
    endforeach()

    if(frame_0_pos STREQUAL frame_1_pos OR frame_0_rot STREQUAL frame_1_rot)
        message(FATAL_ERROR "${preset} orbit binding did not change position and rotation:\n${lines}")
    endif()
    if(frame_2_pos STREQUAL frame_3_pos OR NOT frame_2_rot STREQUAL frame_3_rot)
        message(FATAL_ERROR "${preset} pan binding did not translate without rotating:\n${lines}")
    endif()
    if(frame_4_pos STREQUAL frame_5_pos OR
       NOT frame_4_pos_y STREQUAL frame_5_pos_y)
        message(FATAL_ERROR "${preset} drag zoom binding did not move without rotating:\n${lines}")
    endif()
    if(frame_6_pos STREQUAL frame_7_pos OR
       NOT frame_6_pos_y STREQUAL frame_7_pos_y)
        message(FATAL_ERROR "${preset} wheel binding did not zoom without rotating:\n${lines}")
    endif()
endfunction()

authored_digest(authored_before_digest)

set(authored_bake "${OUT_DIR}/authored.transform_seq.jsonl")
run_bake("${authored_bake}" "${blender_replay}")
file(STRINGS "${authored_bake}" authored_lines)
list(GET authored_lines 0 authored_header)
list(GET authored_lines 1 authored_first)
list(GET authored_lines 9 authored_last)
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

set(default_bake "${OUT_DIR}/default.transform_seq.jsonl")
run_bake("${default_bake}" "${blender_replay}"
    --input-profile keyboard --free-camera)
assert_navigation("${default_bake}" "default Blender")

set(blender_bake "${OUT_DIR}/blender.transform_seq.jsonl")
run_bake("${blender_bake}" "${blender_replay}"
    --input-profile keyboard --free-camera blender)
assert_navigation("${blender_bake}" "Blender")

file(SHA256 "${default_bake}" default_bake_digest)
file(SHA256 "${blender_bake}" blender_bake_digest)
if(NOT default_bake_digest STREQUAL blender_bake_digest)
    message(FATAL_ERROR "--free-camera did not default to the Blender preset")
endif()

set(unity_bake "${OUT_DIR}/unity.transform_seq.jsonl")
run_bake("${unity_bake}" "${unity_replay}"
    --input-profile keyboard --free-camera unity)
assert_navigation("${unity_bake}" "Unity")

set(recorded_input "${OUT_DIR}/recorded.input_seq.jsonl")
execute_process(
    COMMAND "${PLAYER}"
        --headless
        --project "${project_dir}"
        --size 64x64
        --frames 3
        --record-input "${recorded_input}"
        --input-profile keyboard
        --free-camera
    WORKING_DIRECTORY "${PLAYER_DIR}"
    RESULT_VARIABLE record_result
    OUTPUT_VARIABLE record_stdout
    ERROR_VARIABLE record_stderr
)
if(NOT record_result EQUAL 0 OR NOT EXISTS "${recorded_input}")
    message(FATAL_ERROR
        "input recording with the free-camera overlay failed: ${record_result}\n${record_stdout}\n${record_stderr}")
endif()
execute_process(
    COMMAND "${PLAYER}"
        --headless
        --project "${project_dir}"
        --size 64x64
        --replay "${recorded_input}"
        --input-profile keyboard
        --free-camera
    WORKING_DIRECTORY "${PLAYER_DIR}"
    RESULT_VARIABLE replay_result
    OUTPUT_VARIABLE replay_stdout
    ERROR_VARIABLE replay_stderr
)
if(NOT replay_result EQUAL 0)
    message(FATAL_ERROR
        "input replay with the free-camera overlay failed: ${replay_result}\n${replay_stdout}\n${replay_stderr}")
endif()
execute_process(
    COMMAND "${PLAYER}"
        --headless
        --project "${project_dir}"
        --size 64x64
        --frames 1
        --input-profile keyboard
        --input-action-overlay engine://input/overlays/free_camera_blender.json
    WORKING_DIRECTORY "${PLAYER_DIR}"
    RESULT_VARIABLE public_overlay_result
    OUTPUT_VARIABLE public_overlay_stdout
    ERROR_VARIABLE public_overlay_stderr
)
if(NOT public_overlay_result EQUAL 0)
    message(FATAL_ERROR
        "public input-action overlay CLI failed: ${public_overlay_result}\n${public_overlay_stdout}\n${public_overlay_stderr}")
endif()

execute_process(
    COMMAND "${PLAYER}" --headless --project "${project_dir}" --free-camera unknown
    WORKING_DIRECTORY "${PLAYER_DIR}"
    RESULT_VARIABLE invalid_result
    OUTPUT_VARIABLE invalid_stdout
    ERROR_VARIABLE invalid_stderr
)
if(invalid_result EQUAL 0 OR
   NOT invalid_stderr MATCHES "--free-camera must be one of: blender, unity")
    message(FATAL_ERROR
        "unknown free-camera preset was not rejected clearly:\n${invalid_stdout}\n${invalid_stderr}")
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
