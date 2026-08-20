foreach(required IN ITEMS FIXTURE_SOURCE MODULE WORK_ROOT GENERATOR)
  if(NOT DEFINED TEST_${required} OR TEST_${required} STREQUAL "")
    message(FATAL_ERROR "Missing TEST_${required}")
  endif()
endforeach()
get_filename_component(work_name "${TEST_WORK_ROOT}" NAME)
if(NOT work_name MATCHES "^pelican_wp333_[0-9a-f]+$")
  message(FATAL_ERROR "Refusing unsafe test work root: ${TEST_WORK_ROOT}")
endif()

set(test_commit "0123456789abcdef0123456789abcdef01234567")
set(generation_limit 2)
set(negative_build "${TEST_WORK_ROOT}/negative-build")
set(positive_build "${TEST_WORK_ROOT}/positive-build")
set(archive_root "${TEST_WORK_ROOT}/archive")
file(REMOVE_RECURSE "${TEST_WORK_ROOT}")
file(MAKE_DIRECTORY "${TEST_WORK_ROOT}")

function(configure_and_build build_dir archive_enabled marker)
  set(configure_command
      "${CMAKE_COMMAND}"
      -S "${TEST_FIXTURE_SOURCE}"
      -B "${build_dir}"
      -G "${TEST_GENERATOR}"
      "-DBUILD_MARKER=${marker}"
      "-DARCHIVE_ENABLED=${archive_enabled}"
      "-DARCHIVE_ROOT=${archive_root}"
      "-DMODULE_PATH=${TEST_MODULE}"
      "-DSOURCE_COMMIT=${test_commit}"
      "-DMAX_GENERATIONS=${generation_limit}")
  if(DEFINED TEST_GENERATOR_PLATFORM AND NOT TEST_GENERATOR_PLATFORM STREQUAL "")
    list(APPEND configure_command -A "${TEST_GENERATOR_PLATFORM}")
  endif()
  if(DEFINED TEST_GENERATOR_TOOLSET AND NOT TEST_GENERATOR_TOOLSET STREQUAL "")
    list(APPEND configure_command -T "${TEST_GENERATOR_TOOLSET}")
  endif()
  execute_process(
      COMMAND ${configure_command}
      RESULT_VARIABLE configure_result
      OUTPUT_VARIABLE configure_output
      ERROR_VARIABLE configure_error)
  if(NOT configure_result EQUAL 0)
    message(FATAL_ERROR
        "Fixture configure failed (${marker}):\n${configure_output}\n${configure_error}")
  endif()
  execute_process(
      COMMAND "${CMAKE_COMMAND}" --build "${build_dir}" --config Debug
      RESULT_VARIABLE build_result
      OUTPUT_VARIABLE build_output
      ERROR_VARIABLE build_error)
  if(NOT build_result EQUAL 0)
    message(FATAL_ERROR
        "Fixture build failed (${marker}):\n${build_output}\n${build_error}")
  endif()
endfunction()

function(require_hash_changed first_path first_hash second_path label)
  if(NOT EXISTS "${second_path}")
    message(FATAL_ERROR "${label} is missing: ${second_path}")
  endif()
  file(SHA256 "${second_path}" second_hash)
  if(second_hash STREQUAL first_hash)
    message(FATAL_ERROR "${label} did not change across consecutive builds")
  endif()
endfunction()

function(find_archived_pair binary_hash pdb_hash result_out)
  file(GLOB manifests
      "${archive_root}/pelican_symbol_probe/generation-*/manifest.txt")
  set(matches)
  foreach(manifest IN LISTS manifests)
    file(READ "${manifest}" contents)
    string(FIND "${contents}" "binary_sha256=${binary_hash}\n" binary_match)
    string(FIND "${contents}" "pdb_sha256=${pdb_hash}\n" pdb_match)
    if(NOT binary_match EQUAL -1 AND NOT pdb_match EQUAL -1)
      list(APPEND matches "${manifest}")
    endif()
  endforeach()
  list(LENGTH matches match_count)
  if(match_count GREATER 1)
    message(FATAL_ERROR "An archived binary/PDB pair is not unique")
  elseif(match_count EQUAL 1)
    list(GET matches 0 match)
    set(${result_out} "${match}" PARENT_SCOPE)
  else()
    set(${result_out} "" PARENT_SCOPE)
  endif()
endfunction()

function(require_generation_count expected)
  file(GLOB manifests
      "${archive_root}/pelican_symbol_probe/generation-*/manifest.txt")
  list(LENGTH manifests actual)
  if(NOT actual EQUAL expected)
    message(FATAL_ERROR
        "Expected ${expected} retained generations, found ${actual}")
  endif()
endfunction()

