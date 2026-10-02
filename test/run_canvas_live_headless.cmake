# CV-1b: the canvas document CV-1 lowers with the cli, run as it is written.
# A blank project gets the same pelican.canvas nodes, its present pass reads
# the output by the author's id ("input": ["out"]), and the player draws it
# headless without `pelican_cli canvas-lower`: the engine lowers the document
# where it reads it. Frames 1 and 61 are held against the CPU reference of the
# authored document with CV-1's tolerance (see run_canvas_headless.cmake):
# corner_radius reads 0 at frame 1 and 60 at frame 61, so the two differ only
# when the lowering's render.scalar reached the value graph.
#
# The engine writes the generated shaders under the project's .pelican (one
# directory per shader pair) and nothing else: the authored document keeps its
# bytes, and no shaders/generated appears.

foreach(variable IN ITEMS CLI PLAYER PLAYER_DIR COMPARE OUT_DIR)
    if(NOT DEFINED ${variable})
        message(FATAL_ERROR "${variable} is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${OUT_DIR}")
file(MAKE_DIRECTORY "${OUT_DIR}/frames")
set(project_dir "${OUT_DIR}/authored")

execute_process(
    COMMAND "${CLI}" project init "${project_dir}" --template blank
    RESULT_VARIABLE init_result
    OUTPUT_VARIABLE init_stdout
    ERROR_VARIABLE init_stderr)
if(NOT init_result EQUAL 0)
    message(FATAL_ERROR "project init failed (${init_result})\n${init_stdout}\n${init_stderr}")
endif()

file(READ "${project_dir}/project.json" project)
string(JSON project SET "${project}" requires "[\"canvas\"]")
file(WRITE "${project_dir}/project.json" "${project}")

set(document "${project_dir}/passes/main_rendering_config.json")
file(WRITE "${document}" [=[
{
  "schema": "pelican.graph",
  "version": 1,
  "definitions": {},
  "nodes": [
    {"id": "clock", "op": "value.input", "parameters": {"name": "time"}, "inputs": {}, "outputs": ["value"]},
    {"id": "radius_curve", "op": "value.curve",
     "parameters": {"keys": [{"time": 0, "value": 0}, {"time": 0.5, "value": 0}, {"time": 0.51, "value": 60}, {"time": 100, "value": 60}]},
     "inputs": {"time": {"node": "clock", "port": "value"}}, "outputs": ["value"]},
    {"id": "card", "op": "shape.rect",
     "parameters": {"size": [200, 140], "center": [100, 90], "corner_radius": 0},
     "inputs": {"corner_radius": {"node": "radius_curve", "port": "value"}}, "outputs": ["geometry"]},
    {"id": "card_fill", "op": "shape.fill", "parameters": {"color": [0.2, 0.4, 0.9, 1.0]},
     "inputs": {"geometry": {"node": "card", "port": "geometry"}}, "outputs": ["layer"]},
    {"id": "dot", "op": "shape.ellipse", "parameters": {"size": [100, 100], "center": [160, 160]},
     "inputs": {}, "outputs": ["geometry"]},
    {"id": "dot_fill", "op": "shape.fill", "parameters": {"color": [1.0, 0.5, 0.1, 1.0]},
     "inputs": {"geometry": {"node": "dot", "port": "geometry"}}, "outputs": ["layer"]},
    {"id": "ring", "op": "shape.ellipse", "parameters": {"size": [160, 90], "center": [128, 200], "rotation": 30},
     "inputs": {}, "outputs": ["geometry"]},
    {"id": "ring_stroke", "op": "shape.stroke", "parameters": {"color": [1.0, 1.0, 1.0, 1.0], "width": 6},
     "inputs": {"geometry": {"node": "ring", "port": "geometry"}}, "outputs": ["layer"]},
    {"id": "frame", "op": "shape.rect", "parameters": {"size": [80, 60], "center": [190, 60]},
     "inputs": {}, "outputs": ["geometry"]},
    {"id": "frame_stroke", "op": "shape.stroke", "parameters": {"color": [0.0, 1.0, 0.0, 1.0], "width": 4, "align": "inner"},
     "inputs": {"geometry": {"node": "frame", "port": "geometry"}}, "outputs": ["layer"]},
    {"id": "hidden", "op": "shape.rect", "parameters": {"size": [256, 256], "center": [128, 128]},
     "inputs": {}, "outputs": ["geometry"]},
    {"id": "hidden_fill", "op": "shape.fill", "parameters": {"color": [1.0, 0.0, 1.0, 1.0]},
     "inputs": {"geometry": {"node": "hidden", "port": "geometry"}}, "outputs": ["layer"]},
    {"id": "group", "op": "canvas.stack",
     "parameters": {"size": [256, 256], "layers": [{"input": "frame", "name": "frame"}, {"input": "hidden", "visible": false}]},
     "inputs": {"frame": {"node": "frame_stroke", "port": "layer"}, "hidden": {"node": "hidden_fill", "port": "layer"}},
     "outputs": ["layer"]},
    {"id": "root", "op": "canvas.stack",
     "parameters": {"size": [256, 256], "layers": [
       {"input": "card", "name": "card"},
       {"input": "dot", "name": "dot"},
       {"input": "ring", "name": "ring"},
       {"input": "group", "name": "group", "opacity": 0.5}]},
     "inputs": {"card": {"node": "card_fill", "port": "layer"}, "dot": {"node": "dot_fill", "port": "layer"},
                "ring": {"node": "ring_stroke", "port": "layer"}, "group": {"node": "group", "port": "layer"}},
     "outputs": ["layer"]},
    {"id": "out", "op": "canvas.output", "parameters": {"fit": "contain"},
     "inputs": {"layer": {"node": "root", "port": "layer"}}, "outputs": ["image"]}
  ],
  "outputs": {},
  "rendering": {
    "rendering_passes": [
      {
        "name": "main_render",
        "passes": [
          {
            "name": "present",
            "type": "fullscreen",
            "input": ["out"],
            "output": {"color": ["display"], "depth": null},
            "shader": {"vertex": "engine://fullscreen", "fragment": "engine://passthrough"}
          }
        ]
      }
    ]
  }
}
]=])
file(READ "${document}" authored_bytes HEX)

