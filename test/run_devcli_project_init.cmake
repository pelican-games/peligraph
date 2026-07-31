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
file(MAKE_DIRECTORY "${OUT_DIR}")
set(project_dir "${OUT_DIR}/new_project")

execute_process(
    COMMAND "${CLI}" project init "${project_dir}"
    RESULT_VARIABLE init_result
    OUTPUT_VARIABLE init_stdout
    ERROR_VARIABLE init_stderr
)
if(NOT init_result EQUAL 0)
    message(FATAL_ERROR "project init failed with ${init_result}\nstdout:\n${init_stdout}\nstderr:\n${init_stderr}")
endif()

foreach(path IN ITEMS
    project.json
    scenes/main.scene.json
    assets/asset_data.json
    assets/.gitkeep
    input/actions.json
    input/profiles/keyboard.json
    passes/main_rendering_config.json
    ui/ui_overlay.json
    code/CMakeLists.txt
    code/game.cpp
    .gitattributes
    .gitignore
    README.md
)
    if(NOT EXISTS "${project_dir}/${path}")
        message(FATAL_ERROR "project init did not create ${path}")
    endif()
endforeach()

file(READ "${project_dir}/passes/main_rendering_config.json" rendering_config)
string(JSON generated_preset ERROR_VARIABLE preset_error
    GET "${rendering_config}" pipeline preset)
if(NOT preset_error STREQUAL "NOTFOUND" OR
   NOT generated_preset STREQUAL "engine://render_pipelines/hybrid_v1.json")
    message(FATAL_ERROR
        "generated rendering config does not select hybrid_v1\n${rendering_config}")
endif()
string(JSON generated_feature_count ERROR_VARIABLE feature_count_error
    LENGTH "${rendering_config}" features)
string(JSON generated_feature ERROR_VARIABLE feature_error
    GET "${rendering_config}" features 0)
if(NOT feature_count_error STREQUAL "NOTFOUND" OR
   NOT feature_error STREQUAL "NOTFOUND" OR
   NOT generated_feature_count EQUAL 1 OR
   NOT generated_feature STREQUAL "engine://features/shadow_directional.json")
    message(FATAL_ERROR
        "generated rendering config does not select the default feature set\n${rendering_config}")
endif()
foreach(forbidden_member IN ITEMS render_targets rendering_passes)
    string(JSON ignored ERROR_VARIABLE member_error
        TYPE "${rendering_config}" "${forbidden_member}")
    if(member_error STREQUAL "NOTFOUND")
        message(FATAL_ERROR
            "generated preset config unexpectedly owns ${forbidden_member}\n${rendering_config}")
    endif()
endforeach()

file(READ "${project_dir}/.gitattributes" gitattributes)
foreach(line IN ITEMS "*.glb -text" "*.vrm -text" "*.png -text" "*.wav -text" "*.spv -text")
    string(FIND "${gitattributes}" "${line}" found_at)
    if(found_at EQUAL -1)
        message(FATAL_ERROR ".gitattributes missing ${line}\n${gitattributes}")
    endif()
endforeach()
string(FIND "${gitattributes}" "# *.glb filter=lfs diff=lfs merge=lfs -text" lfs_found_at)
if(lfs_found_at EQUAL -1)
    message(FATAL_ERROR ".gitattributes missing commented LFS track lines\n${gitattributes}")
endif()

file(READ "${project_dir}/.gitignore" gitignore)
foreach(line IN ITEMS ".pelican/" "build/")
    string(FIND "${gitignore}" "${line}" found_at)
    if(found_at EQUAL -1)
        message(FATAL_ERROR ".gitignore missing ${line}\n${gitignore}")
    endif()
endforeach()

file(READ "${project_dir}/README.md" readme)
string(FIND "${readme}" "Place model files under `assets/models/`" readme_found_at)
if(readme_found_at EQUAL -1)
    message(FATAL_ERROR "README does not describe first asset placement steps\n${readme}")
endif()

file(MAKE_DIRECTORY "${OUT_DIR}/non_empty")
file(WRITE "${OUT_DIR}/non_empty/existing.txt" "keep\n")
execute_process(
    COMMAND "${CLI}" project init "${OUT_DIR}/non_empty"
    RESULT_VARIABLE reject_result
    OUTPUT_VARIABLE reject_stdout
    ERROR_VARIABLE reject_stderr
)
if(reject_result EQUAL 0)
    message(FATAL_ERROR "project init unexpectedly accepted a non-empty directory")
endif()
string(CONCAT reject_output "${reject_stdout}" "${reject_stderr}")
string(FIND "${reject_output}" "non_empty" reject_name_found_at)
if(reject_name_found_at EQUAL -1)
    message(FATAL_ERROR "non-empty directory error did not name the directory\n${reject_output}")
endif()

file(MAKE_DIRECTORY "${OUT_DIR}/frames")
execute_process(
    COMMAND "${PLAYER}"
        --headless
        --project "${project_dir}"
        --frames 2
        --size 160x90
        --fps 30
        --render-out "${OUT_DIR}/frames/init.png"
    RESULT_VARIABLE player_result
    OUTPUT_VARIABLE player_stdout
    ERROR_VARIABLE player_stderr
)
if(NOT player_result EQUAL 0)
    message(FATAL_ERROR "generated project failed in pelican_player with ${player_result}\nstdout:\n${player_stdout}\nstderr:\n${player_stderr}")
endif()
if(player_stdout MATCHES "Validation Error|VUID-" OR player_stderr MATCHES "Validation Error|VUID-")
    message(FATAL_ERROR "generated project emitted Vulkan validation errors\nstdout:\n${player_stdout}\nstderr:\n${player_stderr}")
endif()
if(NOT EXISTS "${OUT_DIR}/frames/init.png")
    message(FATAL_ERROR "generated project did not render an output PNG")
endif()
file(SIZE "${OUT_DIR}/frames/init.png" png_size)
if(png_size EQUAL 0)
    message(FATAL_ERROR "generated project output PNG is empty")
endif()
