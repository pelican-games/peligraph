# CV-3a: the neighbourhood filters drawn by the player, the engine lowering
# the document as it reads it (as canvas_text_headless_player). A 384 x 192
# stack over a dark colour holds three copies of a PSD layer (the card of
# fixtures/canvas_psd, drawn twice its size) -- a drop shadow (spread 0.2),
# a blur whose radius a curve drives (2, then 6 after half a second;
# max_radius 8) and an outer glow of size 40 (halved once) -- and three words
# in Arial: a drop shadow and a glow whose size a curve drives (2, then 5;
# max_radius 6), both drawn from the glyphs' distances in the text's own draw,
# and a blur (the glyphs composited first). Six small copies of the card
# under each colour filter (CV-3a reads their numbers through the item's
# parameters; the levels' gamma and the hue an edge drives) sit at the
# right. The canvas.output has a fixed `size` (CV-3a: lowered with
# extent_scale 1 beside it), the frame's size.
#
# Frames 1 and 61 are held against the CPU reference of the authored document
# at the curves' values then, and must differ (the driven radii move the
# picture). No font is committed: the test is skipped without Arial
# (C:/Windows/Fonts/arial.ttf). The frames are left in OUT_DIR/frames.
#
# Tolerance, measured on the development machine (NVIDIA, Debug, 384 x 192):
# both frames agree with the reference to within 2/255 in every channel on
# 99.997 % of the pixels (73726 of 73728; within 1/255 on 99.985 % and
# 99.990 %, mean 0.05/255), the rest at glyph edges, up to 5/255. The bar is
# 99.9 %.

foreach(variable IN ITEMS CLI PLAYER PLAYER_DIR COMPARE OUT_DIR FIXTURE)
    if(NOT DEFINED ${variable})
        message(FATAL_ERROR "${variable} is required")
    endif()
endforeach()

set(latin_font "C:/Windows/Fonts/arial.ttf")
if(NOT EXISTS "${latin_font}")
    message(STATUS "PELICAN_CANVAS_TEXT_SKIP_NO_FONT: ${latin_font} is not on this machine")
    return()
endif()

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
file(MAKE_DIRECTORY "${project_dir}/fonts" "${project_dir}/imports")
file(COPY_FILE "${latin_font}" "${project_dir}/fonts/latin.ttf")
set(card "${FIXTURE}/layers/0002_Card.png")
file(COPY_FILE "${card}" "${project_dir}/imports/card.png")

