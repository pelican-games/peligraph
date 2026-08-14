if(NOT DEFINED PROJECTS_DIR OR "${PROJECTS_DIR}" STREQUAL "")
    message(STATUS
        "PELICAN_PROJECT_CATALOG_SMOKE_SKIP_NO_PROJECTS_DIR: "
        "configure PELICAN_TEST_PROJECTS_DIR to select a smoke-test corpus")
    return()
endif()

foreach(required IN ITEMS PLAYER PNG_NONUNIFORM_CHECK STUDIO_ARGUMENTS_PROBE
                          GAME_LOGIC_DIR GAME_LOGIC_PREFIX GAME_LOGIC_SUFFIX
                          OUT_DIR)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "${required} is required")
    endif()
endforeach()

function(require_no_validation_errors label stdout stderr)
    if(stdout MATCHES "Validation Error|VUID-" OR
       stderr MATCHES "Validation Error|VUID-")
        message(FATAL_ERROR
            "${label} emitted Vulkan validation errors\n"
            "stdout:\n${stdout}\nstderr:\n${stderr}")
    endif()
endfunction()

function(response_for_id stdout id output_var)
    string(REPLACE "\r\n" "\n" normalized "${stdout}")
    string(REPLACE "\r" "\n" normalized "${normalized}")
    string(REPLACE ";" "\\;" normalized "${normalized}")
    string(REPLACE "\n" ";" lines "${normalized}")
    set(found "")
    foreach(line IN LISTS lines)
        if(line MATCHES "\\\"id\\\":${id}([,}])")
            set(found "${line}")
        endif()
    endforeach()
    if(found STREQUAL "")
        message(FATAL_ERROR "missing JSON-RPC id ${id}:\n${stdout}")
    endif()
    string(JSON result_type ERROR_VARIABLE json_error TYPE "${found}" result)
    if(json_error OR NOT result_type)
        message(FATAL_ERROR
            "JSON-RPC id ${id} did not return a result:\n${found}")
    endif()
    set(${output_var} "${found}" PARENT_SCOPE)
endfunction()

function(studio_arguments output_var project_dir)
    execute_process(
        COMMAND "${STUDIO_ARGUMENTS_PROBE}" "${project_dir}" ${ARGN}
        RESULT_VARIABLE arguments_result
        OUTPUT_VARIABLE arguments_stdout
        ERROR_VARIABLE arguments_stderr
    )
    if(NOT arguments_result EQUAL 0)
        message(FATAL_ERROR
            "studioPlayerArguments probe failed with ${arguments_result}\n"
            "stdout:\n${arguments_stdout}\nstderr:\n${arguments_stderr}")
    endif()
    string(REPLACE "\r\n" "\n" arguments_stdout "${arguments_stdout}")
    string(REPLACE "\r" "\n" arguments_stdout "${arguments_stdout}")
    string(REGEX REPLACE "\n$" "" arguments_stdout "${arguments_stdout}")
    if(arguments_stdout STREQUAL "")
        message(FATAL_ERROR "studioPlayerArguments probe returned no arguments")
    endif()
    string(REPLACE ";" "\\;" arguments_stdout "${arguments_stdout}")
    string(REPLACE "\n" ";" arguments "${arguments_stdout}")
    set(${output_var} "${arguments}" PARENT_SCOPE)
endfunction()

# Discover at test execution time so a newly added project.json is covered
# without maintaining a second, hand-written project catalog in CMake.
file(GLOB_RECURSE project_manifests LIST_DIRECTORIES false
    "${PROJECTS_DIR}/*/project.json")
list(SORT project_manifests)
list(LENGTH project_manifests project_count)
if(project_count EQUAL 0)
    message(FATAL_ERROR "no projects were discovered below ${PROJECTS_DIR}")
endif()

file(REMOVE_RECURSE "${OUT_DIR}")
file(MAKE_DIRECTORY "${OUT_DIR}")

