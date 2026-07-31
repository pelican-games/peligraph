if(NOT DEFINED CLI)
    message(FATAL_ERROR "CLI is required")
endif()
if(NOT DEFINED FIXTURE_DIR)
    message(FATAL_ERROR "FIXTURE_DIR is required")
endif()
if(NOT DEFINED OUT_DIR)
    message(FATAL_ERROR "OUT_DIR is required")
endif()

file(REMOVE_RECURSE "${OUT_DIR}")
file(MAKE_DIRECTORY
    "${OUT_DIR}/project/assets"
    "${OUT_DIR}/project/imports/houdini/delivery_a"
)

file(WRITE "${OUT_DIR}/project/project.json" [=[
{
  "schema": "pelican.project",
  "version": 1,
  "name": "devcli import test",
  "engine_min_version": "0.1.0",
  "basic_config": {
    "asset_data_json": "assets/asset_data.json"
  }
}
]=])
file(WRITE "${OUT_DIR}/project/assets/asset_data.json"
    "{\"schema\":\"pelican.asset_data\",\"version\":1,\"models\":[]}\n")
file(COPY "${FIXTURE_DIR}/valid/" DESTINATION "${OUT_DIR}/project/imports/houdini/delivery_a")

execute_process(
    COMMAND "${CLI}" import "${OUT_DIR}/project/imports/houdini/delivery_a" --project "${OUT_DIR}/project"
    RESULT_VARIABLE first_result
    OUTPUT_VARIABLE first_stdout
    ERROR_VARIABLE first_stderr
)
if(NOT first_result EQUAL 0)
    message(FATAL_ERROR "valid import failed with ${first_result}\nstdout:\n${first_stdout}\nstderr:\n${first_stderr}")
endif()

execute_process(
    COMMAND "${CLI}" import "${OUT_DIR}/project/imports/houdini/delivery_a" --project "${OUT_DIR}/project/project.json"
    RESULT_VARIABLE second_result
    OUTPUT_VARIABLE second_stdout
    ERROR_VARIABLE second_stderr
)
if(NOT second_result EQUAL 0)
    message(FATAL_ERROR "second import failed with ${second_result}\nstdout:\n${second_stdout}\nstderr:\n${second_stderr}")
endif()

file(READ "${OUT_DIR}/project/assets/asset_data.json" asset_data)
string(REGEX MATCHALL "\"name\"[ \r\n\t]*:[ \r\n\t]*\"debris\"" debris_name_matches "${asset_data}")
list(LENGTH debris_name_matches debris_name_count)
if(NOT debris_name_count EQUAL 1)
    message(FATAL_ERROR "import should register debris exactly once; asset_data_json:\n${asset_data}")
endif()
if(NOT asset_data MATCHES "imports/houdini/delivery_a/debris\\.glb")
    message(FATAL_ERROR "import did not write a project-relative GLB path; asset_data_json:\n${asset_data}")
endif()
if(asset_data MATCHES "debris_sim")
    message(FATAL_ERROR "transform_seq output should be verified but not registered; asset_data_json:\n${asset_data}")
endif()

function(expect_import_failure fixture_name expected_pattern)
    set(delivery "${OUT_DIR}/project/imports/houdini/${fixture_name}")
    file(MAKE_DIRECTORY "${delivery}")
    file(COPY "${FIXTURE_DIR}/invalid/${fixture_name}/" DESTINATION "${delivery}")
    execute_process(
        COMMAND "${CLI}" import "${delivery}" --project "${OUT_DIR}/project"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE stdout
        ERROR_VARIABLE stderr
    )
    if(result EQUAL 0)
        message(FATAL_ERROR "invalid import ${fixture_name} unexpectedly succeeded\nstdout:\n${stdout}\nstderr:\n${stderr}")
    endif()
    string(CONCAT combined "${stdout}" "${stderr}")
    if(NOT combined MATCHES "${expected_pattern}")
        message(FATAL_ERROR "invalid import ${fixture_name} error did not match ${expected_pattern}\nstdout:\n${stdout}\nstderr:\n${stderr}")
    endif()
endfunction()

expect_import_failure("sha_mismatch" "sha256 mismatch")
expect_import_failure("absolute_output" "relative")
expect_import_failure("escape_output" "escape")
expect_import_failure("unknown_schema" "output schema")
expect_import_failure("bad_schema" "schema")
