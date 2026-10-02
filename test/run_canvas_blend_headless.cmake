# CV-2b: blend modes and adjustment layers through the player.
#
# The PSD (fixtures/canvas_psd_blend: eight blend modes over a gradient, an
# invert, a levels and a hue / saturation adjustment layer, each masked to a
# region, a multiply in a pass-through group and a screen alone in an
# isolated one) is imported by psd-canvas, lowered by `pelican_cli
# canvas-lower` and drawn headless at its size. The frame is held against the
# CPU reference of the authored document and against reference.png, psd-tools'
# composite of the PSD.
#
# A second project (fixtures/canvas_psd_blend/modes.graph.json) draws every
# blend mode the canvas names, every colour filter on a backdrop, an
# adjustment layer that blends, a clip layer that blends and a filter on a
# shape; it is held against the CPU reference only.
#
# Tolerance, measured on the development machine (NVIDIA, Debug): see the
# bars below and the CV-2b report.

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
    file(COPY "${FIXTURE}/layers" "${FIXTURE}/masks" DESTINATION "${root}/imports/psd_blend")
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

set(rendering [=[{
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
}]=])
set(present "  \"rendering\": ${rendering}")

set(images)
foreach(directory IN ITEMS layers masks)
    file(GLOB pngs RELATIVE "${FIXTURE}/${directory}" "${FIXTURE}/${directory}/*.png")
    foreach(png IN LISTS pngs)
        list(APPEND images --image "project://imports/psd_blend/${directory}/${png}=${FIXTURE}/${directory}/${png}")
    endforeach()
endforeach()

# --- the PSD -----------------------------------------------------------------
set(authored "${OUT_DIR}/psd/authored")
blank_project("${authored}")
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
compare("the PSD frame against the CPU reference"
    "${authored}/passes/main_rendering_config.json" out "${OUT_DIR}/psd/frames/0001.png"
    --tolerance 2 --min-fraction 1 ${images})
compare("the PSD frame against reference.png"
    "${authored}/passes/main_rendering_config.json" out "${OUT_DIR}/psd/frames/0001.png"
    --tolerance 2 --min-fraction 1 --against "${FIXTURE}/reference.png")

# --- every mode and filter -----------------------------------------------------
set(authored "${OUT_DIR}/modes/authored")
blank_project("${authored}")
file(READ "${FIXTURE}/modes.graph.json" document)
string(JSON document SET "${document}" rendering "${rendering}")
file(WRITE "${authored}/passes/main_rendering_config.json" "${document}")
lower_and_draw("${authored}" "${OUT_DIR}/modes/lowered" 104x40 "${OUT_DIR}/modes/frames/0001.png")
compare("the modes frame against the CPU reference"
    "${authored}/passes/main_rendering_config.json" out "${OUT_DIR}/modes/frames/0001.png"
    --tolerance 2 --min-fraction 1 ${images})
