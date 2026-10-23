# O-1: `pelican_player --start-time <sec>` starts a headless run at a named time
# by replaying the frames that lead to it.
#
# The claim being tested is an equivalence, so it is checked as one: the frame a
# started run draws first must be byte-identical to the frame a plain run draws
# at the same step. The fixture's only visible content is a
# `value.trigger_clock`, uploaded to a one-float buffer and read by the present
# shader, so the picture is the clock -- a run that skipped the replay, or
# replayed the wrong number of frames, draws a different colour rather than the
# same one.

if(NOT DEFINED PLAYER)
    message(FATAL_ERROR "PLAYER is required")
endif()
if(NOT DEFINED PLAYER_DIR)
    message(FATAL_ERROR "PLAYER_DIR is required")
endif()
if(NOT DEFINED OUT_DIR)
    message(FATAL_ERROR "OUT_DIR is required")
endif()

# The same rejection cases run without a GPU in the limits-only CTest.
if(LIMITS_ONLY)
    include("${CMAKE_CURRENT_LIST_DIR}/run_offline_start_time_limits.cmake")
    return()
endif()

file(REMOVE_RECURSE "${OUT_DIR}")
file(MAKE_DIRECTORY "${OUT_DIR}/project/passes")
file(MAKE_DIRECTORY "${OUT_DIR}/project/shaders")
file(MAKE_DIRECTORY "${OUT_DIR}/project/ui")
file(MAKE_DIRECTORY "${OUT_DIR}/frames")
file(MAKE_DIRECTORY "${OUT_DIR}/started")

file(WRITE "${OUT_DIR}/project/project.json" [=[
{
  "schema": "pelican.project",
  "version": 1,
  "name": "offline start time fixture",
  "engine_min_version": "0.1.0",
  "basic_config": {
    "window_title": "Offline Start Time",
    "window_size": {"width": 64, "height": 64},
    "fullscreen": false,
    "framerate": 60,
    "camera": {"yfov": 0.7853981633974483, "znear": 0.1, "zfar": 100.0},
    "default_scene_id": "default_scene",
    "scene_data_json": "scene.json",
    "asset_data_json": "assets.json",
    "rendering_config_json": "passes/main.json",
    "ui_config_json": "ui/ui.json",
    "default_rendering_pass": "main"
  }
}
]=])

file(WRITE "${OUT_DIR}/project/scene.json" [=[
{
  "schema": "pelican.scene",
  "version": 2,
  "scenes": {"default_scene": {"objects": []}}
}
]=])
file(WRITE "${OUT_DIR}/project/assets.json" "{\"schema\":\"pelican.asset_data\",\"version\":2,\"models\":[]}\n")
file(WRITE "${OUT_DIR}/project/ui/ui.json" "{\"schema\":\"pelican.ui\",\"version\":1,\"key\":\"empty\",\"root\":{\"id\":\"root\",\"type\":\"panel\"}}\n")

file(WRITE "${OUT_DIR}/project/shaders/fullscreen.vert" [=[
#version 450
vec2 positions[6] = vec2[](
    vec2(-1.0, -1.0), vec2( 1.0, -1.0), vec2( 1.0,  1.0),
    vec2(-1.0, -1.0), vec2( 1.0,  1.0), vec2(-1.0,  1.0));
void main() {
    gl_Position = vec4(positions[gl_VertexIndex], 0.0, 1.0);
}
]=])

# The elapsed clock is the only thing on screen. It wraps rather than saturating,
# so neighbouring frames stay far apart in colour however long the run is, and a
# quantized ramp keeps the gap well above any sRGB rounding: one frame of
# difference is many least significant bits, not one.
file(WRITE "${OUT_DIR}/project/shaders/clock_present.frag" [=[
#version 450
layout(std430, set = 1, binding = 0) readonly buffer ClockValue {
    float value;
} clock_value;
layout(location = 0) out vec4 outColor;
void main() {
    float wrapped = fract(clock_value.value * 4.0);
    float stepped = floor(wrapped * 64.0) / 64.0;
    outColor = vec4(stepped, 1.0 - stepped, 0.25, 1.0);
}
]=])

