# X-1: the code half of packaging.
#
# Run from the repository root:
#   cmake -DPELICAN_PACKAGE_TEMPLATE_SMOKE_CONFIG=Debug -P test/run_package_template_smoke.cmake
#
# This script intentionally is not registered with ctest: it configures and
# builds a player, twice.  It is the same shape as
# run_project_code_smoke.cmake and is meant for CI and for hand runs.
#
# What it judges, in order:
#   1. A package template copied outside the engine tree, with no `.git`,
#      installs into a project made by `pelican_cli project init`.
#   2. A player configured with -DPELICAN_PROJECT=<that project> builds the
#      package's code through pelican_packages() / pelican_package_sources(),
#      and the package's system prints its line in a headless run.
#   3. Control 1: adding `#include "vkcore/renderer.hpp"` to the package source
#      makes the build fail.  "Only the public SDK" is judged here and nowhere
#      else -- the project's own game logic target does carry src/core.
#   4. Control 2: `requires.sdk_abi: 999` is refused at install by name.

if(NOT DEFINED PELICAN_PACKAGE_TEMPLATE_SMOKE_CONFIG)
    set(PELICAN_PACKAGE_TEMPLATE_SMOKE_CONFIG Debug)
endif()

get_filename_component(SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(BUILD_DIR "${SOURCE_DIR}/build-package-template")

if(DEFINED ENV{TEMP})
    set(OUTSIDE_ROOT "$ENV{TEMP}/pelican_x1_package_template")
elseif(DEFINED ENV{TMPDIR})
    set(OUTSIDE_ROOT "$ENV{TMPDIR}/pelican_x1_package_template")
else()
    message(FATAL_ERROR "no temp directory: this smoke needs somewhere outside the engine tree")
endif()

if(WIN32)
    set(EXE_SUFFIX ".exe")
else()
    set(EXE_SUFFIX "")
endif()

function(run_process label)
    execute_process(
        COMMAND ${ARGN}
        WORKING_DIRECTORY "${SOURCE_DIR}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE stdout
        ERROR_VARIABLE stderr
    )
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "${label} failed with ${result}\nstdout:\n${stdout}\nstderr:\n${stderr}")
    endif()
    set("${label}_STDOUT" "${stdout}" PARENT_SCOPE)
    set("${label}_STDERR" "${stderr}" PARENT_SCOPE)
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

# Everything but the fetched dependency sources is thrown away, so a rerun
# does not re-download half the internet.
file(GLOB build_entries LIST_DIRECTORIES true "${BUILD_DIR}/*")
foreach(entry IN LISTS build_entries)
    get_filename_component(entry_name "${entry}" NAME)
    if(NOT entry_name STREQUAL "_deps")
        file(REMOVE_RECURSE "${entry}")
    endif()
endforeach()
file(REMOVE_RECURSE "${OUTSIDE_ROOT}")
file(MAKE_DIRECTORY "${OUTSIDE_ROOT}")

set(configure_common
    -S "${SOURCE_DIR}"
    -B "${BUILD_DIR}"
    -DSKIP_DEVSTUDIO=ON
    -DBUILD_TESTING=OFF
    -DPELICAN_WITH_SPIRV_LINK=OFF
    # Nothing here needs XR, and its SDK wants a Python generator step.
    -DPELICAN_WITH_OPENXR=OFF
    "-DCMAKE_BUILD_TYPE=${PELICAN_PACKAGE_TEMPLATE_SMOKE_CONFIG}")

# Optional: point this at a build tree's _deps to reuse already-fetched
# sources, for example
#   -DPELICAN_PACKAGE_TEMPLATE_SMOKE_DEPS=<primary worktree>/build/_deps
if(DEFINED PELICAN_PACKAGE_TEMPLATE_SMOKE_DEPS)
    foreach(dep IN ITEMS argparse battery-embed catch2 glfw glm imgui_vendor
                         miniaudio nlohmann picosha2 quill spirv_headers
                         spirv_reflect spirv_tools stb tinyexr tinygltf vma-hpp)
        set(dep_source "${PELICAN_PACKAGE_TEMPLATE_SMOKE_DEPS}/${dep}-src")
        if(EXISTS "${dep_source}")
            string(TOUPPER "${dep}" dep_option)
            list(APPEND configure_common
                "-DFETCHCONTENT_SOURCE_DIR_${dep_option}=${dep_source}")
        endif()
    endforeach()
endif()

# ---------------------------------------------------------------------------
# 1. Build the CLI, make a project outside the tree, install the package.
# ---------------------------------------------------------------------------
# PELICAN_PROJECT is a cache entry: clear it explicitly so a rerun does not
# configure against the temporary project the previous run deleted.
run_process(configure_cli "${CMAKE_COMMAND}" ${configure_common} "-DPELICAN_PROJECT=")
run_process(build_cli "${CMAKE_COMMAND}" --build "${BUILD_DIR}"
    --config "${PELICAN_PACKAGE_TEMPLATE_SMOKE_CONFIG}" --target pelican_cli)
set(cli "${BUILD_DIR}/dist/${PELICAN_PACKAGE_TEMPLATE_SMOKE_CONFIG}/pelican_cli${EXE_SUFFIX}")
if(NOT EXISTS "${cli}")
    find_built_executable("${BUILD_DIR}" "pelican_cli" cli)
