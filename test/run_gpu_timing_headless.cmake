if(NOT DEFINED PLAYER)
    message(FATAL_ERROR "PLAYER is required")
endif()
if(NOT DEFINED PLAYER_DIR)
    message(FATAL_ERROR "PLAYER_DIR is required")
endif()
if(NOT DEFINED OUT_DIR)
    message(FATAL_ERROR "OUT_DIR is required")
endif()

file(REMOVE_RECURSE "${OUT_DIR}")
file(MAKE_DIRECTORY
    "${OUT_DIR}/project/scenes"
    "${OUT_DIR}/project/assets"
    "${OUT_DIR}/project/passes"
    "${OUT_DIR}/project/shaders"
    "${OUT_DIR}/project/ui"
)

file(WRITE "${OUT_DIR}/project/project.json" [=[
{
  "schema": "pelican.project",
  "version": 1,
  "name": "gpu_timing_headless",
  "engine_min_version": "0.1.0",
  "basic_config": {
    "window_title": "GPU Timing Headless",
    "window_size": {"width": 32, "height": 32},
    "fullscreen": false,
    "framerate": 30,
    "camera": {"yfov": 0.7853981633974483, "znear": 0.1, "zfar": 100.0, "up": [0.0, 1.0, 0.0]},
    "default_scene_id": "default_scene",
    "scene_data_json": "scenes/main.scene.json",
    "asset_data_json": "assets/asset_data.json",
    "rendering_config_json": "passes/main.json",
    "default_rendering_pass": "main",
    "ui_config_json": "ui/ui.json"
  }
}
]=])

file(WRITE "${OUT_DIR}/project/scenes/main.scene.json" [=[
{
  "schema": "pelican.scene",
  "version": 1,
  "scenes": {
    "default_scene": {
      "objects": []
    }
  }
}
]=])
file(WRITE "${OUT_DIR}/project/assets/asset_data.json" "{\"models\":[]}\n")
file(WRITE "${OUT_DIR}/project/ui/ui.json" "{\"schema\":\"pelican.ui\",\"version\":1,\"key\":\"empty\",\"root\":{\"id\":\"root\",\"type\":\"panel\"}}\n")
file(WRITE "${OUT_DIR}/project/shaders/present.frag" [=[
#version 450
layout(location = 0) out vec4 outColor;
void main() {
    outColor = vec4(0.03, 0.04, 0.07, 1.0);
}
]=])
file(WRITE "${OUT_DIR}/project/passes/main.json" [=[
{
  "features": ["engine://features/gpu_timing.json"],
  "render_targets": [],
  "rendering_passes": [
    {
      "name": "main",
      "passes": [
        {
          "name": "present",
          "type": "fullscreen",
          "output": {"color": "swapchain", "depth": null},
          "shader": {
            "vertex": "engine://fullscreen",
            "fragment": "shaders/present"
          }
        }
      ]
    }
  ]
}
]=])

execute_process(
    COMMAND "${PLAYER}"
        --headless
        --project "${OUT_DIR}/project"
        --frames 3
        --size 32x32
        --fps 30
    WORKING_DIRECTORY "${PLAYER_DIR}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
)

set(log_text "${stdout}\n${stderr}")
if(NOT result EQUAL 0)
    message(FATAL_ERROR "gpu timing headless run failed with ${result}\nstdout:\n${stdout}\nstderr:\n${stderr}")
endif()

if(log_text MATCHES "Validation Error|VUID-")
    message(FATAL_ERROR "gpu timing headless run emitted Vulkan validation errors\nstdout:\n${stdout}\nstderr:\n${stderr}")
endif()

if(NOT log_text MATCHES "pelican frame metrics")
    message(FATAL_ERROR "gpu timing headless run did not emit frame metrics\nstdout:\n${stdout}\nstderr:\n${stderr}")
endif()
if(NOT log_text MATCHES "cpu_update_ms_avg=[0-9]+\\.[0-9]+")
    message(FATAL_ERROR "gpu timing metrics did not include numeric update time\nstdout:\n${stdout}\nstderr:\n${stderr}")
endif()
if(NOT log_text MATCHES "cpu_render_ms_avg=[0-9]+\\.[0-9]+")
    message(FATAL_ERROR "gpu timing metrics did not include numeric render time\nstdout:\n${stdout}\nstderr:\n${stderr}")
endif()
if(NOT log_text MATCHES "cpu_present_wait_ms_avg=[0-9]+\\.[0-9]+")
    message(FATAL_ERROR "gpu timing metrics did not include numeric present wait time\nstdout:\n${stdout}\nstderr:\n${stderr}")
endif()

# Exercise the additive get_status.gpu_timing schema. Three current-time
# renders rotate back to the first in-flight slot, which reclaims and
# publishes at least one completed timestamp range without an explicit wait.
set(rpc_script "${OUT_DIR}/gpu_timing_status.ndjson")
file(WRITE "${rpc_script}"
    "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"render_frame\",\"params\":{}}\n"
    "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"step_frame\",\"params\":{}}\n"
    "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"step_frame\",\"params\":{}}\n"
    "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"get_status\",\"params\":{}}\n"
)
execute_process(
    COMMAND "${PLAYER}"
        --rpc
        --headless
        --project "${OUT_DIR}/project"
        --size 32x32
    WORKING_DIRECTORY "${PLAYER_DIR}"
    INPUT_FILE "${rpc_script}"
    RESULT_VARIABLE rpc_result
    OUTPUT_VARIABLE rpc_stdout
    ERROR_VARIABLE rpc_stderr
)
if(NOT rpc_result EQUAL 0)
    message(FATAL_ERROR "gpu timing RPC status run failed with ${rpc_result}\nstdout:\n${rpc_stdout}\nstderr:\n${rpc_stderr}")
endif()
if(NOT rpc_stdout MATCHES [=["gpu_timing":\{"dropped_samples":0,"enabled":true,"history_capacity":120,"history_count":1]=])
    message(FATAL_ERROR "get_status.gpu_timing did not expose the enabled 120-frame schema\nstdout:\n${rpc_stdout}")
endif()
if(NOT rpc_stdout MATCHES [=["schema_version":2,"supported":true]=] OR
   NOT rpc_stdout MATCHES [=["view_index":0]=] OR
   NOT rpc_stdout MATCHES [=["subrange":"barriers"]=] OR
   NOT rpc_stdout MATCHES [=["subrange":"body"]=] OR
   NOT rpc_stdout MATCHES [=["create_count":1]=])
    message(FATAL_ERROR "get_status.gpu_timing omitted identity, subranges, or pool reuse counters\nstdout:\n${rpc_stdout}")
endif()
