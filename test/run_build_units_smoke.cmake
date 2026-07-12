# Build-unit OFF smoke.
#
# Run from the repository root:
#   cmake -DPELICAN_BUILD_UNIT_SMOKE_CONFIG=Debug -P test/run_build_units_smoke.cmake
#
# This script intentionally is not registered with ctest. It configures and
# builds four separate single-OFF build directories, then verifies that the
# corresponding input fails with "This binary was built with PELICAN_WITH_X=OFF".

if(NOT DEFINED PELICAN_BUILD_UNIT_SMOKE_CONFIG)
    set(PELICAN_BUILD_UNIT_SMOKE_CONFIG Debug)
endif()

get_filename_component(SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(ARTIFACT_ROOT "${SOURCE_DIR}/build-unit-smoke-artifacts")
if(WIN32 AND NOT DEFINED Python3_EXECUTABLE)
    set(Python3_EXECUTABLE "%APPDATA%/uv/python/cpython-3.12.13-windows-x86_64-none/python.exe")
endif()

if(WIN32)
    set(EXE_SUFFIX ".exe")
else()
    set(EXE_SUFFIX "")
endif()

function(run_process label expected_success)
    execute_process(
        COMMAND ${ARGN}
        WORKING_DIRECTORY "${SOURCE_DIR}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE stdout
        ERROR_VARIABLE stderr
    )

    if(expected_success)
        if(NOT result EQUAL 0)
            message(FATAL_ERROR "${label} failed with ${result}\nstdout:\n${stdout}\nstderr:\n${stderr}")
        endif()
    else()
        if(result EQUAL 0)
            message(FATAL_ERROR "${label} unexpectedly succeeded\nstdout:\n${stdout}\nstderr:\n${stderr}")
        endif()
    endif()

    set("${label}_STDOUT" "${stdout}" PARENT_SCOPE)
    set("${label}_STDERR" "${stderr}" PARENT_SCOPE)
endfunction()

function(configure_and_build label option_name out_build_dir)
    set(build_dir "${SOURCE_DIR}/build-off-${label}")
    file(REMOVE_RECURSE "${build_dir}")

    run_process(
        "configure_${label}"
        TRUE
        "${CMAKE_COMMAND}"
            -S "${SOURCE_DIR}"
            -B "${build_dir}"
            -DSKIP_DEVSTUDIO=ON
            "-D${option_name}=OFF"
            "-DCMAKE_BUILD_TYPE=${PELICAN_BUILD_UNIT_SMOKE_CONFIG}"
            "-DPython3_EXECUTABLE=${Python3_EXECUTABLE}"
    )

    run_process(
        "build_${label}"
        TRUE
        "${CMAKE_COMMAND}"
            --build "${build_dir}"
            --config "${PELICAN_BUILD_UNIT_SMOKE_CONFIG}"
    )

    set("${out_build_dir}" "${build_dir}" PARENT_SCOPE)
endfunction()

function(verify_imgui_absent build_dir)
    file(GLOB_RECURSE build_metadata LIST_DIRECTORIES false
        "${build_dir}/build.ninja"
        "${build_dir}/*.vcxproj"
        "${build_dir}/*DependInfo.cmake"
        "${build_dir}/compile_commands.json"
    )
    foreach(metadata IN LISTS build_metadata)
        file(READ "${metadata}" contents)
        if(contents MATCHES "src[/\\\\]core[/\\\\]imgui" OR
           contents MATCHES "imgui_vendor-src.*(imgui|backends)[/\\\\].*\\.cpp")
            message(FATAL_ERROR "PELICAN_WITH_IMGUI=OFF still compiles an ImGui source: ${metadata}")
        endif()
    endforeach()

    file(GLOB_RECURSE artifacts LIST_DIRECTORIES false "${build_dir}/*")
    foreach(artifact IN LISTS artifacts)
        get_filename_component(name "${artifact}" NAME)
        string(TOLOWER "${name}" lower_name)
        if(lower_name MATCHES "imgui.*\\.(obj|o|lib|a|ttf|otf)$")
            message(FATAL_ERROR "PELICAN_WITH_IMGUI=OFF emitted ImGui object/library/font asset: ${artifact}")
        endif()
    endforeach()

    if(WIN32)
        find_program(DUMPBIN_EXECUTABLE dumpbin)
        if(NOT DUMPBIN_EXECUTABLE)
            file(GLOB dumpbin_candidates
                "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Tools/MSVC/*/bin/Hostx64/x64/dumpbin.exe"
                "C:/Program Files/Microsoft Visual Studio/2022/*/VC/Tools/MSVC/*/bin/Hostx64/x64/dumpbin.exe"
            )
            if(dumpbin_candidates)
                list(SORT dumpbin_candidates ORDER DESCENDING)
                list(GET dumpbin_candidates 0 DUMPBIN_EXECUTABLE)
            endif()
        endif()
        if(DUMPBIN_EXECUTABLE)
            file(GLOB_RECURSE core_libraries LIST_DIRECTORIES false "${build_dir}/*pelican_core.lib")
            foreach(core_library IN LISTS core_libraries)
                execute_process(
                    COMMAND "${DUMPBIN_EXECUTABLE}" /symbols "${core_library}"
                    RESULT_VARIABLE dumpbin_result
                    OUTPUT_VARIABLE symbols
                    ERROR_VARIABLE dumpbin_error
                )
                if(NOT dumpbin_result EQUAL 0)
                    message(FATAL_ERROR "dumpbin failed for ${core_library}: ${dumpbin_error}")
                endif()
                if(symbols MATCHES "ImGui(::|_)" OR symbols MATCHES "imgui_impl")
                    message(FATAL_ERROR "PELICAN_WITH_IMGUI=OFF retained ImGui symbols in ${core_library}")
                endif()
            endforeach()
        endif()
    endif()
endfunction()

function(find_built_executable build_dir target_name out_path)
    file(GLOB_RECURSE candidates LIST_DIRECTORIES false "${build_dir}/*${target_name}${EXE_SUFFIX}")
    set(found "")
    foreach(candidate IN LISTS candidates)
        get_filename_component(filename "${candidate}" NAME)
        if(filename STREQUAL "${target_name}${EXE_SUFFIX}")
            set(found "${candidate}")
            break()
        endif()
    endforeach()

    if(found STREQUAL "")
        message(FATAL_ERROR "failed to find ${target_name}${EXE_SUFFIX} under ${build_dir}")
    endif()
    set("${out_path}" "${found}" PARENT_SCOPE)
endfunction()

function(expect_disabled_error label feature_name)
    execute_process(
        COMMAND ${ARGN}
        WORKING_DIRECTORY "${SOURCE_DIR}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE stdout
        ERROR_VARIABLE stderr
    )

    if(result EQUAL 0)
        message(FATAL_ERROR "${label} unexpectedly succeeded\nstdout:\n${stdout}\nstderr:\n${stderr}")
    endif()

    set(combined "${stdout}\n${stderr}")
    set(expected "This binary was built with ${feature_name}=OFF")
    if(NOT combined MATCHES "${expected}")
        message(FATAL_ERROR "${label} did not report the expected OFF error '${expected}'\nstdout:\n${stdout}\nstderr:\n${stderr}")
    endif()
endfunction()

function(write_common_project root)
    file(MAKE_DIRECTORY
        "${root}/assets"
        "${root}/passes"
        "${root}/scenes"
        "${root}/ui"
    )
    file(WRITE "${root}/project.json" [=[
{
  "schema": "pelican.project",
  "version": 1,
  "name": "build unit smoke",
  "engine_min_version": "0.1.0",
  "basic_config": {
    "window_title": "Build Unit Smoke",
    "window_size": {"width": 64, "height": 64},
    "fullscreen": false,
    "framerate": 30,
    "camera": {"yfov": 0.7853981633974483, "znear": 0.1, "zfar": 100.0, "up": [0.0, 1.0, 0.0]},
    "default_scene_id": "default_scene",
    "scene_data_json": "scenes/main.scene.json",
    "asset_data_json": "assets/asset_data.json",
    "rendering_config_json": "passes/main.json",
    "ui_config_json": "ui/ui.json",
    "default_rendering_pass": "main"
  }
}
]=])
    file(WRITE "${root}/passes/main.json" [=[
{
  "render_targets": [],
  "rendering_passes": [
    {
      "name": "main",
      "passes": [
        {
          "name": "present",
          "type": "ui",
          "output": {"color": "swapchain", "depth": null}
        }
      ]
    }
  ]
}
]=])
endfunction()

function(write_empty_project root)
    write_common_project("${root}")
    file(WRITE "${root}/assets/asset_data.json" "{\"models\":[]}\n")
    file(WRITE "${root}/scenes/main.scene.json" [=[
{
  "schema": "pelican.scene",
  "version": 1,
  "scenes": {"default_scene": {"objects": []}}
}
]=])
    file(WRITE "${root}/ui/ui.json" "{\"images\":[]}\n")
endfunction()

function(write_exr_project root)
    write_empty_project("${root}")
    file(WRITE "${root}/assets/dummy.exr" "not an exr; extension is enough for the OFF check\n")
    file(WRITE "${root}/ui/ui.json" [=[
{
  "images": [
    {"name": "exr_smoke", "file": "assets/dummy.exr"}
  ]
}
]=])
endfunction()

function(write_vat_project root)
    write_common_project("${root}")
    file(WRITE "${root}/assets/asset_data.json" [=[
{
  "models": [
    {"name": "vat_model", "path": "assets/tiny_vat.glb"}
  ]
}
]=])
    file(WRITE "${root}/scenes/main.scene.json" [=[
{
  "schema": "pelican.scene",
  "version": 1,
  "scenes": {
    "default_scene": {
      "objects": [
        {
          "name": "VatObject",
          "components": [
            {
              "name": "transform",
              "pos": [0.0, 0.0, 0.0],
              "rotation": [0.0, 0.0, 0.0, 1.0],
              "scale": [1.0, 1.0, 1.0]
            },
            {"name": "simplemodelview", "model": "vat_model"}
          ]
        }
      ]
    }
  }
}
]=])
    file(WRITE "${root}/ui/ui.json" "{\"images\":[]}\n")
endfunction()

if(PELICAN_BUILD_UNIT_SMOKE_PARSE_ONLY)
    message(STATUS "build-unit OFF smoke fixture parsed successfully")
    return()
endif()

file(REMOVE_RECURSE "${ARTIFACT_ROOT}")
file(MAKE_DIRECTORY "${ARTIFACT_ROOT}")

configure_and_build("audio" "PELICAN_WITH_AUDIO" audio_build_dir)
find_built_executable("${audio_build_dir}" "pelican_test_audio_disabled_probe" audio_probe)
expect_disabled_error(
    "audio_gamecontext_api"
    "PELICAN_WITH_AUDIO"
    "${audio_probe}"
)

configure_and_build("vat" "PELICAN_WITH_VAT" vat_build_dir)
find_built_executable("${vat_build_dir}" "pelican_player" vat_player)
find_built_executable("${vat_build_dir}" "pelican_test_vat_fixture_writer" vat_writer)
set(vat_project "${ARTIFACT_ROOT}/vat_project")
write_vat_project("${vat_project}")
run_process("write_vat_fixture" TRUE "${vat_writer}" "${vat_project}/assets/tiny_vat.glb")
expect_disabled_error(
    "vat_play_vat_cli"
    "PELICAN_WITH_VAT"
    "${vat_player}" --headless --project "${vat_project}" --frames 0 --play-vat "assets/tiny_vat.glb"
)
expect_disabled_error(
    "vat_asset_load"
    "PELICAN_WITH_VAT"
    "${vat_player}" --headless --project "${vat_project}" --frames 1 --size 64x64
)

configure_and_build("exr" "PELICAN_WITH_EXR" exr_build_dir)
find_built_executable("${exr_build_dir}" "pelican_player" exr_player)
set(exr_project "${ARTIFACT_ROOT}/exr_project")
write_exr_project("${exr_project}")
expect_disabled_error(
    "exr_ui_reference"
    "PELICAN_WITH_EXR"
    "${exr_player}" --headless --project "${exr_project}" --frames 1 --size 64x64
)

configure_and_build("rpc" "PELICAN_WITH_RPC" rpc_build_dir)
find_built_executable("${rpc_build_dir}" "pelican_player" rpc_player)
expect_disabled_error(
    "rpc_cli"
    "PELICAN_WITH_RPC"
    "${rpc_player}" --rpc --headless
)

configure_and_build("seqplayer" "PELICAN_WITH_SEQPLAYER" seq_build_dir)
find_built_executable("${seq_build_dir}" "pelican_player" seq_player)
set(seq_project "${ARTIFACT_ROOT}/seq_project")
write_empty_project("${seq_project}")
set(seq_file "${ARTIFACT_ROOT}/tiny_seq.jsonl")
file(WRITE "${seq_file}"
    "{\"schema\":\"pelican.transform_seq\",\"version\":1,\"fps\":30,\"objects\":[\"a\"]}\n"
    "{\"t\":0,\"transforms\":[{\"pos\":[0,0,0],\"rot\":[0,0,0,1],\"scale\":[1,1,1]}]}\n"
)
expect_disabled_error(
    "seqplayer_cli"
    "PELICAN_WITH_SEQPLAYER"
    "${seq_player}" --headless --project "${seq_project}" --frames 0 --play-seq "${seq_file}"
)

configure_and_build("imgui" "PELICAN_WITH_IMGUI" imgui_build_dir)
verify_imgui_absent("${imgui_build_dir}")

message(STATUS "build-unit OFF smoke passed for AUDIO, VAT, EXR, RPC, SEQPLAYER, and IMGUI")
