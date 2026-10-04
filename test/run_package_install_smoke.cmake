# X-1: the half of packaging that does not build code.
#
# A package template is copied out of the engine tree (into the machine's temp
# directory, with no `.git`), a fresh project is created with
# `pelican_cli project init`, and the package is installed into it.  From there
# the vendored feature has to reach the catalog under its namespaced name, be
# addable through the editor RPC, change what the GPU draws, survive the source
# directory being deleted, and come back out through `uninstall_package`.
#
# The code half is `run_package_template_smoke.cmake`, which is not registered
# with ctest because it configures and builds a player.

foreach(required IN ITEMS CLI PLAYER PLAYER_DIR SOURCE_DIR OUT_DIR)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "${required} is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${OUT_DIR}")
file(MAKE_DIRECTORY "${OUT_DIR}")

# "Outside the engine tree" is the point of the exercise, so the package the
# test installs never lives under the source or build directory.
if(DEFINED ENV{TEMP})
    set(outside_root "$ENV{TEMP}/pelican_x1_package_smoke")
elseif(DEFINED ENV{TMPDIR})
    set(outside_root "$ENV{TMPDIR}/pelican_x1_package_smoke")
else()
    set(outside_root "${OUT_DIR}/outside")
endif()
file(REMOVE_RECURSE "${outside_root}")
file(MAKE_DIRECTORY "${outside_root}")

set(template_source "${SOURCE_DIR}/test/fixtures/package_template")
if(NOT EXISTS "${template_source}/package.json")
    message(FATAL_ERROR "package template fixture is missing: ${template_source}")
endif()

# A package travels as content, not as a checkout: the copy carries no `.git`.
file(COPY "${template_source}/" DESTINATION "${outside_root}/glowkit"
     PATTERN ".git" EXCLUDE)
set(package_dir "${outside_root}/glowkit")
if(EXISTS "${package_dir}/.git")
    message(FATAL_ERROR "the copied package still holds a .git directory")
endif()

function(run_cli label expect_success)
    execute_process(
        COMMAND "${CLI}" ${ARGN}
        RESULT_VARIABLE result
        OUTPUT_VARIABLE stdout
        ERROR_VARIABLE stderr
    )
    set(combined "${stdout}${stderr}")
    if(expect_success AND NOT result EQUAL 0)
        message(FATAL_ERROR "${label} failed with ${result}\n${combined}")
    endif()
    if(NOT expect_success AND result EQUAL 0)
        message(FATAL_ERROR "${label} unexpectedly succeeded\n${combined}")
    endif()
    set(${label}_OUTPUT "${combined}" PARENT_SCOPE)
endfunction()

function(require_text haystack needle label)
    string(FIND "${haystack}" "${needle}" found_at)
    if(found_at EQUAL -1)
        message(FATAL_ERROR "${label}: expected to find '${needle}'\n${haystack}")
    endif()
endfunction()

function(forbid_text haystack needle label)
    string(FIND "${haystack}" "${needle}" found_at)
    if(NOT found_at EQUAL -1)
        message(FATAL_ERROR "${label}: did not expect '${needle}'\n${haystack}")
    endif()
endfunction()

# One RPC player run per script file; stdout comes back for inspection.
function(run_rpc label project script_lines out_var)
    set(script_path "${OUT_DIR}/${label}.ndjson")
    file(WRITE "${script_path}" "${script_lines}")
    execute_process(
        COMMAND "${PLAYER}"
            --rpc
            --headless
            --project "${project}"
            --size 160x90
            --fps 30
        WORKING_DIRECTORY "${PLAYER_DIR}"
        INPUT_FILE "${script_path}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE stdout
        ERROR_VARIABLE stderr
    )
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "${label}: rpc player run failed with ${result}\nstdout:\n${stdout}\nstderr:\n${stderr}")
    endif()
    if(stdout MATCHES "Validation Error|VUID-" OR stderr MATCHES "Validation Error|VUID-")
        message(FATAL_ERROR "${label}: rpc player run emitted Vulkan validation errors\nstdout:\n${stdout}\nstderr:\n${stderr}")
    endif()
    set(${out_var} "${stdout}" PARENT_SCOPE)
endfunction()

