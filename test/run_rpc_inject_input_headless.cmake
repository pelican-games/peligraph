if(NOT DEFINED PLAYER)
    message(FATAL_ERROR "PLAYER is required")
endif()
if(NOT DEFINED PLAYER_DIR)
    message(FATAL_ERROR "PLAYER_DIR is required")
endif()
if(NOT DEFINED PROJECT_DIR)
    message(FATAL_ERROR "PROJECT_DIR is required")
endif()
if(NOT DEFINED OUT_DIR)
    message(FATAL_ERROR "OUT_DIR is required")
endif()

file(REMOVE_RECURSE "${OUT_DIR}")
file(MAKE_DIRECTORY
    "${OUT_DIR}/project/assets/models"
    "${OUT_DIR}/project/input"
    "${OUT_DIR}/project/passes"
    "${OUT_DIR}/project/scenes"
    "${OUT_DIR}/project/ui"
)
get_filename_component(source_root "${PROJECT_DIR}/../.." ABSOLUTE)
file(COPY "${source_root}/test/fixtures/ground.glb" DESTINATION "${OUT_DIR}/project/assets/models")
file(RENAME "${OUT_DIR}/project/assets/models/ground.glb" "${OUT_DIR}/project/assets/models/character.glb")
configure_file("${PROJECT_DIR}/input/actions.json" "${OUT_DIR}/project/input/actions.json" COPYONLY)
configure_file("${PROJECT_DIR}/passes/main_rendering_config.json" "${OUT_DIR}/project/passes/main_rendering_config.json" COPYONLY)

