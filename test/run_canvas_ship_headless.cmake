# CV-5: the ship policy against the edit policy, drawn by the player. The PSD
# fixture (fixtures/canvas_psd) gets a disc in the middle of its stack whose
# centre a value.curve moves: Background and Panel below it, Isolated and Dot
# above. `canvas-lower --policy ship --player` folds the two static runs into
# one image each (drawn by the same player from each run alone), and the
# shipped project is drawn at two times against the edit-lowered one.
#
# Also held: the report names the two runs, fewer instances are drawn, the
# shipped document holds no canvas / shape / filter op and requires no
# canvas, the PSD's layer files are not shipped, and a second ship lowering of
# the same project writes the same bytes (document and images).
#
# The whole script runs under pelican_gpu_lease_run; canvas-lower's player
# runs inside that lease (it takes none of its own).
#
# Tolerance, measured on the development machine (NVIDIA, Debug, 64 x 32):
# frames 1 and 31 are within 1/255 of the edit frames on every pixel (mean
# 0.08/255 and 0.09/255); the bar is 2/255 on every pixel (the folded runs
# pass through one 8-bit PNG). Instances drawn: edit 10, ship 4.

foreach(variable IN ITEMS CLI PLAYER PLAYER_DIR COMPARE OUT_DIR FIXTURE)
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
file(COPY "${FIXTURE}/layers" "${FIXTURE}/masks" DESTINATION "${authored}/imports/psd_fixture")

# The fixture's definition with a moving disc between Panel and Isolated.
file(READ "${FIXTURE}/canvas.graph.json" graph)
string(JSON definition MEMBER "${graph}" definitions 0)
string(JSON count LENGTH "${graph}" definitions ${definition} nodes)
math(EXPR last "${count} - 1")
set(root_index "")
foreach(index RANGE ${last})
    string(JSON id GET "${graph}" definitions ${definition} nodes ${index} id)
    if(id STREQUAL "root")
        set(root_index ${index})
    endif()
endforeach()
if(root_index STREQUAL "")
    message(FATAL_ERROR "the fixture has no root stack")
endif()
string(JSON graph SET "${graph}" definitions ${definition} nodes ${root_index} parameters layers [=[[
  {"input": "l0000_background", "mask": "l0000_background_mask", "name": "Background"},
  {"input": "l0001_panel", "name": "Panel", "opacity": 0.901961, "pass_through": true},
  {"input": "circle", "name": "Circle"},
  {"input": "l0004_isolated", "name": "Isolated", "opacity": 0.705882},
  {"input": "l0007_dot", "mask": "l0007_dot_mask", "name": "Dot"}
]]=])
string(JSON graph SET "${graph}" definitions ${definition} nodes ${root_index} inputs circle
    [=[{"node": "circle_fill", "port": "layer"}]=])
set(added [=[
  {"id": "clock", "op": "value.input", "parameters": {"name": "time"}, "inputs": {}, "outputs": ["value"]}
]=] [=[
  {"id": "circle_x", "op": "value.curve",
   "parameters": {"keys": [{"time": 0, "value": 6}, {"time": 1, "value": 58}]},
   "inputs": {"time": {"node": "clock", "port": "value"}}, "outputs": ["value"]}
]=] [=[
  {"id": "circle", "op": "shape.ellipse", "parameters": {"size": [14, 14], "center": [6, 16]},
   "inputs": {"center.x": {"node": "circle_x", "port": "value"}}, "outputs": ["geometry"]}
]=] [=[
  {"id": "circle_fill", "op": "shape.fill", "parameters": {"color": [1.0, 0.35, 0.1, 0.85]},
   "inputs": {"geometry": {"node": "circle", "port": "geometry"}}, "outputs": ["layer"]}
]=])
foreach(node IN LISTS added)
    string(JSON count LENGTH "${graph}" definitions ${definition} nodes)
    string(JSON graph SET "${graph}" definitions ${definition} nodes ${count} "${node}")
