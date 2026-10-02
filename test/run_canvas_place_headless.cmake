# CV-6a: a layer stack placed in 3D by canvas.place, drawn by the player
# (the engine lowering the document as it reads it) onto the colour of a
# forward material pass and tested against its depth.
#
# The scene is the box of test/fixtures/studio_material_pass_authoring (a
# flat-tinted cube at z = 3, turned 30 degrees) and a board of 512 x 256 stack
# units, 200 per world unit, through the box's middle, facing the camera. Its
# layers: a background colour (depth 0), a green dot (depth 60) and, when the
# system's Arial is there, a white word (depth 120). position.x follows a
# curve: still until t = 0.6, then 0.6 to the side from t = 0.9.
#
# Three cameras, one run each: A straight on, B from the side (the parallax),
# C close to the word (the edges, to look at). The probe holds each run's
# frame against canvasplace.hpp: the dot's centroid against its projected
# centre, the box's middle (in front of the board) showing the box, the board
# beside the box showing its background. The frames are left in OUT_DIR.

foreach(variable IN ITEMS CLI PLAYER PLAYER_DIR PROBE FIXTURE OUT_DIR)
    if(NOT DEFINED ${variable})
        message(FATAL_ERROR "${variable} is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${OUT_DIR}")
file(MAKE_DIRECTORY "${OUT_DIR}")
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
string(JSON project SET "${project}" basic_config asset_data_json "\"assets.json\"")
file(WRITE "${project_dir}/project.json" "${project}")
foreach(file IN ITEMS checker_box.glb material_bindings.json assets.json materials/g1_flat.material.json
                      shaders/g1_flat.surface)
    get_filename_component(directory "${project_dir}/${file}" DIRECTORY)
    file(MAKE_DIRECTORY "${directory}")
    file(COPY_FILE "${FIXTURE}/${file}" "${project_dir}/${file}")
endforeach()

set(font "C:/Windows/Fonts/arial.ttf")
set(word_nodes "")
set(word_layer "")
set(word_input "")
if(EXISTS "${font}")
    file(MAKE_DIRECTORY "${project_dir}/fonts")
    file(COPY_FILE "${font}" "${project_dir}/fonts/latin.ttf")
    set(word_nodes [=[
    {"id": "word", "op": "shape.text",
     "parameters": {"font": "project://fonts/latin.ttf", "text": "Placed", "size": 72, "origin": [300, 150]},
     "inputs": {}, "outputs": ["geometry"]},
    {"id": "word_fill", "op": "shape.fill", "parameters": {"color": [1.0, 1.0, 1.0, 1.0]},
     "inputs": {"geometry": {"node": "word", "port": "geometry"}}, "outputs": ["layer"]},]=])
    set(word_layer [=[, {"input": "word", "name": "word", "depth": 120}]=])
    set(word_input [=[, "word": {"node": "word_fill", "port": "layer"}]=])
else()
    message(STATUS "no ${font}: the board has no word")
endif()

