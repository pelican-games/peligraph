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
file(WRITE "${OUT_DIR}/ui/ui.json" "{\"schema\":\"pelican.ui\",\"version\":1,\"key\":\"empty\",\"root\":{\"id\":\"root\",\"type\":\"panel\"}}\n")

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

file(WRITE "${OUT_DIR}/shaders/transform_image.comp" [=[
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "pelican_frame.glsl"
#include "pelican_resource_ports.glsl"
layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;
void main() {
    vec2 uv = vec2(0.5) / vec2(pelican_size_seed_image());
    vec4 value = pelican_sample_seed_image(uv);
    float frame_light_probe =
        pelicanFrame.time_delta.x +
        pelicanFrame.camera_position.x +
        float(pelicanLights.directionalLightCount);
    if (isnan(frame_light_probe)) {
        value = vec4(1.0, 0.0, 1.0, 1.0);
    }
    pelican_store_result_image(ivec2(0, 0), value);
}
]=])

file(WRITE "${OUT_DIR}/shaders/sample_image.frag" [=[
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "pelican_resource_ports.glsl"
layout(location = 0) in vec2 outUV;
layout(location = 0) out vec4 outColor;
void main() {
    outColor = pelican_sample_source_image(outUV);
}
]=])

file(WRITE "${OUT_DIR}/shaders/image_to_buffer.comp" [=[
#version 450
layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;
layout(rgba8, set = 1, binding = 0) uniform readonly image2D sampled_color;
layout(std430, set = 1, binding = 1) buffer RawColor {
    vec4 value;
} raw_color;
void main() {
    raw_color.value = imageLoad(sampled_color, ivec2(0, 0));
}
]=])

file(WRITE "${OUT_DIR}/shaders/transform_color.comp" [=[
#version 450
layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;
layout(std430, set = 1, binding = 0) readonly buffer RawColor {
    vec4 value;
} raw_color;
layout(std430, set = 1, binding = 1) buffer ComputeColor {
    vec4 value;
} compute_color;
void main() {
    compute_color.value = raw_color.value;
}
]=])

file(WRITE "${OUT_DIR}/shaders/build_dispatch.comp" [=[
#version 450
layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;
layout(std430, set = 1, binding = 0) buffer DispatchArguments {
    uint x;
    uint y;
    uint z;
} dispatch_arguments;
void main() {
    dispatch_arguments.x = 1;
    dispatch_arguments.y = 1;
    dispatch_arguments.z = 1;
}
]=])

file(WRITE "${OUT_DIR}/shaders/compute_present.frag" [=[
#version 450
layout(std430, set = 1, binding = 0) readonly buffer ComputeColor {
    vec4 value;
} compute_color;
layout(location = 0) out vec4 outColor;
void main() {
    outColor = compute_color.value;
}
]=])

file(WRITE "${OUT_DIR}/passes/main.json" [=[
{
  "buffers": [
    {"name": "raw_color", "size": 16, "lifetime": "persistent"},
    {"name": "compute_color", "size": 16, "lifetime": "persistent"},
    {
      "name": "dispatch_arguments",
      "size": 12,
      "lifetime": "persistent",
      "command_layout": "compute_dispatch"
    }
  ],
  "render_targets": [
    {"name": "material_albedo", "extent_scale": 1.0, "format": "B8G8R8A8_UNORM", "format_class": "scene", "role": "color", "usage": ["COLOR_ATTACHMENT"]},
    {"name": "material_normal", "extent_scale": 1.0, "format": "R16G16B16A16_SFLOAT", "format_class": "data", "role": "data", "usage": ["COLOR_ATTACHMENT"]},
    {"name": "seed_image", "extent_scale": 1.0, "format": "R8G8B8A8_UNORM", "format_class": "data", "role": "data", "usage": ["COLOR_ATTACHMENT", "STORAGE", "SAMPLED"]},
    {"name": "material_worldpos", "extent_scale": 1.0, "format": "R16G16B16A16_SFLOAT", "format_class": "data", "role": "data", "usage": ["COLOR_ATTACHMENT"]},
    {"name": "material_emissive", "extent_scale": 1.0, "format": "B8G8R8A8_UNORM", "format_class": "scene", "role": "color", "usage": ["COLOR_ATTACHMENT"]},
    {"name": "result_image", "extent_scale": 1.0, "format": "R8G8B8A8_UNORM", "format_class": "data", "role": "data", "usage": ["COLOR_ATTACHMENT", "STORAGE", "SAMPLED"]},
    {"name": "material_depth", "extent_scale": 1.0, "format": "D32_SFLOAT", "format_class": "data", "role": "data", "usage": ["DEPTH_STENCIL_ATTACHMENT"]},
    {"name": "sampled_color", "extent_scale": 1.0, "format": "R8G8B8A8_UNORM", "format_class": "data", "role": "data", "usage": ["COLOR_ATTACHMENT", "STORAGE"]}
  ],
  "rendering_passes": [
    {
      "name": "main",
      "passes": [
        {
          "name": "seed_render",
          "type": "material",
          "output": {
            "color": ["material_albedo", "material_normal", "seed_image", "material_worldpos", "material_emissive"],
            "depth": "material_depth"
          },
          "clear_color": [0.10, 0.75, 0.35, 1.0]
        },
        {
          "name": "initialize_result",
          "type": "fullscreen",
          "input": ["seed_image"],
          "resource_ports": {
            "source_image": {
              "resource": "seed_image",
              "access": "sampled"
            }
          },
          "output": {"color": "result_image", "depth": null},
          "shader": {"vertex": "shaders/fullscreen", "fragment": "shaders/sample_image"}
        },
        {
          "name": "sample_render",
          "type": "fullscreen",
          "after": ["transform_image"],
          "input": ["result_image"],
          "resource_ports": {
            "source_image": {
              "resource": "result_image",
              "access": "sampled"
            }
          },
          "output": {"color": "sampled_color", "depth": null},
          "shader": {"vertex": "shaders/fullscreen", "fragment": "shaders/sample_image"}
        },
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
      "name": "transform_image",
      "shader": "shaders/transform_image",
      "reads": ["seed_image"],
      "writes": ["result_image"],
      "resource_ports": {
        "seed_image": {
          "access": "sampled",
          "sampling": {
            "filter": "nearest",
            "address": "clamp_to_edge"
          }
        },
        "result_image": {
          "access": "storage"
        }
      },
      "after": ["initialize_result"],
      "dispatch": {"groups": [1, 1, 1]},
      "schedule": "per_frame"
    },
    {
      "name": "image_to_buffer",
      "shader": "shaders/image_to_buffer",
      "reads": ["sampled_color"],
      "writes": ["raw_color"],
      "dispatch": {"groups": [1, 1, 1]},
      "schedule": "per_frame"
    },
    {
      "name": "build_dispatch",
      "shader": "shaders/build_dispatch",
      "writes": ["dispatch_arguments"],
      "dispatch": {"groups": [1, 1, 1]},
      "schedule": "per_frame"
    },
    {
      "name": "transform_color",
      "shader": "shaders/transform_color",
      "reads": ["raw_color"],
      "writes": ["compute_color"],
      "dispatch": {
        "indirect": {
          "buffer": "dispatch_arguments"
        }
      },
      "schedule": "per_frame"
    }
  ]
}
]=])

execute_process(
    COMMAND "${PLAYER}"
        --headless
        --project "${OUT_DIR}"
        --frames 5
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
