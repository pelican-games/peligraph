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
string(ASCII 239 187 191 sv1_bom)
file(CONFIGURE OUTPUT "${OUT_DIR}/project/assets/asset_data.json"
    CONTENT "${sv1_bom}{\n  \"version\":2,\n  \"schema\":\"pelican.asset_data\",\n  \"models\":[]\n}\n" NEWLINE_STYLE CRLF)
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

file(READ "${OUT_DIR}/project/assets/asset_data.json" sv1_hex HEX)
if(NOT sv1_hex MATCHES "^efbbbf7b0d0a" OR NOT sv1_hex MATCHES "0d0a7d0d0a$")
    message(FATAL_ERROR "SV1 import must retain BOM, CRLF and final newline: ${sv1_hex}")
endif()
string(REPLACE "0d0a" "" sv1_without_crlf "${sv1_hex}")
if(sv1_without_crlf MATCHES "0a")
    message(FATAL_ERROR "SV1 import emitted an LF without CR")
endif()
string(FIND "${asset_data}" "\"version\"" sv1_version)
string(FIND "${asset_data}" "\"schema\"" sv1_schema)
string(FIND "${asset_data}" "\"models\"" sv1_models)
if(NOT sv1_version LESS sv1_schema OR NOT sv1_schema LESS sv1_models)
    message(FATAL_ERROR "SV1 import changed original key order")
endif()

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