set(document "${project_dir}/passes/main_rendering_config.json")
file(WRITE "${document}" [=[
{
  "schema": "pelican.graph",
  "version": 1,
  "definitions": {},
  "nodes": [
    {"id": "clock", "op": "value.input", "parameters": {"name": "time"}, "inputs": {}, "outputs": ["value"]},
    {"id": "radius_curve", "op": "value.curve",
     "parameters": {"keys": [{"time": 0, "value": 2}, {"time": 0.5, "value": 2}, {"time": 0.51, "value": 6}, {"time": 100, "value": 6}]},
     "inputs": {"time": {"node": "clock", "port": "value"}}, "outputs": ["value"]},
    {"id": "glow_curve", "op": "value.curve",
     "parameters": {"keys": [{"time": 0, "value": 2}, {"time": 0.5, "value": 2}, {"time": 0.51, "value": 5}, {"time": 100, "value": 5}]},
     "inputs": {"time": {"node": "clock", "port": "value"}}, "outputs": ["value"]},
    {"id": "back", "op": "canvas.color", "parameters": {"color": [0.12, 0.13, 0.18, 1.0]}, "inputs": {}, "outputs": ["layer"]},
    {"id": "card_a", "op": "canvas.image",
     "parameters": {"asset": "project://imports/card.png", "offset": [16, 16], "size": [80, 32]}, "inputs": {}, "outputs": ["layer"]},
    {"id": "card_a_shadow", "op": "filter.drop_shadow",
     "parameters": {"offset": [6, 6], "radius": 10, "spread": 0.2, "color": [0.0, 0.0, 0.0], "opacity": 0.75},
     "inputs": {"layer": {"node": "card_a", "port": "layer"}}, "outputs": ["layer"]},
    {"id": "card_b", "op": "canvas.image",
     "parameters": {"asset": "project://imports/card.png", "offset": [16, 76], "size": [80, 32]}, "inputs": {}, "outputs": ["layer"]},
    {"id": "card_b_blur", "op": "filter.blur", "parameters": {"radius": 2, "max_radius": 8},
     "inputs": {"layer": {"node": "card_b", "port": "layer"}, "radius": {"node": "radius_curve", "port": "value"}},
     "outputs": ["layer"]},
    {"id": "card_c", "op": "canvas.image",
     "parameters": {"asset": "project://imports/card.png", "offset": [16, 136], "size": [80, 32]}, "inputs": {}, "outputs": ["layer"]},
    {"id": "card_c_glow", "op": "filter.outer_glow",
     "parameters": {"radius": 40, "color": [1.0, 0.85, 0.3], "opacity": 0.9},
     "inputs": {"layer": {"node": "card_c", "port": "layer"}}, "outputs": ["layer"]},
    {"id": "word_a", "op": "shape.text",
     "parameters": {"font": "project://fonts/latin.ttf", "text": "Ab", "size": 40, "origin": [150, 60]},
     "inputs": {}, "outputs": ["geometry"]},
    {"id": "word_a_fill", "op": "shape.fill", "parameters": {"color": [1.0, 1.0, 1.0, 1.0]},
     "inputs": {"geometry": {"node": "word_a", "port": "geometry"}}, "outputs": ["layer"]},
    {"id": "word_a_shadow", "op": "filter.drop_shadow",
     "parameters": {"offset": [3, 3], "radius": 4, "color": [0.0, 0.0, 0.0], "opacity": 0.8},
     "inputs": {"layer": {"node": "word_a_fill", "port": "layer"}}, "outputs": ["layer"]},
    {"id": "word_b", "op": "shape.text",
     "parameters": {"font": "project://fonts/latin.ttf", "text": "Cd", "size": 40, "origin": [270, 60]},
     "inputs": {}, "outputs": ["geometry"]},
    {"id": "word_b_fill", "op": "shape.fill", "parameters": {"color": [1.0, 0.95, 0.8, 1.0]},
     "inputs": {"geometry": {"node": "word_b", "port": "geometry"}}, "outputs": ["layer"]},
    {"id": "word_b_glow", "op": "filter.outer_glow",
     "parameters": {"radius": 2, "max_radius": 6, "color": [0.3, 0.8, 1.0], "opacity": 1.0},
     "inputs": {"layer": {"node": "word_b_fill", "port": "layer"}, "radius": {"node": "glow_curve", "port": "value"}},
     "outputs": ["layer"]},
    {"id": "word_c", "op": "shape.text",
     "parameters": {"font": "project://fonts/latin.ttf", "text": "Ef", "size": 40, "origin": [150, 150]},
     "inputs": {}, "outputs": ["geometry"]},
    {"id": "word_c_fill", "op": "shape.fill", "parameters": {"color": [1.0, 1.0, 1.0, 1.0]},
     "inputs": {"geometry": {"node": "word_c", "port": "geometry"}}, "outputs": ["layer"]},
    {"id": "word_c_blur", "op": "filter.blur", "parameters": {"radius": 1.5},
     "inputs": {"layer": {"node": "word_c_fill", "port": "layer"}}, "outputs": ["layer"]},
    {"id": "tile_1", "op": "canvas.image",
     "parameters": {"asset": "project://imports/card.png", "offset": [280, 100], "size": [40, 16]}, "inputs": {}, "outputs": ["layer"]},
    {"id": "tile_1_invert", "op": "filter.invert",
     "inputs": {"layer": {"node": "tile_1", "port": "layer"}}, "outputs": ["layer"]},
    {"id": "tile_2", "op": "canvas.image",
     "parameters": {"asset": "project://imports/card.png", "offset": [330, 100], "size": [40, 16]}, "inputs": {}, "outputs": ["layer"]},
    {"id": "tile_2_contrast", "op": "filter.brightness_contrast", "parameters": {"brightness": 30, "contrast": 20},
     "inputs": {"layer": {"node": "tile_2", "port": "layer"}}, "outputs": ["layer"]},
    {"id": "tile_3", "op": "canvas.image",
     "parameters": {"asset": "project://imports/card.png", "offset": [280, 126], "size": [40, 16]}, "inputs": {}, "outputs": ["layer"]},
    {"id": "tile_3_levels", "op": "filter.levels",
     "parameters": {"input_floor": 20, "input_ceiling": 230, "gamma": 1.0, "channels": {"blue": {"output_floor": 60}}},
     "inputs": {"layer": {"node": "tile_3", "port": "layer"}, "gamma": {"node": "radius_curve", "port": "value"}},
     "outputs": ["layer"]},
    {"id": "tile_4", "op": "canvas.image",
     "parameters": {"asset": "project://imports/card.png", "offset": [330, 126], "size": [40, 16]}, "inputs": {}, "outputs": ["layer"]},
    {"id": "tile_4_hue", "op": "filter.hue_saturation", "parameters": {"hue": 0, "saturation": -30, "lightness": 10},
     "inputs": {"layer": {"node": "tile_4", "port": "layer"}, "hue": {"node": "glow_curve", "port": "value"}},
     "outputs": ["layer"]},
    {"id": "tile_5", "op": "canvas.image",
     "parameters": {"asset": "project://imports/card.png", "offset": [280, 152], "size": [40, 16]}, "inputs": {}, "outputs": ["layer"]},
    {"id": "tile_5_map", "op": "filter.gradient_map",
     "parameters": {"stops": [{"position": 0.0, "color": [0.0, 0.2, 0.6]}, {"position": 1.0, "color": [1.0, 0.9, 0.3]}]},
     "inputs": {"layer": {"node": "tile_5", "port": "layer"}}, "outputs": ["layer"]},
    {"id": "tile_6", "op": "canvas.image",
     "parameters": {"asset": "project://imports/card.png", "offset": [330, 152], "size": [40, 16]}, "inputs": {}, "outputs": ["layer"]},
    {"id": "tile_6_overlay", "op": "filter.color_overlay", "parameters": {"color": [0.2, 0.8, 0.4, 0.5]},
     "inputs": {"layer": {"node": "tile_6", "port": "layer"}}, "outputs": ["layer"]},
    {"id": "root", "op": "canvas.stack",
     "parameters": {"size": [384, 192], "layers": [
       {"input": "back"}, {"input": "card_a_shadow"}, {"input": "card_b_blur"}, {"input": "card_c_glow"},
       {"input": "word_a_shadow"}, {"input": "word_b_glow"}, {"input": "word_c_blur"},
       {"input": "tile_1_invert"}, {"input": "tile_2_contrast"}, {"input": "tile_3_levels"},
       {"input": "tile_4_hue"}, {"input": "tile_5_map"}, {"input": "tile_6_overlay"}]},
     "inputs": {"back": {"node": "back", "port": "layer"},
                "card_a_shadow": {"node": "card_a_shadow", "port": "layer"},
                "card_b_blur": {"node": "card_b_blur", "port": "layer"},
                "card_c_glow": {"node": "card_c_glow", "port": "layer"},
                "word_a_shadow": {"node": "word_a_shadow", "port": "layer"},
                "word_b_glow": {"node": "word_b_glow", "port": "layer"},
                "word_c_blur": {"node": "word_c_blur", "port": "layer"},
                "tile_1_invert": {"node": "tile_1_invert", "port": "layer"},
                "tile_2_contrast": {"node": "tile_2_contrast", "port": "layer"},
                "tile_3_levels": {"node": "tile_3_levels", "port": "layer"},
                "tile_4_hue": {"node": "tile_4_hue", "port": "layer"},
                "tile_5_map": {"node": "tile_5_map", "port": "layer"},
                "tile_6_overlay": {"node": "tile_6_overlay", "port": "layer"}},
     "outputs": ["layer"]},
    {"id": "out", "op": "canvas.output", "parameters": {"size": [384, 192], "fit": "stretch"},
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

execute_process(
    COMMAND "${PLAYER}" --headless --project "${project_dir}" --size 384x192 --fps 60 --frames 61
            --render-out "${OUT_DIR}/frames/%04d.png"
    WORKING_DIRECTORY "${PLAYER_DIR}"
    RESULT_VARIABLE player_result
    OUTPUT_VARIABLE player_stdout
    ERROR_VARIABLE player_stderr
    TIMEOUT 900)
if(NOT player_result EQUAL 0)
    message(FATAL_ERROR "the player failed (${player_result})\n${player_stdout}\n${player_stderr}")
endif()
if(player_stdout MATCHES "Validation Error|VUID-" OR player_stderr MATCHES "Validation Error|VUID-")
    message(FATAL_ERROR "the player emitted Vulkan validation errors\n${player_stdout}\n${player_stderr}")
endif()

if(NOT DEFINED MIN_FRACTION)
    set(MIN_FRACTION 0.999)
endif()
function(compare frame radius glow)
    execute_process(
        COMMAND "${COMPARE}" "${document}" out "${OUT_DIR}/frames/${frame}.png" --project "${project_dir}"
                --image "project://imports/card.png=${card}"
                --tolerance 2 --min-fraction ${MIN_FRACTION} radius_curve.value=${radius} glow_curve.value=${glow}
        RESULT_VARIABLE compare_result
        OUTPUT_VARIABLE compare_stdout
        ERROR_VARIABLE compare_stderr)
    message(STATUS "frame ${frame} (blur radius ${radius}, glow size ${glow}) against the CPU reference: ${compare_stdout}")
    if(NOT compare_result EQUAL 0)
        message(FATAL_ERROR "frame ${frame} differs from the reference (${compare_result})\n${compare_stdout}${compare_stderr}")
    endif()
endfunction()
compare(0001 2 2)
compare(0061 6 5)

file(READ "${OUT_DIR}/frames/0001.png" first HEX)
file(READ "${OUT_DIR}/frames/0061.png" later HEX)
if(first STREQUAL later)
    message(FATAL_ERROR "frames 1 and 61 are identical; the driven radii moved nothing")
endif()
