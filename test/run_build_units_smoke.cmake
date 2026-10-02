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
include("${SOURCE_DIR}/test/required_capability_ctest.cmake")
pelican_verify_feature_ledger("${PELICAN_FEATURE_LEDGER_DEFAULT}")
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
    get_cmake_property(smoke_variables VARIABLES)
    foreach(variable IN LISTS smoke_variables)
        if(variable MATCHES "^FETCHCONTENT_SOURCE_DIR_[A-Z0-9_-]+$" AND
           DEFINED ${variable} AND NOT "${${variable}}" STREQUAL "")
            list(APPEND forwarded_arguments
                "-D${variable}=${${variable}}")
        endif()
    endforeach()

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

function(run_rt1b_test build_dir label)
    find_built_executable(
        "${build_dir}"
        "pelican_test_raytracing_rt1b_test"
        rt1b_test)
    run_process(
        "${label}_raytracing_rt1b"
        TRUE
        "${rt1b_test}")
endfunction()

function(run_rt2b_test build_dir label)
    find_built_executable(
        "${build_dir}"
        "pelican_test_raytracing_rt2b_test"
        rt2b_test)
    run_process(
        "${label}_raytracing_rt2b"
        TRUE
        "${rt2b_test}")
    set_property(
        GLOBAL PROPERTY
        "PELICAN_WP369_RT2B_EXECUTED_${label}"
        TRUE)
endfunction()

function(require_rt2b_test_execution label)
    get_property(
        executed GLOBAL PROPERTY
        "PELICAN_WP369_RT2B_EXECUTED_${label}")
    if(NOT executed)
        message(FATAL_ERROR
            "pelican.wp369.rt2b_matrix_execution_missing@1: ${label}")
    endif()
endfunction()

function(run_rt1b_case build_dir label test_name)
    find_built_executable(
        "${build_dir}"
        "pelican_test_raytracing_rt1b_test"
        rt1b_test)
    run_process(
        "${label}_raytracing_rt1b_targeted"
        TRUE
        "${rt1b_test}"
        "${test_name}"
        -w NoAssertions)
endfunction()

# WP367 v3 item 8: these are concrete test IDs, not a claim that running the
# RT1b executable incidentally covers RT2a.  The ON rows observe the closed
# typed grammar, the shared integer CPU/GLSL predicate, canonical resolver +
# shaderc + final SPIR-V stage, paired hit-stage ABI, sparse production wire,
# and the production GPU actual/always pipelines including reload.  The OFF
# row observes only the named pre-document-load compiler gate.
function(run_rt2a_compiler_on_contrast build_dir label)
    message(STATUS
        "${label} RT2a observables: typed grammar; integer IR oracle; canonical .rahit resolver/shaderc/final-stage; all-physical paired ABI; physical-domain sparse wire; production GPU A/B+record markers+per-input negatives+SBT+reload")
    message(STATUS
        "${label} RT2a capability lane: flat/headless is the functional guarantee; XR and preview are configuration coverage only in this job")
    foreach(test_name IN ITEMS
            "WP367 typed triangle hit groups expose only optional anyhit"
            "WP367 integer predicate generator drives both CPU oracle lanes"
            "WP367 canonical anyhit resolves compiles and selects final SPIR-V stage"
            "WP366/WP367 final SPIR-V hit-pair ABI is a load-time invariant")
        run_rt1b_case("${build_dir}" "${label}_rt2a" "${test_name}")
    endforeach()
    run_process(
        "${label}_rt2a_sparse_wire"
        TRUE
        "${CMAKE_CTEST_COMMAND}"
            --test-dir "${build_dir}"
            --build-config "${PELICAN_BUILD_UNIT_SMOKE_CONFIG}"
            --output-on-failure
            --no-tests=error
            -R "^WP367 runtime wire projects sparse sorted anyhit occurrences$")
    pelican_run_required_capability_ctest(
        BUILD_DIR "${build_dir}"
        CONFIG "${PELICAN_BUILD_UNIT_SMOKE_CONFIG}"
        REGEX "^WP367 production anyhit A B proves integer predicates occurrence wire SBT and reload$"
        LABEL "${label}_rt2a_gpu_actual_always"
        REPORT_DIR "${ARTIFACT_ROOT}/required-capability/${label}"
        FUNCTIONAL_LANE "flat/headless production RT2a")
