if(NOT DEFINED CLI OR NOT DEFINED WRITER OR NOT DEFINED OUT_DIR)
    message(FATAL_ERROR "CLI, WRITER, and OUT_DIR are required")
endif()

file(REMOVE_RECURSE "${OUT_DIR}")
file(MAKE_DIRECTORY "${OUT_DIR}/project/levels" "${OUT_DIR}/project/ui")
file(WRITE "${OUT_DIR}/project/project.json" [=[
{
  "schema": "pelican.project",
  "version": 1,
  "name": "WP84 rules fixture",
  "engine_min_version": "0.1.0",
  "basic_config": {"asset_data_json": "assets.json"}
}
]=])
file(WRITE "${OUT_DIR}/project/assets.json"
     "{\"schema\":\"pelican.asset_data\",\"version\":1,\"models\":[]}\n")
file(WRITE "${OUT_DIR}/project/notes.txt" "unmatched\n")
file(WRITE "${OUT_DIR}/project/ui/missing.psd" "not opened because the external tool is missing\n")
file(WRITE "${OUT_DIR}/project/imports.rules.json" [=[
{
  "schema": "pelican.import_rules",
  "version": 1,
  "rules": [
    {"match": "levels/*.glb", "recipe": "extract_scene"},
    {"match": "ui/*.psd", "recipe": "psd_layers"}
  ],
  "defaults": {".png": null}
}
]=])

execute_process(COMMAND "${WRITER}" "${OUT_DIR}/project/levels/fixture.glb" RESULT_VARIABLE writer_result)
if(NOT writer_result EQUAL 0)
    message(FATAL_ERROR "failed to create WP84 glTF fixture")
endif()

# An unmatched source is the zero-work path: no stdout and not even imports/ creation.
execute_process(
    COMMAND "${CLI}" import --rules imports.rules.json --project "${OUT_DIR}/project" --source notes.txt
    RESULT_VARIABLE unmatched_result
    OUTPUT_VARIABLE unmatched_stdout
    ERROR_VARIABLE unmatched_stderr
)
if(NOT unmatched_result EQUAL 0 OR NOT unmatched_stdout STREQUAL "" OR NOT unmatched_stderr STREQUAL "")
    message(FATAL_ERROR "unmatched input was not silent\nstdout:\n${unmatched_stdout}\nstderr:\n${unmatched_stderr}")
endif()
if(EXISTS "${OUT_DIR}/project/imports")
    message(FATAL_ERROR "unmatched input created imports/; zero-work contract was broken")
endif()

set(GLB_DELIVERY "${OUT_DIR}/project/imports/extract_scene/levels/fixture")
execute_process(
    COMMAND "${CLI}" import --rules imports.rules.json --project "${OUT_DIR}/project" --source levels/fixture.glb
    RESULT_VARIABLE first_result
    OUTPUT_VARIABLE first_stdout
    ERROR_VARIABLE first_stderr
)
if(NOT first_result EQUAL 0)
    message(FATAL_ERROR "extract_scene rules import failed\n${first_stdout}\n${first_stderr}")
endif()
foreach(required "${GLB_DELIVERY}/scene.json" "${GLB_DELIVERY}/manifest.json")
    if(NOT EXISTS "${required}")
        message(FATAL_ERROR "rules import did not create ${required}")
    endif()
endforeach()
file(SHA256 "${GLB_DELIVERY}/scene.json" scene_hash)
file(SHA256 "${GLB_DELIVERY}/manifest.json" manifest_hash)

# Identical regeneration compares byte-for-byte and leaves the delivery intact without --force.
execute_process(
    COMMAND "${CLI}" import --rules imports.rules.json --project "${OUT_DIR}/project" --source levels/fixture.glb
    RESULT_VARIABLE second_result
    ERROR_VARIABLE second_stderr
)
if(NOT second_result EQUAL 0)
    message(FATAL_ERROR "idempotent rules import failed\n${second_stderr}")
endif()
file(SHA256 "${GLB_DELIVERY}/scene.json" scene_hash_2)
file(SHA256 "${GLB_DELIVERY}/manifest.json" manifest_hash_2)
if(NOT scene_hash STREQUAL scene_hash_2 OR NOT manifest_hash STREQUAL manifest_hash_2)
    message(FATAL_ERROR "identical input changed generated delivery bytes")
endif()

# A hand-edited generated file is protected unless --force is explicit.
file(APPEND "${GLB_DELIVERY}/scene.json" "edited\n")
execute_process(
    COMMAND "${CLI}" import --rules imports.rules.json --project "${OUT_DIR}/project" --source levels/fixture.glb
    RESULT_VARIABLE protected_result
    OUTPUT_VARIABLE protected_stdout
    ERROR_VARIABLE protected_stderr
)
if(protected_result EQUAL 0)
    message(FATAL_ERROR "edited generated output was overwritten without --force")
endif()
string(CONCAT protected_combined "${protected_stdout}" "${protected_stderr}")
if(NOT protected_combined MATCHES "--force")
    message(FATAL_ERROR "overwrite refusal did not name --force\n${protected_combined}")
endif()
execute_process(
    COMMAND "${CLI}" import --rules imports.rules.json --project "${OUT_DIR}/project" --source levels/fixture.glb --force
    RESULT_VARIABLE force_result
    ERROR_VARIABLE force_stderr
)
if(NOT force_result EQUAL 0)
    message(FATAL_ERROR "--force regeneration failed\n${force_stderr}")
endif()
file(SHA256 "${GLB_DELIVERY}/scene.json" forced_scene_hash)
file(SHA256 "${GLB_DELIVERY}/manifest.json" forced_manifest_hash)
if(NOT scene_hash STREQUAL forced_scene_hash OR NOT manifest_hash STREQUAL forced_manifest_hash)
    message(FATAL_ERROR "--force regeneration was not byte deterministic")
endif()

# Tool discovery is lazy, named, and provides an actionable uv installation path.
execute_process(
    COMMAND "${CLI}" import --rules imports.rules.json --project "${OUT_DIR}/project" --source ui/missing.psd
            --import-tools "${OUT_DIR}/does-not-exist/pelican-import-tools"
    RESULT_VARIABLE missing_result
    OUTPUT_VARIABLE missing_stdout
    ERROR_VARIABLE missing_stderr
)
if(missing_result EQUAL 0)
    message(FATAL_ERROR "missing pelican-import-tools unexpectedly succeeded")
endif()
string(CONCAT missing_combined "${missing_stdout}" "${missing_stderr}")
if(NOT missing_combined MATCHES "pelican-import-tools" OR NOT missing_combined MATCHES "uv sync")
    message(FATAL_ERROR "missing tool error lacks tool name or installation guidance\n${missing_combined}")
endif()
