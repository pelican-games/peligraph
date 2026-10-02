# CV-5: `pelican_cli canvas-lower --policy ship` without a player, no GPU. A
# blank project draws an image of the PSD fixture, a disc whose centre a
# value.curve moves and, when the system's Arial is there, a line of
# shape.text. Without --player the static runs stay (the report says
# canvas.flatten_skipped {reason: no_player}); the constants are folded into
# the shader; the lowered document holds no canvas, shape or filter op; the
# project requires no canvas; and the font, baked into the atlas, is not
# copied. The policy's arguments are refused by name when they are wrong.

foreach(variable IN ITEMS CLI OUT_DIR FIXTURE)
    if(NOT DEFINED ${variable})
        message(FATAL_ERROR "${variable} is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${OUT_DIR}")
file(MAKE_DIRECTORY "${OUT_DIR}")
set(authored "${OUT_DIR}/authored")
execute_process(
    COMMAND "${CLI}" project init "${authored}" --template blank
    RESULT_VARIABLE init_result
    OUTPUT_VARIABLE init_stdout
    ERROR_VARIABLE init_stderr)
if(NOT init_result EQUAL 0)
    message(FATAL_ERROR "project init failed (${init_result})\n${init_stdout}\n${init_stderr}")
endif()
file(READ "${authored}/project.json" project)
string(JSON project SET "${project}" requires "[\"canvas\"]")
file(WRITE "${authored}/project.json" "${project}")
file(COPY "${FIXTURE}/layers/0000_Background.png" DESTINATION "${authored}/imports")

set(font "C:/Windows/Fonts/arial.ttf")
set(text_nodes "")
set(text_layer "")
set(text_input "")
if(EXISTS "${font}")
    file(MAKE_DIRECTORY "${authored}/fonts")
    file(COPY_FILE "${font}" "${authored}/fonts/latin.ttf")
    set(text_nodes [=[
    {"id": "title", "op": "shape.text",
     "parameters": {"font": "project://fonts/latin.ttf", "text": "AVATAR", "size": 12, "origin": [2, 20]},
     "inputs": {}, "outputs": ["geometry"]},
    {"id": "title_fill", "op": "shape.fill", "parameters": {"color": [1.0, 1.0, 1.0, 1.0]},
     "inputs": {"geometry": {"node": "title", "port": "geometry"}}, "outputs": ["layer"]},]=])
    set(text_layer [=[, {"input": "title", "name": "Title"}]=])
    set(text_input [=[, "title": {"node": "title_fill", "port": "layer"}]=])
else()
    message(STATUS "no ${font}: the font is left out")
endif()