set(catalog_frames 10)
set(catalog_frame_names
    0001 0002 0003 0004 0005 0006 0007 0008 0009 0010)
set(editor_project_dir "")

foreach(project_manifest IN LISTS project_manifests)
    get_filename_component(project_dir "${project_manifest}" DIRECTORY)
    file(RELATIVE_PATH project_relative "${PROJECTS_DIR}" "${project_dir}")
    string(REPLACE "/" "_" output_stem "${project_relative}")
    string(REPLACE "\\" "_" output_stem "${output_stem}")
    string(REGEX REPLACE "[^A-Za-z0-9_]" "_"
        game_logic_stem "${project_relative}")

    set(expectations_path "${project_dir}/catalog_smoke.json")
    if(NOT EXISTS "${expectations_path}")
        message(FATAL_ERROR
            "project '${project_relative}' has no recorded catalog smoke "
            "thresholds at ${expectations_path}")
    endif()
    file(READ "${expectations_path}" expectations)
    string(JSON expectations_schema ERROR_VARIABLE expectations_error
        GET "${expectations}" schema)
    string(JSON expectations_version ERROR_VARIABLE version_error
        GET "${expectations}" version)
    string(JSON minimum_distinct_colors ERROR_VARIABLE colors_error
        GET "${expectations}" minimum_distinct_colors)
    string(JSON minimum_non_modal_ratio ERROR_VARIABLE ratio_error
        GET "${expectations}" minimum_non_modal_ratio)
    if(expectations_error OR version_error OR colors_error OR ratio_error OR
       NOT expectations_schema STREQUAL "pelican.project_catalog_smoke" OR
       NOT expectations_version EQUAL 1)
        message(FATAL_ERROR
            "project '${project_relative}' has invalid catalog smoke "
            "expectations: ${expectations_path}")
    endif()

    string(JSON editor_type ERROR_VARIABLE editor_error
        TYPE "${expectations}" editor)
    if(NOT editor_error)
        if(NOT editor_project_dir STREQUAL "")
            message(FATAL_ERROR
                "catalog smoke declares more than one editor project: "
                "'${editor_project_relative}' and '${project_relative}'")
        endif()
        if(NOT editor_type STREQUAL "OBJECT")
            message(FATAL_ERROR
                "project '${project_relative}' editor smoke declaration must be an object")
        endif()
        set(editor_project_dir "${project_dir}")
        set(editor_project_relative "${project_relative}")
        foreach(field IN ITEMS pick_x pick_y scene_id declaration_index)
            string(JSON editor_${field} ERROR_VARIABLE editor_field_error
                GET "${expectations}" editor ${field})
            if(editor_field_error)
                message(FATAL_ERROR
                    "project '${project_relative}' editor smoke is missing '${field}'")
            endif()
        endforeach()
    endif()

    set(game_logic_arguments)
    if(EXISTS "${project_dir}/code/CMakeLists.txt")
        set(game_logic_path
            "${GAME_LOGIC_DIR}/${GAME_LOGIC_PREFIX}pelican_catalog_${game_logic_stem}_game_logic${GAME_LOGIC_SUFFIX}")
        if(NOT EXISTS "${game_logic_path}")
            message(FATAL_ERROR
                "project '${project_relative}' declares code but its catalog "
                "game-logic artifact is missing: ${game_logic_path}. Reconfigure "
                "after adding a project to the selected corpus.")
        endif()
        list(APPEND game_logic_arguments --game-logic "${game_logic_path}")
    endif()

    set(project_output_dir "${OUT_DIR}/${output_stem}")
    file(MAKE_DIRECTORY "${project_output_dir}")
    set(output_pattern "${project_output_dir}/%04d.png")

    # An explicit frame count avoids both known empty checks: example stays
    # byte-identical through frame 6 without input, and replay plus an implicit
    # frame count writes the wrong frame for a single output path. A numbered
    # path makes frame 1 and the actual final frame independently observable.
    execute_process(
        COMMAND "${PLAYER}"
            --headless
            --project "${project_dir}"
            ${game_logic_arguments}
            --frames ${catalog_frames}
            --size 160x90
            --fps 30
            --render-out "${output_pattern}"
        RESULT_VARIABLE player_result
        OUTPUT_VARIABLE player_stdout
        ERROR_VARIABLE player_stderr
        TIMEOUT 180
    )
    if(NOT player_result EQUAL 0)
        message(FATAL_ERROR
            "project '${project_relative}' failed in pelican_player with ${player_result}\n"
            "stdout:\n${player_stdout}\nstderr:\n${player_stderr}")
    endif()
    require_no_validation_errors(
        "project '${project_relative}'" "${player_stdout}" "${player_stderr}")

    foreach(frame_name IN LISTS catalog_frame_names)
        set(frame_path "${project_output_dir}/${frame_name}.png")
        if(NOT EXISTS "${frame_path}")
            message(FATAL_ERROR
                "project '${project_relative}' did not render numbered frame ${frame_path}")
        endif()
    endforeach()
    set(first_png "${project_output_dir}/0001.png")
    set(final_png "${project_output_dir}/0010.png")

    execute_process(
        COMMAND "${PNG_NONUNIFORM_CHECK}" "${final_png}"
            "${minimum_distinct_colors}" "${minimum_non_modal_ratio}"
        RESULT_VARIABLE image_result
        OUTPUT_VARIABLE image_stdout
        ERROR_VARIABLE image_stderr
    )
    if(NOT image_result EQUAL 0)
        message(FATAL_ERROR
            "project '${project_relative}' final frame missed its recorded content thresholds\n"
            "stdout:\n${image_stdout}\nstderr:\n${image_stderr}")
    endif()

    # An authored animation graph is an entry-point declaration that the
    # project expects animation. Compare the running project's first and final
    # captures, rather than merely finding animation data in an asset.
    file(GLOB_RECURSE animation_declarations LIST_DIRECTORIES false
        "${project_dir}/*.anim_graph.json")
    if(animation_declarations)
        file(READ "${first_png}" first_hex HEX)
        file(READ "${final_png}" final_hex HEX)
        if(first_hex STREQUAL final_hex)
            message(FATAL_ERROR
                "project '${project_relative}' declares animation but numbered "
                "frame ${catalog_frames} is byte-identical to frame 1")
        endif()
    endif()

    string(STRIP "${image_stdout}" image_summary)
    message(STATUS "project '${project_relative}' passed: ${image_summary}")
