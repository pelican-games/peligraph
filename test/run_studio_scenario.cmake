# One Studio scenario, run against a real window and a real engine.
#
# The scenario file names gestures and expectations; tools/studio_drive.py
# performs them over Studio's `--automation` channel and writes report.json.
# This script only builds the world the scenario needs: a private copy of the
# fixture project (two of them, so "re-read it from disk" is a question that can
# be answered), Qt on PATH, and the player the viewport should launch.

foreach(required IN ITEMS STUDIO PLAYER DRIVER SCENARIO FIXTURE_DIR OUT_DIR PYTHON)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "${required} is required")
    endif()
endforeach()

if(NOT EXISTS "${STUDIO}")
    message(FATAL_ERROR "pelican_studio was not built: ${STUDIO}")
endif()
if(NOT EXISTS "${SCENARIO}")
    message(FATAL_ERROR "no such scenario: ${SCENARIO}")
endif()

get_filename_component(scenario_name "${SCENARIO}" NAME_WE)
set(run_dir "${OUT_DIR}/${scenario_name}")
file(REMOVE_RECURSE "${run_dir}")
file(MAKE_DIRECTORY "${run_dir}")

# The scenario edits the document it opens. A test that edits a directory in the
# repository is a test that only passes the first time, so both projects it can
# open are copies made here and thrown away next run.
set(project_dir "${run_dir}/project")
set(other_project_dir "${run_dir}/project_control")
file(COPY "${FIXTURE_DIR}/" DESTINATION "${project_dir}")
file(COPY "${FIXTURE_DIR}/" DESTINATION "${other_project_dir}")

# Studio is not deployed with its own Qt: the DLLs come from the Qt that built
# it, which is where Qt6Network and Qt6Test live too.
set(scenario_path_env "$ENV{PATH}")
if(DEFINED QT_BIN_DIR AND NOT "${QT_BIN_DIR}" STREQUAL "")
    file(TO_NATIVE_PATH "${QT_BIN_DIR}" qt_bin_native)
    if(WIN32)
        set(scenario_path_env "${qt_bin_native};$ENV{PATH}")
    else()
        set(scenario_path_env "${qt_bin_native}:$ENV{PATH}")
    endif()
endif()

# PM-3a: a project that runs game code gets the DLL beside it (a copy, so a
# rebuild is never blocked by the engine holding the file) and Studio passes
# it to every engine it starts.
set(scenario_extra_env)
if(DEFINED GAME_LOGIC AND NOT "${GAME_LOGIC}" STREQUAL "")
    if(NOT EXISTS "${GAME_LOGIC}")
        message(FATAL_ERROR "the scenario's game logic was not built: ${GAME_LOGIC}")
    endif()
    get_filename_component(game_logic_name "${GAME_LOGIC}" NAME)
    file(COPY "${GAME_LOGIC}" DESTINATION "${run_dir}/game_logic")
    file(TO_NATIVE_PATH "${run_dir}/game_logic/${game_logic_name}" game_logic_native)
    list(APPEND scenario_extra_env
        "PELICAN_STUDIO_PLAYER_ARGUMENTS=--game-logic \"${game_logic_native}\"")
endif()

set(report "${run_dir}/report.json")
execute_process(
    COMMAND ${CMAKE_COMMAND} -E env "PATH=${scenario_path_env}" ${scenario_extra_env}
        "${PYTHON}" -B "${DRIVER}"
            --studio "${STUDIO}"
            --player "${PLAYER}"
            --scenario "${SCENARIO}"
            --project "${project_dir}"
            --var "other_project=${other_project_dir}"
            --artifacts "${run_dir}"
            --report "${report}"
    RESULT_VARIABLE driver_result
    OUTPUT_VARIABLE driver_stdout
    ERROR_VARIABLE driver_stderr
    # Under the intrinsic GPU timeout, so a scenario that hangs is reported here
    # with its report path rather than killed by CTest with nothing to read.
    TIMEOUT 540
)

message(STATUS "studio scenario ${scenario_name}:\n${driver_stdout}")
if(NOT driver_result EQUAL 0)
    message(FATAL_ERROR
        "studio scenario '${scenario_name}' failed with ${driver_result}\n"
        "${driver_stderr}\n"
        "report: ${report}\n"
        "stdout:\n${driver_stdout}")
endif()
if(NOT EXISTS "${report}")
    message(FATAL_ERROR
        "studio scenario '${scenario_name}' wrote no report at ${report}")
endif()
message(STATUS "studio scenario '${scenario_name}' passed; report: ${report}")
