# VO-4: runs as Spout2's PATCH_COMMAND. Checks the tag still names the commit
# this repository was reviewed against, then applies the one safety patch. The
# patch step re-runs with the tag's update step, so an applied patch is left alone.
foreach(required IN ITEMS SPOUT2_SOURCE_DIR SPOUT2_COMMIT SPOUT2_PATCH)
  if(NOT DEFINED ${required})
    message(FATAL_ERROR "patch_spout2.cmake: missing ${required}")
  endif()
endforeach()
find_package(Git REQUIRED)

execute_process(
  COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD
  WORKING_DIRECTORY "${SPOUT2_SOURCE_DIR}"
  OUTPUT_VARIABLE head
  OUTPUT_STRIP_TRAILING_WHITESPACE
  RESULT_VARIABLE head_result)
if(NOT head_result EQUAL 0 OR NOT head STREQUAL SPOUT2_COMMIT)
  message(FATAL_ERROR
    "output.spout2_commit_mismatch@1: expected ${SPOUT2_COMMIT}, found ${head}")
endif()

execute_process(
  COMMAND "${GIT_EXECUTABLE}" apply --check --reverse --ignore-whitespace "${SPOUT2_PATCH}"
  WORKING_DIRECTORY "${SPOUT2_SOURCE_DIR}"
  RESULT_VARIABLE already_applied
  OUTPUT_QUIET ERROR_QUIET)
if(already_applied EQUAL 0)
  return()
endif()
execute_process(
  COMMAND "${GIT_EXECUTABLE}" apply --ignore-whitespace "${SPOUT2_PATCH}"
  WORKING_DIRECTORY "${SPOUT2_SOURCE_DIR}"
  RESULT_VARIABLE apply_result)
if(NOT apply_result EQUAL 0)
  message(FATAL_ERROR "output.spout2_patch_failed@1: ${SPOUT2_PATCH}")
endif()
