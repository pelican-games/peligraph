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
    "${OUT_DIR}/project/assets"
    "${OUT_DIR}/project/scenes"
    "${OUT_DIR}/project/passes"
    "${OUT_DIR}/project/ui"
)
get_filename_component(source_root "${PROJECT_DIR}/../.." ABSOLUTE)
configure_file("${source_root}/test/fixtures/ground.glb" "${OUT_DIR}/project/assets/ground.glb" COPYONLY)

file(WRITE "${OUT_DIR}/project/project.json" [=[
{
  "schema": "pelican.project",
  "version": 1,
  "name": "rpc_scene_flow",
  "engine_min_version": "0.1.0",
  "basic_config": {
    "window_title": "RPC Scene Flow",
    "window_size": {"width": 160, "height": 90},
    "fullscreen": false,
    "framerate": 30,
    "camera": {"yfov": 0.7853981633974483, "znear": 0.1, "zfar": 1000.0, "up": [0.0, 1.0, 0.0]},
    "default_scene_id": "default_scene",
    "scene_data_json": "scenes/main.scene.json",
    "asset_data_json": "assets/asset_data.json",
    "rendering_config_json": "passes/main_rendering_config.json",
    "default_rendering_pass": "main_render",
    "ui_config_json": "ui/ui_overlay.json"
  }
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
    },
    "scene_flow_second": {
      "objects": [
        {
          "name": "SceneFlowCamera",
          "components": [
            {"name": "transform", "pos": [0.0, 1.0, -4.0], "rotation": [0.0, 0.0, 0.0, 1.0], "scale": [1.0, 1.0, 1.0]},
            {"name": "camera", "type": "perspective", "yfov": 0.78539816339, "znear": 0.1, "zfar": 1000.0}
          ]
        },
        {
          "name": "SceneFlowLight",
          "components": [
            {
              "name": "light",
              "type": "directional",
              "direction": [0.0, -1.0, 0.0],
              "intensity": 1.0,
              "color": [0.4, 0.7, 1.0]
            }
          ]
        }
      ]
    }
  }
}
]=])
file(WRITE "${OUT_DIR}/project/assets/asset_data.json" "{\"models\":[]}\n")
file(WRITE "${OUT_DIR}/project/ui/ui_overlay.json" "{\"images\":[]}\n")
configure_file("${PROJECT_DIR}/passes/main_rendering_config.json" "${OUT_DIR}/project/passes/main_rendering_config.json" COPYONLY)

