if(NOT DEFINED PLAYER OR NOT DEFINED PLAYER_DIR OR
   NOT DEFINED EXAMPLE_PASS OR NOT DEFINED OUT_DIR)
    message(FATAL_ERROR "PLAYER, PLAYER_DIR, EXAMPLE_PASS, and OUT_DIR are required")
endif()

file(REMOVE_RECURSE "${OUT_DIR}")
set(project_dir "${OUT_DIR}/project")
file(MAKE_DIRECTORY
    "${project_dir}/assets"
    "${project_dir}/passes"
    "${project_dir}/scenes"
    "${project_dir}/ui")
configure_file("${EXAMPLE_PASS}" "${project_dir}/passes/main.json" COPYONLY)
file(WRITE "${project_dir}/project.json" [=[
{
  "schema": "pelican.project",
  "version": 1,
  "name": "modal-transform-record-replay",
  "engine_min_version": "0.1.0",
  "basic_config": {
    "window_title": "Modal Transform Record Replay",
    "window_size": {"width": 640, "height": 640},
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
          "name": "Camera",
          "components": [
            {"name": "transform", "pos": [1.0, 2.0, -3.0], "rotation": [0.0, 0.0, 0.0, 1.0], "scale": [1.0, 1.0, 1.0]},
            {"name": "camera"}
          ]
        },
        {
          "name": "TransformTarget",
          "components": [
            {"name": "transform", "pos": [0.0, 0.0, 0.0], "rotation": [0.0, 0.0, 0.0, 1.0], "scale": [1.0, 1.0, 1.0]}
          ]
        }
      ]
    }
  }
}
]=])
set(recording "${OUT_DIR}/modal.input_seq.jsonl")
set(record_script "${OUT_DIR}/record.ndjson")
set(replay_script "${OUT_DIR}/replay.ndjson")
set(disabled_script "${OUT_DIR}/disabled.ndjson")
set(selection [=[{"kind":"declaration","scene_id":"default_scene","declaration_index":1}]=])

file(WRITE "${record_script}"
"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"set_gizmo\",\"params\":{\"selection\":${selection},\"mode\":\"translate\"}}\n"
"{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"inject_input\",\"params\":{\"events\":[{\"type\":\"key_down\",\"key\":\"G\"},{\"type\":\"mouse_move\",\"x\":100,\"y\":100}]}}\n"
"{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"step_frame\",\"params\":{}}\n"
"{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"inject_input\",\"params\":{\"events\":[{\"type\":\"key_up\",\"key\":\"G\"},{\"type\":\"key_down\",\"key\":\"X\"}]}}\n"
"{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"step_frame\",\"params\":{}}\n"
"{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"inject_input\",\"params\":{\"events\":[{\"type\":\"key_up\",\"key\":\"X\"},{\"type\":\"mouse_move\",\"x\":120,\"y\":100}]}}\n"
"{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"step_frame\",\"params\":{}}\n"
"{\"jsonrpc\":\"2.0\",\"id\":8,\"method\":\"inject_input\",\"params\":{\"events\":[{\"type\":\"key_down\",\"key\":\"Enter\"}]}}\n"
"{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"step_frame\",\"params\":{}}\n"
"{\"jsonrpc\":\"2.0\",\"id\":10,\"method\":\"get_modal_transform\",\"params\":{}}\n")

set(common_replay_script
"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"set_gizmo\",\"params\":{\"selection\":${selection},\"mode\":\"translate\"}}\n"
"{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"step_frame\",\"params\":{}}\n"
"{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"step_frame\",\"params\":{}}\n"
"{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"step_frame\",\"params\":{}}\n"
"{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"step_frame\",\"params\":{}}\n"
"{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"get_modal_transform\",\"params\":{}}\n")
file(WRITE "${replay_script}" ${common_replay_script})
file(WRITE "${disabled_script}" ${common_replay_script})

function(run_player label input_file output_var)
    execute_process(
        COMMAND "${PLAYER}" --rpc --headless
            --project "${project_dir}" --size 640x640 --fps 30
            --feature-overlay engine://features/editor.json
            ${ARGN}
        WORKING_DIRECTORY "${PLAYER_DIR}"
        INPUT_FILE "${input_file}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE stdout
        ERROR_VARIABLE stderr
        TIMEOUT 120
    )
    if(NOT result EQUAL 0)
        message(FATAL_ERROR
            "${label} failed: ${result}\nstdout:\n${stdout}\nstderr:\n${stderr}")
    endif()
    set(${output_var} "${stdout}" PARENT_SCOPE)
