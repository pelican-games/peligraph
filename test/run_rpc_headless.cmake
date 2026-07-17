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
if(NOT DEFINED RENDERDOC_STATUS)
    set(RENDERDOC_STATUS "absent")
endif()
if(NOT DEFINED RENDERDOC_REASON)
    set(RENDERDOC_REASON "renderdoc_not_injected")
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
  "name": "rpc_headless",
  "engine_min_version": "0.1.0",
  "basic_config": {
    "window_title": "RPC Headless",
    "window_size": {"width": 160, "height": 90},
    "fullscreen": false,
    "framerate": 30,
    "seed": 1234,
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
    }
  }
}
]=])
file(WRITE "${OUT_DIR}/project/assets/asset_data.json" "{\"models\":[]}\n")
file(WRITE "${OUT_DIR}/project/ui/ui_overlay.json" "{\"schema\":\"pelican.ui\",\"version\":1,\"key\":\"empty\",\"root\":{\"id\":\"root\",\"type\":\"panel\"}}\n")
configure_file("${PROJECT_DIR}/passes/main_rendering_config.json" "${OUT_DIR}/project/passes/main_rendering_config.json" COPYONLY)

set(script_path "${OUT_DIR}/rpc_script.ndjson")
set(capture_path "${OUT_DIR}/rpc_capture.png")
set(capture_left_path "${OUT_DIR}/rpc_capture_left.png")
set(capture_right_path "${OUT_DIR}/rpc_capture_right.png")
file(WRITE "${script_path}"
"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"get_status\",\"params\":{}}\n"
"{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"set_time\",\"params\":{\"t\":1.25}}\n"
"{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"load_gltf\",\"params\":{\"path\":\"assets/ground.glb\",\"name\":\"movable\"}}\n"
"{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"update_transforms\",\"params\":{\"objects\":[\"movable\"],\"transforms\":[{\"pos\":[3.0,0.0,-0.75],\"rotation\":[0.0,0.0,0.0,1.0],\"scale\":[0.6,0.6,0.6]}]}}\n"
"{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"step_frame\",\"params\":{}}\n"
"{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"capture\",\"params\":{\"path\":\"${capture_left_path}\"}}\n"
"{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"update_transforms\",\"params\":{\"objects\":[\"movable\"],\"transforms\":[{\"pos\":[3.0,0.0,0.75],\"rotation\":[0.0,0.0,0.0,1.0],\"scale\":[0.6,0.6,0.6]}]}}\n"
"{\"jsonrpc\":\"2.0\",\"id\":8,\"method\":\"render_frame\",\"params\":{}}\n"
"{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"capture\",\"params\":{\"path\":\"${capture_right_path}\"}}\n"
"{\"jsonrpc\":\"2.0\",\"id\":10,\"method\":\"get_status\",\"params\":{}}\n"
"{\"jsonrpc\":\"2.0\",\"id\":11,\"method\":\"get_frame_plan\",\"params\":{}}\n"
"{\"jsonrpc\":\"2.0\",\"id\":12,\"method\":\"update_transforms\",\"params\":{\"objects\":[\"missing_object\"],\"transforms\":[{\"pos\":[0.0,0.0,0.0],\"rotation\":[0.0,0.0,0.0,1.0],\"scale\":[1.0,1.0,1.0]}]}}\n"
"{\"jsonrpc\":\"2.0\",\"id\":13,\"method\":\"load_gltf\",\"params\":{\"path\":\"../escape.glb\",\"name\":\"escaped\"}}\n"
"{\"jsonrpc\":\"2.0\",\"id\":14,\"method\":\"update_transforms\",\"params\":{\"objects\":[\"movable\"],\"transforms\":[]}}\n"
"{bad json\n"
"{\"jsonrpc\":\"2.0\",\"method\":\"render_frame\",\"params\":{}}\n"
"{\"jsonrpc\":\"2.0\",\"id\":15,\"method\":\"missing_method\",\"params\":{}}\n"
"{\"jsonrpc\":\"2.0\",\"id\":16,\"method\":\"set_seed\",\"params\":{\"seed\":99}}\n"
"{\"jsonrpc\":\"2.0\",\"id\":17,\"method\":\"get_status\",\"params\":{}}\n"
"{\"jsonrpc\":\"2.0\",\"id\":18,\"method\":\"update_transforms\",\"params\":{\"objects\":[\"movable\"],\"transforms\":[{\"pos\":[0.0,0.0,0.0],\"rot\":[0.0,0.0,0.0,1.0],\"scale\":[1.0,1.0,1.0]}]}}\n"
"{\"jsonrpc\":\"2.0\",\"id\":19,\"method\":\"capture_gpu\",\"params\":{}}\n"
)