endif()

# A package travels as content: the copy carries no version-control metadata,
# and it lives outside the engine tree for the whole run.
file(COPY "${SOURCE_DIR}/test/fixtures/package_template/"
     DESTINATION "${OUTSIDE_ROOT}/glowkit" PATTERN ".git" EXCLUDE)
set(package_dir "${OUTSIDE_ROOT}/glowkit")
set(project_dir "${OUTSIDE_ROOT}/host")

run_process(project_init "${cli}" project init "${project_dir}")

# Control 2: the SDK epoch is checked by name before anything is copied.
file(COPY "${package_dir}/" DESTINATION "${OUTSIDE_ROOT}/bad_abi")
file(READ "${package_dir}/package.json" package_manifest)
string(REPLACE "\"sdk_abi\": 2" "\"sdk_abi\": 999" bad_abi_manifest "${package_manifest}")
file(WRITE "${OUTSIDE_ROOT}/bad_abi/package.json" "${bad_abi_manifest}")
execute_process(
    COMMAND "${cli}" package install "${OUTSIDE_ROOT}/bad_abi" --project "${project_dir}"
    RESULT_VARIABLE abi_result
    OUTPUT_VARIABLE abi_stdout
    ERROR_VARIABLE abi_stderr)
if(abi_result EQUAL 0)
    message(FATAL_ERROR "control 2: a package declaring sdk_abi 999 installed anyway")
endif()
string(FIND "${abi_stdout}${abi_stderr}" "package_sdk_abi_mismatch" abi_found_at)
if(abi_found_at EQUAL -1)
    message(FATAL_ERROR "control 2: the SDK epoch rejection was not named\n${abi_stdout}${abi_stderr}")
endif()

run_process(package_install "${cli}" package install "${package_dir}" --project "${project_dir}")
if(NOT EXISTS "${project_dir}/packages/acme.glowkit/code/system.cpp")
    message(FATAL_ERROR "the package code was not vendored")
endif()

# ---------------------------------------------------------------------------
# 2. Configure the player against that project and build the package code.
# ---------------------------------------------------------------------------
run_process(configure_player "${CMAKE_COMMAND}" ${configure_common}
    "-DPELICAN_PROJECT=${project_dir}")
run_process(build_player "${CMAKE_COMMAND}" --build "${BUILD_DIR}"
    --config "${PELICAN_PACKAGE_TEMPLATE_SMOKE_CONFIG}")
find_built_executable("${BUILD_DIR}" "pelican_player" player)

file(MAKE_DIRECTORY "${OUTSIDE_ROOT}/frames")
execute_process(
    COMMAND "${player}"
        --headless
        --project "${project_dir}"
        --frames 3
        --size 160x90
        --fps 30
        --render-out "${OUTSIDE_ROOT}/frames/out.png"
    WORKING_DIRECTORY "${SOURCE_DIR}"
    RESULT_VARIABLE run_result
    OUTPUT_VARIABLE run_stdout
    ERROR_VARIABLE run_stderr)
if(NOT run_result EQUAL 0)
    message(FATAL_ERROR "the package-code headless run failed with ${run_result}\nstdout:\n${run_stdout}\nstderr:\n${run_stderr}")
endif()
string(FIND "${run_stderr}" "acme.glowkit: package system running" system_found_at)
if(system_found_at EQUAL -1)
    message(FATAL_ERROR
        "the package's system did not run\nstdout:\n${run_stdout}\nstderr:\n${run_stderr}")
endif()

# ---------------------------------------------------------------------------
# 3. Control 1: the public SDK is the whole include path of a package target.
# ---------------------------------------------------------------------------
set(package_source "${project_dir}/packages/acme.glowkit/code/system.cpp")
file(READ "${package_source}" package_source_bytes)
file(WRITE "${package_source}"
     "#include \"vkcore/renderer.hpp\"\n${package_source_bytes}")
execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${BUILD_DIR}"
        --config "${PELICAN_PACKAGE_TEMPLATE_SMOKE_CONFIG}"
        --target pelican_game_logic
    WORKING_DIRECTORY "${SOURCE_DIR}"
    RESULT_VARIABLE internal_result
    OUTPUT_VARIABLE internal_stdout
    ERROR_VARIABLE internal_stderr)
file(WRITE "${package_source}" "${package_source_bytes}")
if(internal_result EQUAL 0)
    message(FATAL_ERROR
        "control 1: package code reached an engine-internal header and the build stayed green")
endif()
string(FIND "${internal_stdout}${internal_stderr}" "vkcore/renderer.hpp" internal_found_at)
if(internal_found_at EQUAL -1)
    message(FATAL_ERROR
        "control 1: the build failed for an unrelated reason\nstdout:\n${internal_stdout}\nstderr:\n${internal_stderr}")
endif()
message(STATUS "control 1 diagnostic:")
message(STATUS "${internal_stderr}${internal_stdout}")

# Restore the green build so a following run starts from a clean state.
run_process(rebuild_package "${CMAKE_COMMAND}" --build "${BUILD_DIR}"
    --config "${PELICAN_PACKAGE_TEMPLATE_SMOKE_CONFIG}" --target pelican_game_logic)

file(REMOVE_RECURSE "${OUTSIDE_ROOT}")
message(STATUS "X-1 package template smoke passed")