# The `result` of request `id` in a run_rpc transcript, as JSON text. The
# checks below read fields by name: the catalog entry grows fields (FR-1's
# `required_targets`, FR-2's `insertion`), and a pattern over the serialized
# order breaks on each one.
function(rpc_result label stdout id out_var)
    string(REPLACE "\r\n" "\n" rest "${stdout}")
    set(found "")
    while(NOT rest STREQUAL "")
        string(FIND "${rest}" "\n" newline_at)
        if(newline_at EQUAL -1)
            set(line "${rest}")
            set(rest "")
        else()
            string(SUBSTRING "${rest}" 0 ${newline_at} line)
            math(EXPR after_newline "${newline_at} + 1")
            string(SUBSTRING "${rest}" ${after_newline} -1 rest)
        endif()
        string(JSON line_id ERROR_VARIABLE line_error GET "${line}" id)
        if(line_error STREQUAL "NOTFOUND" AND "${line_id}" STREQUAL "${id}")
            string(JSON found ERROR_VARIABLE result_error GET "${line}" result)
            if(NOT result_error STREQUAL "NOTFOUND")
                message(FATAL_ERROR "${label}: request ${id} returned no result\n${line}")
            endif()
        endif()
    endwhile()
    if(found STREQUAL "")
        message(FATAL_ERROR "${label}: no response to request ${id}\n${stdout}")
    endif()
    set(${out_var} "${found}" PARENT_SCOPE)
endfunction()

# The entry of a `list_render_features` result whose `reference` is the given
# one, as JSON text; fails when the catalog has no such entry.
function(catalog_entry label result reference out_var)
    string(JSON feature_count ERROR_VARIABLE count_error LENGTH "${result}" features)
    if(NOT count_error STREQUAL "NOTFOUND")
        message(FATAL_ERROR "${label}: the result has no features array\n${result}")
    endif()
    set(entry "")
    if(feature_count GREATER 0)
        math(EXPR last_feature "${feature_count} - 1")
        foreach(feature_index RANGE 0 ${last_feature})
            string(JSON entry_reference ERROR_VARIABLE reference_error
                GET "${result}" features ${feature_index} reference)
            if(reference_error STREQUAL "NOTFOUND" AND
               "${entry_reference}" STREQUAL "${reference}")
                string(JSON entry GET "${result}" features ${feature_index})
            endif()
        endforeach()
    endif()
    if(entry STREQUAL "")
        message(FATAL_ERROR "${label}: the catalog has no entry for ${reference}\n${result}")
    endif()
    set(${out_var} "${entry}" PARENT_SCOPE)
endfunction()

# A JSON boolean reads back as ON / OFF.
function(require_field label entry field expected)
    string(JSON value ERROR_VARIABLE field_error GET "${entry}" ${field})
    if(NOT field_error STREQUAL "NOTFOUND")
        message(FATAL_ERROR "${label}: the entry has no '${field}'\n${entry}")
    endif()
    if(NOT "${value}" STREQUAL "${expected}")
        message(FATAL_ERROR "${label}: '${field}' is '${value}', not '${expected}'\n${entry}")
    endif()
endfunction()

# --------------------------------------------------------------------------
# 1. `package inspect` reads a package without installing it, and every named
#    rejection is observed on a deliberately broken copy.
# --------------------------------------------------------------------------
run_cli(inspect TRUE package inspect "${package_dir}")
require_text("${inspect_OUTPUT}" "\"namespace\": \"acme.glowkit\"" "package inspect")
require_text("${inspect_OUTPUT}" "\"features/glow.json\"" "package inspect")
require_text("${inspect_OUTPUT}" "\"code\": true" "package inspect")

function(make_broken_copy name)
    file(REMOVE_RECURSE "${outside_root}/${name}")
    file(COPY "${package_dir}/" DESTINATION "${outside_root}/${name}")
endfunction()

make_broken_copy(no_manifest)
file(REMOVE "${outside_root}/no_manifest/package.json")
run_cli(no_manifest FALSE package inspect "${outside_root}/no_manifest")
require_text("${no_manifest_OUTPUT}" "package_manifest_missing" "missing manifest")

file(READ "${package_dir}/package.json" template_manifest)

make_broken_copy(future_engine)
string(REPLACE "\"engine_min_version\": \"0.1.0\"" "\"engine_min_version\": \"9.0.0\""
       future_manifest "${template_manifest}")
file(WRITE "${outside_root}/future_engine/package.json" "${future_manifest}")

make_broken_copy(bad_abi)
string(REPLACE "\"sdk_abi\": 2" "\"sdk_abi\": 999" abi_manifest "${template_manifest}")
file(WRITE "${outside_root}/bad_abi/package.json" "${abi_manifest}")

make_broken_copy(needs_dependency)
string(REPLACE "\"dependencies\": []"
       "\"dependencies\": [{\"namespace\": \"acme.absent\", \"package_version\": \"1.0.0\"}]"
       dependency_manifest "${template_manifest}")
file(WRITE "${outside_root}/needs_dependency/package.json" "${dependency_manifest}")