function(validate_rpc_stdout stdout label)
    string(REPLACE "\r\n" "\n" normalized "${stdout}")
    string(REPLACE "\r" "\n" normalized "${normalized}")
    string(REGEX REPLACE "\n$" "" trimmed "${normalized}")
    if(trimmed STREQUAL "")
        message(FATAL_ERROR "${label}: rpc stdout was empty")
    endif()

    string(REPLACE "\n" ";" lines "${trimmed}")
    list(LENGTH lines line_count)
    if(NOT line_count EQUAL 21)
        message(FATAL_ERROR "${label}: expected 21 JSON-RPC response lines, got ${line_count}\nstdout:\n${stdout}")
    endif()

    foreach(line IN LISTS lines)
        if(NOT line MATCHES [=[^\{.*"jsonrpc":"2\.0".*\}$]=])
            message(FATAL_ERROR "${label}: stdout contains a non-protocol line:\n${line}\nfull stdout:\n${stdout}")
        endif()
    endforeach()

    list(GET lines 0 line0)
    list(GET lines 1 line1)
    list(GET lines 2 line2)
    list(GET lines 3 line3)
    list(GET lines 4 line4)
    list(GET lines 5 line5)
    list(GET lines 6 line6)
    list(GET lines 7 line7)
    list(GET lines 8 line8)
    list(GET lines 9 line9)
    list(GET lines 10 line10)
    list(GET lines 11 line11)
    list(GET lines 12 line12)
    list(GET lines 13 line13)
    list(GET lines 14 line14)
    list(GET lines 15 line15)
    list(GET lines 16 line16)
    list(GET lines 17 line17)
    list(GET lines 18 line18)
    list(GET lines 19 line19)
    list(GET lines 20 line20)

    if(NOT line0 MATCHES [=["id":1]=] OR NOT line0 MATCHES [=["instance_id":"[0-9a-fA-F-]+"]=] OR NOT line0 MATCHES [=["project_root"]=] OR NOT line0 MATCHES [=["scene":"default_scene"]=] OR NOT line0 MATCHES [=["frame":0]=] OR NOT line0 MATCHES [=["time":0\.0]=] OR NOT line0 MATCHES [=["seed":1234]=] OR NOT line0 MATCHES [=["xr":\{"active":false.*"timing":\{.*"wait_frame_count":0.*"view_configuration":null\}]=] OR NOT line0 MATCHES [=["contract":2]=] OR NOT line0 MATCHES [=["readback_encoding":"srgb"]=] OR NOT line0 MATCHES [=["capture":"available"]=] OR NOT line0 MATCHES [=["gpu_timing":\{"dropped_samples":0,"enabled":false,"history_capacity":120]=])
        message(FATAL_ERROR "${label}: get_status initial response did not include expected fields:\n${line0}")
    endif()
    if(NOT line0 MATCHES [=["memory":\{"driver_available":]=] OR
       NOT line0 MATCHES [=["engine_categories":\[]=] OR
       NOT line0 MATCHES [=["schema_version":1]=])
        message(FATAL_ERROR "${label}: get_status.memory did not expose the additive diagnostics schema:\n${line0}")
    endif()
    if(NOT line0 MATCHES "\"renderdoc\":\"${RENDERDOC_STATUS}\"" OR NOT line0 MATCHES "\"reason\":\"${RENDERDOC_REASON}\"")
        message(FATAL_ERROR "${label}: get_status RenderDoc state was not ${RENDERDOC_STATUS}/${RENDERDOC_REASON}:\n${line0}")
    endif()
    if(NOT line0 MATCHES [=["startup":\{]=] OR NOT line0 MATCHES [=["config_ms":[0-9]]=] OR NOT line0 MATCHES [=["vulkan_ms":[0-9]]=] OR NOT line0 MATCHES [=["shaders_ms":[0-9]]=] OR NOT line0 MATCHES [=["shader_cache_hits":[0-9]]=] OR NOT line0 MATCHES [=["shader_cache_requests":[0-9]]=] OR NOT line0 MATCHES [=["models_ms":[0-9]]=] OR NOT line0 MATCHES [=["total_ms":[0-9]]=] OR NOT line0 MATCHES [=["complete":true]=])
        message(FATAL_ERROR "${label}: get_status startup report was incomplete:\n${line0}")
    endif()
    if(NOT line0 MATCHES [=["modules":\{]=] OR NOT line0 MATCHES [=["phase":"running"]=] OR NOT line0 MATCHES [=["creation_frozen":true]=] OR NOT line0 MATCHES [=["initialized_after_runtime_start":\[\]]=])
        message(FATAL_ERROR "${label}: runtime module graph was not frozen before RPC dispatch:\n${line0}")
    endif()
    if(NOT line1 MATCHES [=["id":2]=] OR NOT line1 MATCHES [=["result"]=] OR NOT line1 MATCHES [=["t":1\.25]=])
        message(FATAL_ERROR "${label}: set_time response did not look successful:\n${line1}")
    endif()
    if(NOT line2 MATCHES [=["id":3]=] OR NOT line2 MATCHES [=["name":"movable"]=] OR NOT line2 MATCHES [=["path"]=])
        message(FATAL_ERROR "${label}: load_gltf response did not include path and name:\n${line2}")
    endif()
    if(NOT line3 MATCHES [=["id":4]=] OR NOT line3 MATCHES [=["queued":1]=])
        message(FATAL_ERROR "${label}: update_transforms did not queue one update:\n${line3}")
    endif()
    if(NOT line4 MATCHES [=["id":5]=] OR NOT line4 MATCHES [=["frame":1]=])
        message(FATAL_ERROR "${label}: step_frame response did not report frame 1:\n${line4}")
    endif()
    if(NOT line5 MATCHES [=["id":6]=] OR NOT line5 MATCHES [=["path"]=] OR NOT line5 MATCHES [=["encoding":"srgb"]=] OR NOT line5 MATCHES [=["contract":2]=])
        message(FATAL_ERROR "${label}: first capture response did not include a path:\n${line5}")
    endif()
    if(NOT line6 MATCHES [=["id":7]=] OR NOT line6 MATCHES [=["queued":1]=])
        message(FATAL_ERROR "${label}: second update_transforms did not queue one update:\n${line6}")
    endif()
    if(NOT line7 MATCHES [=["id":8]=] OR NOT line7 MATCHES [=["frame":1]=])
        message(FATAL_ERROR "${label}: render_frame response did not keep frame 1:\n${line7}")
    endif()
    if(NOT line8 MATCHES [=["id":9]=] OR NOT line8 MATCHES [=["path"]=] OR NOT line8 MATCHES [=["encoding":"srgb"]=] OR NOT line8 MATCHES [=["contract":2]=])
        message(FATAL_ERROR "${label}: second capture response did not include a path:\n${line8}")
    endif()
    if(NOT line9 MATCHES [=["id":10]=] OR NOT line9 MATCHES [=["instance_id":"[0-9a-fA-F-]+"]=] OR NOT line9 MATCHES [=["scene":"default_scene"]=] OR NOT line9 MATCHES [=["frame":1]=] OR NOT line9 MATCHES [=["seed":1234]=])
        message(FATAL_ERROR "${label}: get_status final response did not include expected fields:\n${line9}")
    endif()
    if(NOT line10 MATCHES [=["id":11]=] OR NOT line10 MATCHES [=["schema":"pelican\.frame_plan"]=] OR NOT line10 MATCHES [=["version":1]=] OR NOT line10 MATCHES [=["graph":"main_render"]=])
        message(FATAL_ERROR "${label}: get_frame_plan response did not include the expected frame plan schema:\n${line10}")
    endif()
    if(NOT line11 MATCHES [=["id":12]=] OR NOT line11 MATCHES [=["code":-32000]=] OR NOT line11 MATCHES [=[missing_object]=])
        message(FATAL_ERROR "${label}: unknown object did not return -32000 with the object name:\n${line11}")
    endif()
    if(NOT line12 MATCHES [=["id":13]=] OR NOT line12 MATCHES [=["code":-32000]=])
        message(FATAL_ERROR "${label}: escaping load_gltf path did not return -32000:\n${line12}")
    endif()
    if(NOT line13 MATCHES [=["id":14]=] OR NOT line13 MATCHES [=["code":-32602]=])
        message(FATAL_ERROR "${label}: malformed update_transforms params did not return -32602:\n${line13}")
    endif()
    if(NOT line14 MATCHES [=["code":-32700]=])
        message(FATAL_ERROR "${label}: malformed JSON did not return -32700:\n${line14}")
    endif()
    if(NOT line15 MATCHES [=["code":-32600]=])
        message(FATAL_ERROR "${label}: id-less request did not return -32600:\n${line15}")
    endif()
    if(NOT line16 MATCHES [=["id":15]=] OR NOT line16 MATCHES [=["code":-32601]=])
        message(FATAL_ERROR "${label}: unknown method did not return -32601:\n${line16}")
    endif()
    if(NOT line17 MATCHES [=["id":16]=] OR NOT line17 MATCHES [=["seed":99]=])
        message(FATAL_ERROR "${label}: set_seed response did not include the updated seed:\n${line17}")
    endif()
    if(NOT line18 MATCHES [=["id":17]=] OR NOT line18 MATCHES [=["instance_id":"[0-9a-fA-F-]+"]=] OR NOT line18 MATCHES [=["frame":1]=] OR NOT line18 MATCHES [=["seed":99]=])
        message(FATAL_ERROR "${label}: get_status after set_seed did not include updated seed:\n${line18}")
    endif()
    if(NOT line19 MATCHES [=["id":18]=] OR NOT line19 MATCHES [=["code":-32602]=] OR NOT line19 MATCHES [=['rot']=] OR NOT line19 MATCHES [=['rotation']=])
        message(FATAL_ERROR "${label}: legacy rot field did not name the rotation replacement:\n${line19}")
    endif()
    if(NOT line20 MATCHES [=["id":19]=] OR NOT line20 MATCHES [=["code":-32010]=] OR NOT line20 MATCHES "\"reason\":\"${RENDERDOC_REASON}\"" OR NOT line20 MATCHES [=["source":"rpc"]=])
        message(FATAL_ERROR "${label}: uninjected capture_gpu did not return the named RenderDoc error:\n${line20}")
    endif()
endfunction()

function(normalize_rpc_stdout stdout output_var)
    string(REGEX REPLACE [=["instance_id":"[0-9a-fA-F-]+"]=] [=["instance_id":"<uuid>"]=] normalized "${stdout}")
    # Startup durations and cold/warm hit counts are observational diagnostics,
    # not deterministic simulation state. Preserve the field in protocol
    # validation above, then mask its values for the existing two-run equality gate.
    string(REGEX REPLACE [=["startup":\{[^\}]*\}]=] [=["startup":"<measured>"]=] normalized "${normalized}")
    # GPU timestamps are diagnostics. Validate their schema above, then mask
    # the complete object for the two-run deterministic transcript gate.
    string(REGEX REPLACE [=["gpu_timing":\{.*\},"input"]=] [=["gpu_timing":"<measured>","input"]=] normalized "${normalized}")
    string(REGEX REPLACE [=["memory":\{.*\},"modules"]=] [=["memory":"<measured>","modules"]=] normalized "${normalized}")
    set(${output_var} "${normalized}" PARENT_SCOPE)
endfunction()

function(run_rpc_once label output_var)
    file(REMOVE "${capture_path}")
    file(REMOVE "${capture_left_path}")
    file(REMOVE "${capture_right_path}")
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
    validate_rpc_stdout("${stdout}" "${label}")

    if(NOT EXISTS "${capture_left_path}")
        message(FATAL_ERROR "${label}: first rpc capture output missing: ${capture_left_path}")
    endif()
    if(NOT EXISTS "${capture_right_path}")
        message(FATAL_ERROR "${label}: second rpc capture output missing: ${capture_right_path}")
    endif()
    file(SIZE "${capture_left_path}" left_size)
    file(SIZE "${capture_right_path}" right_size)
    if(left_size EQUAL 0)
        message(FATAL_ERROR "${label}: first rpc capture output is empty: ${capture_left_path}")
    endif()
    if(right_size EQUAL 0)
        message(FATAL_ERROR "${label}: second rpc capture output is empty: ${capture_right_path}")
    endif()

    file(READ "${capture_left_path}" left_hex HEX)
    file(READ "${capture_right_path}" right_hex HEX)
    if(left_hex STREQUAL right_hex)
        message(FATAL_ERROR "${label}: update_transforms did not change captured PNG bytes")
    endif()

    normalize_rpc_stdout("${stdout}" normalized_stdout)
    set(${output_var} "${normalized_stdout}" PARENT_SCOPE)
endfunction()

run_rpc_once("first run" first_stdout)
run_rpc_once("second run" second_stdout)

if(NOT first_stdout STREQUAL second_stdout)
    message(FATAL_ERROR "rpc response stream is not deterministic\nfirst:\n${first_stdout}\nsecond:\n${second_stdout}")
endif()
