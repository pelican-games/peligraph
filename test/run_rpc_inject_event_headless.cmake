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

file(WRITE "${OUT_DIR}/project/project.json" [=[
{
  "schema": "pelican.project",
  "version": 1,
  "name": "rpc_inject_event",
  "engine_min_version": "0.1.0",
  "basic_config": {
    "window_title": "RPC Inject Event",
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
          "name": "DefaultCamera",
          "components": [
            {"name": "transform", "pos": [0.0, 1.0, -4.0], "rotation": [0.0, 0.0, 0.0, 1.0], "scale": [1.0, 1.0, 1.0]},
            {"name": "camera", "type": "perspective", "yfov": 0.78539816339, "znear": 0.1, "zfar": 1000.0}
          ]
        },
        {
          "name": "DefaultLight",
          "components": [
            {"name": "light", "type": "directional", "direction": [-1.0, -0.25, 0.0], "intensity": 5.0, "color": [1.0, 1.0, 1.0]}
          ]
        }
      ]
    },
    "scene_flow_second": {
      "objects": [
        {
          "name": "SecondCamera",
          "components": [
            {"name": "transform", "pos": [0.0, 1.0, -4.0], "rotation": [0.0, 0.0, 0.0, 1.0], "scale": [1.0, 1.0, 1.0]},
            {"name": "camera", "type": "perspective", "yfov": 0.78539816339, "znear": 0.1, "zfar": 1000.0}
          ]
        },
        {
          "name": "SecondLight",
          "components": [
            {"name": "light", "type": "directional", "direction": [0.0, -1.0, 0.0], "intensity": 1.0, "color": [0.4, 0.7, 1.0]}
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

set(script_path "${OUT_DIR}/rpc_inject_event.ndjson")
file(WRITE "${script_path}"
"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"inject_event\",\"params\":{\"type\":\"Wp56InjectedEvent\",\"payload\":{\"seed\":5609}}}\n"
"{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"get_status\",\"params\":{}}\n"
"{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"step_frame\",\"params\":{}}\n"
"{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"get_status\",\"params\":{}}\n"
"{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"load_scene\",\"params\":{\"name\":\"scene_flow_second\"}}\n"
"{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"get_status\",\"params\":{}}\n"
"{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"step_frame\",\"params\":{}}\n"
"{\"jsonrpc\":\"2.0\",\"id\":8,\"method\":\"get_status\",\"params\":{}}\n"
"{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"inject_event\",\"params\":{\"type\":\"UnknownEvent\",\"payload\":{}}}\n"
"{\"jsonrpc\":\"2.0\",\"id\":10,\"method\":\"inject_event\",\"params\":{\"type\":\"Wp56InjectedEvent\"}}\n"
)

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
    message(FATAL_ERROR "rpc inject_event player run failed with ${result}\nstdout:\n${stdout}\nstderr:\n${stderr}")
endif()
if(stdout MATCHES "Validation Error|VUID-" OR stderr MATCHES "Validation Error|VUID-")
    message(FATAL_ERROR "rpc inject_event player run emitted Vulkan validation errors\nstdout:\n${stdout}\nstderr:\n${stderr}")
endif()

string(REPLACE "\r\n" "\n" normalized "${stdout}")
string(REPLACE "\r" "\n" normalized "${normalized}")
string(REGEX REPLACE "\n$" "" trimmed "${normalized}")
if(trimmed STREQUAL "")
    message(FATAL_ERROR "rpc inject_event stdout was empty")
endif()

string(REPLACE "\n" ";" lines "${trimmed}")
list(LENGTH lines line_count)
if(NOT line_count EQUAL 10)
    message(FATAL_ERROR "expected 10 JSON-RPC response lines, got ${line_count}\nstdout:\n${stdout}")
endif()

foreach(line IN LISTS lines)
    if(NOT line MATCHES [=[^\{.*"jsonrpc":"2\.0".*\}$]=])
        message(FATAL_ERROR "stdout contains a non-protocol line:\n${line}\nfull stdout:\n${stdout}")
    endif()
endforeach()

list(GET lines 0 line0)
list(GET lines 2 line2)
list(GET lines 3 line3)
list(GET lines 4 line4)
list(GET lines 5 line5)
list(GET lines 6 line6)
list(GET lines 7 line7)
list(GET lines 8 line8)
list(GET lines 9 line9)

if(NOT line0 MATCHES [=["id":1]=] OR NOT line0 MATCHES [=["queued":1]=])
    message(FATAL_ERROR "inject_event did not queue one event:\n${line0}")
endif()
if(NOT line2 MATCHES [=["id":3]=] OR NOT line2 MATCHES [=["frame":1]=])
    message(FATAL_ERROR "first step_frame response was unexpected:\n${line2}")
endif()
if(NOT line3 MATCHES [=["id":4]=] OR NOT line3 MATCHES [=["seed":5609]=])
    message(FATAL_ERROR "injected event was not delivered on the next frame:\n${line3}")
endif()
if(NOT line4 MATCHES [=["id":5]=] OR NOT line4 MATCHES [=["name":"scene_flow_second"]=])
    message(FATAL_ERROR "load_scene response was unexpected:\n${line4}")
endif()
if(NOT line5 MATCHES [=["id":6]=] OR NOT line5 MATCHES [=["seed":5609]=] OR NOT line5 MATCHES [=["scene":"scene_flow_second"]=])
    message(FATAL_ERROR "SceneLoaded was delivered too early or scene status was wrong:\n${line5}")
endif()
if(NOT line6 MATCHES [=["id":7]=] OR NOT line6 MATCHES [=["frame":2]=])
    message(FATAL_ERROR "second step_frame response was unexpected:\n${line6}")
endif()
if(NOT line7 MATCHES [=["id":8]=] OR NOT line7 MATCHES [=["seed":5602]=])
    message(FATAL_ERROR "SceneLoaded was not delivered on the next frame:\n${line7}")
endif()
if(NOT line8 MATCHES [=["id":9]=] OR NOT line8 MATCHES [=["code":-32602]=] OR NOT line8 MATCHES [=[UnknownEvent]=])
    message(FATAL_ERROR "unknown inject_event type did not return a named -32602:\n${line8}")
endif()
if(NOT line9 MATCHES [=["id":10]=] OR NOT line9 MATCHES [=["code":-32602]=] OR NOT line9 MATCHES [=[payload]=])
    message(FATAL_ERROR "malformed inject_event payload did not return a named -32602:\n${line9}")
endif()
