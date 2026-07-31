if(NOT DEFINED PLAYER)
    message(FATAL_ERROR "PLAYER is required")
endif()
if(NOT DEFINED PROJECT_DIR)
    message(FATAL_ERROR "PROJECT_DIR is required")
endif()
if(NOT DEFINED RENDER_CONFIG)
    message(FATAL_ERROR "RENDER_CONFIG is required")
endif()
if(NOT DEFINED OUT_DIR)
    message(FATAL_ERROR "OUT_DIR is required")
endif()

file(READ "${RENDER_CONFIG}" rendering_config)
string(JSON preset ERROR_VARIABLE preset_error
    GET "${rendering_config}" pipeline preset)
if(NOT preset_error STREQUAL "NOTFOUND" OR
   NOT preset STREQUAL "engine://render_pipelines/hybrid_v1.json")
    message(FATAL_ERROR
        "project rendering config does not select hybrid_v1\n${rendering_config}")
endif()
string(JSON feature_count ERROR_VARIABLE feature_count_error
    LENGTH "${rendering_config}" features)
string(JSON shadow_feature ERROR_VARIABLE shadow_feature_error
    GET "${rendering_config}" features 0)
string(JSON sky_feature ERROR_VARIABLE sky_feature_error
    GET "${rendering_config}" features 1)
if(NOT feature_count_error STREQUAL "NOTFOUND" OR
   NOT shadow_feature_error STREQUAL "NOTFOUND" OR
   NOT sky_feature_error STREQUAL "NOTFOUND" OR
   NOT feature_count EQUAL 2 OR
   NOT shadow_feature STREQUAL "engine://features/shadow_directional.json" OR
   NOT sky_feature STREQUAL "engine://features/sky_ambient.json")
    message(FATAL_ERROR
        "project rendering config does not select the default feature set\n${rendering_config}")
endif()
foreach(forbidden_member IN ITEMS render_targets rendering_passes)
    string(JSON ignored ERROR_VARIABLE member_error
        TYPE "${rendering_config}" "${forbidden_member}")
    if(member_error STREQUAL "NOTFOUND")
        message(FATAL_ERROR
            "project preset config unexpectedly owns ${forbidden_member}\n${rendering_config}")
    endif()
endforeach()

file(READ "${PROJECT_DIR}/scene.json" scene_config)
if(scene_config MATCHES "\"name\"[ \t\r\n]*:[ \t\r\n]*\"light\"")
    message(FATAL_ERROR
        "preset project must exercise sky ambient with zero scene lights\n${scene_config}")
endif()

file(READ "${PROJECT_DIR}/project.json" project_config)
string(JSON default_pass ERROR_VARIABLE default_pass_error
    GET "${project_config}" basic_config default_rendering_pass)
if(NOT default_pass_error STREQUAL "NOTFOUND" OR
   NOT default_pass STREQUAL "main_render")
    message(FATAL_ERROR
        "preset project does not select main_render\n${project_config}")
endif()

file(REMOVE_RECURSE "${OUT_DIR}")
file(MAKE_DIRECTORY "${OUT_DIR}")
set(output_png "${OUT_DIR}/preset.png")
execute_process(
    COMMAND "${PLAYER}"
        --headless
        --project "${PROJECT_DIR}"
        --frames 3
        --size 160x90
        --fps 30
        --render-out "${output_png}"
        --dump-frame-plan
    RESULT_VARIABLE player_result
    OUTPUT_VARIABLE player_stdout
    ERROR_VARIABLE player_stderr
)
if(NOT player_result EQUAL 0)
    message(FATAL_ERROR
        "preset project failed in pelican_player with ${player_result}\n"
        "stdout:\n${player_stdout}\nstderr:\n${player_stderr}")
endif()
if(player_stdout MATCHES "Validation Error|VUID-" OR
   player_stderr MATCHES "Validation Error|VUID-")
    message(FATAL_ERROR
        "preset project emitted Vulkan validation errors\n"
        "stdout:\n${player_stdout}\nstderr:\n${player_stderr}")
endif()
string(FIND "${player_stderr}" "\"graph\": \"main_render\"" graph_at)
if(graph_at EQUAL -1)
    message(FATAL_ERROR
        "preset project frame plan does not contain main_render\n${player_stderr}")
endif()
foreach(expected_node IN ITEMS
        shadow_depth
        deferred_geometry
        deferred_lighting
        sky_background
        forward_opaque
        __snapshot_opaque_color
        __snapshot_opaque_depth
        forward_transparent
        scene_present)
    string(FIND "${player_stderr}" "\"name\": \"${expected_node}\"" node_at)
    if(node_at EQUAL -1)
        message(FATAL_ERROR
            "preset project frame plan is missing ${expected_node}\n${player_stderr}")
    endif()
endforeach()
if(NOT EXISTS "${output_png}")
    message(FATAL_ERROR "preset project did not render an output PNG")
endif()
file(SIZE "${output_png}" png_size)
if(png_size LESS 64)
    message(FATAL_ERROR
        "preset project output PNG is unexpectedly small: ${png_size} bytes")
endif()
