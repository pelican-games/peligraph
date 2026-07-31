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
  "name": "rpc_set_camera",
  "engine_min_version": "0.1.0",
  "basic_config": {
    "window_title": "RPC Set Camera",
    "window_size": {"width": 160, "height": 90},
    "fullscreen": false,
    "framerate": 30,
    "camera": {"yfov": 0.78539816339, "znear": 0.1, "zfar": 1000.0, "up": [0.0, 1.0, 0.0]},
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
          "name": "PerspectiveCamera",
          "components": [
            {"name": "transform", "pos": [0.0, 1.0, -4.0], "rotation": [0.0, 0.0, 0.0, 1.0], "scale": [1.0, 1.0, 1.0]},
            {"name": "camera", "type": "perspective", "yfov": 0.78539816339, "znear": 0.1, "zfar": 1000.0}
          ]
        },
        {
          "name": "OrthoCamera",
          "components": [
            {"name": "transform", "pos": [0.0, 1.0, -4.0], "rotation": [0.0, 0.0, 0.0, 1.0], "scale": [1.0, 1.0, 1.0]},
            {"name": "camera", "type": "orthographic", "xmag": 1.0, "ymag": 0.55, "znear": 0.1, "zfar": 1000.0}
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
file(WRITE "${OUT_DIR}/project/assets/asset_data.json" "{\"schema\":\"pelican.asset_data\",\"version\":1,\"models\":[]}\n")
file(WRITE "${OUT_DIR}/project/ui/ui_overlay.json" "{\"schema\":\"pelican.ui\",\"version\":1,\"key\":\"empty\",\"root\":{\"id\":\"root\",\"type\":\"panel\"}}\n")
configure_file("${PROJECT_DIR}/passes/main_rendering_config.json" "${OUT_DIR}/project/passes/main_rendering_config.json" COPYONLY)

set(script_path "${OUT_DIR}/rpc_set_camera.ndjson")
set(perspective_path "${OUT_DIR}/perspective.png")
set(orthographic_path "${OUT_DIR}/orthographic.png")
file(WRITE "${script_path}"
"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"load_gltf\",\"params\":{\"path\":\"assets/ground.glb\",\"name\":\"target\"}}\n"
"{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"update_transforms\",\"params\":{\"objects\":[\"target\"],\"transforms\":[{\"pos\":[0.0,0.0,0.0],\"rotation\":[0.0,0.0,0.0,1.0],\"scale\":[0.7,0.7,0.7]}]}}\n"
"{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"step_frame\",\"params\":{}}\n"
"{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"set_camera\",\"params\":{\"name\":\"PerspectiveCamera\"}}\n"
"{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"render_frame\",\"params\":{}}\n"
"{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"capture\",\"params\":{\"path\":\"${perspective_path}\"}}\n"
"{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"set_camera\",\"params\":{\"name\":\"OrthoCamera\"}}\n"
"{\"jsonrpc\":\"2.0\",\"id\":8,\"method\":\"render_frame\",\"params\":{}}\n"
"{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"capture\",\"params\":{\"path\":\"${orthographic_path}\"}}\n"
"{\"jsonrpc\":\"2.0\",\"id\":10,\"method\":\"set_camera\",\"params\":{\"name\":\"MissingCamera\"}}\n"
)

function(validate_stdout stdout label)
    string(REPLACE "\r\n" "\n" normalized "${stdout}")
    string(REPLACE "\r" "\n" normalized "${normalized}")
    string(REGEX REPLACE "\n$" "" trimmed "${normalized}")
    # Keep semicolons inside a JSON response from becoming CMake list separators.
    string(REPLACE ";" "\\;" list_safe "${trimmed}")
    string(REPLACE "\n" ";" lines "${list_safe}")
    list(LENGTH lines line_count)
    if(NOT line_count EQUAL 10)
        message(FATAL_ERROR "${label}: expected 10 JSON-RPC lines, got ${line_count}\nstdout:\n${stdout}")
    endif()

    foreach(line IN LISTS lines)
        if(NOT line MATCHES [=[^\{.*"jsonrpc":"2\.0".*\}$]=])
            message(FATAL_ERROR "${label}: stdout contains a non-protocol line:\n${line}\nfull stdout:\n${stdout}")
        endif()
    endforeach()

    list(GET lines 3 line3)
    list(GET lines 6 line6)
    list(GET lines 9 line9)
    if(NOT line3 MATCHES [=["id":4]=] OR NOT line3 MATCHES [=["name":"PerspectiveCamera"]=])
        message(FATAL_ERROR "${label}: set_camera PerspectiveCamera response was unexpected:\n${line3}")
    endif()
    if(NOT line6 MATCHES [=["id":7]=] OR NOT line6 MATCHES [=["name":"OrthoCamera"]=])
        message(FATAL_ERROR "${label}: set_camera OrthoCamera response was unexpected:\n${line6}")
    endif()
    if(NOT line9 MATCHES [=["id":10]=] OR NOT line9 MATCHES [=["code":-32000]=] OR NOT line9 MATCHES [=[MissingCamera]=])
        message(FATAL_ERROR "${label}: unknown camera error did not include expected code and name:\n${line9}")
    endif()
endfunction()

function(run_rpc_once label output_var)
    file(REMOVE "${perspective_path}")
    file(REMOVE "${orthographic_path}")
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

    foreach(path IN ITEMS "${perspective_path}" "${orthographic_path}")
        if(NOT EXISTS "${path}")
            message(FATAL_ERROR "${label}: capture output missing: ${path}")
        endif()
        file(SIZE "${path}" size)
        if(size EQUAL 0)
            message(FATAL_ERROR "${label}: capture output is empty: ${path}")
        endif()
    endforeach()

    file(READ "${perspective_path}" perspective_hex HEX)
    file(READ "${orthographic_path}" orthographic_hex HEX)
    if(perspective_hex STREQUAL orthographic_hex)
        message(FATAL_ERROR "${label}: set_camera did not change captured PNG bytes")
    endif()

    set(${output_var} "${stdout}" PARENT_SCOPE)
endfunction()

run_rpc_once("first run" first_stdout)
run_rpc_once("second run" second_stdout)

if(NOT first_stdout STREQUAL second_stdout)
    message(FATAL_ERROR "rpc set_camera response stream is not deterministic\nfirst:\n${first_stdout}\nsecond:\n${second_stdout}")
endif()
