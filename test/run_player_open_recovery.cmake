# MG-1: the player resolves a document transaction a crash left behind before
# any parser reads project.json. The fixture leaves the state a crash inside a
# project document transaction leaves: a prepared manifest (no committed
# marker) and project.json already replaced by the candidate bytes -- here a
# project.json whose version this engine refuses. The gate's recovery puts the
# original bytes back and the project opens. Without it the player parses the
# candidate first and stops at "project.json version must be exactly 1".
foreach(required IN ITEMS CLI PLAYER OUT_DIR)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "${required} is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${OUT_DIR}")
file(MAKE_DIRECTORY "${OUT_DIR}")
set(project_dir "${OUT_DIR}/project")
execute_process(
    COMMAND "${CLI}" project init "${project_dir}"
    RESULT_VARIABLE init_result
    OUTPUT_VARIABLE init_stdout
    ERROR_VARIABLE init_stderr)
if(NOT init_result EQUAL 0)
    message(FATAL_ERROR "project init failed with ${init_result}\n${init_stdout}\n${init_stderr}")
endif()

set(project_file "${project_dir}/project.json")
file(READ "${project_file}" original)
file(SHA256 "${project_file}" original_digest)
string(REPLACE "\"version\": 1," "\"version\": 99," candidate "${original}")
if(candidate STREQUAL original)
    message(FATAL_ERROR "the fixture could not form the candidate project.json\n${original}")
endif()

set(transaction "${project_dir}/.pelican/render-authoring.transaction")
file(MAKE_DIRECTORY "${transaction}")
# The backup is the original's exact bytes (file(WRITE) would translate line
# endings on Windows); the candidate's digest is taken from what was written.
file(COPY_FILE "${project_file}" "${transaction}/0.before")
file(WRITE "${project_file}" "${candidate}")
file(COPY_FILE "${project_file}" "${transaction}/0.next")
file(SHA256 "${project_file}" candidate_digest)
file(WRITE "${transaction}/manifest.json" "{
  \"schema\": \"pelican.render_authoring_transaction\",
  \"version\": 1,
  \"status\": \"prepared\",
  \"documents\": [
    {
      \"destination\": \"project.json\",
      \"operation\": \"replace\",
      \"expected_digest\": \"${original_digest}\",
      \"next_digest\": \"${candidate_digest}\",
      \"backup\": \"0.before\",
      \"stage\": \"0.next\"
    }
  ],
  \"created_directories\": []
}
")

execute_process(
    COMMAND "${PLAYER}" --headless --project "${project_dir}" --frames 2
            --size 160x90 --fps 30 --render-out "${OUT_DIR}/frame.png"
    RESULT_VARIABLE player_result
    OUTPUT_VARIABLE player_stdout
    ERROR_VARIABLE player_stderr)
if(NOT player_result EQUAL 0)
    message(FATAL_ERROR
        "the player did not open a project whose interrupted transaction it had to roll back "
        "(exit ${player_result})\nstdout:\n${player_stdout}\nstderr:\n${player_stderr}")
endif()
file(SHA256 "${project_file}" restored_digest)
if(NOT restored_digest STREQUAL original_digest)
    file(READ "${project_file}" restored)
    message(FATAL_ERROR "project.json was not rolled back\n${restored}")
endif()
if(EXISTS "${transaction}")
    message(FATAL_ERROR "the rolled-back transaction directory was left behind")
endif()
if(NOT EXISTS "${OUT_DIR}/frame.png")
    message(FATAL_ERROR "the recovered project did not render a frame")
endif()
message(STATUS "the player rolled back the prepared transaction before parsing project.json")