execute_process(
    COMMAND "${PLAYER}" --headless --project "${project_dir}" --size 256x256 --fps 60 --frames 61
            --render-out "${OUT_DIR}/frames/%04d.png"
    WORKING_DIRECTORY "${PLAYER_DIR}"
    RESULT_VARIABLE player_result
    OUTPUT_VARIABLE player_stdout
    ERROR_VARIABLE player_stderr
    TIMEOUT 600)
if(NOT player_result EQUAL 0)
    message(FATAL_ERROR "the player failed (${player_result})\n${player_stdout}\n${player_stderr}")
endif()
if(player_stdout MATCHES "Validation Error|VUID-" OR player_stderr MATCHES "Validation Error|VUID-")
    message(FATAL_ERROR "the player emitted Vulkan validation errors\n${player_stdout}\n${player_stderr}")
endif()

# The lowered document is a view the engine compiles; nothing writes it back.
file(READ "${document}" after_bytes HEX)
if(NOT after_bytes STREQUAL authored_bytes)
    message(FATAL_ERROR "the player changed the authored document")
endif()
if(EXISTS "${project_dir}/shaders/generated")
    message(FATAL_ERROR "the engine wrote shaders/generated into the project")
endif()
file(GLOB generated LIST_DIRECTORIES false "${project_dir}/.pelican/generated/canvas/*/*")
list(LENGTH generated generated_count)
if(NOT generated_count EQUAL 2)
    message(FATAL_ERROR "expected one generated vertex and fragment pair under .pelican/generated/canvas: ${generated}")
endif()

function(compare frame radius)
    execute_process(
        COMMAND "${COMPARE}" "${document}" out "${OUT_DIR}/frames/${frame}.png"
                --tolerance 2 --min-fraction 0.995 radius_curve.value=${radius}
        RESULT_VARIABLE compare_result
        OUTPUT_VARIABLE compare_stdout
        ERROR_VARIABLE compare_stderr)
    message(STATUS "frame ${frame} (corner_radius ${radius}): ${compare_stdout}")
    if(NOT compare_result EQUAL 0)
        message(FATAL_ERROR "frame ${frame} differs from the reference (${compare_result})\n${compare_stdout}${compare_stderr}")
    endif()
endfunction()
compare(0001 0)
compare(0061 60)

file(READ "${OUT_DIR}/frames/0001.png" first HEX)
file(READ "${OUT_DIR}/frames/0061.png" later HEX)
if(first STREQUAL later)
    message(FATAL_ERROR "frames 1 and 61 are identical; the corner_radius edge moved nothing")
endif()