endforeach()

if(editor_project_dir STREQUAL "")
    message(FATAL_ERROR
        "the selected project catalog has no editor smoke declaration")
endif()

set(editor_output_dir "${OUT_DIR}/editor")
file(MAKE_DIRECTORY "${editor_output_dir}")
set(unselected_capture "${editor_output_dir}/unselected.png")
set(selected_capture "${editor_output_dir}/selected.png")
file(TO_CMAKE_PATH "${unselected_capture}" unselected_capture_json)
file(TO_CMAKE_PATH "${selected_capture}" selected_capture_json)
set(editor_selection
    "{\"kind\":\"declaration\",\"scene_id\":\"${editor_scene_id}\",\"declaration_index\":${editor_declaration_index}}")

set(editor_rpc_script "${editor_output_dir}/editor.ndjson")
file(WRITE "${editor_rpc_script}"
    "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"step_frame\",\"params\":{}}\n"
    "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"set_gizmo\",\"params\":{\"selection\":null,\"mode\":\"translate\"}}\n"
    "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"capture\",\"params\":{\"path\":\"${unselected_capture_json}\"}}\n"
    "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"scene_tree\",\"params\":{}}\n"
    "{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"pick_object\",\"params\":{\"x\":${editor_pick_x},\"y\":${editor_pick_y}}}\n"
    "{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"set_gizmo\",\"params\":{\"selection\":${editor_selection},\"mode\":\"translate\"}}\n"
    "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"render_frame\",\"params\":{}}\n"
    "{\"jsonrpc\":\"2.0\",\"id\":8,\"method\":\"capture\",\"params\":{\"path\":\"${selected_capture_json}\"}}\n"
    "{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"get_frame_plan\",\"params\":{}}\n"
    "{\"jsonrpc\":\"2.0\",\"id\":10,\"method\":\"get_modal_transform\",\"params\":{}}\n")

