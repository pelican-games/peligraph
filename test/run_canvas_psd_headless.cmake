# CV-2a: a PSD layer stack imported by psd-canvas (fixtures/canvas_psd, every
# layer blending normal) comes out of the player as reference.png shows it.
# A blank project gets the import's layers and masks under
# imports/psd_fixture, its definition, an instance of it and a canvas.output
# reading the instance; `pelican_cli canvas-lower` lowers it and the player
# draws the lowered copy headless at the PSD's size. The frame is held
# against the CPU reference of the authored document (fed the same PNGs) and
# against reference.png, the PSD's own composite.
#
# A second project stacks a canvas.from_image of what a render.fullscreen pass
# drew under a circle, held against the CPU reference.
#
# Tolerance, measured on the development machine (NVIDIA, Debug, 64 x 32):
# the PSD frame is within 1/255 of the CPU reference and of reference.png on
# every pixel (mean 0.06/255 and 0.31/255); the bar is 2/255 on every pixel.
# The from_image frame is within 2/255 of the CPU reference on 99.8 % of the
# pixels; the 4 left are on the circle's antialiased edge (up to 5/255), as in
# canvas_lower_headless_player. The bar there is 99.5 %.

foreach(variable IN ITEMS CLI PLAYER PLAYER_DIR COMPARE OUT_DIR FIXTURE)
    if(NOT DEFINED ${variable})
        message(FATAL_ERROR "${variable} is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${OUT_DIR}")
file(MAKE_DIRECTORY "${OUT_DIR}")

function(blank_project root)
    execute_process(
        COMMAND "${CLI}" project init "${root}" --template blank
        RESULT_VARIABLE init_result
        OUTPUT_VARIABLE init_stdout
        ERROR_VARIABLE init_stderr)
    if(NOT init_result EQUAL 0)
        message(FATAL_ERROR "project init failed (${init_result})\n${init_stdout}\n${init_stderr}")
    endif()
    file(READ "${root}/project.json" project)
    string(JSON project SET "${project}" requires "[\"canvas\"]")
    file(WRITE "${root}/project.json" "${project}")
endfunction()

# Lowers `authored` into `lowered`, draws one frame at `size` and leaves it at
# `frame`.
function(lower_and_draw authored lowered size frame)
    execute_process(
        COMMAND "${CLI}" canvas-lower --project "${authored}" --out "${lowered}"
        RESULT_VARIABLE lower_result
        OUTPUT_VARIABLE report
        ERROR_VARIABLE lower_stderr)
    if(NOT lower_result EQUAL 0)
        message(FATAL_ERROR "canvas-lower failed (${lower_result})\n${report}\n${lower_stderr}")
    endif()
    message(STATUS "canvas-lower: ${report}")
    get_filename_component(frames "${frame}" DIRECTORY)
    file(MAKE_DIRECTORY "${frames}")
    execute_process(
        COMMAND "${PLAYER}" --headless --project "${lowered}" --size ${size} --fps 60 --frames 1
                --render-out "${frames}/%04d.png"
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
    if(NOT EXISTS "${frame}")
        message(FATAL_ERROR "the player wrote no ${frame}")
    endif()
endfunction()

function(compare what)
    execute_process(
        COMMAND "${COMPARE}" ${ARGN}
        RESULT_VARIABLE compare_result
        OUTPUT_VARIABLE compare_stdout
        ERROR_VARIABLE compare_stderr)
    message(STATUS "${what}: ${compare_stdout}")
    if(NOT compare_result EQUAL 0)
        message(FATAL_ERROR "${what} differs (${compare_result})\n${compare_stdout}${compare_stderr}")
    endif()
endfunction()

set(present [=[
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
]=])

# --- the PSD -----------------------------------------------------------------
set(authored "${OUT_DIR}/psd/authored")
blank_project("${authored}")
file(COPY "${FIXTURE}/layers" "${FIXTURE}/masks" DESTINATION "${authored}/imports/psd_fixture")
file(READ "${FIXTURE}/canvas.graph.json" graph)
string(JSON definitions GET "${graph}" definitions)
string(JSON definition MEMBER "${graph}" definitions 0)
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
${present}
}
")
lower_and_draw("${authored}" "${OUT_DIR}/psd/lowered" 64x32 "${OUT_DIR}/psd/frames/0001.png")

