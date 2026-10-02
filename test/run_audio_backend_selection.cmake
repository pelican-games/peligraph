# AU-3: the player's audio backend is chosen at launch -- --audio-backend,
# else the PELICAN_AUDIO_BACKEND environment variable, else the window decides
# (device with one, offline headless). An unknown name stops the launch by
# name before anything opens.
#
# MODE=refusal needs no GPU: the refusal comes from the argument parser.
# MODE=status starts headless --rpc players and reads get_status.audio.
if(NOT DEFINED PLAYER)
    message(FATAL_ERROR "PLAYER is required")
endif()
if(NOT DEFINED PLAYER_DIR)
    message(FATAL_ERROR "PLAYER_DIR is required")
endif()
if(NOT DEFINED OUT_DIR)
    message(FATAL_ERROR "OUT_DIR is required")
endif()
if(NOT DEFINED MODE)
    message(FATAL_ERROR "MODE is required (refusal or status)")
endif()

set(environment_name PELICAN_AUDIO_BACKEND)
# What ctest inherited must not decide a case: every case sets or clears it.
unset(ENV{${environment_name}})

file(REMOVE_RECURSE "${OUT_DIR}")
file(MAKE_DIRECTORY
    "${OUT_DIR}/project/assets"
    "${OUT_DIR}/project/scenes"
    "${OUT_DIR}/project/passes"
    "${OUT_DIR}/project/ui")
file(WRITE "${OUT_DIR}/project/project.json" [=[
{
  "schema": "pelican.project",
  "version": 1,
  "name": "audio_backend_selection",
  "engine_min_version": "0.1.0",
  "basic_config": {
    "window_title": "Audio Backend Selection",
    "window_size": {"width": 64, "height": 64},
    "fullscreen": false,
    "framerate": 30,
    "seed": 1,
    "camera": {"yfov": 0.7853981633974483, "znear": 0.1, "zfar": 100.0, "up": [0.0, 1.0, 0.0]},
    "default_scene_id": "default_scene",
    "scene_data_json": "scenes/main.scene.json",
    "asset_data_json": "assets/asset_data.json",
    "rendering_config_json": "passes/main_rendering_config.json",
    "default_rendering_pass": "main_render",
    "ui_config_json": "ui/ui.json"
  }
}
]=])
file(WRITE "${OUT_DIR}/project/scenes/main.scene.json"
    "{\"schema\":\"pelican.scene\",\"version\":2,\"scenes\":{\"default_scene\":{\"objects\":[]}}}\n")
file(WRITE "${OUT_DIR}/project/assets/asset_data.json" "{\"schema\":\"pelican.asset_data\",\"version\":2,\"models\":[]}\n")
file(WRITE "${OUT_DIR}/project/ui/ui.json"
    "{\"schema\":\"pelican.ui\",\"version\":1,\"key\":\"empty\",\"root\":{\"id\":\"root\",\"type\":\"panel\"}}\n")
get_filename_component(source_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
configure_file("${source_root}/test/fixtures/hybrid_sky_rendering_config.json"
    "${OUT_DIR}/project/passes/main_rendering_config.json" COPYONLY)

# Runs the player with `environment` as PELICAN_AUDIO_BACKEND (empty: unset).
function(run_player label environment)
    if(environment STREQUAL "")
        unset(ENV{${environment_name}})
    else()
        set(ENV{${environment_name}} "${environment}")
    endif()
    set(script_path "${OUT_DIR}/${label}.ndjson")
    file(WRITE "${script_path}" "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"get_status\",\"params\":{}}\n")
    execute_process(
        COMMAND "${PLAYER}" ${ARGN}
        WORKING_DIRECTORY "${PLAYER_DIR}"
        INPUT_FILE "${script_path}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE stdout
        ERROR_VARIABLE stderr
        TIMEOUT 120)
    unset(ENV{${environment_name}})
    set(run_result "${result}" PARENT_SCOPE)
    set(run_stdout "${stdout}" PARENT_SCOPE)
    set(run_stderr "${stderr}" PARENT_SCOPE)
endfunction()

# A launch that names no backend in the three words stops before anything
# opens, naming the refusal and where the name came from. The project it
# names does not exist, so a player that let the name through stops at the
# project -- red here, and never on a GPU this test holds no lease for.
function(expect_refused label environment source)
    run_player("${label}" "${environment}" --headless --project "${OUT_DIR}/missing_project" ${ARGN})
    if(run_result EQUAL 0)
        message(FATAL_ERROR "${label}: the player accepted an unknown audio backend\nstdout:\n${run_stdout}\nstderr:\n${run_stderr}")
    endif()
    set(combined "${run_stdout}\n${run_stderr}")
    string(FIND "${combined}" "audio.backend_unknown@1: ${source} must be one of: device, offline, null" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "${label}: the refusal did not name audio.backend_unknown@1 and ${source}\nstdout:\n${run_stdout}\nstderr:\n${run_stderr}")
    endif()
    if(combined MATCHES [=["jsonrpc"]=])
        message(FATAL_ERROR "${label}: the player answered RPC after refusing its launch\nstdout:\n${run_stdout}")
    endif()
endfunction()

# get_status.audio of a headless --rpc player, exactly.
function(expect_backend label environment expected)
    run_player("${label}" "${environment}" --rpc --headless --project "${OUT_DIR}/project" --size 64x64 ${ARGN})
    if(NOT run_result EQUAL 0)
        message(FATAL_ERROR "${label}: the player failed with ${run_result}\nstdout:\n${run_stdout}\nstderr:\n${run_stderr}")
    endif()
    if(NOT run_stdout MATCHES "\"audio\":${expected}")
        message(FATAL_ERROR "${label}: get_status.audio was not ${expected}\nstdout:\n${run_stdout}\nstderr:\n${run_stderr}")
    endif()
endfunction()

if(MODE STREQUAL "refusal")
    expect_refused(argument_unknown "" "--audio-backend" --audio-backend speakers)
    expect_refused(argument_empty "" "--audio-backend" --audio-backend "")
    expect_refused(environment_unknown "speakers" "PELICAN_AUDIO_BACKEND")
    # Names are exact: the variable is not read case-insensitively.
    expect_refused(environment_case "NULL" "PELICAN_AUDIO_BACKEND")
elseif(MODE STREQUAL "status")
    # The control: nothing named, a headless player runs the device-less engine.
    expect_backend(default "" [=[\{"available":true,"backend":"offline","voices":\[\]\}]=])
    # The environment silences a player that names nothing itself.
    expect_backend(environment_null "null" [=[\{"available":true,"backend":"null","voices":\[\]\}]=])
    # The argument wins over the environment, whichever way round.
    expect_backend(argument_over_environment "null" [=[\{"available":true,"backend":"offline","voices":\[\]\}]=]
        --audio-backend offline)
    expect_backend(argument_null "offline" [=[\{"available":true,"backend":"null","voices":\[\]\}]=]
        --audio-backend null)
    # An argument that wins leaves the environment unread: a name it would
    # refuse does not stop the launch.
    expect_backend(argument_over_unknown_environment "speakers" [=[\{"available":true,"backend":"null","voices":\[\]\}]=]
        --audio-backend null)
else()
    message(FATAL_ERROR "unknown MODE: ${MODE}")
endif()