make_broken_copy(nameless_feature)
file(READ "${package_dir}/features/glow.json" template_feature)
string(REPLACE "\"name\": \"glow\"," "" nameless_feature "${template_feature}")
file(WRITE "${outside_root}/nameless_feature/features/glow.json" "${nameless_feature}")

make_broken_copy(unknown_key)
string(REPLACE "\"description\":" "\"surprise\": 1,\n  \"description\":"
       unknown_manifest "${template_manifest}")
file(WRITE "${outside_root}/unknown_key/package.json" "${unknown_manifest}")
run_cli(unknown_key FALSE package inspect "${outside_root}/unknown_key")
require_text("${unknown_key_OUTPUT}" "package_manifest_invalid" "unknown manifest key")

# --------------------------------------------------------------------------
# 2. A fresh project, and what the catalog looks like before any install.
# --------------------------------------------------------------------------
set(host "${OUT_DIR}/host")
run_cli(init TRUE project init "${host}")

set(baseline_png "${OUT_DIR}/baseline.png")
set(installed_png "${OUT_DIR}/installed.png")
set(vendored_png "${OUT_DIR}/vendored.png")

run_rpc(before "${host}"
"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"list_render_features\",\"params\":{}}\n\
{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"get_render_features\",\"params\":{}}\n\
{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"step_frame\",\"params\":{}}\n\
{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"step_frame\",\"params\":{}}\n\
{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"capture\",\"params\":{\"path\":\"${baseline_png}\"}}\n"
    before_stdout)
forbid_text("${before_stdout}" "acme.glowkit.glow" "catalog before install")
require_text("${before_stdout}" "\"source\":\"engine\"" "catalog before install")
if(NOT EXISTS "${baseline_png}")
    message(FATAL_ERROR "the baseline capture was not written")
endif()

rpc_result("get_render_features before install" "${before_stdout}" 2 before_features)
string(JSON config_digest ERROR_VARIABLE config_digest_error
    GET "${before_features}" source_digest hex)
if(NOT config_digest_error STREQUAL "NOTFOUND" OR config_digest STREQUAL "")
    message(FATAL_ERROR "could not read the rendering config digest\n${before_stdout}")
endif()

# --------------------------------------------------------------------------
# 3. Install, and the named rejections that install performs.
# --------------------------------------------------------------------------
run_cli(reject_engine FALSE package install "${outside_root}/future_engine" --project "${host}")
require_text("${reject_engine_OUTPUT}" "package_engine_too_new" "engine_min_version rejection")

run_cli(reject_abi FALSE package install "${outside_root}/bad_abi" --project "${host}")
require_text("${reject_abi_OUTPUT}" "package_sdk_abi_mismatch" "sdk_abi rejection")

run_cli(reject_dependency FALSE package install "${outside_root}/needs_dependency" --project "${host}")
require_text("${reject_dependency_OUTPUT}" "package_dependency_missing" "dependency rejection")
require_text("${reject_dependency_OUTPUT}" "acme.absent" "dependency rejection names the package")

run_cli(reject_feature FALSE package install "${outside_root}/nameless_feature" --project "${host}")
require_text("${reject_feature_OUTPUT}" "package_feature_invalid" "nameless feature rejection")

run_cli(reject_missing FALSE package install "${outside_root}/no_manifest" --project "${host}")
require_text("${reject_missing_OUTPUT}" "package_manifest_missing" "missing manifest rejection")

# Nothing above copied anything: the checks run before the first byte.
if(EXISTS "${host}/packages")
    message(FATAL_ERROR "a rejected install left something under packages/")
endif()

run_cli(install TRUE package install "${package_dir}" --project "${host}")
require_text("${install_OUTPUT}" "\"namespace\": \"acme.glowkit\"" "install report")
require_text("${install_OUTPUT}" "project://packages/acme.glowkit/features/glow.json" "install report")
require_text("${install_OUTPUT}" "\"acme.glowkit.glow\"" "install report")

run_cli(install_again FALSE package install "${package_dir}" --project "${host}")
require_text("${install_again_OUTPUT}" "package_already_installed" "second install")

file(READ "${host}/project.json" host_project)
require_text("${host_project}" "\"packages\"" "project.json")
require_text("${host_project}" "\"acme.glowkit\"" "project.json")
string(JSON package_count ERROR_VARIABLE package_count_error LENGTH "${host_project}" packages)
if(NOT package_count_error STREQUAL "NOTFOUND" OR NOT package_count EQUAL 1)
    message(FATAL_ERROR "project.json does not record exactly one package\n${host_project}")