endforeach()
string(JSON definitions GET "${graph}" definitions)
file(WRITE "${authored}/passes/main_rendering_config.json" "{
  \"schema\": \"pelican.graph\",
  \"version\": 1,
  \"definitions\": ${definitions},
  \"nodes\": [
    {\"id\": \"psd\", \"op\": \"graph.instance\", \"definition\": \"${definition}\"},
    {\"id\": \"out\", \"op\": \"canvas.output\", \"parameters\": {\"fit\": \"stretch\"},
     \"inputs\": {\"layer\": {\"node\": \"psd\", \"port\": \"layer\"}}, \"outputs\": [\"image\"]}
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
            \"input\": [\"out$canvas_image\"],
            \"output\": {\"color\": [\"display\"], \"depth\": null},
            \"shader\": {\"vertex\": \"engine://fullscreen\", \"fragment\": \"engine://passthrough\"}
          }
        ]
      }
    ]
  }
}
")

function(lower out)
    execute_process(
        COMMAND "${CLI}" canvas-lower --project "${authored}" --out "${out}" ${ARGN}
        WORKING_DIRECTORY "${PLAYER_DIR}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE report
        ERROR_VARIABLE stderr
        TIMEOUT 900)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "canvas-lower ${ARGN} failed (${result})\n${report}\n${stderr}")
    endif()
    set(report "${report}" PARENT_SCOPE)
endfunction()

function(draw lowered frames)
    file(MAKE_DIRECTORY "${frames}")
    execute_process(
        COMMAND "${PLAYER}" --headless --project "${lowered}" --size 64x32 --fps 60 --frames 31
                --render-out "${frames}/%04d.png"
        WORKING_DIRECTORY "${PLAYER_DIR}"
        RESULT_VARIABLE player_result
        OUTPUT_VARIABLE player_stdout
        ERROR_VARIABLE player_stderr
        TIMEOUT 600)
    if(NOT player_result EQUAL 0)
        message(FATAL_ERROR "the player failed on ${lowered} (${player_result})\n${player_stdout}\n${player_stderr}")
    endif()
    if(player_stdout MATCHES "Validation Error|VUID-" OR player_stderr MATCHES "Validation Error|VUID-")
        message(FATAL_ERROR "the player emitted Vulkan validation errors\n${player_stdout}\n${player_stderr}")
    endif()
endfunction()

lower("${OUT_DIR}/edit")
set(edit_report "${report}")
lower("${OUT_DIR}/ship" --policy ship --player "${PLAYER}")
set(ship_report "${report}")
message(STATUS "canvas-lower --policy ship: ${ship_report}")

# Two runs: the two layers below the disc, the two above it.
string(JSON flattened LENGTH "${ship_report}" flattened)
if(NOT flattened EQUAL 2)
    message(FATAL_ERROR "expected two folded runs\n${ship_report}")
endif()
foreach(run IN ITEMS 0 1)
    string(JSON layers GET "${ship_report}" flattened ${run} layers)
    string(JSON reason GET "${ship_report}" flattened ${run} reason)
    string(JSON image GET "${ship_report}" flattened ${run} image)
    string(JSON output GET "${ship_report}" flattened ${run} output)
    string(REGEX REPLACE "[ \n]" "" layers "${layers}")
    list(APPEND seen "${output}|${layers}|${reason}")
    if(NOT EXISTS "${OUT_DIR}/ship/${image}")
        message(FATAL_ERROR "the folded run's image ${image} was not written")
    endif()
endforeach()
set(expected_runs "out|[\"Background\",\"Panel\"]|static_from_bottom" "out|[\"Isolated\",\"Dot\"]|static_above_cut")
if(NOT seen STREQUAL expected_runs)
    message(FATAL_ERROR "the folded runs are ${seen}, not ${expected_runs}")
