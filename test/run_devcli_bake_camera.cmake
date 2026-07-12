if(NOT DEFINED CLI)
    message(FATAL_ERROR "CLI is required")
endif()
if(NOT DEFINED SETUP_PLAYER)
    message(FATAL_ERROR "SETUP_PLAYER is required")
endif()

set(camera_player "${PLAYER}")
set(PLAYER "${SETUP_PLAYER}")
include("${CMAKE_CURRENT_LIST_DIR}/run_input_record_replay_headless.cmake")
set(PLAYER "${camera_player}")

set(scene_path "${OUT_DIR}/project/scenes/main.scene.json")
file(READ "${scene_path}" scene)
string(REPLACE
    [=[{"name": "camera"}]=]
    [=[{"name": "camera", "controller": {"type": "fly", "speed": 3.0, "sensitivity": 1.0}}]=]
    scene "${scene}")
file(WRITE "${scene_path}" "${scene}")

function(run_bake name sequence_hex_var)
    execute_process(
        COMMAND "${CLI}" bake-camera
            --replay "${recording_path}"
            --project "${OUT_DIR}/project"
            --player "${PLAYER}"
            --name "${name}"
        WORKING_DIRECTORY "${PLAYER_DIR}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE stdout
        ERROR_VARIABLE stderr
    )
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "bake-camera ${name} failed: ${result}\nstdout:\n${stdout}\nstderr:\n${stderr}")
    endif()
    set(delivery "${OUT_DIR}/project/imports/pelican-camera/${name}")
    set(sequence "${delivery}/camera.transform_seq.jsonl")
    set(manifest "${delivery}/manifest.json")
    if(NOT EXISTS "${sequence}" OR NOT EXISTS "${manifest}")
        message(FATAL_ERROR "bake-camera ${name} did not create transform_seq + manifest")
    endif()

    file(READ "${sequence}" sequence_text)
    if(NOT sequence_text MATCHES [=["schema":"pelican.transform_seq"]=] OR
       NOT sequence_text MATCHES [=["version":1]=] OR
       NOT sequence_text MATCHES [=["objects":\["camera"\]]=])
        message(FATAL_ERROR "camera transform_seq header is invalid:\n${sequence_text}")
    endif()
    string(REGEX MATCHALL [=["transforms":]=] transforms "${sequence_text}")
    list(LENGTH transforms transform_count)
    if(NOT transform_count EQUAL 3)
        message(FATAL_ERROR "camera transform_seq expected 3 samples, got ${transform_count}")
    endif()
    if(NOT sequence_text MATCHES [=["pos":\[0.1]=])
        message(FATAL_ERROR "camera bake did not sample replay-driven fly camera world motion:\n${sequence_text}")
    endif()

    file(READ "${manifest}" manifest_text)
    if(NOT manifest_text MATCHES [=["schema": "pelican.import"]=] OR
       NOT manifest_text MATCHES [=["schema": "pelican.transform_seq"]=] OR
       NOT manifest_text MATCHES [=["version": 1]=] OR
       NOT manifest_text MATCHES [=["sha256": "[0-9a-f]]=])
        message(FATAL_ERROR "camera bake manifest is invalid:\n${manifest_text}")
    endif()

    execute_process(
        COMMAND "${CLI}" import "${delivery}" --project "${OUT_DIR}/project"
        RESULT_VARIABLE import_result
        OUTPUT_VARIABLE import_stdout
        ERROR_VARIABLE import_stderr
    )
    if(NOT import_result EQUAL 0 OR NOT import_stdout MATCHES [=[verified 1 outputs]=])
        message(FATAL_ERROR "camera bake manifest failed pelican_cli import verification:\n${import_stdout}\n${import_stderr}")
    endif()

    file(READ "${sequence}" sequence_hex HEX)
    set(${sequence_hex_var} "${sequence_hex}" PARENT_SCOPE)
endfunction()

run_bake("wp89_first" first_sequence_hex)
run_bake("wp89_second" second_sequence_hex)
if(NOT first_sequence_hex STREQUAL second_sequence_hex)
    message(FATAL_ERROR "same replay produced different camera transform_seq bytes")
endif()