set(document "${project_dir}/passes/main_rendering_config.json")
file(WRITE "${document}" "{
  \"schema\": \"pelican.graph\",
  \"version\": 1,
  \"definitions\": {},
  \"nodes\": [
    {\"id\": \"scene_color\", \"op\": \"render.image\",
     \"parameters\": {\"type\": {\"semantic\": \"pelican.render.color_signal@1\",
                               \"arguments\": {\"reference\": \"scene\", \"transfer\": \"linear\", \"range\": \"extended\"}},
                    \"format\": \"R16G16B16A16_SFLOAT\", \"extent_scale\": 1.0, \"usage\": [\"COLOR_ATTACHMENT\", \"SAMPLED\"]},
     \"inputs\": {}, \"outputs\": [\"resource\"]},
    {\"id\": \"scene_depth\", \"op\": \"render.image\",
     \"parameters\": {\"type\": {\"semantic\": \"pelican.render.depth@1\",
                               \"arguments\": {\"representation\": \"device\", \"space\": \"projection\"}},
                    \"format\": \"D32_SFLOAT\", \"extent_scale\": 1.0, \"usage\": [\"DEPTH_STENCIL_ATTACHMENT\", \"SAMPLED\"]},
     \"inputs\": {}, \"outputs\": [\"resource\"]},
    {\"id\": \"fwd\", \"op\": \"render.material_pass\",
     \"parameters\": {\"material_contract\": \"forward_opaque_v1\", \"depth_store_op\": \"store\",
                    \"clear_color\": [0.07, 0.07, 0.09, 1.0]},
     \"inputs\": {\"color\": {\"node\": \"scene_color\", \"port\": \"resource\"},
                \"depth\": {\"node\": \"scene_depth\", \"port\": \"resource\"}},
     \"outputs\": [\"color\", \"depth\"]},
    {\"id\": \"clock\", \"op\": \"value.input\", \"parameters\": {\"name\": \"time\"}, \"inputs\": {}, \"outputs\": [\"value\"]},
    {\"id\": \"slide\", \"op\": \"value.curve\",
     \"parameters\": {\"keys\": [{\"time\": 0, \"value\": 0}, {\"time\": 0.6, \"value\": 0},
                               {\"time\": 0.9, \"value\": 0.6}, {\"time\": 10, \"value\": 0.6}]},
     \"inputs\": {\"time\": {\"node\": \"clock\", \"port\": \"value\"}}, \"outputs\": [\"value\"]},
    {\"id\": \"back\", \"op\": \"canvas.color\", \"parameters\": {\"color\": [0.15, 0.2, 0.45, 1.0]},
     \"inputs\": {}, \"outputs\": [\"layer\"]},
    {\"id\": \"dot\", \"op\": \"shape.ellipse\", \"parameters\": {\"size\": [70, 70], \"center\": [80, 70]},
     \"inputs\": {}, \"outputs\": [\"geometry\"]},
    {\"id\": \"dot_fill\", \"op\": \"shape.fill\", \"parameters\": {\"color\": [0.1, 0.9, 0.2, 1.0]},
     \"inputs\": {\"geometry\": {\"node\": \"dot\", \"port\": \"geometry\"}}, \"outputs\": [\"layer\"]},
${word_nodes}
    {\"id\": \"board\", \"op\": \"canvas.stack\",
     \"parameters\": {\"size\": [512, 256], \"layers\": [{\"input\": \"back\", \"name\": \"back\"},
                                                     {\"input\": \"dot\", \"name\": \"dot\", \"depth\": 60}${word_layer}]},
     \"inputs\": {\"back\": {\"node\": \"back\", \"port\": \"layer\"}, \"dot\": {\"node\": \"dot_fill\", \"port\": \"layer\"}${word_input}},
     \"outputs\": [\"layer\"]},
    {\"id\": \"sign\", \"op\": \"canvas.place\",
     \"parameters\": {\"position\": [0, 0, 3.0], \"rotation\": [0, 180, 0], \"pixels_per_unit\": 200},
     \"inputs\": {\"layer\": {\"node\": \"board\", \"port\": \"layer\"},
                \"color\": {\"node\": \"fwd\", \"port\": \"color\"}, \"depth\": {\"node\": \"fwd\", \"port\": \"depth\"},
                \"position.x\": {\"node\": \"slide\", \"port\": \"value\"}},
     \"outputs\": [\"color\"]}
  ],
  \"outputs\": {},
  \"rendering\": {
    \"material_route_default\": \"forward\",
    \"rendering_passes\": [
      {
        \"name\": \"main_render\",
        \"passes\": [
          {
            \"name\": \"present\",
            \"type\": \"fullscreen\",
            \"input\": [\"scene_color\"],
            \"output\": {\"color\": [\"display\"], \"depth\": null},
            \"shader\": {\"vertex\": \"engine://fullscreen\", \"fragment\": \"engine://passthrough\"}
          }
        ]
      }
    ]
  }
}
")

set(width 960)
set(height 540)
set(yfov 0.7853981633974483)

# One run of the player with the camera at <position> turned by <quaternion>
# (x y z w); the frames go to OUT_DIR/<name>.
function(render name frames position quaternion)
    string(REPLACE ";" ", " position_list "${position}")
    string(REPLACE ";" ", " quaternion_list "${quaternion}")
    file(WRITE "${project_dir}/scenes/main.scene.json" "{\"schema\": \"pelican.scene\", \"version\": 2,
  \"scenes\": {\"default_scene\": {\"objects\": [
    {\"name\": \"Camera\", \"components\": [
      {\"name\": \"transform\", \"pos\": [${position_list}], \"rotation\": [${quaternion_list}], \"scale\": [1, 1, 1]},
      {\"name\": \"camera\", \"type\": \"perspective\", \"yfov\": ${yfov}, \"znear\": 0.1, \"zfar\": 100.0}]},
    {\"name\": \"CheckerBox\", \"components\": [
      {\"name\": \"transform\", \"pos\": [0.0, 0.0, 3.0], \"rotation\": [0.0, 0.2588190451, 0.0, 0.9659258263],
       \"scale\": [1.0, 1.0, 1.0]},
      {\"name\": \"simplemodelview\", \"model\": \"checker_box\"}]}
  ]}}}
")
    file(MAKE_DIRECTORY "${OUT_DIR}/${name}")
    execute_process(
        COMMAND "${PLAYER}" --headless --project "${project_dir}" --size ${width}x${height} --fps 2 --frames ${frames}
                --render-out "${OUT_DIR}/${name}/%04d.png"
        WORKING_DIRECTORY "${PLAYER_DIR}"
        RESULT_VARIABLE player_result
        OUTPUT_VARIABLE player_stdout
        ERROR_VARIABLE player_stderr
        TIMEOUT 900)
    file(WRITE "${OUT_DIR}/${name}/player.txt" "${player_stdout}\n${player_stderr}")
    if(NOT player_result EQUAL 0)
        message(FATAL_ERROR "the player failed (${player_result}) for ${name}\n${player_stdout}\n${player_stderr}")
    endif()
    if(player_stdout MATCHES "Validation Error|VUID-" OR player_stderr MATCHES "Validation Error|VUID-")
        message(FATAL_ERROR "the player emitted Vulkan validation errors for ${name}\n${player_stdout}\n${player_stderr}")
    endif()
endfunction()

set(failures "")
# <name> <frame> <camera: px py pz qx qy qz qw> <slide value> then probe arguments.
function(probe name frame camera slide)
    execute_process(
        COMMAND "${PROBE}" "${document}" sign "${OUT_DIR}/${name}/${frame}.png" --project "${project_dir}"
                --camera ${camera} --yfov ${yfov} --znear 0.1 --zfar 100 slide.value=${slide} ${ARGN}
        RESULT_VARIABLE probe_result
        OUTPUT_VARIABLE probe_stdout
        ERROR_VARIABLE probe_stderr)
    message(STATUS "${name}/${frame}:\n${probe_stdout}${probe_stderr}")
    if(NOT probe_result EQUAL 0)
        set(failures "${failures} ${name}/${frame}" PARENT_SCOPE)
    endif()
endfunction()

# The camera looks along its +z with its +y up; turned 180 degrees about z, the
# world's +y is up on the screen (the blank template's camera has up -y).
set(camera_a 0 0 0 0 0 1 0)
# From x = 1.5, turned toward the board's middle (yaw -26.565 degrees, then the same roll).
set(camera_b 1.5 0 0 -0.2297529205 0 0.9732489895 0)
set(camera_c -0.75 0.2 1.4 0 0 1 0)
render(a 4 "0;0;0" "0;0;1;0")
render(b 1 "1.5;0;0" "-0.2297529205;0;0.9732489895;0")
render(c 1 "-0.75;0.2;1.4" "0;0;1;0")

# Measured on the development machine (NVIDIA, Debug, 960 x 540): the dot's
# centroid is 0.01 / 0.08 pixels from its projected centre straight on (before
# and after the slide), 1.2 pixels from the side (a projected circle's centroid
# is not its projected centre); the bar is 2.5 pixels.
# The dot (sRGB 26 230 51 in the frame) where the arithmetic puts it, the box
# (255 179 63) in front of the board's middle, the board (38 51 115) beside it.
probe(a 0001 "${camera_a}" 0 --mark dot_fill 26 230 51 2.5
      --pixel 480 270 255 179 63 24 --pixel 250 200 38 51 115 24)
probe(a 0004 "${camera_a}" 0.6 --mark dot_fill 26 230 51 2.5)
# From the side the dot (depth 60) sits 15 pixels off where the board's
# plane would put it, and the pixels follow.
probe(b 0001 "${camera_b}" 0 --mark dot_fill 26 230 51 2.5 --min-parallax 10)
if(NOT failures STREQUAL "")
    message(FATAL_ERROR "the frames differ from the placement:${failures}")
endif()