endfunction()

run_player(record "${record_script}" record_stdout
    --record-input "${recording}" --editor-transform blender)
if(NOT EXISTS "${recording}")
    message(FATAL_ERROR "--record-input did not create ${recording}")
endif()
file(READ "${recording}" recording_bytes)
if(NOT recording_bytes MATCHES [=["schema":"pelican.input_seq"]=] OR
   NOT recording_bytes MATCHES [=["type":"cursor_move"]=] OR
   NOT recording_bytes MATCHES [=["code":"G"]=] OR
   NOT recording_bytes MATCHES [=["code":"X"]=] OR
   NOT recording_bytes MATCHES [=["code":"Enter"]=])
    message(FATAL_ERROR
        "recorded input does not contain the modal keyboard/mouse sequence:\n${recording_bytes}")
endif()

run_player(replay "${replay_script}" replay_stdout
    --replay "${recording}" --editor-transform blender)
# Rule 10 negative control: replay the exact same bytes without the editor
# action overlay. The feature overlay still exposes the same selection/gizmo.
run_player(disabled "${disabled_script}" disabled_stdout
    --replay "${recording}")

function(response_for_id stdout id output_var)
    string(REPLACE "\r\n" "\n" normalized "${stdout}")
    string(REPLACE "\r" "\n" normalized "${normalized}")
    string(REPLACE ";" "\\;" normalized "${normalized}")
    string(REPLACE "\n" ";" lines "${normalized}")
    set(found "")
    foreach(line IN LISTS lines)
        if(line MATCHES "\\\"id\\\":${id}([,}])")
            set(found "${line}")
        endif()
    endforeach()
    if(found STREQUAL "")
        message(FATAL_ERROR "missing JSON-RPC id ${id}:\n${stdout}")
    endif()
    set(${output_var} "${found}" PARENT_SCOPE)
endfunction()

response_for_id("${record_stdout}" 10 record_state)
response_for_id("${replay_stdout}" 6 replay_state)
response_for_id("${disabled_stdout}" 6 disabled_state)

foreach(pair IN ITEMS
        "record_state;record" "replay_state;replay" "disabled_state;disabled")
    list(GET pair 0 state_var)
    list(GET pair 1 label)
    string(JSON contract ERROR_VARIABLE json_error
        GET "${${state_var}}" result contract)
    if(json_error OR NOT contract EQUAL 1)
        message(FATAL_ERROR "${label} modal state is not contract 1: ${${state_var}}")
    endif()
endforeach()

string(JSON record_phase GET "${record_state}" result phase)
string(JSON replay_phase GET "${replay_state}" result phase)
string(JSON disabled_phase GET "${disabled_state}" result phase)
string(JSON record_mode GET "${record_state}" result mode)
string(JSON replay_mode GET "${replay_state}" result mode)
string(JSON record_axis GET "${record_state}" result axis)
string(JSON replay_axis GET "${replay_state}" result axis)
string(JSON record_delta GET "${record_state}" result delta)
string(JSON replay_delta GET "${replay_state}" result delta)
string(JSON record_x GET "${record_state}" result delta translation 0)
string(JSON replay_x GET "${replay_state}" result delta translation 0)
string(JSON disabled_x GET "${disabled_state}" result delta translation 0)
string(JSON disabled_enabled GET "${disabled_state}" result enabled)

if(NOT record_phase STREQUAL "confirmed" OR
   NOT replay_phase STREQUAL "confirmed" OR
   NOT record_mode STREQUAL "translate" OR
   NOT replay_mode STREQUAL "translate" OR
   NOT record_axis STREQUAL "x" OR NOT replay_axis STREQUAL "x")
    message(FATAL_ERROR
        "record/replay did not reproduce G then X confirmation:\nrecord=${record_state}\nreplay=${replay_state}")
endif()
if(NOT record_delta STREQUAL replay_delta OR
   NOT record_x STREQUAL replay_x OR record_x EQUAL 0)
    message(FATAL_ERROR
        "record/replay transform deltas differ or are zero:\nrecord=${record_delta}\nreplay=${replay_delta}")
endif()
if(NOT disabled_phase STREQUAL "idle" OR disabled_enabled OR
   NOT disabled_x EQUAL 0 OR record_x STREQUAL disabled_x)
    message(FATAL_ERROR
        "disabled negative control did not differ:\nrecord=${record_state}\ndisabled=${disabled_state}")
endif()
