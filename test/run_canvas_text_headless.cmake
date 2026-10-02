# CV-4: shape.text drawn by the player, the engine lowering the document as it
# reads it (as canvas_live_headless_player): the font is a file of the
# project, the lowering bakes the glyphs the document uses into an MTSDF atlas
# under .pelican/generated/canvas, and the glyph items read it. The frame is
# held against the CPU reference, which bakes the same atlas from the same
# font and reads it the way the shader does.
#
# No font is committed: the Latin lines need the system's Arial
# (C:/Windows/Fonts/arial.ttf) and the test is skipped without it; a Japanese
# line is added when Yu Gothic (YuGothR.ttc, face 0) is there too. The canvas
# is drawn at twice its size, so the 32-texel cells are magnified (about 5
# pixels per texel at size 72) and the small line is near one pixel per texel.
#
# Tolerance, measured on the development machine (NVIDIA, Debug, 1024 x 512,
# Arial and Yu Gothic): 99.987 % of the pixels are within 8/255 of the CPU
# reference (mean 0.10/255); the rest are on glyph edges, up to 16/255
# (presumably the GPU's fixed-point filter weights against the CPU's exact
# bilinear read of the atlas). The bar is 8/255 on 99.5 %. The frame and
# the atlases are left in OUT_DIR/frames.

foreach(variable IN ITEMS CLI PLAYER PLAYER_DIR COMPARE OUT_DIR)
    if(NOT DEFINED ${variable})
        message(FATAL_ERROR "${variable} is required")
    endif()
endforeach()

set(latin_font "C:/Windows/Fonts/arial.ttf")
set(japanese_font "C:/Windows/Fonts/YuGothR.ttc")
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
file(MAKE_DIRECTORY "${project_dir}/fonts")
file(COPY_FILE "${latin_font}" "${project_dir}/fonts/latin.ttf")

# Latin: a large line with kerning pairs (AV, AT, Te, To) filled and stroked
# outside, a small line filled. Japanese, when there is a font for it: one
# line, stroked outside and filled, centred.
set(text_nodes [=[
    {"id": "back", "op": "canvas.color", "parameters": {"color": [0.08, 0.09, 0.14, 1.0]}, "inputs": {}, "outputs": ["layer"]},
    {"id": "title", "op": "shape.text",
     "parameters": {"font": "project://fonts/latin.ttf", "text": "AVATAR Te To!", "size": 72, "origin": [16, 76]},
     "inputs": {}, "outputs": ["geometry"]},
    {"id": "title_stroke", "op": "shape.stroke", "parameters": {"color": [1.0, 0.55, 0.1, 1.0], "width": 3, "align": "outer"},
     "inputs": {"geometry": {"node": "title", "port": "geometry"}}, "outputs": ["layer"]},
    {"id": "title_fill", "op": "shape.fill", "parameters": {"color": [1.0, 1.0, 1.0, 1.0]},
     "inputs": {"geometry": {"node": "title", "port": "geometry"}}, "outputs": ["layer"]},
    {"id": "small", "op": "shape.text",
     "parameters": {"font": "project://fonts/latin.ttf", "text": "The quick brown fox jumps over the lazy dog 0123",
                    "size": 20, "origin": [16, 112]},
     "inputs": {}, "outputs": ["geometry"]},
    {"id": "small_fill", "op": "shape.fill", "parameters": {"color": [0.7, 0.9, 1.0, 1.0]},
     "inputs": {"geometry": {"node": "small", "port": "geometry"}}, "outputs": ["layer"]}]=])
set(layers [=[{"input": "back"}, {"input": "title_stroke"}, {"input": "title_fill"}, {"input": "small_fill"}]=])
set(inputs [=["back": {"node": "back", "port": "layer"}, "title_stroke": {"node": "title_stroke", "port": "layer"},
               "title_fill": {"node": "title_fill", "port": "layer"}, "small_fill": {"node": "small_fill", "port": "layer"}]=])