# `trigger` is a constant 1, so the clock starts on the first evaluated frame and
# its elapsed time is (engine time - one step) from then on. Reaching a given
# elapsed value therefore requires every frame in between: exactly the property
# `--start-time` has to reproduce.
file(WRITE "${OUT_DIR}/project/passes/main.json" [=[
{
  "schema": "pelican.graph",
  "version": 1,
  "nodes": [
    {"id": "clock_time", "op": "value.input", "parameters": {"name": "time"}, "outputs": ["value"]},
    {"id": "always", "op": "value.constant", "parameters": {"value": 1}, "outputs": ["value"]},
    {"id": "clock", "op": "value.trigger_clock",
     "inputs": {"time": {"node": "clock_time", "port": "value"},
                "trigger": {"node": "always", "port": "value"}},
     "outputs": ["value"]},
    {"id": "clock_buffer", "op": "render.buffer",
     "parameters": {"element": "float"},
     "inputs": {"value": {"node": "clock", "port": "value"}},
     "outputs": ["resource"]}
  ],
  "outputs": {},
  "rendering": {
    "render_targets": [
      {"name": "material_albedo", "extent_scale": 1.0, "format": "B8G8R8A8_UNORM", "format_class": "scene", "role": "color", "usage": ["COLOR_ATTACHMENT"]},
      {"name": "material_normal", "extent_scale": 1.0, "format": "R16G16B16A16_SFLOAT", "format_class": "data", "role": "data", "usage": ["COLOR_ATTACHMENT"]},
      {"name": "material_extra", "extent_scale": 1.0, "format": "R8G8B8A8_UNORM", "format_class": "data", "role": "data", "usage": ["COLOR_ATTACHMENT"]},
      {"name": "material_worldpos", "extent_scale": 1.0, "format": "R16G16B16A16_SFLOAT", "format_class": "data", "role": "data", "usage": ["COLOR_ATTACHMENT"]},
      {"name": "material_emissive", "extent_scale": 1.0, "format": "B8G8R8A8_UNORM", "format_class": "scene", "role": "color", "usage": ["COLOR_ATTACHMENT"]},
      {"name": "material_depth", "extent_scale": 1.0, "format": "D32_SFLOAT", "format_class": "data", "role": "data", "usage": ["DEPTH_STENCIL_ATTACHMENT"]}
    ],
    "rendering_passes": [
      {
        "name": "main",
        "passes": [
          {
            "name": "scene_render",
            "type": "material",
            "output": {
              "color": ["material_albedo", "material_normal", "material_extra", "material_worldpos", "material_emissive"],
              "depth": "material_depth"
            },
            "clear_color": [0.0, 0.0, 0.0, 1.0]
          },
          {
            "name": "present",
            "type": "fullscreen",
            "input": ["clock_buffer"],
            "output": {"color": "swapchain", "depth": null},
            "shader": {"vertex": "shaders/fullscreen", "fragment": "shaders/clock_present"}
          }
        ]
      }
    ]
  }
}
]=])

function(run_player label expect_success out_stderr)
    execute_process(
        COMMAND "${PLAYER}" --headless --project "${OUT_DIR}/project" --size 64x64 --fps 60 ${ARGN}
        WORKING_DIRECTORY "${PLAYER_DIR}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE stdout
        ERROR_VARIABLE stderr
        TIMEOUT 600
    )
    if(expect_success AND NOT result EQUAL 0)
        message(FATAL_ERROR "${label} failed with ${result}\nstdout:\n${stdout}\nstderr:\n${stderr}")
    endif()
    if(NOT expect_success AND result EQUAL 0)
        message(FATAL_ERROR "${label} was expected to fail but succeeded\nstderr:\n${stderr}")
    endif()
    if(expect_success AND (stdout MATCHES "Validation Error|VUID-" OR stderr MATCHES "Validation Error|VUID-"))
        message(FATAL_ERROR "${label} emitted Vulkan validation errors\nstdout:\n${stdout}\nstderr:\n${stderr}")
    endif()
    set(${out_stderr} "${stderr}" PARENT_SCOPE)
endfunction()

function(read_hex label path out_hex)
    if(NOT EXISTS "${path}")
        message(FATAL_ERROR "${label} did not write ${path}")
    endif()
    file(SIZE "${path}" size)
    if(size EQUAL 0)
        message(FATAL_ERROR "${label} wrote an empty ${path}")
    endif()
    file(READ "${path}" hex HEX)
    set(${out_hex} "${hex}" PARENT_SCOPE)
endfunction()

# The plain run is the oracle: 121 frames of the ordinary fixed-step timeline.
run_player("plain 121 frames" TRUE plain_stderr
    --frames 121 --render-out "${OUT_DIR}/frames/%04d.png")
read_hex("plain run" "${OUT_DIR}/frames/0121.png" frame_121_hex)
read_hex("plain run" "${OUT_DIR}/frames/0120.png" frame_120_hex)
read_hex("plain run" "${OUT_DIR}/frames/0119.png" frame_119_hex)
read_hex("plain run" "${OUT_DIR}/frames/0001.png" frame_001_hex)