file(WRITE "${OUT_DIR}/project/project.json" [=[
{
  "schema": "pelican.project",
  "version": 1,
  "name": "rpc_inject_input",
  "engine_min_version": "0.1.0",
  "basic_config": {
    "window_title": "RPC Inject Input",
    "window_size": {"width": 160, "height": 90},
    "fullscreen": false,
    "framerate": 30,
    "camera": {"yfov": 0.7853981633974483, "znear": 0.1, "zfar": 1000.0, "up": [0.0, 1.0, 0.0]},
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

file(WRITE "${OUT_DIR}/project/assets/asset_data.json" [=[
{
  "models": [
    {"name": "character", "path": "assets/models/character.glb"}
  ]
}
]=])

file(WRITE "${OUT_DIR}/project/scenes/main.scene.json" [=[
{
  "schema": "pelican.scene",
  "version": 1,
  "scenes": {
    "default_scene": {
      "objects": [
        {
          "name": "Camera",
          "components": [
            {"name": "transform", "pos": [0.0, 0.0, -3.0], "rotation": [0.0, 0.0, 0.0, 1.0], "scale": [1.0, 1.0, 1.0]},
            {"name": "camera"}
          ]
        },
        {
          "name": "KeyLight",
          "components": [
            {
              "name": "light",
              "type": "directional",
              "direction": [-1.0, -0.25, 0.0],
              "intensity": 5.0,
              "color": [1.0, 1.0, 1.0]
            }
          ]
        }
      ]
    }
  }
}
]=])
file(WRITE "${OUT_DIR}/project/ui/ui_overlay.json" "{\"images\":[]}\n")

set(script_path "${OUT_DIR}/rpc_inject_input.ndjson")
set(start_path "${OUT_DIR}/start.png")
set(moved_path "${OUT_DIR}/moved.png")
file(WRITE "${script_path}"
"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"step_frame\",\"params\":{}}\n"
"{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"capture\",\"params\":{\"path\":\"${start_path}\"}}\n"
"{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"inject_input\",\"params\":{\"events\":[{\"type\":\"key_down\",\"key\":\"W\"}]}}\n"
)
foreach(id RANGE 4 15)
    file(APPEND "${script_path}" "{\"jsonrpc\":\"2.0\",\"id\":${id},\"method\":\"step_frame\",\"params\":{}}\n")
endforeach()
file(APPEND "${script_path}"
"{\"jsonrpc\":\"2.0\",\"id\":16,\"method\":\"capture\",\"params\":{\"path\":\"${moved_path}\"}}\n"
"{\"jsonrpc\":\"2.0\",\"id\":17,\"method\":\"inject_input\",\"params\":{\"events\":[{\"type\":\"teleport\"}]}}\n"
"{\"jsonrpc\":\"2.0\",\"id\":18,\"method\":\"inject_input\",\"params\":{\"events\":[{\"type\":\"key_down\"}]}}\n"
"{\"jsonrpc\":\"2.0\",\"id\":19,\"method\":\"inject_input\",\"params\":{\"events\":[{\"type\":\"key_down\",\"key\":\"not_a_key\"}]}}\n"
)

function(validate_stdout stdout label)
    string(REPLACE "\r\n" "\n" normalized "${stdout}")
    string(REPLACE "\r" "\n" normalized "${normalized}")
    string(REGEX REPLACE "\n$" "" trimmed "${normalized}")
    if(trimmed STREQUAL "")
        message(FATAL_ERROR "${label}: rpc stdout was empty")
    endif()

    string(REPLACE "\n" ";" lines "${trimmed}")
    list(LENGTH lines line_count)
    if(NOT line_count EQUAL 19)
        message(FATAL_ERROR "${label}: expected 19 JSON-RPC response lines, got ${line_count}\nstdout:\n${stdout}")
    endif()

    foreach(line IN LISTS lines)
        if(NOT line MATCHES [=[^\{.*"jsonrpc":"2\.0".*\}$]=])
            message(FATAL_ERROR "${label}: stdout contains a non-protocol line:\n${line}\nfull stdout:\n${stdout}")
        endif()
    endforeach()

    list(GET lines 0 line0)
    list(GET lines 1 line1)
    list(GET lines 2 line2)
    list(GET lines 14 line14)
    list(GET lines 15 line15)
    list(GET lines 16 line16)
    list(GET lines 17 line17)
    list(GET lines 18 line18)

    if(NOT line0 MATCHES [=["id":1]=] OR NOT line0 MATCHES [=["frame":1]=])
        message(FATAL_ERROR "${label}: first step_frame response was unexpected:\n${line0}")
    endif()
    if(NOT line1 MATCHES [=["id":2]=] OR NOT line1 MATCHES [=["path"]=] OR NOT line1 MATCHES [=["encoding":"srgb"]=] OR NOT line1 MATCHES [=["contract":2]=])
        message(FATAL_ERROR "${label}: start capture response was unexpected:\n${line1}")
    endif()
    if(NOT line2 MATCHES [=["id":3]=] OR NOT line2 MATCHES [=["queued":1]=])
        message(FATAL_ERROR "${label}: inject_input did not queue one event:\n${line2}")
    endif()
    if(NOT line14 MATCHES [=["id":15]=] OR NOT line14 MATCHES [=["frame":13]=])
        message(FATAL_ERROR "${label}: final step_frame response was unexpected:\n${line14}")
    endif()
    if(NOT line15 MATCHES [=["id":16]=] OR NOT line15 MATCHES [=["path"]=] OR NOT line15 MATCHES [=["encoding":"srgb"]=] OR NOT line15 MATCHES [=["contract":2]=])
        message(FATAL_ERROR "${label}: moved capture response was unexpected:\n${line15}")
    endif()
    if(NOT line16 MATCHES [=["id":17]=] OR NOT line16 MATCHES [=["code":-32602]=] OR NOT line16 MATCHES [=[teleport]=])
        message(FATAL_ERROR "${label}: unknown inject_input event type did not return a named -32602:\n${line16}")
    endif()
    if(NOT line17 MATCHES [=["id":18]=] OR NOT line17 MATCHES [=["code":-32602]=] OR NOT line17 MATCHES [=[key]=])
        message(FATAL_ERROR "${label}: malformed key event did not return a named -32602:\n${line17}")
    endif()
    if(NOT line18 MATCHES [=["id":19]=] OR NOT line18 MATCHES [=["code":-32602]=] OR NOT line18 MATCHES [=[not_a_key]=])
        message(FATAL_ERROR "${label}: unknown key did not return a named -32602:\n${line18}")
    endif()
endfunction()

function(run_rpc_once label stdout_var start_hex_var moved_hex_var)
    file(REMOVE "${start_path}")
    file(REMOVE "${moved_path}")
    execute_process(
        COMMAND "${PLAYER}"
            --rpc
            --headless
            --project "${OUT_DIR}/project"
            --size 160x90
            --fps 30
        WORKING_DIRECTORY "${PLAYER_DIR}"
        INPUT_FILE "${script_path}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE stdout
        ERROR_VARIABLE stderr
    )

    if(NOT result EQUAL 0)
        message(FATAL_ERROR "${label}: rpc player run failed with ${result}\nstdout:\n${stdout}\nstderr:\n${stderr}")
    endif()
    if(stdout MATCHES "Validation Error|VUID-" OR stderr MATCHES "Validation Error|VUID-")
        message(FATAL_ERROR "${label}: rpc player run emitted Vulkan validation errors\nstdout:\n${stdout}\nstderr:\n${stderr}")
    endif()
    validate_stdout("${stdout}" "${label}")

    foreach(path IN ITEMS "${start_path}" "${moved_path}")
        if(NOT EXISTS "${path}")
            message(FATAL_ERROR "${label}: capture output missing: ${path}")
        endif()
        file(SIZE "${path}" size)
        if(size EQUAL 0)
            message(FATAL_ERROR "${label}: capture output is empty: ${path}")
        endif()
    endforeach()

    file(READ "${start_path}" start_hex HEX)
    file(READ "${moved_path}" moved_hex HEX)
    if(start_hex STREQUAL moved_hex)
        message(FATAL_ERROR "${label}: injected W key did not change captured PNG bytes")
    endif()

    set(${stdout_var} "${stdout}" PARENT_SCOPE)
    set(${start_hex_var} "${start_hex}" PARENT_SCOPE)
    set(${moved_hex_var} "${moved_hex}" PARENT_SCOPE)
endfunction()

run_rpc_once("first run" first_stdout first_start_hex first_moved_hex)
run_rpc_once("second run" second_stdout second_start_hex second_moved_hex)

if(NOT first_stdout STREQUAL second_stdout)
    message(FATAL_ERROR "rpc inject_input response stream is not deterministic\nfirst:\n${first_stdout}\nsecond:\n${second_stdout}")
endif()
if(NOT first_start_hex STREQUAL second_start_hex)
    message(FATAL_ERROR "rpc inject_input start capture is not deterministic")
endif()
if(NOT first_moved_hex STREQUAL second_moved_hex)
    message(FATAL_ERROR "rpc inject_input moved capture is not deterministic")
endif()
