if(NOT DEFINED BATTERY_EMBED_SOURCE_DIR)
  message(FATAL_ERROR
    "BATTERY_EMBED_SOURCE_DIR is required")
endif()

set(battery_embed_cmake
  "${BATTERY_EMBED_SOURCE_DIR}/CMakeLists.txt")
if(NOT EXISTS "${battery_embed_cmake}")
  message(FATAL_ERROR
    "battery-embed CMakeLists.txt was not found: ${battery_embed_cmake}")
endif()

file(READ "${battery_embed_cmake}" contents)
set(contents_changed FALSE)

set(binary_dir_declaration
  [=[set(EMBED_BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR}/embed CACHE INTERNAL "binary directory of the battery::embed library" FORCE)]=])
set(content_aware_writer
  [=[

function(_embed_write_if_different output_path content)
    if (EXISTS "${output_path}")
        file(READ "${output_path}" existing_content)
        if (existing_content STREQUAL content)
            return()
        endif()
    endif()
    file(WRITE "${output_path}" "${content}")
endfunction()
]=])
string(FIND "${contents}"
  "function(_embed_write_if_different"
  writer_already_patched)
if(writer_already_patched EQUAL -1)
  string(FIND "${contents}" "${binary_dir_declaration}"
    writer_patch_location)
  if(writer_patch_location EQUAL -1)
    message(FATAL_ERROR
      "battery-embed v1.2.19 binary-dir declaration changed; "
      "review the low-write patch before updating the dependency")
  endif()
  string(REPLACE
    "${binary_dir_declaration}"
    "${binary_dir_declaration}${content_aware_writer}"
    contents "${contents}")
  set(contents_changed TRUE)
endif()

set(old_source_template_write
  [=[file(WRITE ${EMBED_BINARY_DIR}/embed_source_file_template.cpp "${EMBED_SOURCE_FILE_TEMPLATE}")]=])
set(new_source_template_write
  [=[_embed_write_if_different("${EMBED_BINARY_DIR}/embed_source_file_template.cpp" "${EMBED_SOURCE_FILE_TEMPLATE}")]=])
set(old_impl_write
  [=[file(WRITE ${EMBED_BINARY_DIR}/embed_impl.cpp "${EMBED_MASTER_SOURCE_FILE}")]=])
set(new_impl_write
  [=[_embed_write_if_different("${EMBED_BINARY_DIR}/embed_impl.cpp" "${EMBED_MASTER_SOURCE_FILE}")]=])
set(old_header_template_write
  [=[file(WRITE ${EMBED_BINARY_DIR}/embed_header_file_template.hpp "${EMBED_HEADER_FILE}")]=])
set(new_header_template_write
  [=[_embed_write_if_different("${EMBED_BINARY_DIR}/embed_header_file_template.hpp" "${EMBED_HEADER_FILE}")]=])
set(old_generator_write
  [=[file(WRITE ${EMBED_BINARY_DIR}/generate.cmake ${EMBED_GENERATE_SCRIPT})]=])
set(new_generator_write
  [=[_embed_write_if_different("${EMBED_BINARY_DIR}/generate.cmake" "${EMBED_GENERATE_SCRIPT}")]=])

foreach(write_kind IN ITEMS
    source_template impl header_template generator)
  set(old_write_variable "old_${write_kind}_write")
  set(new_write_variable "new_${write_kind}_write")
  string(FIND "${contents}" "${${new_write_variable}}"
    write_already_patched)
  if(write_already_patched EQUAL -1)
    string(FIND "${contents}" "${${old_write_variable}}"
      write_patch_location)
    if(write_patch_location EQUAL -1)
      message(FATAL_ERROR
        "battery-embed v1.2.19 ${write_kind} write changed; "
        "review the low-write patch before updating the dependency")
    endif()
    string(REPLACE
      "${${old_write_variable}}"
      "${${new_write_variable}}"
      contents "${contents}")
    set(contents_changed TRUE)
  endif()
endforeach()

set(old_target_loop
  [=[    foreach (TARGET ${EMBED_TARGETS})]=])
set(new_target_loop
  [=[    set(embed_targets ${EMBED_TARGETS})
    list(REMOVE_DUPLICATES embed_targets)
    foreach (TARGET IN LISTS embed_targets)]=])
string(FIND "${contents}" "${new_target_loop}"
  target_loop_already_patched)
if(target_loop_already_patched EQUAL -1)
  string(FIND "${contents}" "${old_target_loop}"
    target_loop_patch_location)
  if(target_loop_patch_location EQUAL -1)
    message(FATAL_ERROR
      "battery-embed v1.2.19 target loop changed; "
      "review the low-write patch before updating the dependency")
  endif()
  string(REPLACE "${old_target_loop}" "${new_target_loop}"
    contents "${contents}")
  set(contents_changed TRUE)
endif()

set(old_write
  [=[file(WRITE "${EMBED_HPP}" "${EMBED_HEADER_FILE_GENERATED}")]=])
set(new_write
  [=[file(GENERATE OUTPUT "${EMBED_HPP}" CONTENT "${EMBED_HEADER_FILE_GENERATED}")]=])
string(FIND "${contents}" "${new_write}" already_patched)
if(already_patched EQUAL -1)
  string(FIND "${contents}" "${old_write}" patch_location)
  if(patch_location EQUAL -1)
    message(FATAL_ERROR
      "battery-embed v1.2.19 header generation hook changed; "
      "review the low-write patch before updating the dependency")
  endif()
  string(REPLACE "${old_write}" "${new_write}"
    contents "${contents}")
  set(contents_changed TRUE)
endif()

set(old_generated_source_dependencies
  [=[DEPENDS "${FULL_PATH}" "${EMBED_HPP}" "${EMBED_BINARY_DIR}/generate.cmake" "${EMBED_BINARY_DIR}/embed_source_file_template.cpp"]=])
set(new_generated_source_dependencies
  [=[DEPENDS "${FULL_PATH}" "${EMBED_BINARY_DIR}/generate.cmake" "${EMBED_BINARY_DIR}/embed_source_file_template.cpp"]=])
string(FIND "${contents}"
  "${new_generated_source_dependencies}"
  dependencies_already_patched)
if(dependencies_already_patched EQUAL -1)
  string(FIND "${contents}"
    "${old_generated_source_dependencies}"
    dependencies_patch_location)
  if(dependencies_patch_location EQUAL -1)
    message(FATAL_ERROR
      "battery-embed v1.2.19 generated-source dependencies changed; "
      "review the low-write patch before updating the dependency")
  endif()
  string(REPLACE
    "${old_generated_source_dependencies}"
    "${new_generated_source_dependencies}"
    contents "${contents}")
  set(contents_changed TRUE)
endif()

if(contents_changed)
  file(WRITE "${battery_embed_cmake}" "${contents}")
endif()
