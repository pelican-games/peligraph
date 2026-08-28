# Build-unit feature-contrast smoke.
#
# Run from the repository root:
#   cmake -DPELICAN_BUILD_UNIT_SMOKE_CONFIG=Debug -P test/run_build_units_smoke.cmake
#
# This script intentionally is not registered with ctest. It configures and
# builds a separate directory for every registry contrast. Every row starts
# from the registry baseline and may change only its target plus dependencies
# declared by that registry.

if(NOT DEFINED PELICAN_BUILD_UNIT_SMOKE_CONFIG)
    set(PELICAN_BUILD_UNIT_SMOKE_CONFIG Debug)
endif()

get_filename_component(SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
include("${SOURCE_DIR}/cmake/pelican_feature_registry.cmake")
pelican_verify_feature_ledger("${SOURCE_DIR}/docs/implementation_plan.md")
set(ARTIFACT_ROOT "${SOURCE_DIR}/build-unit-smoke-artifacts")

if(WIN32)
    set(EXE_SUFFIX ".exe")
else()
    set(EXE_SUFFIX "")
endif()
if(NOT CMAKE_CTEST_COMMAND)
    get_filename_component(cmake_bin_dir "${CMAKE_COMMAND}" DIRECTORY)
    set(CMAKE_CTEST_COMMAND "${cmake_bin_dir}/ctest${EXE_SUFFIX}")
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
    if(ARGC GREATER 3)
        message(FATAL_ERROR
            "pelican.feature_registry.smoke_manual_override@1: ${option_name}")
    endif()

    pelican_feature_contrast_arguments("${option_name}" feature_arguments)
    set(forwarded_arguments)
    if(DEFINED CMAKE_PREFIX_PATH AND NOT "${CMAKE_PREFIX_PATH}" STREQUAL "")
        list(APPEND forwarded_arguments
            "-DCMAKE_PREFIX_PATH=${CMAKE_PREFIX_PATH}")
    endif()
    if(DEFINED Python3_EXECUTABLE AND
       NOT "${Python3_EXECUTABLE}" STREQUAL "")
        list(APPEND forwarded_arguments
            "-DPython3_EXECUTABLE=${Python3_EXECUTABLE}")
    endif()

    set(build_dir "${SOURCE_DIR}/build-feature-${label}")
    file(REMOVE_RECURSE "${build_dir}")

    run_process(
        "configure_${label}"
        TRUE
        "${CMAKE_COMMAND}"
            -S "${SOURCE_DIR}"
            -B "${build_dir}"
            -DBUILD_TESTING=ON
            -DPELICAN_PYTHON_TESTS=OFF
            "-DCMAKE_BUILD_TYPE=${PELICAN_BUILD_UNIT_SMOKE_CONFIG}"
            ${feature_arguments}
            ${forwarded_arguments}
    )

    run_process(
        "build_${label}"
        TRUE
        "${CMAKE_COMMAND}"
            --build "${build_dir}"
            --config "${PELICAN_BUILD_UNIT_SMOKE_CONFIG}"
            --parallel
    )

    set("${out_build_dir}" "${build_dir}" PARENT_SCOPE)
endfunction()

function(clean_successful_build build_dir)
    if(PELICAN_BUILD_UNIT_SMOKE_CLEAN_BUILDS)
        file(REMOVE_RECURSE "${build_dir}")
    endif()
endfunction()

function(verify_physics_sources_absent build_dir label)
    set(forbidden_sources ${ARGN})
    file(GLOB_RECURSE build_metadata LIST_DIRECTORIES false
        "${build_dir}/build.ninja"
        "${build_dir}/*.vcxproj"
        "${build_dir}/*DependInfo.cmake"
        "${build_dir}/compile_commands.json"
    )
    foreach(metadata IN LISTS build_metadata)
        file(READ "${metadata}" contents)
        foreach(source IN LISTS forbidden_sources)
            string(FIND "${contents}" "${source}" source_index)
            if(NOT source_index EQUAL -1)
                message(FATAL_ERROR "${label} still compiles ${source}: ${metadata}")
            endif()
        endforeach()
    endforeach()

    file(GLOB_RECURSE artifacts LIST_DIRECTORIES false "${build_dir}/*")
    foreach(artifact IN LISTS artifacts)
        get_filename_component(name "${artifact}" NAME)
        string(TOLOWER "${name}" lower_name)
        foreach(source IN LISTS forbidden_sources)
            get_filename_component(stem "${source}" NAME_WE)
            string(TOLOWER "${stem}" lower_stem)
            if(lower_name MATCHES "^${lower_stem}[.](obj|o)$")
                message(FATAL_ERROR "${label} emitted forbidden physics object: ${artifact}")
            endif()
        endforeach()
    endforeach()
endfunction()

function(verify_builtin_physics_absent build_dir)
    verify_physics_sources_absent(
        "${build_dir}"
        "PELICAN_WITH_BUILTIN_PHYSICS=OFF"
        physquery.cpp
        physqueryaggregate.cpp
        builtinphysicsprovider.cpp
    )
endfunction()

function(verify_jolt_physics_absent build_dir label)
    foreach(path IN ITEMS
            "${build_dir}/_deps/joltphysics-src"
            "${build_dir}/_deps/joltphysics-build"
            "${build_dir}/_deps/joltphysics-subbuild")
        if(EXISTS "${path}")
            message(FATAL_ERROR "${label} unexpectedly fetched or configured Jolt: ${path}")
        endif()
    endforeach()
    verify_physics_sources_absent(
        "${build_dir}"
        "${label}"
        joltphysicsprovider.cpp
    )
endfunction()

function(verify_physics_absent build_dir)
    verify_physics_sources_absent(
        "${build_dir}"
        "PELICAN_WITH_PHYSICS=OFF"
        physquery.cpp
        physqueryaggregate.cpp
        physquerycontract.cpp
        builtinphysicsprovider.cpp
        joltphysicsprovider.cpp
        physicsruntime.cpp
        physicsservice.cpp
        physworld.cpp
    )
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

function(verify_openxr_absent build_dir)
    foreach(path IN ITEMS
            "${build_dir}/_deps/openxr_sdk-src"
            "${build_dir}/_deps/openxr_sdk-build"
            "${build_dir}/_deps/openxr_sdk-subbuild")
        if(EXISTS "${path}")
            message(FATAL_ERROR "PELICAN_WITH_OPENXR=OFF unexpectedly fetched or configured OpenXR-SDK: ${path}")
        endif()
    endforeach()

    file(GLOB_RECURSE build_metadata LIST_DIRECTORIES false
        "${build_dir}/build.ninja"
        "${build_dir}/*.vcxproj"
        "${build_dir}/*DependInfo.cmake"
        "${build_dir}/compile_commands.json"
    )
    foreach(metadata IN LISTS build_metadata)
        file(READ "${metadata}" contents)
        if(contents MATCHES "src[/\\\\]core[/\\\\]openxr[/\\\\]" OR
           contents MATCHES "openxrdiscovery[.]cpp")
            message(FATAL_ERROR "PELICAN_WITH_OPENXR=OFF still compiles an OpenXR source: ${metadata}")
        endif()
    endforeach()

    file(GLOB_RECURSE artifacts LIST_DIRECTORIES false "${build_dir}/*")
    foreach(artifact IN LISTS artifacts)
        get_filename_component(name "${artifact}" NAME)
        string(TOLOWER "${name}" lower_name)
        if(lower_name MATCHES "(pelican_openxr|openxr_loader|openxrdiscovery).*\\.(obj|o|lib|a|dll|so)$")
            message(FATAL_ERROR "PELICAN_WITH_OPENXR=OFF emitted an OpenXR object/library: ${artifact}")
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
        if(NOT DUMPBIN_EXECUTABLE)
            message(FATAL_ERROR "dumpbin is required for the OpenXR OFF symbol check")
        endif()

        file(GLOB_RECURSE symbol_artifacts LIST_DIRECTORIES false
            "${build_dir}/*pelican_core.lib"
            "${build_dir}/*pelican_player.exe"
        )
        foreach(symbol_artifact IN LISTS symbol_artifacts)
            execute_process(
                COMMAND "${DUMPBIN_EXECUTABLE}" /symbols "${symbol_artifact}"
                RESULT_VARIABLE dumpbin_result
                OUTPUT_VARIABLE symbols
                ERROR_VARIABLE dumpbin_error
            )
            if(NOT dumpbin_result EQUAL 0)
                message(FATAL_ERROR "dumpbin failed for ${symbol_artifact}: ${dumpbin_error}")
            endif()
            if(symbols MATCHES "queryDiscovery@OpenXr" OR
               symbols MATCHES "xr(Create|Destroy|Enumerate|Get|Poll|ResultToString)")
                message(FATAL_ERROR "PELICAN_WITH_OPENXR=OFF retained OpenXR symbols in ${symbol_artifact}")
            endif()
        endforeach()
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
  "features": ["engine://features/ui.json"],
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
    file(WRITE "${root}/assets/asset_data.json" "{\"schema\":\"pelican.asset_data\",\"version\":1,\"models\":[]}\n")
    file(WRITE "${root}/scenes/main.scene.json" [=[
{
  "schema": "pelican.scene",
  "version": 1,
  "scenes": {"default_scene": {"objects": []}}
}
]=])
    file(WRITE "${root}/ui/ui.json" "{\"schema\":\"pelican.ui\",\"version\":1,\"key\":\"empty\",\"root\":{\"id\":\"root\",\"type\":\"panel\"}}\n")
endfunction()

function(write_exr_project root)
    write_empty_project("${root}")
    file(WRITE "${root}/ui/dummy.exr" "not an exr; extension is enough for the OFF check\n")
    file(WRITE "${root}/ui/exr.atlas.json" [=[
{
  "schema": "pelican.atlas",
  "version": 1,
  "pages": [{"image": "dummy.exr", "size": [1, 1]}],
  "sprites": {"exr": {"page": 0, "rect": [0, 0, 1, 1]}}
}
]=])
    file(WRITE "${root}/ui/ui.json" [=[
{
  "schema": "pelican.ui",
  "version": 1,
  "key": "exr_smoke",
  "root": {
    "id": "root",
    "type": "panel",
    "children": [
      {"id": "exr", "type": "image", "sprite": "ui/exr.atlas.json#sprite/exr"}
    ]
  }
}
]=])
endfunction()

function(write_vat_project root)
    write_common_project("${root}")
    file(WRITE "${root}/assets/asset_data.json" [=[
{
  "schema": "pelican.asset_data",
  "version": 1,
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
    file(WRITE "${root}/ui/ui.json" "{\"schema\":\"pelican.ui\",\"version\":1,\"key\":\"empty\",\"root\":{\"id\":\"root\",\"type\":\"panel\"}}\n")
endfunction()

function(write_physics_project root)
    write_common_project("${root}")
    file(WRITE "${root}/assets/asset_data.json" "{\"schema\":\"pelican.asset_data\",\"version\":1,\"models\":[]}\n")
    file(WRITE "${root}/scenes/main.scene.json" [=[
{
  "schema": "pelican.scene",
  "version": 1,
  "scenes": {
    "default_scene": {
      "objects": [
        {
          "name": "PhysicsSmoke",
          "components": [
            {
              "name": "transform",
              "pos": [0.0, 0.0, 0.0],
              "rotation": [0.0, 0.0, 0.0, 1.0],
              "scale": [1.0, 1.0, 1.0]
            },
            {"name": "collider", "shape": "sphere", "radius": 1.0}
          ]
        }
      ]
    }
  }
}
]=])
    file(WRITE "${root}/ui/ui.json" "{\"schema\":\"pelican.ui\",\"version\":1,\"key\":\"empty\",\"root\":{\"id\":\"root\",\"type\":\"panel\"}}\n")
endfunction()

function(verify_spirv_link_absent build_dir)
    foreach(path IN ITEMS
            "${build_dir}/_deps/spirv_tools-src"
            "${build_dir}/_deps/spirv_tools-build"
            "${build_dir}/_deps/spirv_tools-subbuild")
        if(EXISTS "${path}")
            message(FATAL_ERROR
                "PELICAN_WITH_SPIRV_LINK=OFF unexpectedly configured SPIRV-Tools: ${path}")
        endif()
    endforeach()

    file(GLOB_RECURSE build_metadata LIST_DIRECTORIES false
        "${build_dir}/build.ninja"
        "${build_dir}/*.vcxproj"
        "${build_dir}/*DependInfo.cmake"
        "${build_dir}/compile_commands.json"
    )
    foreach(metadata IN LISTS build_metadata)
        file(READ "${metadata}" contents)
        if(contents MATCHES
           "src[/\\\\]core[/\\\\]shader[/\\\\]spvlink[.]cpp|src[/\\\\]spvlink[/\\\\]main[.]cpp|SPIRV-Tools-link")
            message(FATAL_ERROR
                "PELICAN_WITH_SPIRV_LINK=OFF retained linker input: ${metadata}")
        endif()
    endforeach()

    file(GLOB_RECURSE artifacts LIST_DIRECTORIES false "${build_dir}/*")
    foreach(artifact IN LISTS artifacts)
        get_filename_component(name "${artifact}" NAME)
        string(TOLOWER "${name}" lower_name)
        if(lower_name MATCHES "^pelican-spv-link([.]exe)?$")
            message(FATAL_ERROR
                "PELICAN_WITH_SPIRV_LINK=OFF emitted linker executable: ${artifact}")
        endif()
    endforeach()
endfunction()

function(verify_devstudio_absent build_dir)
    file(GLOB_RECURSE build_metadata LIST_DIRECTORIES false
        "${build_dir}/build.ninja"
        "${build_dir}/*.vcxproj"
        "${build_dir}/*DependInfo.cmake"
        "${build_dir}/compile_commands.json"
    )
    foreach(metadata IN LISTS build_metadata)
        file(READ "${metadata}" contents)
        if(contents MATCHES "src[/\\\\]devstudio[/\\\\]")
            message(FATAL_ERROR
                "SKIP_DEVSTUDIO=ON retained a Studio source: ${metadata}")
        endif()
    endforeach()

    file(GLOB_RECURSE artifacts LIST_DIRECTORIES false "${build_dir}/*")
    foreach(artifact IN LISTS artifacts)
        get_filename_component(name "${artifact}" NAME)
        string(TOLOWER "${name}" lower_name)
        if(lower_name MATCHES "^pelican_studio([.](exe|lib|a))?$")
            message(FATAL_ERROR
                "SKIP_DEVSTUDIO=ON emitted a Studio artifact: ${artifact}")
        endif()
    endforeach()
endfunction()

function(run_runtime_shader_compiler_smoke)
    configure_and_build(
        "runtime_shader_compiler"
        "PELICAN_RUNTIME_SHADER_COMPILER"
        runtime_shader_compiler_build_dir)
    run_process(
        "runtime_shader_compiler_wp354_discovery"
        TRUE
        "${CMAKE_CTEST_COMMAND}"
            --test-dir "${runtime_shader_compiler_build_dir}"
            --build-config "${PELICAN_BUILD_UNIT_SMOKE_CONFIG}"
            --output-on-failure
            -R "^WP354 production config and provider flow through renderer model and Open action$"
    )
    if(NOT runtime_shader_compiler_wp354_discovery_STDOUT MATCHES
       "100% tests passed, 0 tests failed out of 1")
        message(FATAL_ERROR
            "compiler-OFF did not discover exactly one WP354 production test\n"
            "stdout:\n${runtime_shader_compiler_wp354_discovery_STDOUT}\n"
            "stderr:\n${runtime_shader_compiler_wp354_discovery_STDERR}")
    endif()
    clean_successful_build("${runtime_shader_compiler_build_dir}")
endfunction()

function(run_spirv_link_smoke)
    configure_and_build(
        "spirv_link"
        "PELICAN_WITH_SPIRV_LINK"
        spirv_link_build_dir)
    verify_spirv_link_absent("${spirv_link_build_dir}")
    clean_successful_build("${spirv_link_build_dir}")
endfunction()

function(run_skip_devstudio_smoke)
    configure_and_build(
        "skip_devstudio"
        "SKIP_DEVSTUDIO"
        skip_devstudio_build_dir)
    verify_devstudio_absent("${skip_devstudio_build_dir}")
    clean_successful_build("${skip_devstudio_build_dir}")
endfunction()

function(run_openxr_smoke)
    configure_and_build("openxr" "PELICAN_WITH_OPENXR" openxr_build_dir)
    verify_openxr_absent("${openxr_build_dir}")
    find_built_executable("${openxr_build_dir}" "pelican_player" openxr_player)
    expect_disabled_error(
        "openxr_cli"
        "PELICAN_WITH_OPENXR"
        "${openxr_player}" --xr on
    )
    clean_successful_build("${openxr_build_dir}")
endfunction()

function(verify_renderdoc_absent build_dir)
    file(GLOB_RECURSE build_metadata LIST_DIRECTORIES false
        "${build_dir}/build.ninja"
        "${build_dir}/*.vcxproj"
        "${build_dir}/*DependInfo.cmake"
        "${build_dir}/compile_commands.json"
    )
    foreach(metadata IN LISTS build_metadata)
        file(READ "${metadata}" contents)
        foreach(forbidden IN ITEMS "renderdoccapture.cpp" "renderdoc_app.h")
            string(FIND "${contents}" "${forbidden}" found_at)
            if(NOT found_at EQUAL -1)
                message(FATAL_ERROR "PELICAN_WITH_RENDERDOC=OFF retained ${forbidden}: ${metadata}")
            endif()
        endforeach()
    endforeach()

    file(GLOB_RECURSE artifacts LIST_DIRECTORIES false "${build_dir}/*")
    foreach(artifact IN LISTS artifacts)
        get_filename_component(name "${artifact}" NAME)
        string(TOLOWER "${name}" lower_name)
        if(lower_name MATCHES "^renderdoccapture[.](obj|o)$")
            message(FATAL_ERROR "PELICAN_WITH_RENDERDOC=OFF emitted integration object: ${artifact}")
        endif()
    endforeach()
endfunction()

function(run_renderdoc_smoke)
    configure_and_build("renderdoc" "PELICAN_WITH_RENDERDOC" renderdoc_build_dir)
    verify_renderdoc_absent("${renderdoc_build_dir}")
    clean_successful_build("${renderdoc_build_dir}")
endfunction()

function(verify_standard_render_algorithms_absent build_dir)
    file(GLOB_RECURSE build_metadata LIST_DIRECTORIES false
        "${build_dir}/build.ninja"
        "${build_dir}/*.vcxproj"
        "${build_dir}/*DependInfo.cmake"
        "${build_dir}/compile_commands.json"
    )
    foreach(metadata IN LISTS build_metadata)
        file(READ "${metadata}" contents)
        if(contents MATCHES
            "render_algorithms[/\\\\](standardrenderalgorithms[.]cpp|cube_capture[/\\\\](cubecaptureview[.]cpp|cubecaptureviewprovider[.]cpp)|planar_reflection[/\\\\](standard_prefilter[.]comp|planarreflectionview[.]cpp|planarreflectionviewprovider[.]cpp))")
            message(FATAL_ERROR
                "PELICAN_WITH_STANDARD_RENDER_ALGORITHMS=OFF retained a standard algorithm source: ${metadata}")
        endif()
    endforeach()

    file(GLOB_RECURSE artifacts LIST_DIRECTORIES false "${build_dir}/*")
    foreach(artifact IN LISTS artifacts)
        get_filename_component(name "${artifact}" NAME)
        string(TOLOWER "${name}" lower_name)
        if(lower_name MATCHES
            "(standard_prefilter|standardrenderalgorithms|cubecaptureview|cubecaptureviewprovider|planarreflectionview|planarreflectionviewprovider).*[.](obj|o|lib|a)$")
            message(FATAL_ERROR
                "PELICAN_WITH_STANDARD_RENDER_ALGORITHMS=OFF emitted a standard algorithm artifact: ${artifact}")
        endif()
    endforeach()
endfunction()

function(run_standard_render_algorithms_smoke)
    configure_and_build(
        "standard_render_algorithms"
        "PELICAN_WITH_STANDARD_RENDER_ALGORITHMS"
        standard_render_algorithms_build_dir
    )
    verify_standard_render_algorithms_absent(
        "${standard_render_algorithms_build_dir}")
    find_built_executable(
        "${standard_render_algorithms_build_dir}"
        "pelican_test_pathresolver_test"
        standard_render_algorithms_probe
    )
    run_process(
        "standard_render_algorithm_registry"
        TRUE
        "${standard_render_algorithms_probe}"
        "[render-algorithm]"
    )
    find_built_executable(
        "${standard_render_algorithms_build_dir}"
        "pelican_test_viewfamily_test"
        standard_render_algorithms_family_probe
    )
    run_process(
        "standard_render_algorithm_provider_registry"
        TRUE
        "${standard_render_algorithms_family_probe}"
        "[render-algorithm]"
    )
    clean_successful_build("${standard_render_algorithms_build_dir}")
endfunction()

function(run_audio_smoke)
    configure_and_build("audio" "PELICAN_WITH_AUDIO" audio_build_dir)
    find_built_executable("${audio_build_dir}" "pelican_test_audio_disabled_probe" audio_probe)
    expect_disabled_error(
        "audio_gamecontext_api"
        "PELICAN_WITH_AUDIO"
        "${audio_probe}"
    )
    clean_successful_build("${audio_build_dir}")
endfunction()

function(run_vat_smoke)
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
    clean_successful_build("${vat_build_dir}")
endfunction()

function(run_exr_smoke)
    configure_and_build("exr" "PELICAN_WITH_EXR" exr_build_dir)
    find_built_executable("${exr_build_dir}" "pelican_player" exr_player)
    set(exr_project "${ARTIFACT_ROOT}/exr_project")
    write_exr_project("${exr_project}")
    expect_disabled_error(
        "exr_ui_reference"
        "PELICAN_WITH_EXR"
        "${exr_player}" --headless --project "${exr_project}" --frames 1 --size 64x64
    )
    clean_successful_build("${exr_build_dir}")
endfunction()

function(run_rpc_smoke)
    configure_and_build("rpc" "PELICAN_WITH_RPC" rpc_build_dir)
    find_built_executable("${rpc_build_dir}" "pelican_player" rpc_player)
    expect_disabled_error(
        "rpc_cli"
        "PELICAN_WITH_RPC"
        "${rpc_player}" --rpc --headless
    )
    clean_successful_build("${rpc_build_dir}")
endfunction()

function(run_seqplayer_smoke)
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
    clean_successful_build("${seq_build_dir}")
endfunction()

function(run_imgui_smoke)
    configure_and_build("imgui" "PELICAN_WITH_IMGUI" imgui_build_dir)
    verify_imgui_absent("${imgui_build_dir}")
    clean_successful_build("${imgui_build_dir}")
endfunction()

function(run_builtin_physics_smoke)
    configure_and_build(
        "physics_provider_only"
        "PELICAN_WITH_BUILTIN_PHYSICS"
        physics_provider_build_dir)
    verify_builtin_physics_absent("${physics_provider_build_dir}")
    verify_jolt_physics_absent("${physics_provider_build_dir}" "provider-only physics")
    find_built_executable("${physics_provider_build_dir}" "pelican_test_physicsservice_test" physics_provider_test)
    run_process("physics_provider_only_service" TRUE "${physics_provider_test}")
    run_process(
        "physics_provider_only_game_dll"
        TRUE
        "${CMAKE_CTEST_COMMAND}"
            --test-dir "${physics_provider_build_dir}"
            --build-config "${PELICAN_BUILD_UNIT_SMOKE_CONFIG}"
            --output-on-failure
            -R "^physics_provider_game_dll_e2e$"
    )
    clean_successful_build("${physics_provider_build_dir}")
endfunction()

function(run_physics_smoke)
    configure_and_build("physics" "PELICAN_WITH_PHYSICS" physics_build_dir)
    verify_physics_absent("${physics_build_dir}")
    verify_jolt_physics_absent("${physics_build_dir}" "PELICAN_WITH_PHYSICS=OFF")
    find_built_executable("${physics_build_dir}" "pelican_test_physics_feature_probe" physics_probe)
    run_process("physics_api_stub" TRUE "${physics_probe}")
    find_built_executable("${physics_build_dir}" "pelican_player" physics_player)
    set(physics_project "${ARTIFACT_ROOT}/physics_project")
    write_physics_project("${physics_project}")
    expect_disabled_error(
        "physics_scene_collider"
        "PELICAN_WITH_PHYSICS"
        "${physics_player}" --headless --project "${physics_project}" --frames 1 --size 64x64
    )
    clean_successful_build("${physics_build_dir}")
endfunction()

function(run_jolt_physics_smoke)
    configure_and_build(
        "physics_jolt"
        "PELICAN_WITH_JOLT_PHYSICS"
        physics_jolt_build_dir)
    verify_builtin_physics_absent("${physics_jolt_build_dir}")
    find_built_executable("${physics_jolt_build_dir}" "pelican_test_physicsservice_test" physics_jolt_test)
    run_process("physics_jolt_service" TRUE "${physics_jolt_test}")
    find_built_executable("${physics_jolt_build_dir}" "pelican_test_physics_feature_probe" physics_jolt_probe)
    run_process("physics_jolt_api" TRUE "${physics_jolt_probe}")
    run_process(
        "physics_jolt_game_dll"
        TRUE
        "${CMAKE_CTEST_COMMAND}"
            --test-dir "${physics_jolt_build_dir}"
            --build-config "${PELICAN_BUILD_UNIT_SMOKE_CONFIG}"
            --output-on-failure
            -R "^physics_provider_game_dll_e2e$"
    )
    clean_successful_build("${physics_jolt_build_dir}")
endfunction()

function(run_registered_feature_smoke feature_name)
    set(dry_run FALSE)
    if(PELICAN_BUILD_UNIT_SMOKE_PARSE_ONLY)
        set(dry_run TRUE)
    endif()

    if(feature_name STREQUAL "PELICAN_RUNTIME_SHADER_COMPILER")
        if(NOT dry_run)
            run_runtime_shader_compiler_smoke()
        endif()
    elseif(feature_name STREQUAL "PELICAN_WITH_SPIRV_LINK")
        if(NOT dry_run)
            run_spirv_link_smoke()
        endif()
    elseif(feature_name STREQUAL "PELICAN_WITH_AUDIO")
        if(NOT dry_run)
            run_audio_smoke()
        endif()
    elseif(feature_name STREQUAL "PELICAN_WITH_VAT")
        if(NOT dry_run)
            run_vat_smoke()
        endif()
    elseif(feature_name STREQUAL "PELICAN_WITH_EXR")
        if(NOT dry_run)
            run_exr_smoke()
        endif()
    elseif(feature_name STREQUAL "PELICAN_WITH_RPC")
        if(NOT dry_run)
            run_rpc_smoke()
        endif()
    elseif(feature_name STREQUAL "PELICAN_WITH_SEQPLAYER")
        if(NOT dry_run)
            run_seqplayer_smoke()
        endif()
    elseif(feature_name STREQUAL "PELICAN_WITH_IMGUI")
        if(NOT dry_run)
            run_imgui_smoke()
        endif()
    elseif(feature_name STREQUAL "PELICAN_WITH_PHYSICS")
        if(NOT dry_run)
            run_physics_smoke()
        endif()
    elseif(feature_name STREQUAL "PELICAN_WITH_OPENXR")
        if(NOT dry_run)
            run_openxr_smoke()
        endif()
    elseif(feature_name STREQUAL "PELICAN_WITH_RENDERDOC")
        if(NOT dry_run)
            run_renderdoc_smoke()
        endif()
    elseif(feature_name STREQUAL "PELICAN_WITH_STANDARD_RENDER_ALGORITHMS")
        if(NOT dry_run)
            run_standard_render_algorithms_smoke()
        endif()
    elseif(feature_name STREQUAL "PELICAN_WITH_JOLT_PHYSICS")
        if(NOT dry_run)
            run_jolt_physics_smoke()
        endif()
    elseif(feature_name STREQUAL "PELICAN_WITH_BUILTIN_PHYSICS")
        if(NOT dry_run)
            run_builtin_physics_smoke()
        endif()
    elseif(feature_name STREQUAL "SKIP_DEVSTUDIO")
        if(NOT dry_run)
            run_skip_devstudio_smoke()
        endif()
    else()
        message(FATAL_ERROR
            "pelican.feature_registry.smoke_handler_missing@1: ${feature_name}")
    endif()
endfunction()

if(PELICAN_BUILD_UNIT_SMOKE_PARSE_ONLY)
    foreach(feature_name IN LISTS PELICAN_FEATURE_REGISTRY_NAMES)
        pelican_feature_contrast_arguments("${feature_name}" contrast_arguments)
        run_registered_feature_smoke("${feature_name}")
        message(STATUS
            "validated registry smoke row: ${feature_name} "
            "(${PELICAN_FEATURE_${feature_name}_SMOKE_ID})")
    endforeach()
    message(STATUS "build-unit feature contrast matrix parsed successfully")
    return()
endif()

file(REMOVE_RECURSE "${ARTIFACT_ROOT}")
file(MAKE_DIRECTORY "${ARTIFACT_ROOT}")

if(DEFINED PELICAN_BUILD_UNIT_SMOKE_ONLY)
    string(TOLOWER "${PELICAN_BUILD_UNIT_SMOKE_ONLY}" smoke_only)
    pelican_feature_from_smoke_id("${smoke_only}" selected_feature)
    run_registered_feature_smoke("${selected_feature}")
    message(STATUS
        "feature contrast smoke passed for ${selected_feature} (${smoke_only})")
    return()
endif()

foreach(feature_name IN LISTS PELICAN_FEATURE_REGISTRY_NAMES)
    run_registered_feature_smoke("${feature_name}")
endforeach()

message(STATUS "build-unit feature contrast smoke passed for the registry matrix")