# Without these the equivalence below would also hold for a player that drew the
# same picture on every frame, which would make the whole test vacuous.
if(frame_120_hex STREQUAL frame_119_hex)
    message(FATAL_ERROR "the fixture draws frame 119 and frame 120 identically; the oracle proves nothing")
endif()
if(frame_120_hex STREQUAL frame_001_hex)
    message(FATAL_ERROR "the fixture draws frame 1 and frame 120 identically; the oracle proves nothing")
endif()

# 2.0 s at 60 fps is frame 120. The started run's first captured frame must be
# that frame, and its second must be the one after it: the run continues on the
# same timeline rather than drawing one picture out of nowhere.
run_player("started run" TRUE started_stderr
    --start-time 2.0 --frames 2 --render-out "${OUT_DIR}/started/%04d.png")
# The bytes are the claim, so they are checked first: the reported plan is a
# description of what the run did, and a description cannot stand in for it.
read_hex("started run" "${OUT_DIR}/started/0001.png" started_hex)
if(NOT started_hex STREQUAL frame_120_hex)
    message(FATAL_ERROR
        "--start-time 2.0 did not reproduce frame 120 of a plain run\n"
        "started: ${started_hex}\nplain  : ${frame_120_hex}")
endif()
# Named for what it rules out: replaying one frame too few lands here instead.
if(started_hex STREQUAL frame_119_hex)
    message(FATAL_ERROR "--start-time 2.0 reproduced frame 119, not frame 120")
endif()
read_hex("started run" "${OUT_DIR}/started/0002.png" started_second_hex)
if(NOT started_second_hex STREQUAL frame_121_hex)
    message(FATAL_ERROR
        "--start-time 2.0 --frames 2 did not continue into frame 121\n"
        "started: ${started_second_hex}\nplain  : ${frame_121_hex}")
endif()
if(NOT started_stderr MATCHES "offline: start_time=2 start_frame=120 replay=119 frames=2")
    message(FATAL_ERROR "the started run did not report its replay plan:\n${started_stderr}")
endif()

# A time between two frames is quantized to the nearer one rather than refused
# or silently truncated: 2.004 s at 60 fps is still frame 120.
run_player("quantized run" TRUE quantized_stderr
    --start-time 2.004 --frames 1 --render-out "${OUT_DIR}/started/quantized.png")
if(NOT quantized_stderr MATCHES "start_frame=120")
    message(FATAL_ERROR "--start-time 2.004 did not quantize to frame 120:\n${quantized_stderr}")
endif()
read_hex("quantized run" "${OUT_DIR}/started/quantized.png" quantized_hex)
if(NOT quantized_hex STREQUAL frame_120_hex)
    message(FATAL_ERROR "--start-time 2.004 did not draw the same frame as --start-time 2.0")
endif()

# Named refusals. All three are decided while the command line is read, so none
# of these runs reaches the GPU.
run_player("negative time" FALSE negative_stderr --start-time -1 --frames 1)
if(NOT negative_stderr MATCHES "offline\\.time_invalid@1")
    message(FATAL_ERROR "--start-time -1 was not refused by name:\n${negative_stderr}")
endif()
run_player("non-numeric time" FALSE text_stderr --start-time later --frames 1)
if(NOT text_stderr MATCHES "offline\\.time_invalid@1")
    message(FATAL_ERROR "--start-time later was not refused by name:\n${text_stderr}")
endif()

# FX-6: overflow, replay-budget limits, and nearest-frame boundary semantics.
include("${CMAKE_CURRENT_LIST_DIR}/run_offline_start_time_limits.cmake")

# --headless is not optional: there is no fixed-step timeline to count without it.
execute_process(
    COMMAND "${PLAYER}" --project "${OUT_DIR}/project" --size 64x64 --fps 60 --start-time 1.0 --frames 1
    WORKING_DIRECTORY "${PLAYER_DIR}"
    RESULT_VARIABLE windowed_result
    OUTPUT_VARIABLE windowed_stdout
    ERROR_VARIABLE windowed_stderr
    TIMEOUT 120
)
if(windowed_result EQUAL 0)
    message(FATAL_ERROR "--start-time without --headless was accepted")
endif()
if(NOT windowed_stderr MATCHES "offline\\.headless_required@1")
    message(FATAL_ERROR "--start-time without --headless was not refused by name:\n${windowed_stderr}")
endif()

message(STATUS "offline --start-time: frame120=${frame_120_hex}")