endfunction()

function(run_rt2a_compiler_off_contrast build_dir label)
    message(STATUS
        "${label} RT2a observable: layout_requires_runtime_compiler@1 before any shader document load")
    run_rt1b_case(
        "${build_dir}"
        "${label}_rt2a_off"
        "WP367 compiler contrast rejects layout before shader documents")
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

# PG-2: a game-code facade in an OFF build. The probe's call must fail with
# the prose BuildFeatureDisabledError message as before, and the probe prints
# the code and feature it read from the thrown BuildFeatureCallDisabledError;
# a code-less throw prints no such line.
function(expect_facade_disabled label feature_name)
    expect_disabled_error("${label}" "${feature_name}" ${ARGN})
    execute_process(
        COMMAND ${ARGN}
        WORKING_DIRECTORY "${SOURCE_DIR}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE stdout
        ERROR_VARIABLE stderr
    )
    set(expected "PG2_FACADE code=build_feature_disabled@1 feature=${feature_name}")
    string(FIND "${stdout}
${stderr}" "${expected}" expected_index)
    if(expected_index EQUAL -1)
        message(FATAL_ERROR "${label} did not fail with '${expected}'
stdout:
${stdout}
stderr:
${stderr}")
    endif()
    message(STATUS "${label}: ${expected}")
endfunction()

# PG-2: the [pg2] cases -- every facade entry of the missing module fails with
# build_feature_disabled@1 and the option, the animation facade stays a
# Status, and the participant OFF divisions follow the rule.
#
# PG-2b: the lines after `label` are what the OFF build's [pg2] cases must
# print (a refusal recorded in a noexcept onDestroy, the RPC raycast's name),
# so a case compiled out by a wrong condition cannot pass by printing nothing.
# A facade that throws out of onDestroy ends the run in std::terminate, which
# the test turns into a PG2B_TERMINATE exit and run_process reports.
function(run_pg2_cases build_dir label)
    find_built_executable("${build_dir}" "pelican_test_group_core_foundation" pg2_group)
    run_process("${label}_pg2_core_foundation" TRUE "${pg2_group}" "[pg2]")
    message(STATUS "${label} [pg2] core_foundation:
${${label}_pg2_core_foundation_STDOUT}")
    foreach(expected IN LISTS ARGN)
        string(FIND "${${label}_pg2_core_foundation_STDOUT}" "${expected}" expected_index)
        if(expected_index EQUAL -1)
            message(FATAL_ERROR "${label} [pg2] did not print '${expected}'
stdout:
${${label}_pg2_core_foundation_STDOUT}")
        endif()
    endforeach()
endfunction()

# PG-1: a refusal that is not a thrown BuildFeatureDisabledError but a named
# error, `<requirement>.module_unavailable@1` or a registry name.
function(expect_named_error label code)
    execute_process(
        COMMAND ${ARGN}
        WORKING_DIRECTORY "${SOURCE_DIR}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE stdout
        ERROR_VARIABLE stderr
    )
    if(result EQUAL 0)
        message(FATAL_ERROR "${label} unexpectedly succeeded
stdout:
${stdout}
stderr:
${stderr}")
    endif()
    string(FIND "${stdout}
${stderr}" "${code}" code_index)
    if(code_index EQUAL -1)
        message(FATAL_ERROR "${label} did not report ${code}
stdout:
${stdout}
stderr:
${stderr}")
    endif()
endfunction()

# PG-1 acceptance 1 in an OFF build: the [pg1] cases of the test binaries,
# which assert the refusal by name where the build lacks the module and the
# success where it has it -- scene load and rollback, the editor's
# add_component and spawn, the Studio addition catalog, the requires words,
# package capabilities and the graph registry.
function(run_pg1_cases build_dir label)
    foreach(group IN ITEMS core_foundation project_formats ecs_editor)
        find_built_executable("${build_dir}" "pelican_test_group_${group}" pg1_group)
        run_process("${label}_pg1_${group}" TRUE "${pg1_group}" "[pg1]")
        message(STATUS "${label} [pg1] ${group}:
${${label}_pg1_${group}_STDOUT}")
    endforeach()
endfunction()

# A project whose only content is its `requires`: the player must refuse it
# while reading project.json, before any scene or device work.
function(write_requires_project root requirement)
    write_empty_project("${root}")
    file(READ "${root}/project.json" project_json)
    string(REPLACE "\"engine_min_version\": \"0.1.0\","
        "\"engine_min_version\": \"0.1.0\",
  \"requires\": [\"${requirement}\"],"
        project_json "${project_json}")
    file(WRITE "${root}/project.json" "${project_json}")
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
  "schema": "pelican.graph",
  "version": 1,
  "definitions": {},
  "nodes": [],
  "outputs": {},
  "rendering": {
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
}
]=])
endfunction()

function(write_empty_project root)
    write_common_project("${root}")
    file(WRITE "${root}/assets/asset_data.json" "{\"schema\":\"pelican.asset_data\",\"version\":2,\"models\":[]}\n")
    file(WRITE "${root}/scenes/main.scene.json" [=[
{
  "schema": "pelican.scene",
  "version": 2,
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
  "version": 2,
  "models": [
    {"name": "vat_model", "path": "assets/tiny_vat.glb"}
  ]
}
]=])
    file(WRITE "${root}/scenes/main.scene.json" [=[
{
  "schema": "pelican.scene",
  "version": 2,
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
    # The common project's single `ui` pass is refused by the preview graph
    # policy before any scene is read, so this project terminates in a plain
    # fullscreen pass: the collider has to reach scene prepare to be refused.
    file(WRITE "${root}/passes/main.json" [=[
{
  "schema": "pelican.graph",
  "version": 1,
  "definitions": {},
  "nodes": [],
  "outputs": {},
  "rendering": {
    "features": ["engine://features/ui.json"],
    "render_targets": [],
    "rendering_passes": [
      {
        "name": "main",
        "passes": [
          {
            "name": "present",
            "type": "fullscreen",
            "output": {"color": "swapchain", "depth": null},
            "shader": {"vertex": "engine://fullscreen", "fragment": "engine://fullscreen"}
          }
        ]
      }
    ]
  }
}
]=])
    file(WRITE "${root}/assets/asset_data.json" "{\"schema\":\"pelican.asset_data\",\"version\":2,\"models\":[]}\n")
    file(WRITE "${root}/scenes/main.scene.json" [=[
{
  "schema": "pelican.scene",
  "version": 2,
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
    run_rt2b_test(
        "${runtime_shader_compiler_build_dir}"
        "runtime_shader_compiler")
    require_rt2b_test_execution("runtime_shader_compiler")
    if(PELICAN_BUILD_UNIT_SMOKE_WP369_ONLY)
        clean_successful_build("${runtime_shader_compiler_build_dir}")
        return()
    endif()
    run_rt1b_case(
        "${runtime_shader_compiler_build_dir}"
        "runtime_shader_compiler"
        "WP366a compiler contrast executes the real root-sidecar gate")
    run_rt2a_compiler_off_contrast(
        "${runtime_shader_compiler_build_dir}"
        "runtime_shader_compiler")
    run_rt1b_test(
        "${runtime_shader_compiler_build_dir}"
        "runtime_shader_compiler")
    clean_successful_build("${runtime_shader_compiler_build_dir}")
endfunction()

function(run_spirv_link_smoke)
    configure_and_build(
        "spirv_link"
        "PELICAN_WITH_SPIRV_LINK"
        spirv_link_build_dir)
    verify_spirv_link_absent("${spirv_link_build_dir}")
    run_rt2a_compiler_on_contrast(
        "${spirv_link_build_dir}" "spirv_link")
    run_rt1b_test("${spirv_link_build_dir}" "spirv_link")
    clean_successful_build("${spirv_link_build_dir}")
endfunction()

function(run_skip_devstudio_smoke)
    configure_and_build(
        "skip_devstudio"
        "SKIP_DEVSTUDIO"
        skip_devstudio_build_dir)
    verify_devstudio_absent("${skip_devstudio_build_dir}")
    run_rt2b_test(
        "${skip_devstudio_build_dir}"
        "skip_devstudio")
    require_rt2b_test_execution("skip_devstudio")
    if(PELICAN_BUILD_UNIT_SMOKE_WP369_ONLY)
        clean_successful_build("${skip_devstudio_build_dir}")
        return()
    endif()
    run_rt2a_compiler_on_contrast(
        "${skip_devstudio_build_dir}" "skip_devstudio")
    run_rt1b_test(
        "${skip_devstudio_build_dir}"
        "skip_devstudio")
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
    run_rt2b_test("${openxr_build_dir}" "openxr")
    require_rt2b_test_execution("openxr")
    if(PELICAN_BUILD_UNIT_SMOKE_WP369_ONLY)
        clean_successful_build("${openxr_build_dir}")
        return()
    endif()
    run_rt2a_compiler_on_contrast(
        "${openxr_build_dir}" "openxr")
    run_rt1b_test("${openxr_build_dir}" "openxr")
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

# AV-1: a project whose asset store disagrees with its manifest (one file
# changed, one gone, one not listed) and that declares hot_reload. It runs
# under --rpc, so it draws rpc_headless_player's hybrid pipeline (the empty
# project's bare ui pass is refused by the preview graph).
function(write_hot_reload_project root)
    write_empty_project("${root}")
    configure_file("${SOURCE_DIR}/test/fixtures/hybrid_sky_rendering_config.json"
        "${root}/passes/main.json" COPYONLY)
    file(READ "${root}/project.json" project_json)
    string(REPLACE "\"engine_min_version\": \"0.1.0\","
        "\"engine_min_version\": \"0.1.0\",
  \"asset_stores\": {\"main\": {\"mount\": \"art\", \"manifest\": \"art.manifest.json\"}},
  \"hot_reload\": [\"art\"],"
        project_json "${project_json}")
    string(REPLACE "\"default_rendering_pass\": \"main\""
        "\"default_rendering_pass\": \"main_render\"" project_json "${project_json}")
    file(WRITE "${root}/project.json" "${project_json}")
    file(MAKE_DIRECTORY "${root}/art")
    file(WRITE "${root}/art/a.bin" "original")
    file(SHA256 "${root}/art/a.bin" a_sha)
    file(WRITE "${root}/art.manifest.json" "{
  \"schema\": \"pelican.assets\",
  \"version\": 1,
  \"tool\": {\"name\": \"pelican_cli assets manifest\"},
  \"files\": [
    {\"file\": \"a.bin\", \"size\": 8, \"sha256\": \"${a_sha}\"},
    {\"file\": \"gone.bin\", \"size\": 8, \"sha256\": \"${a_sha}\"}
  ]
}
")
    file(WRITE "${root}/art/a.bin" "changed!!")
    file(WRITE "${root}/art/extra.bin" "extra")
endfunction()

# AV-1: PELICAN_WITH_HOT_RELOAD=OFF (the distribution build) starts without
# checking the asset manifest unless --strict-assets asks, which still stops
# the launch as before; get_status says why there is no file watch and no
# verification.
function(run_hot_reload_smoke)
    configure_and_build("hot_reload" "PELICAN_WITH_HOT_RELOAD" hot_reload_build_dir)
    find_built_executable("${hot_reload_build_dir}" "pelican_player" hot_reload_player)
    set(hot_reload_project "${ARTIFACT_ROOT}/hot_reload_project")
    write_hot_reload_project("${hot_reload_project}")

    run_process("hot_reload_strict_assets" FALSE
        "${hot_reload_player}" --headless --strict-assets --project "${hot_reload_project}" --frames 1 --size 64x64)
    if(NOT "${hot_reload_strict_assets_STDOUT}\n${hot_reload_strict_assets_STDERR}" MATCHES
       "assets manifest: 1 stores, 1 matched, 0 info, 0 warnings, 3 errors")
        message(FATAL_ERROR "PELICAN_WITH_HOT_RELOAD=OFF --strict-assets did not verify and stop:
stdout:
${hot_reload_strict_assets_STDOUT}
stderr:
${hot_reload_strict_assets_STDERR}")
    endif()

    set(rpc_script "${ARTIFACT_ROOT}/hot_reload_rpc.ndjson")
    file(WRITE "${rpc_script}" "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"get_status\",\"params\":{}}\n")
    execute_process(
        COMMAND "${hot_reload_player}" --rpc --headless --project "${hot_reload_project}" --size 64x64
        WORKING_DIRECTORY "${SOURCE_DIR}"
        INPUT_FILE "${rpc_script}"
        RESULT_VARIABLE rpc_result
        OUTPUT_VARIABLE rpc_stdout
        ERROR_VARIABLE rpc_stderr
    )
    set(rpc_output "${rpc_stdout}\n${rpc_stderr}")
    if(NOT rpc_result EQUAL 0 OR
       NOT rpc_stdout MATCHES [=["assets_verification":\{"reason":"PELICAN_WITH_HOT_RELOAD=OFF","state":"not_run"\}]=] OR
       NOT rpc_stdout MATCHES [=["gate_reason":"hot reload build disabled"]=] OR
       rpc_output MATCHES "assets manifest" OR
       NOT rpc_output MATCHES "project.json hot_reload is ignored")
        message(FATAL_ERROR "PELICAN_WITH_HOT_RELOAD=OFF player did not skip the watch and the startup check:
result: ${rpc_result}
stdout:
${rpc_stdout}
stderr:
${rpc_stderr}")
    endif()
    clean_successful_build("${hot_reload_build_dir}")
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
    expect_facade_disabled(
        "audio_gamecontext_api"
        "PELICAN_WITH_AUDIO"
        "${audio_probe}"
    )
    run_pg1_cases("${audio_build_dir}" "audio")
    run_pg2_cases("${audio_build_dir}" "audio"
        "PG2B_DESTROY_RECORDED api=soundState code=build_feature_disabled@1"
        "PG2B_DESTROY_RECORDED api=playSound code=build_feature_disabled@1")
    find_built_executable("${audio_build_dir}" "pelican_player" audio_player)
    set(audio_requires_project "${ARTIFACT_ROOT}/audio_requires_project")
    write_requires_project("${audio_requires_project}" audio)
    expect_named_error(
        "audio_project_requires"
        "audio.module_unavailable@1"
        "${audio_player}" --headless --project "${audio_requires_project}" --frames 1 --size 64x64
    )
    # AU-3: --audio-backend in a build without audio. A backend it cannot open
    # is the OFF error, as --xr on is; null, the silence it has anyway, is
    # accepted (the run goes on to stop at the project's requires); a name
    # none of the three is refused as in any build. The environment variable
    # is left unread: a harness sets it for every player it starts.
    expect_disabled_error(
        "audio_backend_offline"
        "PELICAN_WITH_AUDIO"
        "${audio_player}" --headless --audio-backend offline --project "${audio_requires_project}" --frames 1
    )
    expect_named_error(
        "audio_backend_unknown"
        "audio.backend_unknown@1"
        "${audio_player}" --headless --audio-backend speakers --project "${audio_requires_project}" --frames 1
    )
    expect_named_error(
        "audio_backend_null"
        "audio.module_unavailable@1"
        "${audio_player}" --headless --audio-backend null --project "${audio_requires_project}" --frames 1
    )
    set(ENV{PELICAN_AUDIO_BACKEND} speakers)
    expect_named_error(
        "audio_backend_environment_unread"
        "audio.module_unavailable@1"
        "${audio_player}" --headless --project "${audio_requires_project}" --frames 1
    )
    unset(ENV{PELICAN_AUDIO_BACKEND})
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

function(verify_animation_absent build_dir)
    # The animation unit's own translation units must not be compiled at all.
    # animation_disabled.cpp is the frozen negotiation surface and stays.
    file(GLOB_RECURSE build_metadata LIST_DIRECTORIES false
        "${build_dir}/build.ninja"
        "${build_dir}/*.vcxproj"
        "${build_dir}/*DependInfo.cmake"
        "${build_dir}/compile_commands.json"
    )
    foreach(metadata IN LISTS build_metadata)
        file(READ "${metadata}" contents)
        if(contents MATCHES "(animationservice|animationjobs|animationprobe|animationserviceabi|animationposearena|animationsinkregistry|animationphasescheduler|vrmapplication|vrmaretarget|posegraph|posegraphruntime)[.]cpp" OR
           contents MATCHES "animation[/\\]animgraph[.]cpp")
            message(FATAL_ERROR "PELICAN_WITH_ANIMATION=OFF still compiles an animation source: ${metadata}")
        endif()
    endforeach()

    file(GLOB_RECURSE artifacts LIST_DIRECTORIES false "${build_dir}/*")
    foreach(artifact IN LISTS artifacts)
        get_filename_component(name "${artifact}" NAME)
        string(TOLOWER "${name}" lower_name)
        if(lower_name MATCHES "^(animationservice|animationjobs|animationprobe|animationserviceabi|animationposearena|animationsinkregistry|animationphasescheduler|vrmapplication|vrmaretarget|posegraph|posegraphruntime|animgraph)[.](obj|o)$")
            message(FATAL_ERROR "PELICAN_WITH_ANIMATION=OFF emitted an animation object: ${artifact}")
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
            message(FATAL_ERROR "dumpbin is required for the animation OFF symbol check")
        endif()

        file(GLOB_RECURSE symbol_artifacts LIST_DIRECTORIES false
            "${build_dir}/*pelican_core.lib"
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
            if(symbols MATCHES "animationServiceRuntime" OR
               symbols MATCHES "EvaluatorV1@AnimationGraph" OR
               symbols MATCHES "registerPoseGraphModule" OR
               symbols MATCHES "PoseGraphRuntime@Animation" OR
               symbols MATCHES "applicationServiceRuntime@Vrm" OR
               symbols MATCHES "retargetVrmaClip")
                message(FATAL_ERROR "PELICAN_WITH_ANIMATION=OFF retained animation symbols in ${symbol_artifact}")
            endif()
        endforeach()
    endif()
endfunction()

function(run_animation_smoke)
    configure_and_build("animation" "PELICAN_WITH_ANIMATION" animation_build_dir)
    verify_animation_absent("${animation_build_dir}")
    # The common graph keeps working without the module: the value, render and
    # audio tests are in this binary, and the catalog test asserts by name that
    # pelican.pose is gone.
    find_built_executable(
        "${animation_build_dir}" "pelican_test_group_core_foundation" animation_graph_test)
    run_process("animation_common_graph" TRUE "${animation_graph_test}"
        -w NoAssertions "[graph-composition],[graph-module],[valuegraph],[curve]")
    # AG-1b acceptance 3, measured in the OFF build itself: a scene carrying an
    # animation component fails scene prepare by name, and the scene that was
    # already live is still there afterwards.
    run_process("animation_off_scene_reject" TRUE "${animation_graph_test}"
        -w NoAssertions "[animation-off]")
    clean_successful_build("${animation_build_dir}")
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
    run_rt2a_compiler_on_contrast(
        "${imgui_build_dir}" "imgui")
    run_rt1b_test("${imgui_build_dir}" "imgui")
    clean_successful_build("${imgui_build_dir}")
endfunction()

function(run_builtin_physics_smoke)
    configure_and_build(
        "physics_provider_only"
        "PELICAN_WITH_BUILTIN_PHYSICS"
        physics_provider_build_dir)
    verify_builtin_physics_absent("${physics_provider_build_dir}")
    verify_jolt_physics_absent("${physics_provider_build_dir}" "provider-only physics")
    # physicsservice_test lives in the runtime_services test group (there is
    # no standalone pelican_test_physicsservice_test any more); its cases carry
    # [physics-service].
    find_built_executable("${physics_provider_build_dir}" "pelican_test_group_runtime_services" physics_provider_test)
    run_process("physics_provider_only_service" TRUE "${physics_provider_test}" "[physics-service]")
    # PG-2 acceptance 3: with no provider, every ctx.physics() query is
    # physics.provider_unavailable@1, not an empty answer.
    run_process("physics_provider_only_pg2" TRUE "${physics_provider_test}" "[pg2]")
    string(FIND "${physics_provider_only_pg2_STDOUT}"
        "PG2_PROVIDER_UNAVAILABLE code=physics.provider_unavailable@1" pg2_index)
    if(pg2_index EQUAL -1)
        message(FATAL_ERROR "physics_provider_only_pg2 did not observe physics.provider_unavailable@1
${physics_provider_only_pg2_STDOUT}")
    endif()
    message(STATUS "physics_provider_only [pg2]:
${physics_provider_only_pg2_STDOUT}")
    run_pg2_cases("${physics_provider_build_dir}" "physics_provider_only"
        "PG2B_DESTROY_RECORDED api=raycastAll code=physics.provider_unavailable@1"
        "PG2B_DESTROY_RECORDED api=shapeCastClosest code=physics.provider_unavailable@1")
    run_process(
        "physics_provider_only_game_dll"
        TRUE
        "${CMAKE_CTEST_COMMAND}"
            --test-dir "${physics_provider_build_dir}"
            --build-config "${PELICAN_BUILD_UNIT_SMOKE_CONFIG}"
            --output-on-failure
            --no-tests=error
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
    find_built_executable(
        "${physics_build_dir}" "pelican_test_physics_disabled_probe" physics_api_probe)
    expect_facade_disabled(
        "physics_gamecontext_api"
        "PELICAN_WITH_PHYSICS"
        "${physics_api_probe}"
    )
    find_built_executable("${physics_build_dir}" "pelican_player" physics_player)
    set(physics_project "${ARTIFACT_ROOT}/physics_project")
    write_physics_project("${physics_project}")
    # PG-1: a collider is refused by the module's name, the same name the
    # project's `requires: ["physics"]` below fails under.
    expect_named_error(
        "physics_scene_collider"
        "physics.module_unavailable@1"
        "${physics_player}" --headless --project "${physics_project}" --frames 1 --size 64x64
    )
    set(physics_requires_project "${ARTIFACT_ROOT}/physics_requires_project")
    write_requires_project("${physics_requires_project}" physics)
    expect_named_error(
        "physics_project_requires"
        "physics.module_unavailable@1"
        "${physics_player}" --headless --project "${physics_requires_project}" --frames 1 --size 64x64
    )
    find_built_executable(
        "${physics_build_dir}" "pelican_test_wp360_resolvedscene_test" physics_wp360_test)
    run_process("physics_wp360_matrix" TRUE "${physics_wp360_test}" "[wp360][physics][matrix]")
    run_pg1_cases("${physics_build_dir}" "physics")
    run_pg2_cases("${physics_build_dir}" "physics"
        "PG2B_DESTROY_RECORDED api=raycastAll code=build_feature_disabled@1"
        "PG2B_DESTROY_RECORDED api=shapeCastClosest code=build_feature_disabled@1"
        "PG2B_RPC_RAYCAST code=physics.module_unavailable@1")
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
            --no-tests=error
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
    elseif(feature_name STREQUAL "PELICAN_WITH_ANIMATION")
        if(NOT dry_run)
            run_animation_smoke()
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
    elseif(feature_name STREQUAL "PELICAN_WITH_HOT_RELOAD")
        if(NOT dry_run)
            run_hot_reload_smoke()
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