# These configured extras are passed through the same implementation called by
# EmbeddedViewport::launchCommand(). Its output includes the value-less
# --editor-transform flag used by Studio, followed by the headless test driver.
studio_arguments(editor_arguments "${editor_project_dir}"
    --headless --frames 0 --size 192x192 --fps 30)
execute_process(
    COMMAND "${PLAYER}" ${editor_arguments}
    INPUT_FILE "${editor_rpc_script}"
    RESULT_VARIABLE editor_result
    OUTPUT_VARIABLE editor_stdout
    ERROR_VARIABLE editor_stderr
    TIMEOUT 180
)
if(NOT editor_result EQUAL 0)
    message(FATAL_ERROR
        "editor project '${editor_project_relative}' failed with ${editor_result}\n"
        "stdout:\n${editor_stdout}\nstderr:\n${editor_stderr}")
endif()
require_no_validation_errors(
    "editor project '${editor_project_relative}'"
    "${editor_stdout}" "${editor_stderr}")

foreach(id RANGE 1 10)
    response_for_id("${editor_stdout}" ${id} editor_response_${id})
endforeach()

string(JSON unselected_visible GET "${editor_response_2}" result visible)
if(unselected_visible)
    message(FATAL_ERROR
        "editor negative control unexpectedly had a visible gizmo:\n${editor_response_2}")
endif()

string(JSON scene_id GET "${editor_response_4}" result scene_id)
string(JSON object_count LENGTH "${editor_response_4}" result objects)
set(found_editor_object false)
if(object_count GREATER 0)
    math(EXPR last_object "${object_count} - 1")
    foreach(object_index RANGE 0 ${last_object})
        string(JSON declaration_index GET
            "${editor_response_4}" result objects ${object_index} declaration_index)
        if(declaration_index EQUAL editor_declaration_index)
            set(found_editor_object true)
        endif()
    endforeach()
endif()
if(NOT scene_id STREQUAL editor_scene_id OR NOT found_editor_object)
    message(FATAL_ERROR
        "scene_tree did not resolve the declared editor target:\n${editor_response_4}")
endif()

string(JSON pick_hit_type TYPE "${editor_response_5}" result hit)
if(NOT pick_hit_type STREQUAL "OBJECT")
    message(FATAL_ERROR
        "pick_object did not hit the declared editor target:\n${editor_response_5}")
endif()
string(JSON picked_scene GET "${editor_response_5}" result hit scene_id)
string(JSON picked_index GET "${editor_response_5}" result hit declaration_index)
if(NOT picked_scene STREQUAL editor_scene_id OR
   NOT picked_index EQUAL editor_declaration_index)
    message(FATAL_ERROR
        "pick_object resolved a different target than set_gizmo uses:\n${editor_response_5}")
endif()

string(JSON selected_visible GET "${editor_response_6}" result visible)
string(JSON selected_scene GET
    "${editor_response_6}" result selection scene_id)
string(JSON selected_index GET
    "${editor_response_6}" result selection declaration_index)
if(NOT selected_visible OR NOT selected_scene STREQUAL editor_scene_id OR
   NOT selected_index EQUAL editor_declaration_index)
    message(FATAL_ERROR
        "set_gizmo did not resolve the picked target:\n${editor_response_6}")
endif()