file(WRITE "${authored}/passes/main_rendering_config.json" "{
  \"schema\": \"pelican.graph\",
  \"version\": 1,
  \"definitions\": {},
  \"nodes\": [
    {\"id\": \"clock\", \"op\": \"value.input\", \"parameters\": {\"name\": \"time\"}, \"inputs\": {}, \"outputs\": [\"value\"]},
    {\"id\": \"disc_x\", \"op\": \"value.curve\",
     \"parameters\": {\"keys\": [{\"time\": 0, \"value\": 6}, {\"time\": 1, \"value\": 58}]},
     \"inputs\": {\"time\": {\"node\": \"clock\", \"port\": \"value\"}}, \"outputs\": [\"value\"]},
    {\"id\": \"back\", \"op\": \"canvas.image\",
     \"parameters\": {\"asset\": \"project://imports/0000_Background.png\", \"offset\": [0, 0], \"size\": [64, 32]},
     \"inputs\": {}, \"outputs\": [\"layer\"]},
    {\"id\": \"frame\", \"op\": \"shape.rect\", \"parameters\": {\"size\": [40, 20], \"center\": [32, 16]},
     \"inputs\": {}, \"outputs\": [\"geometry\"]},
    {\"id\": \"frame_fill\", \"op\": \"shape.fill\", \"parameters\": {\"color\": [0.2, 0.3, 0.9, 0.5]},
     \"inputs\": {\"geometry\": {\"node\": \"frame\", \"port\": \"geometry\"}}, \"outputs\": [\"layer\"]},
    {\"id\": \"disc\", \"op\": \"shape.ellipse\", \"parameters\": {\"size\": [10, 10], \"center\": [6, 16]},
     \"inputs\": {\"center.x\": {\"node\": \"disc_x\", \"port\": \"value\"}}, \"outputs\": [\"geometry\"]},
    {\"id\": \"disc_fill\", \"op\": \"shape.fill\", \"parameters\": {\"color\": [1.0, 0.4, 0.1, 1.0]},
     \"inputs\": {\"geometry\": {\"node\": \"disc\", \"port\": \"geometry\"}}, \"outputs\": [\"layer\"]},
${text_nodes}
    {\"id\": \"root\", \"op\": \"canvas.stack\",
     \"parameters\": {\"size\": [64, 32], \"layers\": [{\"input\": \"back\", \"name\": \"Back\"},
       {\"input\": \"frame\", \"name\": \"Frame\"}, {\"input\": \"disc\", \"name\": \"Disc\"}${text_layer}]},
     \"inputs\": {\"back\": {\"node\": \"back\", \"port\": \"layer\"}, \"frame\": {\"node\": \"frame_fill\", \"port\": \"layer\"},
                \"disc\": {\"node\": \"disc_fill\", \"port\": \"layer\"}${text_input}},
     \"outputs\": [\"layer\"]},
    {\"id\": \"out\", \"op\": \"canvas.output\", \"parameters\": {\"fit\": \"stretch\"},
     \"inputs\": {\"layer\": {\"node\": \"root\", \"port\": \"layer\"}}, \"outputs\": [\"image\"]}
  ],
  \"outputs\": {}
}
")

function(lower expected_result policy out)
    execute_process(
        COMMAND "${CLI}" canvas-lower --project "${authored}" --out "${out}" ${ARGN}
        RESULT_VARIABLE result
        OUTPUT_VARIABLE report
        ERROR_VARIABLE stderr)
    if(NOT result EQUAL expected_result)
        message(FATAL_ERROR "canvas-lower ${ARGN} exited ${result}, not ${expected_result}\n${report}\n${stderr}")
    endif()
    set(report "${report}" PARENT_SCOPE)
    set(stderr "${stderr}" PARENT_SCOPE)
endfunction()

# Refused: a policy that is not one, a player for the edit policy, a player that is not there.
lower(1 bogus "${OUT_DIR}/refused" --policy bogus)
if(NOT stderr MATCHES "--policy must be edit or ship")
    message(FATAL_ERROR "an unknown policy was not refused by name: ${stderr}")
endif()
lower(1 edit "${OUT_DIR}/refused" --player "${CLI}")
if(NOT stderr MATCHES "--player draws the static runs of --policy ship")
    message(FATAL_ERROR "a player for the edit policy was not refused: ${stderr}")
endif()
lower(1 ship "${OUT_DIR}/refused" --policy ship --player "${OUT_DIR}/no_player.exe")
if(NOT stderr MATCHES "--player names no file")
    message(FATAL_ERROR "a missing player was not refused: ${stderr}")
endif()

lower(0 edit "${OUT_DIR}/edit")
string(JSON policy GET "${report}" policy)
if(NOT policy STREQUAL "edit")
    message(FATAL_ERROR "the default policy is not edit:\n${report}")
endif()

set(shipped "${OUT_DIR}/ship")
lower(0 ship "${shipped}" --policy ship)
message(STATUS "canvas-lower --policy ship: ${report}")
string(JSON policy GET "${report}" policy)
string(JSON flattened LENGTH "${report}" flattened)
string(JSON folded GET "${report}" folded_constants)
string(JSON removed GET "${report}" removed_assets)
string(JSON diagnostic GET "${report}" diagnostics 0)
if(NOT policy STREQUAL "ship" OR NOT flattened EQUAL 0 OR NOT folded GREATER 0)
    message(FATAL_ERROR "the ship report is not as expected:\n${report}")
endif()
string(JSON code GET "${diagnostic}" code)
string(JSON reason GET "${diagnostic}" reason)
string(JSON runs GET "${diagnostic}" runs)
if(NOT code STREQUAL "canvas.flatten_skipped" OR NOT reason STREQUAL "no_player" OR NOT runs EQUAL 1)
    message(FATAL_ERROR "without a player the runs are not reported as skipped:\n${report}")
endif()

file(READ "${shipped}/passes/main_rendering_config.json" lowered)
if(lowered MATCHES "\"op\": \"(canvas|shape|filter)\\.")
    message(FATAL_ERROR "the shipped document still holds a canvas op: ${CMAKE_MATCH_0}")
endif()
file(READ "${shipped}/project.json" shipped_project)
string(JSON shipped_requires GET "${shipped_project}" requires)
if(shipped_requires MATCHES "canvas")
    message(FATAL_ERROR "the shipped project still requires canvas: ${shipped_requires}")
endif()
# The image is still read (nothing was folded without a player).
if(NOT EXISTS "${shipped}/imports/0000_Background.png" OR removed MATCHES "0000_Background")
    message(FATAL_ERROR "the image the shipped document reads was removed: ${removed}")
endif()
if(EXISTS "${font}")
    if(EXISTS "${shipped}/fonts/latin.ttf" OR NOT removed MATCHES "project://fonts/latin.ttf")
        message(FATAL_ERROR "the font baked into the atlas was shipped: ${removed}")
    endif()
    if(NOT EXISTS "${OUT_DIR}/edit/fonts/latin.ttf")
        message(FATAL_ERROR "the edit policy dropped the font")
    endif()
endif()
