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
file(MAKE_DIRECTORY "${OUT_DIR}/passes")
file(MAKE_DIRECTORY "${OUT_DIR}/shaders")
file(MAKE_DIRECTORY "${OUT_DIR}/ui")

set(capture_path "${OUT_DIR}/compute_buffer.png")

file(WRITE "${OUT_DIR}/project.json" [=[
{
  "schema": "pelican.project",
  "version": 1,
  "name": "compute headless fixture",
  "engine_min_version": "0.1.0",
  "basic_config": {
    "window_title": "Compute Headless",
    "window_size": {"width": 32, "height": 32},
    "fullscreen": false,
    "framerate": 60,
    "camera": {"yfov": 0.7853981633974483, "znear": 0.1, "zfar": 100.0, "up": [0, 1, 0]},
    "default_scene_id": "default_scene",
    "scene_data_json": "scene.json",
    "asset_data_json": "assets.json",
    "rendering_config_json": "passes/main.json",
    "ui_config_json": "ui/ui.json",
    "default_rendering_pass": "main"
  }
}
]=])

file(WRITE "${OUT_DIR}/scene.json" [=[
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
file(WRITE "${OUT_DIR}/assets.json" "{\"models\":[]}\n")
file(WRITE "${OUT_DIR}/ui/ui.json" "{\"images\":[]}\n")

file(WRITE "${OUT_DIR}/shaders/fullscreen.vert" [=[
#version 450
layout(location = 0) out vec2 outUV;
vec2 positions[6] = vec2[](
    vec2(-1.0, -1.0),
    vec2( 1.0, -1.0),
    vec2( 1.0,  1.0),
    vec2(-1.0, -1.0),
    vec2( 1.0,  1.0),
    vec2(-1.0,  1.0)
);
void main() {
    vec2 pos = positions[gl_VertexIndex];
    outUV = pos * 0.5 + 0.5;
    gl_Position = vec4(pos, 0.0, 1.0);
}
]=])

file(WRITE "${OUT_DIR}/shaders/write_color.comp" [=[
#version 450
layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;
layout(std430, set = 1, binding = 0) buffer ComputeColor {
    vec4 color;
} compute_color;
void main() {
    compute_color.color = vec4(0.10, 0.75, 0.35, 1.0);
}
]=])

file(WRITE "${OUT_DIR}/shaders/compute_present.frag" [=[
#version 450
layout(std430, set = 1, binding = 0) readonly buffer ComputeColor {
    vec4 color;
} compute_color;
layout(location = 0) out vec4 outColor;
void main() {
    outColor = compute_color.color;
}
]=])

file(WRITE "${OUT_DIR}/passes/main.json" [=[
{
  "buffers": [
    {"name": "compute_color", "size": 16, "lifetime": "persistent"}
  ],
  "rendering_passes": [
    {
      "name": "main",
      "passes": [
        {
          "name": "present",
          "type": "fullscreen",
          "input": ["compute_color"],
          "output": {"color": "swapchain", "depth": null},
          "shader": {
            "vertex": "shaders/fullscreen",
            "fragment": "shaders/compute_present"
          }
        }
      ]
    }
  ],
  "compute_tasks": [
    {
      "name": "write_color",
      "shader": "shaders/write_color",
      "writes": ["compute_color"],
      "before": ["present"],
      "dispatch": {"groups": [1, 1, 1]},
      "schedule": "per_frame"
    }
  ]
}
]=])

execute_process(
    COMMAND "${PLAYER}"
        --headless
        --project "${OUT_DIR}"
        --frames 1
        --size 32x32
        --render-out "${capture_path}"
    WORKING_DIRECTORY "${PLAYER_DIR}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
)

if(NOT result EQUAL 0)
    message(FATAL_ERROR "compute headless player run failed with ${result}\nstdout:\n${stdout}\nstderr:\n${stderr}")
endif()
if(stdout MATCHES "Validation Error|VUID-" OR stderr MATCHES "Validation Error|VUID-")
    message(FATAL_ERROR "compute headless player run emitted Vulkan validation errors\nstdout:\n${stdout}\nstderr:\n${stderr}")
endif()
if(NOT EXISTS "${capture_path}")
    message(FATAL_ERROR "compute headless capture output missing: ${capture_path}")
endif()
file(SIZE "${capture_path}" size)
if(size EQUAL 0)
    message(FATAL_ERROR "compute headless capture output is empty: ${capture_path}")
endif()
