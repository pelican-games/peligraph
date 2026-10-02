# CV-1: a blank project gets a pelican.canvas document, `pelican_cli
# canvas-lower` lowers it, and the player draws the lowered copy headless.
# Two frames are held against the CPU reference of the authored document
# (renderCanvasReference): frame 1, where the card's corner_radius edge reads
# 0, and frame 61 (one second in), where it reads 60. The two must also differ,
# so the edge-driven parameter is seen to move the picture.
#
# Tolerance, measured on the development machine (NVIDIA, Debug, 256 x 256):
# both frames agree with the reference to within 2/255 in every channel on
# 99.96 % of the pixels (mean difference 0.45/255). The 23 or 24 pixels left
# sit at the inner corners of the inner stroke, where the rectangle's interior
# distance has a crease: the GPU's 2x2 quad derivatives and the reference's
# forward differences take the antialiasing width from different sides of it
# (up to 76/255 there). The bar is 99.5 %.

foreach(variable IN ITEMS CLI PLAYER PLAYER_DIR COMPARE OUT_DIR)
    if(NOT DEFINED ${variable})
        message(FATAL_ERROR "${variable} is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${OUT_DIR}")
file(MAKE_DIRECTORY "${OUT_DIR}/frames")
set(authored "${OUT_DIR}/authored")
set(lowered "${OUT_DIR}/lowered")

execute_process(
    COMMAND "${CLI}" project init "${authored}" --template blank
    RESULT_VARIABLE init_result
    OUTPUT_VARIABLE init_stdout
    ERROR_VARIABLE init_stderr)
if(NOT init_result EQUAL 0)
    message(FATAL_ERROR "project init failed (${init_result})\n${init_stdout}\n${init_stderr}")
endif()

# The project says it needs the canvas module; the lowered copy no longer does.
file(READ "${authored}/project.json" project)
string(JSON project SET "${project}" requires "[\"canvas\"]")
file(WRITE "${authored}/project.json" "${project}")

# The present pass reads the image the lowering names <output>$canvas_image.
file(WRITE "${authored}/passes/main_rendering_config.json" [=[
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
            "input": ["out$canvas_image"],
            "output": {"color": ["display"], "depth": null},
            "shader": {"vertex": "engine://fullscreen", "fragment": "engine://passthrough"}
          }
        ]
      }
    ]
  }
}
]=])

execute_process(
    COMMAND "${CLI}" canvas-lower --project "${authored}" --out "${lowered}"
    RESULT_VARIABLE lower_result
    OUTPUT_VARIABLE report
    ERROR_VARIABLE lower_stderr)
if(NOT lower_result EQUAL 0)
    message(FATAL_ERROR "canvas-lower failed (${lower_result})\n${report}\n${lower_stderr}")
endif()
string(JSON item_count GET "${report}" outputs 0 items)
string(JSON stem GET "${report}" outputs 0 generated 0)
if(NOT item_count EQUAL 4)
    message(FATAL_ERROR "expected 4 items (the hidden layer draws none)\n${report}")
endif()
foreach(stage IN ITEMS vert frag)
    if(NOT EXISTS "${lowered}/shaders/generated/${stem}.${stage}")
        message(FATAL_ERROR "canvas-lower did not write shaders/generated/${stem}.${stage}")
    endif()
endforeach()
file(READ "${lowered}/project.json" lowered_project)
string(JSON lowered_requires GET "${lowered_project}" requires)
if(lowered_requires MATCHES "canvas")
    message(FATAL_ERROR "the lowered project still requires canvas: ${lowered_requires}")
endif()

execute_process(
    COMMAND "${PLAYER}" --headless --project "${lowered}" --size 256x256 --fps 60 --frames 61
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

function(compare frame radius)
    execute_process(
        COMMAND "${COMPARE}" "${authored}/passes/main_rendering_config.json" out "${OUT_DIR}/frames/${frame}.png"
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
