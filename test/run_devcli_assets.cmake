if(NOT DEFINED CLI)
    message(FATAL_ERROR "CLI is required")
endif()
if(NOT DEFINED PLAYER)
    message(FATAL_ERROR "PLAYER is required")
endif()
if(NOT DEFINED OUT_DIR)
    message(FATAL_ERROR "OUT_DIR is required")
endif()

file(REMOVE_RECURSE "${OUT_DIR}")
file(MAKE_DIRECTORY "${OUT_DIR}/project/.pelican" "${OUT_DIR}/external_store/nested")
file(WRITE "${OUT_DIR}/project/project.json" [=[
{
  "schema": "pelican.project",
  "version": 1,
  "name": "assets-cli-test",
  "asset_stores": {
    "main": {"mount": "missing-default-store", "manifest": "assets.manifest.json"}
  },
  "basic_config": {}
}
]=])
file(WRITE "${OUT_DIR}/project/.pelican/local.json"
    "{\"asset_stores\":{\"main\":\"${OUT_DIR}/external_store\"}}\n")
file(WRITE "${OUT_DIR}/external_store/z.bin" "z-content")
file(WRITE "${OUT_DIR}/external_store/nested/a.bin" "a-content")

execute_process(
    COMMAND "${CLI}" assets manifest --project "${OUT_DIR}/project"
    RESULT_VARIABLE first_result
    OUTPUT_VARIABLE first_stdout
    ERROR_VARIABLE first_stderr
)
if(NOT first_result EQUAL 0)
    message(FATAL_ERROR "first assets manifest failed\nstdout:\n${first_stdout}\nstderr:\n${first_stderr}")
endif()
file(READ "${OUT_DIR}/project/assets.manifest.json" first_manifest)

execute_process(
    COMMAND "${CLI}" assets manifest --project "${OUT_DIR}/project"
    RESULT_VARIABLE second_result
    OUTPUT_VARIABLE second_stdout
    ERROR_VARIABLE second_stderr
)
if(NOT second_result EQUAL 0)
    message(FATAL_ERROR "second assets manifest failed\nstdout:\n${second_stdout}\nstderr:\n${second_stderr}")
endif()
file(READ "${OUT_DIR}/project/assets.manifest.json" second_manifest)
if(NOT first_manifest STREQUAL second_manifest)
    message(FATAL_ERROR "assets manifest generation is not byte-idempotent")
endif()
if(NOT second_stdout MATCHES "0 hashed, 2 cached")
    message(FATAL_ERROR "second manifest generation did not use the cache\n${second_stdout}")
endif()

execute_process(
    COMMAND "${CLI}" assets verify --full --project "${OUT_DIR}/project"
    RESULT_VARIABLE clean_result
    OUTPUT_VARIABLE clean_stdout
    ERROR_VARIABLE clean_stderr
)
if(NOT clean_result EQUAL 0)
    message(FATAL_ERROR "clean assets verify failed\nstdout:\n${clean_stdout}\nstderr:\n${clean_stderr}")
endif()

file(WRITE "${OUT_DIR}/external_store/nested/a.bin" "changed-content")
execute_process(
    COMMAND "${CLI}" assets verify --project "${OUT_DIR}/project"
    RESULT_VARIABLE structural_result
    OUTPUT_VARIABLE structural_stdout
    ERROR_VARIABLE structural_stderr
)
if(NOT structural_result EQUAL 0 OR structural_stdout MATCHES "INFO")
    message(FATAL_ERROR "default verify should omit content-only differences\n${structural_stdout}\n${structural_stderr}")
endif()

execute_process(
    COMMAND "${CLI}" assets verify --full --project "${OUT_DIR}/project"
    RESULT_VARIABLE content_result
    OUTPUT_VARIABLE content_stdout
    ERROR_VARIABLE content_stderr
)
if(content_result EQUAL 0 OR NOT content_stdout MATCHES "INFO.*nested/a.bin")
    message(FATAL_ERROR "full verify did not report the content mismatch\n${content_stdout}\n${content_stderr}")
endif()