set(images)
foreach(directory IN ITEMS layers masks)
    file(GLOB pngs RELATIVE "${FIXTURE}/${directory}" "${FIXTURE}/${directory}/*.png")
    foreach(png IN LISTS pngs)
        list(APPEND images --image "project://imports/psd_fixture/${directory}/${png}=${FIXTURE}/${directory}/${png}")
    endforeach()
endforeach()
compare("the PSD frame against the CPU reference"
    "${authored}/passes/main_rendering_config.json" out "${OUT_DIR}/psd/frames/0001.png"
    --tolerance 2 --min-fraction 1 ${images})
compare("the PSD frame against reference.png"
    "${authored}/passes/main_rendering_config.json" out "${OUT_DIR}/psd/frames/0001.png"
    --tolerance 2 --min-fraction 1 --against "${FIXTURE}/reference.png")

# --- canvas.from_image -------------------------------------------------------
set(authored "${OUT_DIR}/from_image/authored")
blank_project("${authored}")
# Linear (0.2, 0.6, 0.4): 51, 153 and 102 of 255, which the reference reads back exactly.
file(WRITE "${authored}/shaders/cv2a_solid.frag" [=[#version 460
layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;
void main() { outColor = vec4(0.2, 0.6, 0.4, 1.0); }
]=])
set(color_type [=[{"semantic": "pelican.render.color_signal@1", "arguments": {"reference": "scene", "transfer": "linear", "range": "extended"}}]=])
file(WRITE "${authored}/passes/main_rendering_config.json" "{
  \"schema\": \"pelican.graph\",
  \"version\": 1,
  \"definitions\": {},
  \"nodes\": [
    {\"id\": \"solid_image\", \"op\": \"render.image\",
     \"parameters\": {\"type\": ${color_type}, \"format\": \"R16G16B16A16_SFLOAT\", \"extent_scale\": 1.0,
                    \"usage\": [\"COLOR_ATTACHMENT\", \"SAMPLED\"]},
     \"inputs\": {}, \"outputs\": [\"resource\"]},
    {\"id\": \"solid\", \"op\": \"render.fullscreen\",
     \"parameters\": {\"shader\": {\"vertex\": \"engine://fullscreen\", \"fragment\": \"project://shaders/cv2a_solid\"},
                    \"ports\": {\"color\": {\"kind\": \"image\", \"type\": ${color_type}, \"access\": \"write\",
                                          \"attachment\": \"color\", \"location\": 0}}},
     \"inputs\": {\"color\": {\"node\": \"solid_image\", \"port\": \"resource\"}}, \"outputs\": [\"color\"]},
    {\"id\": \"drawn\", \"op\": \"canvas.from_image\", \"parameters\": {\"offset\": [0, 0], \"size\": [64, 32]},
     \"inputs\": {\"image\": {\"node\": \"solid\", \"port\": \"color\"}}, \"outputs\": [\"layer\"]},
    {\"id\": \"dot\", \"op\": \"shape.ellipse\", \"parameters\": {\"size\": [20, 20], \"center\": [32, 16]},
     \"inputs\": {}, \"outputs\": [\"geometry\"]},
    {\"id\": \"dot_fill\", \"op\": \"shape.fill\", \"parameters\": {\"color\": [1.0, 0.8, 0.1, 1.0]},
     \"inputs\": {\"geometry\": {\"node\": \"dot\", \"port\": \"geometry\"}}, \"outputs\": [\"layer\"]},
    {\"id\": \"root\", \"op\": \"canvas.stack\",
     \"parameters\": {\"size\": [64, 32], \"layers\": [{\"input\": \"drawn\"}, {\"input\": \"dot\"}]},
     \"inputs\": {\"drawn\": {\"node\": \"drawn\", \"port\": \"layer\"}, \"dot\": {\"node\": \"dot_fill\", \"port\": \"layer\"}},
     \"outputs\": [\"layer\"]},
    {\"id\": \"out\", \"op\": \"canvas.output\", \"parameters\": {\"fit\": \"stretch\"},
     \"inputs\": {\"layer\": {\"node\": \"root\", \"port\": \"layer\"}}, \"outputs\": [\"image\"]}
  ],
  \"outputs\": {},
${present}
}
")
lower_and_draw("${authored}" "${OUT_DIR}/from_image/lowered" 64x32 "${OUT_DIR}/from_image/frames/0001.png")
compare("the from_image frame against the CPU reference"
    "${authored}/passes/main_rendering_config.json" out "${OUT_DIR}/from_image/frames/0001.png"
    --tolerance 2 --min-fraction 0.995 --image-color solid.color=51,153,102,255)