endif()
string(JSON recorded_digest GET "${host_project}" packages 0 sha256)
string(LENGTH "${recorded_digest}" recorded_digest_length)
if(NOT recorded_digest_length EQUAL 64)
    message(FATAL_ERROR "project.json did not record a sha256 digest\n${host_project}")
endif()

# The namespace is in the document, not only in the catalog.
set(vendored_feature "${host}/packages/acme.glowkit/features/glow.json")
if(NOT EXISTS "${vendored_feature}")
    message(FATAL_ERROR "the package feature was not vendored")
endif()
file(READ "${vendored_feature}" vendored_feature_bytes)
string(JSON vendored_name GET "${vendored_feature_bytes}" name)
if(NOT vendored_name STREQUAL "acme.glowkit.glow")
    message(FATAL_ERROR "the vendored feature name is '${vendored_name}', not 'acme.glowkit.glow'")
endif()
if(NOT EXISTS "${host}/packages/acme.glowkit/graphs/pulse.json")
    message(FATAL_ERROR "the package graph document was not vendored")
endif()
if(NOT EXISTS "${host}/packages/acme.glowkit/code/system.cpp")
    message(FATAL_ERROR "the package code was not vendored")
endif()

run_cli(list_ok TRUE package list --project "${host}")
require_text("${list_ok_OUTPUT}" "\"status\": \"installed\"" "package list")
require_text("${list_ok_OUTPUT}" "${recorded_digest}" "package list reports the digest")

# --------------------------------------------------------------------------
# 4. The vendored feature reaches the catalog, the editor and the GPU.
# --------------------------------------------------------------------------
run_rpc(installed "${host}"
"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"list_render_features\",\"params\":{}}\n\
{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"edit_render_features\",\"params\":{\"base_source_digest\":\"${config_digest}\",\"operations\":[{\"op\":\"add\",\"feature\":\"project://packages/acme.glowkit/features/glow.json\"}]}}\n\
{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"step_frame\",\"params\":{}}\n\
{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"get_render_features\",\"params\":{}}\n\
{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"step_frame\",\"params\":{}}\n\
{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"capture\",\"params\":{\"path\":\"${installed_png}\"}}\n\
{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"uninstall_package\",\"params\":{\"namespace\":\"acme.glowkit\"}}\n"
    installed_stdout)
require_text("${installed_stdout}" "\"status\":\"accepted\"" "edit_render_features add")
require_text("${installed_stdout}" "\"name\":\"acme.glowkit.glow\"" "catalog after install")
require_text("${installed_stdout}" "\"source\":\"package\"" "catalog after install")
require_text("${installed_stdout}" "project://packages/acme.glowkit/features/glow.json" "catalog after install")
# The package half of the catalog is available in this build: the template
# declares runtime_shader_compiler "optional" and uses engine shaders only.
rpc_result("catalog after install" "${installed_stdout}" 1 installed_catalog)
catalog_entry("catalog after install" "${installed_catalog}"
    "project://packages/acme.glowkit/features/glow.json" installed_entry)
require_field("catalog after install" "${installed_entry}" name "acme.glowkit.glow")
require_field("catalog after install" "${installed_entry}" source "package")
require_field("catalog after install" "${installed_entry}" available ON)
require_field("catalog after install" "${installed_entry}" hot_add_supported ON)
require_field("catalog after install" "${installed_entry}" requires_runtime_module OFF)
# Still referenced: the uninstall is refused by name and enumerates what holds
# it.
require_text("${installed_stdout}" "package_in_use" "uninstall while referenced")

file(READ "${host}/passes/main_rendering_config.json" host_config)
require_text("${host_config}" "project://packages/acme.glowkit/features/glow.json"
             "the edit reached the rendering config")

if(NOT EXISTS "${installed_png}")
    message(FATAL_ERROR "the post-install capture was not written")
endif()
file(SIZE "${installed_png}" installed_png_size)
if(installed_png_size EQUAL 0)
    message(FATAL_ERROR "the post-install capture is empty")
endif()
file(READ "${baseline_png}" baseline_bytes HEX)
file(READ "${installed_png}" installed_bytes HEX)
if(baseline_bytes STREQUAL installed_bytes)
    message(FATAL_ERROR "adding the package feature did not change a pixel")
endif()

# --------------------------------------------------------------------------
# 5. Vendoring means the source can go away.
# --------------------------------------------------------------------------
file(REMOVE_RECURSE "${package_dir}")
if(EXISTS "${package_dir}")
    message(FATAL_ERROR "could not remove the package source")