file(REMOVE "${OUT_DIR}/external_store/z.bin")
file(WRITE "${OUT_DIR}/external_store/extra.bin" "extra")
execute_process(
    COMMAND "${CLI}" assets verify --project "${OUT_DIR}/project"
    RESULT_VARIABLE warning_result
    OUTPUT_VARIABLE warning_stdout
    ERROR_VARIABLE warning_stderr
)
if(warning_result EQUAL 0 OR NOT warning_stdout MATCHES "WARNING.*z.bin" OR
   NOT warning_stdout MATCHES "WARNING.*extra.bin")
    message(FATAL_ERROR "structural verify did not name missing and extra files\n${warning_stdout}\n${warning_stderr}")
endif()

execute_process(
    COMMAND "${CLI}" assets status --project "${OUT_DIR}/project"
    RESULT_VARIABLE status_result
    OUTPUT_VARIABLE status_stdout
    ERROR_VARIABLE status_stderr
)
if(status_result EQUAL 0 OR NOT status_stdout MATCHES "OK store main:.*external_store" OR
   NOT status_stdout MATCHES "MISSING asset main:.*z.bin")
    message(FATAL_ERROR "assets status did not show the resolved override and missing expected path\n${status_stdout}\n${status_stderr}")
endif()

# Exercise the real player startup gate with a complete generated project.
set(startup_project "${OUT_DIR}/startup_project")
execute_process(
    COMMAND "${CLI}" project init "${startup_project}"
    RESULT_VARIABLE init_result
    OUTPUT_VARIABLE init_stdout
    ERROR_VARIABLE init_stderr
)
if(NOT init_result EQUAL 0)
    message(FATAL_ERROR "project init for startup assets test failed\n${init_stdout}\n${init_stderr}")
endif()
file(READ "${startup_project}/project.json" startup_json)
string(REPLACE
    "  \"basic_config\""
    "  \"asset_stores\": {\"main\": {\"mount\": \"assets\", \"manifest\": \"assets.manifest.json\"}},\n  \"basic_config\""
    startup_json "${startup_json}")
file(WRITE "${startup_project}/project.json" "${startup_json}")
file(WRITE "${startup_project}/assets/unused.bin" "original")
execute_process(
    COMMAND "${CLI}" assets manifest --project "${startup_project}"
    RESULT_VARIABLE startup_manifest_result
    OUTPUT_VARIABLE startup_manifest_stdout
    ERROR_VARIABLE startup_manifest_stderr
)
if(NOT startup_manifest_result EQUAL 0)
    message(FATAL_ERROR "startup manifest generation failed\n${startup_manifest_stdout}\n${startup_manifest_stderr}")
endif()
file(WRITE "${startup_project}/assets/unused.bin" "changed and larger")
file(MAKE_DIRECTORY "${OUT_DIR}/frames")
execute_process(
    COMMAND "${PLAYER}" --headless --project "${startup_project}" --frames 1 --size 64x64
        --render-out "${OUT_DIR}/frames/non_strict.png"
    RESULT_VARIABLE player_result
    OUTPUT_VARIABLE player_stdout
    ERROR_VARIABLE player_stderr
)
if(NOT player_result EQUAL 0 OR NOT EXISTS "${OUT_DIR}/frames/non_strict.png")
    message(FATAL_ERROR "non-strict startup verification stopped project loading\n${player_stdout}\n${player_stderr}")
endif()
string(CONCAT player_output "${player_stdout}" "${player_stderr}")
if(NOT player_output MATCHES "assets manifest: 1 stores,.*1 info, 0 warnings, 0 errors")
    message(FATAL_ERROR "startup summary is missing or misclassified\n${player_output}")
endif()

execute_process(
    COMMAND "${PLAYER}" --headless --project "${startup_project}" --frames 1 --size 64x64
        --strict-assets
    RESULT_VARIABLE strict_result
    OUTPUT_VARIABLE strict_stdout
    ERROR_VARIABLE strict_stderr
)
if(strict_result EQUAL 0)
    message(FATAL_ERROR "--strict-assets unexpectedly allowed a content mismatch")
endif()
string(CONCAT strict_output "${strict_stdout}" "${strict_stderr}")
if(NOT strict_output MATCHES "ERROR")
    message(FATAL_ERROR "strict startup did not report ERROR severity\n${strict_output}")
endif()