# Negative control: the second dist_debug link overwrites the first PDB, and
# no copy with the first PDB's identity remains anywhere in dist_debug.
configure_and_build("${negative_build}" OFF first)
set(negative_exe "${negative_build}/dist_debug/pelican_symbol_probe.exe")
set(negative_pdb "${negative_build}/dist_debug/pelican_symbol_probe.pdb")
file(SHA256 "${negative_exe}" negative_first_exe_hash)
file(SHA256 "${negative_pdb}" negative_first_pdb_hash)
configure_and_build("${negative_build}" OFF second)
require_hash_changed("${negative_exe}" "${negative_first_exe_hash}"
                     "${negative_exe}" "negative-control executable")
require_hash_changed("${negative_pdb}" "${negative_first_pdb_hash}"
                     "${negative_pdb}" "negative-control PDB")
file(GLOB negative_current_pdbs "${negative_build}/dist_debug/*.pdb")
foreach(pdb IN LISTS negative_current_pdbs)
  file(SHA256 "${pdb}" current_hash)
  if(current_hash STREQUAL negative_first_pdb_hash)
    message(FATAL_ERROR "Negative control unexpectedly retained the first PDB")
  endif()
endforeach()

# Enabled path: after the same two consecutive dist_debug builds, the first
# exact executable/PDB pair remains and its PDB name identifies one full commit.
configure_and_build("${positive_build}" ON first)
set(positive_exe "${positive_build}/dist_debug/pelican_symbol_probe.exe")
set(positive_pdb "${positive_build}/dist_debug/pelican_symbol_probe.pdb")
if(EXISTS "${positive_build}/dist_debug/pelican_symbol_probe-${test_commit}.pdb")
  message(FATAL_ERROR "Enabled archive changed the established dist_debug PDB name")
endif()
file(SHA256 "${positive_exe}" first_exe_hash)
file(SHA256 "${positive_pdb}" first_pdb_hash)
configure_and_build("${positive_build}" ON second)
file(SHA256 "${positive_exe}" second_exe_hash)
file(SHA256 "${positive_pdb}" second_pdb_hash)
if(second_exe_hash STREQUAL first_exe_hash OR second_pdb_hash STREQUAL first_pdb_hash)
  message(FATAL_ERROR "Enabled fixture did not produce a distinct second build")
endif()
find_archived_pair("${first_exe_hash}" "${first_pdb_hash}" first_manifest)
if(first_manifest STREQUAL "")
  message(FATAL_ERROR "The first binary's matching PDB was not retained")
endif()
file(READ "${first_manifest}" first_manifest_contents)
string(FIND "${first_manifest_contents}"
       "source_commit=${test_commit}\n" commit_match)
if(commit_match EQUAL -1)
  message(FATAL_ERROR "The retained PDB does not resolve to the expected commit")
endif()
get_filename_component(first_generation "${first_manifest}" DIRECTORY)
set(first_archived_pdb
    "${first_generation}/pelican_symbol_probe-${test_commit}.pdb")
set(first_archived_exe "${first_generation}/pelican_symbol_probe.exe")
if(NOT EXISTS "${first_archived_pdb}" OR NOT EXISTS "${first_archived_exe}")
  message(FATAL_ERROR "The retained executable/PDB pair is incomplete")
endif()
file(STRINGS "${first_archived_exe}" embedded_pdb_names
     REGEX "pelican_symbol_probe-${test_commit}\\.pdb")
if(NOT embedded_pdb_names)
  message(FATAL_ERROR "The executable's dump-visible PDB name lacks its commit")
endif()
require_generation_count(2)

# The archive is a sibling of the fixture build, never a child of it.
get_filename_component(archive_abs "${archive_root}" ABSOLUTE)
get_filename_component(build_abs "${positive_build}" ABSOLUTE)
file(TO_CMAKE_PATH "${archive_abs}" archive_abs)
file(TO_CMAKE_PATH "${build_abs}" build_abs)
string(TOLOWER "${archive_abs}/" archive_compare)
string(TOLOWER "${build_abs}/" build_compare)
string(FIND "${archive_compare}" "${build_compare}" archive_in_build)
if(archive_in_build EQUAL 0)
  message(FATAL_ERROR "Crash-symbol archive is inside the build directory")
endif()

# A third build exceeds the limit of two: generation one is removed while the
# two newest exact pairs remain retrievable.
configure_and_build("${positive_build}" ON third)
file(SHA256 "${positive_exe}" third_exe_hash)
file(SHA256 "${positive_pdb}" third_pdb_hash)
require_generation_count(2)
find_archived_pair("${first_exe_hash}" "${first_pdb_hash}" first_after_prune)
find_archived_pair("${second_exe_hash}" "${second_pdb_hash}" second_after_prune)
find_archived_pair("${third_exe_hash}" "${third_pdb_hash}" third_after_prune)
if(NOT first_after_prune STREQUAL "" OR
   second_after_prune STREQUAL "" OR
   third_after_prune STREQUAL "")
  message(FATAL_ERROR "Generation limit did not prune only the oldest pair")
endif()

message(STATUS "WP333 crash-symbol archive contract: PASS")