endif()
string(JSON edit_instances GET "${edit_report}" outputs 0 instances)
string(JSON ship_instances GET "${ship_report}" outputs 0 instances)
string(JSON folded GET "${ship_report}" folded_constants)
message(STATUS "instances drawn: edit ${edit_instances}, ship ${ship_instances}; constants folded: ${folded}")
if(NOT ship_instances LESS edit_instances)
    message(FATAL_ERROR "ship draws ${ship_instances} instances, edit ${edit_instances}")
endif()

# Nothing of the canvas is shipped.
file(READ "${OUT_DIR}/ship/passes/main_rendering_config.json" shipped)
if(shipped MATCHES "\"op\": \"(canvas|shape|filter)\\.")
    message(FATAL_ERROR "the shipped document still holds a canvas op: ${CMAKE_MATCH_0}")
endif()
file(READ "${OUT_DIR}/ship/project.json" shipped_project)
string(JSON shipped_requires GET "${shipped_project}" requires)
if(shipped_requires MATCHES "canvas")
    message(FATAL_ERROR "the shipped project still requires canvas: ${shipped_requires}")
endif()
string(JSON removed LENGTH "${ship_report}" removed_assets)
if(NOT removed EQUAL 12 OR EXISTS "${OUT_DIR}/ship/imports/psd_fixture")
    message(FATAL_ERROR "the PSD's 12 layer and mask files are still shipped\n${ship_report}")
endif()

# The same picture at two times.
draw("${OUT_DIR}/edit" "${OUT_DIR}/edit_frames")
draw("${OUT_DIR}/ship" "${OUT_DIR}/ship_frames")
foreach(frame IN ITEMS 0001 0031)
    execute_process(
        COMMAND "${COMPARE}" none out "${OUT_DIR}/ship_frames/${frame}.png" --tolerance 2 --min-fraction 1
                --against "${OUT_DIR}/edit_frames/${frame}.png"
        RESULT_VARIABLE compare_result
        OUTPUT_VARIABLE compare_stdout
        ERROR_VARIABLE compare_stderr)
    message(STATUS "frame ${frame}, ship against edit: ${compare_stdout}")
    if(NOT compare_result EQUAL 0)
        message(FATAL_ERROR "frame ${frame} differs (${compare_result})\n${compare_stdout}${compare_stderr}")
    endif()
endforeach()
file(READ "${OUT_DIR}/ship_frames/0001.png" first HEX)
file(READ "${OUT_DIR}/ship_frames/0031.png" later HEX)
if(first STREQUAL later)
    message(FATAL_ERROR "frames 1 and 31 are identical; the disc did not move")
endif()

# The same project ships to the same bytes.
lower("${OUT_DIR}/ship_again" --policy ship --player "${PLAYER}")
file(GLOB_RECURSE shipped_files RELATIVE "${OUT_DIR}/ship" "${OUT_DIR}/ship/passes/*" "${OUT_DIR}/ship/canvas_baked/*"
     "${OUT_DIR}/ship/shaders/generated/*")
foreach(file IN LISTS shipped_files)
    file(SHA256 "${OUT_DIR}/ship/${file}" one)
    if(NOT EXISTS "${OUT_DIR}/ship_again/${file}")
        message(FATAL_ERROR "the second ship lowering wrote no ${file}")
    endif()
    file(SHA256 "${OUT_DIR}/ship_again/${file}" two)
    if(NOT one STREQUAL two)
        message(FATAL_ERROR "the second ship lowering wrote ${file} differently")
    endif()
endforeach()

