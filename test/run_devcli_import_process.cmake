if(NOT DEFINED CLI OR NOT DEFINED CHILD OR NOT DEFINED OUT_DIR)
    message(FATAL_ERROR "CLI, CHILD, and OUT_DIR are required")
endif()

file(REMOVE_RECURSE "${OUT_DIR}")
file(MAKE_DIRECTORY "${OUT_DIR}/project/ui")
file(WRITE "${OUT_DIR}/project/project.json" [=[
{
  "schema": "pelican.project",
  "version": 1,
  "name": "WP173 process fixture",
  "engine_min_version": "0.1.0",
  "basic_config": {"asset_data_json": "assets.json"}
}
]=])
file(WRITE "${OUT_DIR}/project/assets.json"
     "{\"schema\":\"pelican.asset_data\",\"version\":1,\"models\":[]}\n")
file(WRITE "${OUT_DIR}/project/ui/hang.psd" "fixture\n")
file(WRITE "${OUT_DIR}/project/imports.rules.json" [=[
{
  "schema": "pelican.import_rules",
  "version": 1,
  "rules": [{"match": "ui/*.psd", "recipe": "psd_layers"}]
}
]=])

set(LOG_PATH "${OUT_DIR}/import-process.log")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env "PELICAN_WP173_CHILD_MODE=hang"
            "${CLI}" import --rules imports.rules.json --project "${OUT_DIR}/project"
            --source ui/hang.psd --import-tools "${CHILD}" --timeout-ms 250 --log "${LOG_PATH}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr
    TIMEOUT 10
)
if(result EQUAL 0 OR result MATCHES "timeout")
    message(FATAL_ERROR "devcli timeout fixture did not terminate normally\nresult: ${result}\n${stdout}\n${stderr}")
endif()
string(CONCAT combined "${stdout}" "${stderr}")
foreach(required "pelican-import-tools" "timed out after 250 ms" "${LOG_PATH}")
    string(FIND "${combined}" "${required}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "timeout error did not name '${required}'\n${combined}")
    endif()
endforeach()
if(NOT EXISTS "${LOG_PATH}")
    message(FATAL_ERROR "devcli did not preserve the importer log")
endif()
file(READ "${LOG_PATH}" log)
foreach(required "importer-stdout-marker" "importer-stderr-marker" "timeout=1")
    string(FIND "${log}" "${required}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "importer log did not capture '${required}'\n${log}")
    endif()
endforeach()