endif()
execute_process(
    COMMAND "${PLAYER}"
        --headless
        --project "${host}"
        --frames 2
        --size 160x90
        --fps 30
        --render-out "${vendored_png}"
    WORKING_DIRECTORY "${PLAYER_DIR}"
    RESULT_VARIABLE vendored_result
    OUTPUT_VARIABLE vendored_stdout
    ERROR_VARIABLE vendored_stderr
)
if(NOT vendored_result EQUAL 0)
    message(FATAL_ERROR "the project stopped rendering once the package source was deleted\nstdout:\n${vendored_stdout}\nstderr:\n${vendored_stderr}")
endif()
file(READ "${vendored_png}" vendored_bytes HEX)
if(baseline_bytes STREQUAL vendored_bytes)
    message(FATAL_ERROR "the vendored feature stopped drawing once its source directory was deleted")
endif()

# --------------------------------------------------------------------------
# 6. Uninstall, once nothing references the package.
# --------------------------------------------------------------------------
file(WRITE "${host}/passes/main_rendering_config.json" [=[
{
  "schema": "pelican.graph",
  "version": 1,
  "definitions": {},
  "nodes": [],
  "outputs": {},
  "rendering": {
    "pipeline": { "preset": "engine://render_pipelines/hybrid_v1.json" },
    "features": [
      "engine://features/shadow_directional.json",
      "engine://features/sky_ambient.json"
    ]
  }
}
]=])

run_rpc(uninstall "${host}"
"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"uninstall_package\",\"params\":{\"namespace\":\"acme.glowkit\"}}\n\
{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"list_render_features\",\"params\":{}}\n"
    uninstall_stdout)
require_text("${uninstall_stdout}" "\"status\":\"uninstalled\"" "uninstall")
forbid_text("${uninstall_stdout}" "acme.glowkit.glow" "catalog after uninstall")
if(EXISTS "${host}/packages/acme.glowkit")
    message(FATAL_ERROR "uninstall left the vendored directory behind")
endif()
file(READ "${host}/project.json" host_project_after)
string(JSON package_count_after ERROR_VARIABLE package_count_after_error
    LENGTH "${host_project_after}" packages)
if(NOT package_count_after_error STREQUAL "NOTFOUND" OR NOT package_count_after EQUAL 0)
    message(FATAL_ERROR "uninstall did not drop the project.json entry\n${host_project_after}")
endif()

# --------------------------------------------------------------------------
# 7. A vendored package that was damaged after installing must not stop the
#    project from opening.  The engine enumerator throws on a document with no
#    name; the package enumerator reports it instead.
# --------------------------------------------------------------------------
set(broken_host "${OUT_DIR}/broken_host")
file(COPY "${template_source}/" DESTINATION "${outside_root}/glowkit"
     PATTERN ".git" EXCLUDE)
run_cli(broken_init TRUE project init "${broken_host}")
run_cli(broken_install TRUE package install "${package_dir}" --project "${broken_host}")
file(READ "${broken_host}/packages/acme.glowkit/features/glow.json" broken_feature)
string(REPLACE "\"name\": \"acme.glowkit.glow\"," "" broken_feature "${broken_feature}")
file(WRITE "${broken_host}/packages/acme.glowkit/features/glow.json" "${broken_feature}")

run_rpc(broken "${broken_host}"
"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"list_render_features\",\"params\":{}}\n\
{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"get_render_features\",\"params\":{}}\n"
    broken_stdout)
rpc_result("damaged package entry" "${broken_stdout}" 1 broken_catalog)
catalog_entry("damaged package entry" "${broken_catalog}"
    "project://packages/acme.glowkit/features/glow.json" broken_entry)
require_field("damaged package entry" "${broken_entry}" source "package")
require_field("damaged package entry" "${broken_entry}" available OFF)
string(JSON broken_reason ERROR_VARIABLE broken_reason_error
    GET "${broken_entry}" unavailable_reason)
if(NOT broken_reason_error STREQUAL "NOTFOUND" OR broken_reason STREQUAL "")
    message(FATAL_ERROR "the damaged package entry gives no unavailable_reason\n${broken_entry}")
endif()
require_text("${broken_stdout}" "\"id\":2" "get_render_features still answers")
require_text("${broken_stdout}" "\"source_digest\"" "get_render_features still answers")

# The recorded digest no longer matches the bytes on disk, and `package list`
# is the production path that says so.
run_cli(list_modified FALSE package list --project "${broken_host}")
require_text("${list_modified_OUTPUT}" "\"status\": \"modified\"" "package list sees the edit")
require_text("${list_modified_OUTPUT}" "package_digest_mismatch" "package list sees the edit")

file(REMOVE_RECURSE "${outside_root}")
message(STATUS "X-1 package install smoke passed")