# CV-3a: a static run that holds a neighbourhood filter is folded the same
# way -- the fixture's background blurred (radius 2) under a moving disc. The
# run (the blurred background) becomes one image drawn by the player, blur
# and all, and no filter is shipped; the shipped frames are held against the
# edit-lowered ones at two times, within 1/255 on every pixel.
set(authored "${OUT_DIR}/blur_authored")
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
file(COPY "${FIXTURE}/layers" DESTINATION "${authored}/imports/psd_fixture")
file(WRITE "${authored}/passes/main_rendering_config.json" [=[
{
  "schema": "pelican.graph",
  "version": 1,
  "definitions": {},
  "nodes": [
    {"id": "clock", "op": "value.input", "parameters": {"name": "time"}, "inputs": {}, "outputs": ["value"]},
    {"id": "circle_x", "op": "value.curve",
     "parameters": {"keys": [{"time": 0, "value": 8}, {"time": 1, "value": 56}]},
     "inputs": {"time": {"node": "clock", "port": "value"}}, "outputs": ["value"]},
    {"id": "background", "op": "canvas.image",
     "parameters": {"asset": "project://imports/psd_fixture/layers/0000_Background.png", "offset": [0, 0], "size": [64, 32]},
     "inputs": {}, "outputs": ["layer"]},
    {"id": "background_blur", "op": "filter.blur", "parameters": {"radius": 2},
     "inputs": {"layer": {"node": "background", "port": "layer"}}, "outputs": ["layer"]},
    {"id": "circle", "op": "shape.ellipse", "parameters": {"size": [12, 12], "center": [8, 16]},
     "inputs": {"center.x": {"node": "circle_x", "port": "value"}}, "outputs": ["geometry"]},
    {"id": "circle_fill", "op": "shape.fill", "parameters": {"color": [1.0, 0.35, 0.1, 0.85]},
     "inputs": {"geometry": {"node": "circle", "port": "geometry"}}, "outputs": ["layer"]},
    {"id": "root", "op": "canvas.stack",
     "parameters": {"size": [64, 32], "layers": [{"input": "blurred", "name": "Blurred"}, {"input": "disc", "name": "Disc"}]},
     "inputs": {"blurred": {"node": "background_blur", "port": "layer"}, "disc": {"node": "circle_fill", "port": "layer"}},
     "outputs": ["layer"]},
    {"id": "out", "op": "canvas.output", "parameters": {"fit": "stretch"},
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
lower("${OUT_DIR}/blur_edit")
lower("${OUT_DIR}/blur_ship" --policy ship --player "${PLAYER}")
set(blur_report "${report}")
message(STATUS "canvas-lower --policy ship (blur): ${blur_report}")
string(JSON flattened LENGTH "${blur_report}" flattened)
if(NOT flattened EQUAL 1)
    message(FATAL_ERROR "expected the blurred background folded into one image\n${blur_report}")
endif()
string(JSON layers GET "${blur_report}" flattened 0 layers)
string(JSON reason GET "${blur_report}" flattened 0 reason)
string(REGEX REPLACE "[ \n]" "" layers "${layers}")
if(NOT layers STREQUAL "[\"Blurred\"]" OR NOT reason STREQUAL "static_from_bottom")
    message(FATAL_ERROR "the folded run is ${layers} (${reason}), not [\"Blurred\"] (static_from_bottom)")
endif()
file(READ "${OUT_DIR}/blur_ship/passes/main_rendering_config.json" shipped)
if(shipped MATCHES "\"op\": \"(canvas|shape|filter)\." OR shipped MATCHES "canvas_blur")
    message(FATAL_ERROR "the shipped document still draws the blur: ${CMAKE_MATCH_0}")
endif()
draw("${OUT_DIR}/blur_edit" "${OUT_DIR}/blur_edit_frames")
draw("${OUT_DIR}/blur_ship" "${OUT_DIR}/blur_ship_frames")
foreach(frame IN ITEMS 0001 0031)
    execute_process(
        COMMAND "${COMPARE}" none out "${OUT_DIR}/blur_ship_frames/${frame}.png" --tolerance 1 --min-fraction 1
                --against "${OUT_DIR}/blur_edit_frames/${frame}.png"
        RESULT_VARIABLE compare_result
        OUTPUT_VARIABLE compare_stdout
        ERROR_VARIABLE compare_stderr)
    message(STATUS "blur frame ${frame}, ship against edit: ${compare_stdout}")
    if(NOT compare_result EQUAL 0)
        message(FATAL_ERROR "blur frame ${frame} differs (${compare_result})\n${compare_stdout}${compare_stderr}")
    endif()
endforeach()