string(JSON plan_node_count LENGTH "${editor_response_9}" result nodes)
set(found_gizmo_pass false)
set(found_picking_pass false)
if(plan_node_count GREATER 0)
    math(EXPR last_plan_node "${plan_node_count} - 1")
    foreach(node_index RANGE 0 ${last_plan_node})
        string(JSON node_name GET
            "${editor_response_9}" result nodes ${node_index} name)
        if(node_name STREQUAL "gizmo_pass")
            set(found_gizmo_pass true)
        elseif(node_name STREQUAL "picking_pass")
            set(found_picking_pass true)
        endif()
    endforeach()
endif()
if(NOT found_gizmo_pass OR NOT found_picking_pass)
    message(FATAL_ERROR
        "the running editor frame plan lacks gizmo_pass or picking_pass")
endif()

foreach(capture IN ITEMS "${unselected_capture}" "${selected_capture}")
    execute_process(
        COMMAND "${PNG_NONUNIFORM_CHECK}" "${capture}" 1 0
        RESULT_VARIABLE capture_result
        OUTPUT_VARIABLE capture_stdout
        ERROR_VARIABLE capture_stderr
    )
    if(NOT capture_result EQUAL 0)
        message(FATAL_ERROR
            "editor capture could not be decoded: ${capture}\n"
            "stdout:\n${capture_stdout}\nstderr:\n${capture_stderr}")
    endif()
endforeach()
file(READ "${unselected_capture}" unselected_hex HEX)
file(READ "${selected_capture}" selected_hex HEX)
if(unselected_hex STREQUAL selected_hex)
    message(FATAL_ERROR
        "the selected gizmo capture is byte-identical to the no-selection "
        "capture from the same Studio-argv session")
endif()

# Assert the value resolved by the running player, not the presence of the
# value-less flag. The explicit grab profile is the entry-point negative
# control: it intentionally has no modal G/R/S bindings.
set(grab_rpc_script "${editor_output_dir}/grab.ndjson")
file(WRITE "${grab_rpc_script}"
    "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"get_modal_transform\",\"params\":{}}\n")
studio_arguments(grab_arguments "${editor_project_dir}"
    --headless --frames 0 --size 192x192 --fps 30
    --editor-transform=grab)
execute_process(
    COMMAND "${PLAYER}" ${grab_arguments}
    INPUT_FILE "${grab_rpc_script}"
    RESULT_VARIABLE grab_result
    OUTPUT_VARIABLE grab_stdout
    ERROR_VARIABLE grab_stderr
    TIMEOUT 180
)
if(NOT grab_result EQUAL 0)
    message(FATAL_ERROR
        "editor grab-profile control failed with ${grab_result}\n"
        "stdout:\n${grab_stdout}\nstderr:\n${grab_stderr}")
endif()
require_no_validation_errors(
    "editor grab-profile control" "${grab_stdout}" "${grab_stderr}")
response_for_id("${grab_stdout}" 1 grab_response)

foreach(binding IN ITEMS "translate;G" "rotate;R" "scale;S")
    list(GET binding 0 action)
    list(GET binding 1 expected_key)
    string(JSON resolved_key ERROR_VARIABLE binding_error
        GET "${editor_response_10}" result bindings ${action})
    if(binding_error OR NOT resolved_key STREQUAL expected_key)
        message(FATAL_ERROR
            "Studio's value-less --editor-transform resolved gizmo.${action} "
            "to '${resolved_key}', not '${expected_key}':\n${editor_response_10}")
    endif()
endforeach()
string(JSON bare_bindings GET "${editor_response_10}" result bindings)
string(JSON grab_bindings GET "${grab_response}" result bindings)
if(bare_bindings STREQUAL grab_bindings)
    message(FATAL_ERROR
        "Studio's value-less --editor-transform resolved to the same bindings "
        "as the deliberately inert grab profile")
endif()

message(STATUS
    "editor project '${editor_project_relative}' passed: scene_tree -> "
    "pick_object -> set_gizmo -> render_frame -> capture; selected and "
    "unselected captures differ")
message(STATUS "all ${project_count} discovered projects passed headless smoke")
