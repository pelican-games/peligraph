if(NOT PELICAN_ARCHIVE_CONFIG STREQUAL "Debug")
  return()
endif()

foreach(required IN ITEMS TARGET BINARY PDB ROOT COMMIT MAX_GENERATIONS)
  if(NOT DEFINED PELICAN_ARCHIVE_${required} OR
     PELICAN_ARCHIVE_${required} STREQUAL "")
    message(FATAL_ERROR "Missing PELICAN_ARCHIVE_${required}")
  endif()
endforeach()
if(NOT EXISTS "${PELICAN_ARCHIVE_BINARY}")
  message(FATAL_ERROR "Crash-symbol binary does not exist: ${PELICAN_ARCHIVE_BINARY}")
endif()
if(NOT EXISTS "${PELICAN_ARCHIVE_PDB}")
  message(FATAL_ERROR "Crash-symbol PDB does not exist: ${PELICAN_ARCHIVE_PDB}")
endif()
if(NOT PELICAN_ARCHIVE_TARGET MATCHES "^[A-Za-z0-9_.-]+$")
  message(FATAL_ERROR "Invalid crash-symbol target name: ${PELICAN_ARCHIVE_TARGET}")
endif()
string(LENGTH "${PELICAN_ARCHIVE_COMMIT}" commit_length)
if(NOT commit_length EQUAL 40 OR
   NOT PELICAN_ARCHIVE_COMMIT MATCHES "^[0-9a-f]+$")
  message(FATAL_ERROR "Invalid crash-symbol commit: ${PELICAN_ARCHIVE_COMMIT}")
endif()
if(NOT PELICAN_ARCHIVE_MAX_GENERATIONS MATCHES "^[1-9][0-9]*$")
  message(FATAL_ERROR "Invalid crash-symbol generation limit")
endif()

get_filename_component(binary_name "${PELICAN_ARCHIVE_BINARY}" NAME)
set(pdb_name "${PELICAN_ARCHIVE_TARGET}-${PELICAN_ARCHIVE_COMMIT}.pdb")
file(SHA256 "${PELICAN_ARCHIVE_BINARY}" binary_sha256)
file(SHA256 "${PELICAN_ARCHIVE_PDB}" pdb_sha256)

set(target_root "${PELICAN_ARCHIVE_ROOT}/${PELICAN_ARCHIVE_TARGET}")
file(MAKE_DIRECTORY "${target_root}")
set(counter_file "${target_root}/next-generation.txt")
if(EXISTS "${counter_file}")
  file(READ "${counter_file}" next_generation)
  string(STRIP "${next_generation}" next_generation)
else()
  set(next_generation 1)
endif()
if(NOT next_generation MATCHES "^[1-9][0-9]*$")
  message(FATAL_ERROR "Invalid crash-symbol generation counter: ${next_generation}")
endif()

while(TRUE)
  set(padded_generation "0000000000${next_generation}")
  string(LENGTH "${padded_generation}" padded_length)
  math(EXPR padded_start "${padded_length} - 10")
  string(SUBSTRING "${padded_generation}" ${padded_start} 10 padded_generation)
  file(GLOB generation_collision
      "${target_root}/generation-${padded_generation}-*")
  if(NOT generation_collision)
    break()
  endif()
  math(EXPR next_generation "${next_generation} + 1")
endwhile()
math(EXPR following_generation "${next_generation} + 1")
file(WRITE "${counter_file}" "${following_generation}\n")

set(source_dirty false)
if(PELICAN_ARCHIVE_SOURCE_DIR AND PELICAN_ARCHIVE_GIT_EXECUTABLE)
  execute_process(
    COMMAND "${PELICAN_ARCHIVE_GIT_EXECUTABLE}" -C "${PELICAN_ARCHIVE_SOURCE_DIR}"
            status --porcelain --untracked-files=normal
    RESULT_VARIABLE git_status_result
    OUTPUT_VARIABLE git_status
    ERROR_VARIABLE git_status_error
    OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(NOT git_status_result EQUAL 0)
    message(FATAL_ERROR
        "Cannot inspect source state for crash-symbol archive: ${git_status_error}")
  elseif(NOT git_status STREQUAL "")
    set(source_dirty true)
  endif()
endif()
if(source_dirty)
  set(dirty_suffix "-dirty")
else()
  set(dirty_suffix "")
endif()
set(generation_dir
    "${target_root}/generation-${padded_generation}-${PELICAN_ARCHIVE_COMMIT}${dirty_suffix}")
file(MAKE_DIRECTORY "${generation_dir}")
file(COPY "${PELICAN_ARCHIVE_BINARY}" DESTINATION "${generation_dir}")
configure_file("${PELICAN_ARCHIVE_PDB}" "${generation_dir}/${pdb_name}" COPYONLY)
file(SHA256 "${generation_dir}/${binary_name}" archived_binary_sha256)
file(SHA256 "${generation_dir}/${pdb_name}" archived_pdb_sha256)
if(NOT archived_binary_sha256 STREQUAL binary_sha256 OR
   NOT archived_pdb_sha256 STREQUAL pdb_sha256)
  message(FATAL_ERROR "Crash-symbol archive verification failed: ${generation_dir}")
endif()
string(TIMESTAMP archived_at "%Y-%m-%dT%H:%M:%SZ" UTC)
file(WRITE "${generation_dir}/manifest.txt"
    "format=pelican-crash-symbols-v1\n"
    "target=${PELICAN_ARCHIVE_TARGET}\n"
    "source_commit=${PELICAN_ARCHIVE_COMMIT}\n"
    "source_dirty=${source_dirty}\n"
    "archived_at=${archived_at}\n"
    "binary_file=${binary_name}\n"
    "binary_sha256=${binary_sha256}\n"
    "pdb_file=${pdb_name}\n"
    "pdb_sha256=${pdb_sha256}\n")

file(GLOB generation_candidates LIST_DIRECTORIES TRUE
    "${target_root}/generation-*")
set(generations)
foreach(candidate IN LISTS generation_candidates)
  if(IS_DIRECTORY "${candidate}")
    list(APPEND generations "${candidate}")
  endif()
endforeach()
list(SORT generations)
list(LENGTH generations generation_count)
while(generation_count GREATER PELICAN_ARCHIVE_MAX_GENERATIONS)
  list(GET generations 0 oldest_generation)
  file(REMOVE_RECURSE "${oldest_generation}")
  list(REMOVE_AT generations 0)
  list(LENGTH generations generation_count)
endwhile()

message(STATUS "Crash symbols archived: ${generation_dir}")