set(script_path "${OUT_DIR}/rpc_scene_flow.ndjson")
set(default_capture_path "${OUT_DIR}/scene_default.png")
set(second_capture_path "${OUT_DIR}/scene_second.png")
file(WRITE "${script_path}"
"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"get_status\",\"params\":{}}\n"
"{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"load_gltf\",\"params\":{\"path\":\"assets/ground.glb\",\"name\":\"transient\"}}\n"
"{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"update_transforms\",\"params\":{\"objects\":[\"transient\"],\"transforms\":[{\"pos\":[3.0,0.0,0.0],\"rot\":[0.0,0.0,0.0,1.0],\"scale\":[0.7,0.7,0.7]}]}}\n"
"{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"step_frame\",\"params\":{}}\n"
"{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"capture\",\"params\":{\"path\":\"${default_capture_path}\"}}\n"
"{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"load_scene\",\"params\":{\"name\":\"scene_flow_second\"}}\n"
"{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"get_status\",\"params\":{}}\n"
"{\"jsonrpc\":\"2.0\",\"id\":8,\"method\":\"render_frame\",\"params\":{}}\n"
"{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"capture\",\"params\":{\"path\":\"${second_capture_path}\"}}\n"
"{\"jsonrpc\":\"2.0\",\"id\":10,\"method\":\"update_transforms\",\"params\":{\"objects\":[\"transient\"],\"transforms\":[{\"pos\":[3.0,0.0,0.0],\"rot\":[0.0,0.0,0.0,1.0],\"scale\":[0.7,0.7,0.7]}]}}\n"
"{\"jsonrpc\":\"2.0\",\"id\":11,\"method\":\"load_scene\",\"params\":{\"name\":\"missing_scene\"}}\n"
"{\"jsonrpc\":\"2.0\",\"id\":12,\"method\":\"load_scene\",\"params\":{\"name\":\"default_scene\"}}\n"
"{\"jsonrpc\":\"2.0\",\"id\":13,\"method\":\"load_scene\",\"params\":{\"name\":\"scene_flow_second\"}}\n"
"{\"jsonrpc\":\"2.0\",\"id\":14,\"method\":\"load_scene\",\"params\":{\"name\":\"default_scene\"}}\n"
"{\"jsonrpc\":\"2.0\",\"id\":15,\"method\":\"load_scene\",\"params\":{\"name\":\"scene_flow_second\"}}\n"
"{\"jsonrpc\":\"2.0\",\"id\":16,\"method\":\"get_status\",\"params\":{}}\n"
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
    if(NOT line_count EQUAL 16)
        message(FATAL_ERROR "${label}: expected 16 JSON-RPC response lines, got ${line_count}\nstdout:\n${stdout}")
    endif()

    foreach(line IN LISTS lines)
        if(NOT line MATCHES [=[^\{.*"jsonrpc":"2\.0".*\}$]=])
            message(FATAL_ERROR "${label}: stdout contains a non-protocol line:\n${line}\nfull stdout:\n${stdout}")
        endif()
    endforeach()

    list(GET lines 0 line0)
    list(GET lines 3 line3)
    list(GET lines 5 line5)
    list(GET lines 6 line6)
    list(GET lines 9 line9)
    list(GET lines 10 line10)
    list(GET lines 11 line11)
    list(GET lines 12 line12)
    list(GET lines 13 line13)
    list(GET lines 14 line14)
    list(GET lines 15 line15)

    if(NOT line0 MATCHES [=["id":1]=] OR NOT line0 MATCHES [=["scene":"default_scene"]=] OR NOT line0 MATCHES [=["frame":0]=])
        message(FATAL_ERROR "${label}: initial get_status did not include default scene:\n${line0}")
    endif()
    if(NOT line3 MATCHES [=["id":4]=] OR NOT line3 MATCHES [=["frame":1]=])
        message(FATAL_ERROR "${label}: step_frame response did not report frame 1:\n${line3}")
    endif()
    if(NOT line5 MATCHES [=["id":6]=] OR NOT line5 MATCHES [=["name":"scene_flow_second"]=])
        message(FATAL_ERROR "${label}: load_scene response was unexpected:\n${line5}")
    endif()
    if(NOT line6 MATCHES [=["id":7]=] OR NOT line6 MATCHES [=["scene":"scene_flow_second"]=] OR NOT line6 MATCHES [=["frame":1]=])
        message(FATAL_ERROR "${label}: get_status after load_scene did not keep frame and scene:\n${line6}")
    endif()
    if(NOT line9 MATCHES [=["id":10]=] OR NOT line9 MATCHES [=["code":-32000]=] OR NOT line9 MATCHES [=[transient]=])
        message(FATAL_ERROR "${label}: transient object survived scene switch:\n${line9}")
    endif()
    if(NOT line10 MATCHES [=["id":11]=] OR NOT line10 MATCHES [=["code":-32000]=] OR NOT line10 MATCHES [=[missing_scene]=])
        message(FATAL_ERROR "${label}: unknown scene error did not include name:\n${line10}")
    endif()
    if(NOT line11 MATCHES [=["id":12]=] OR NOT line11 MATCHES [=["name":"default_scene"]=])
        message(FATAL_ERROR "${label}: reload default_scene response was unexpected:\n${line11}")
    endif()
    if(NOT line12 MATCHES [=["id":13]=] OR NOT line12 MATCHES [=["name":"scene_flow_second"]=])
        message(FATAL_ERROR "${label}: second switch response was unexpected:\n${line12}")
    endif()
    if(NOT line13 MATCHES [=["id":14]=] OR NOT line13 MATCHES [=["name":"default_scene"]=])
        message(FATAL_ERROR "${label}: third switch response was unexpected:\n${line13}")
    endif()
    if(NOT line14 MATCHES [=["id":15]=] OR NOT line14 MATCHES [=["name":"scene_flow_second"]=])
        message(FATAL_ERROR "${label}: fourth switch response was unexpected:\n${line14}")
    endif()
    if(NOT line15 MATCHES [=["id":16]=] OR NOT line15 MATCHES [=["scene":"scene_flow_second"]=] OR NOT line15 MATCHES [=["frame":1]=])
        message(FATAL_ERROR "${label}: final get_status did not keep scene and frame:\n${line15}")
    endif()
endfunction()

function(normalize_rpc_stdout stdout output_var)
    string(REGEX REPLACE [=["instance_id":"[0-9a-fA-F-]+"]=] [=["instance_id":"<uuid>"]=] normalized "${stdout}")
    set(${output_var} "${normalized}" PARENT_SCOPE)
endfunction()

function(run_rpc_once label output_var)
    file(REMOVE "${default_capture_path}")
    file(REMOVE "${second_capture_path}")
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

    foreach(path IN ITEMS "${default_capture_path}" "${second_capture_path}")
        if(NOT EXISTS "${path}")
            message(FATAL_ERROR "${label}: capture output missing: ${path}")
        endif()
        file(SIZE "${path}" size)
        if(size EQUAL 0)
            message(FATAL_ERROR "${label}: capture output is empty: ${path}")
        endif()
    endforeach()

    file(READ "${default_capture_path}" default_hex HEX)
    file(READ "${second_capture_path}" second_hex HEX)
    if(default_hex STREQUAL second_hex)
        message(FATAL_ERROR "${label}: load_scene did not change captured PNG bytes")
    endif()

    normalize_rpc_stdout("${stdout}" normalized_stdout)
    set(${output_var} "${normalized_stdout}" PARENT_SCOPE)
endfunction()

run_rpc_once("first run" first_stdout)
run_rpc_once("second run" second_stdout)

if(NOT first_stdout STREQUAL second_stdout)
    message(FATAL_ERROR "rpc scene flow response stream is not deterministic\nfirst:\n${first_stdout}\nsecond:\n${second_stdout}")
endif()
