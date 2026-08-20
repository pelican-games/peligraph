include_guard(GLOBAL)

set(_PELICAN_CRASH_SYMBOL_ARCHIVE_SCRIPT
    "${CMAKE_CURRENT_LIST_DIR}/archive_crash_symbols.cmake")

function(pelican_read_git_identity)
  cmake_parse_arguments(IDENTITY "" "SOURCE_DIR;COMMIT_OUT;GIT_OUT" "" ${ARGN})
  if(NOT IDENTITY_SOURCE_DIR OR NOT IDENTITY_COMMIT_OUT OR NOT IDENTITY_GIT_OUT)
    message(FATAL_ERROR "pelican_read_git_identity requires SOURCE_DIR, COMMIT_OUT, and GIT_OUT")
  endif()

  find_package(Git REQUIRED)
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${IDENTITY_SOURCE_DIR}" rev-parse --verify HEAD
    RESULT_VARIABLE git_commit_result
    OUTPUT_VARIABLE git_commit
    ERROR_VARIABLE git_commit_error
    OUTPUT_STRIP_TRAILING_WHITESPACE
  )
  string(LENGTH "${git_commit}" git_commit_length)
  if(NOT git_commit_result EQUAL 0 OR
     NOT git_commit_length EQUAL 40 OR
     NOT git_commit MATCHES "^[0-9a-fA-F]+$")
    message(FATAL_ERROR
        "Cannot identify the source commit for crash symbols: ${git_commit_error}")
  endif()
  string(TOLOWER "${git_commit}" git_commit)

  execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${IDENTITY_SOURCE_DIR}" rev-parse --git-path HEAD
    RESULT_VARIABLE git_head_result
    OUTPUT_VARIABLE git_head_path
    OUTPUT_STRIP_TRAILING_WHITESPACE
  )
  if(NOT git_head_result EQUAL 0)
    message(FATAL_ERROR "Cannot locate Git HEAD for crash-symbol reconfiguration")
  endif()
  set(git_identity_dependencies "${git_head_path}")
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${IDENTITY_SOURCE_DIR}" symbolic-ref -q HEAD
    RESULT_VARIABLE git_ref_result
    OUTPUT_VARIABLE git_ref
    OUTPUT_STRIP_TRAILING_WHITESPACE
  )
  if(git_ref_result EQUAL 0)
    execute_process(
      COMMAND "${GIT_EXECUTABLE}" -C "${IDENTITY_SOURCE_DIR}"
              rev-parse --git-path "${git_ref}"
      OUTPUT_VARIABLE git_ref_path
      OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    list(APPEND git_identity_dependencies "${git_ref_path}")
  endif()
  foreach(git_dependency IN LISTS git_identity_dependencies)
    get_filename_component(git_dependency "${git_dependency}" ABSOLUTE
                           BASE_DIR "${IDENTITY_SOURCE_DIR}")
    set_property(DIRECTORY "${IDENTITY_SOURCE_DIR}" APPEND PROPERTY
                 CMAKE_CONFIGURE_DEPENDS "${git_dependency}")
  endforeach()

  set(${IDENTITY_COMMIT_OUT} "${git_commit}" PARENT_SCOPE)
  set(${IDENTITY_GIT_OUT} "${GIT_EXECUTABLE}" PARENT_SCOPE)
endfunction()

function(_pelican_path_is_within candidate parent result_out)
  get_filename_component(candidate_abs "${candidate}" ABSOLUTE)
  get_filename_component(parent_abs "${parent}" ABSOLUTE)
  file(TO_CMAKE_PATH "${candidate_abs}" candidate_abs)
  file(TO_CMAKE_PATH "${parent_abs}" parent_abs)
  string(REGEX REPLACE "/+$" "" candidate_abs "${candidate_abs}")
  string(REGEX REPLACE "/+$" "" parent_abs "${parent_abs}")
  if(WIN32)
    string(TOLOWER "${candidate_abs}" candidate_abs)
    string(TOLOWER "${parent_abs}" parent_abs)
  endif()
  string(FIND "${candidate_abs}/" "${parent_abs}/" parent_prefix)
  if(parent_prefix EQUAL 0)
    set(${result_out} TRUE PARENT_SCOPE)
  else()
    set(${result_out} FALSE PARENT_SCOPE)
  endif()
endfunction()

function(pelican_preserve_crash_symbols)
  cmake_parse_arguments(
    ARCHIVE ""
    "TARGET;ARCHIVE_ROOT;SOURCE_COMMIT;SOURCE_DIR;GIT_EXECUTABLE;MAX_GENERATIONS"
    "" ${ARGN})
  foreach(required IN ITEMS TARGET ARCHIVE_ROOT SOURCE_COMMIT MAX_GENERATIONS)
    if(NOT DEFINED ARCHIVE_${required} OR ARCHIVE_${required} STREQUAL "")
      message(FATAL_ERROR "pelican_preserve_crash_symbols requires ${required}")
    endif()
  endforeach()
  if(NOT MSVC)
    message(FATAL_ERROR "pelican_preserve_crash_symbols requires MSVC linker PDBs")
  endif()
  if(NOT TARGET ${ARCHIVE_TARGET})
    message(FATAL_ERROR "Unknown crash-symbol target: ${ARCHIVE_TARGET}")
  endif()

  string(LENGTH "${ARCHIVE_SOURCE_COMMIT}" commit_length)
  if(NOT commit_length EQUAL 40 OR
     NOT ARCHIVE_SOURCE_COMMIT MATCHES "^[0-9a-fA-F]+$")
    message(FATAL_ERROR "SOURCE_COMMIT must be a full 40-digit hexadecimal commit")
  endif()
  string(TOLOWER "${ARCHIVE_SOURCE_COMMIT}" source_commit)
  if(NOT ARCHIVE_MAX_GENERATIONS MATCHES "^[1-9][0-9]*$")
    message(FATAL_ERROR "MAX_GENERATIONS must be a positive integer")
  endif()

  get_filename_component(archive_root "${ARCHIVE_ARCHIVE_ROOT}" ABSOLUTE
                         BASE_DIR "${CMAKE_SOURCE_DIR}")
  foreach(forbidden_root IN ITEMS
      "${CMAKE_BINARY_DIR}"
      "${CMAKE_SOURCE_DIR}/dist"
      "${CMAKE_SOURCE_DIR}/dist_debug")
    _pelican_path_is_within("${archive_root}" "${forbidden_root}" archive_is_forbidden)
    if(archive_is_forbidden)
      message(FATAL_ERROR
          "Crash-symbol archive must be outside build outputs: ${archive_root}")
    endif()
  endforeach()

  if((ARCHIVE_SOURCE_DIR AND NOT ARCHIVE_GIT_EXECUTABLE) OR
     (ARCHIVE_GIT_EXECUTABLE AND NOT ARCHIVE_SOURCE_DIR))
    message(FATAL_ERROR "SOURCE_DIR and GIT_EXECUTABLE must be provided together")
  endif()
  # Keep the established dist_debug PDB filename, but make the CodeView record
  # carried by a crash dump identify the full source commit.
  target_link_options(${ARCHIVE_TARGET} PRIVATE
      "$<$<CONFIG:Debug>:/PDBALTPATH:${ARCHIVE_TARGET}-${source_commit}.pdb>")
  add_custom_command(TARGET ${ARCHIVE_TARGET} POST_BUILD
    COMMAND "${CMAKE_COMMAND}"
      "-DPELICAN_ARCHIVE_CONFIG=$<CONFIG>"
      "-DPELICAN_ARCHIVE_TARGET=${ARCHIVE_TARGET}"
      "-DPELICAN_ARCHIVE_BINARY=$<TARGET_FILE:${ARCHIVE_TARGET}>"
      "-DPELICAN_ARCHIVE_PDB=$<TARGET_PDB_FILE:${ARCHIVE_TARGET}>"
      "-DPELICAN_ARCHIVE_ROOT=${archive_root}"
      "-DPELICAN_ARCHIVE_COMMIT=${source_commit}"
      "-DPELICAN_ARCHIVE_SOURCE_DIR=${ARCHIVE_SOURCE_DIR}"
      "-DPELICAN_ARCHIVE_GIT_EXECUTABLE=${ARCHIVE_GIT_EXECUTABLE}"
      "-DPELICAN_ARCHIVE_MAX_GENERATIONS=${ARCHIVE_MAX_GENERATIONS}"
      -P "${_PELICAN_CRASH_SYMBOL_ARCHIVE_SCRIPT}"
    COMMENT "Preserving ${ARCHIVE_TARGET} crash symbols"
    VERBATIM
  )
endfunction()