if(EXISTS "${japanese_font}")
    file(COPY_FILE "${japanese_font}" "${project_dir}/fonts/japanese.ttc")
    string(APPEND text_nodes [=[,
    {"id": "kanji", "op": "shape.text",
     "parameters": {"font": "project://fonts/japanese.ttc", "face": 0, "text": "永遠の文字、鬱。", "size": 56,
                    "origin": [256, 200], "align": "center"},
     "inputs": {}, "outputs": ["geometry"]},
    {"id": "kanji_stroke", "op": "shape.stroke", "parameters": {"color": [0.2, 0.5, 1.0, 1.0], "width": 1.5, "align": "outer"},
     "inputs": {"geometry": {"node": "kanji", "port": "geometry"}}, "outputs": ["layer"]},
    {"id": "kanji_fill", "op": "shape.fill", "parameters": {"color": [1.0, 0.95, 0.8, 1.0]},
     "inputs": {"geometry": {"node": "kanji", "port": "geometry"}}, "outputs": ["layer"]}]=])
    string(APPEND layers [=[, {"input": "kanji_stroke"}, {"input": "kanji_fill"}]=])
    string(APPEND inputs [=[, "kanji_fill": {"node": "kanji_fill", "port": "layer"},
               "kanji_stroke": {"node": "kanji_stroke", "port": "layer"}]=])
    set(expected_atlases 2)
else()
    message(STATUS "no ${japanese_font}: the Japanese line is left out")
    set(expected_atlases 1)
endif()

set(document "${project_dir}/passes/main_rendering_config.json")
file(WRITE "${document}" "{
  \"schema\": \"pelican.graph\",
  \"version\": 1,
  \"definitions\": {},
  \"nodes\": [
${text_nodes},
    {\"id\": \"root\", \"op\": \"canvas.stack\",
     \"parameters\": {\"size\": [512, 256], \"layers\": [${layers}]},
     \"inputs\": {${inputs}},
     \"outputs\": [\"layer\"]},
    {\"id\": \"out\", \"op\": \"canvas.output\", \"parameters\": {\"fit\": \"contain\"},
     \"inputs\": {\"layer\": {\"node\": \"root\", \"port\": \"layer\"}}, \"outputs\": [\"image\"]}
  ],
  \"outputs\": {},
  \"rendering\": {
    \"rendering_passes\": [
      {
        \"name\": \"main_render\",
        \"passes\": [
          {
            \"name\": \"present\",
            \"type\": \"fullscreen\",
            \"input\": [\"out\"],
            \"output\": {\"color\": [\"display\"], \"depth\": null},
            \"shader\": {\"vertex\": \"engine://fullscreen\", \"fragment\": \"engine://passthrough\"}
          }
        ]
      }
    ]
  }
}
")

execute_process(
    COMMAND "${PLAYER}" --headless --project "${project_dir}" --size 1024x512 --fps 60 --frames 1
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

# One atlas per font, each in a directory of its own beside the shader pair.
file(GLOB atlases LIST_DIRECTORIES false "${project_dir}/.pelican/generated/canvas/*/*.png")
list(LENGTH atlases atlas_count)
if(NOT atlas_count EQUAL expected_atlases)
    message(FATAL_ERROR "expected ${expected_atlases} glyph atlases under .pelican/generated/canvas: ${atlases}")
endif()
foreach(atlas IN LISTS atlases)
    get_filename_component(name "${atlas}" NAME)
    file(COPY_FILE "${atlas}" "${OUT_DIR}/frames/${name}")
endforeach()

execute_process(
    COMMAND "${COMPARE}" "${document}" out "${OUT_DIR}/frames/0001.png" --project "${project_dir}"
            --tolerance 8 --min-fraction 0.995
    RESULT_VARIABLE compare_result
    OUTPUT_VARIABLE compare_stdout
    ERROR_VARIABLE compare_stderr)
message(STATUS "frame against the CPU reference: ${compare_stdout}")
if(NOT compare_result EQUAL 0)
    message(FATAL_ERROR "the frame differs from the reference (${compare_result})\n${compare_stdout}${compare_stderr}")
endif()
