function(pelican_collect_directory_targets directory output_variable)
  get_property(directory_targets DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)
  get_property(subdirectories DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)

  set(all_targets ${directory_targets})
  foreach(subdirectory IN LISTS subdirectories)
    pelican_collect_directory_targets("${subdirectory}" child_targets)
    list(APPEND all_targets ${child_targets})
  endforeach()

  list(REMOVE_DUPLICATES all_targets)
  set(${output_variable} "${all_targets}" PARENT_SCOPE)
endfunction()

function(_pelican_walk_link_boundary state_key current_target link_path)
  get_target_property(aliased_target "${current_target}" ALIASED_TARGET)
  if(aliased_target)
    set(current_target "${aliased_target}")
  endif()

  get_property(visited_targets GLOBAL PROPERTY "${state_key}_VISITED")
  if(current_target IN_LIST visited_targets)
    return()
  endif()
  list(APPEND visited_targets "${current_target}")
  set_property(GLOBAL PROPERTY "${state_key}_VISITED" "${visited_targets}")

  get_property(forbidden_targets GLOBAL PROPERTY "${state_key}_FORBIDDEN")
  if(current_target IN_LIST forbidden_targets)
    message(FATAL_ERROR
      "D0 link boundary violation: ${link_path} reaches forbidden target "
      "'${current_target}'")
  endif()

  set(link_entries)
  foreach(link_property IN ITEMS LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
    get_target_property(property_value "${current_target}" "${link_property}")
    if(property_value AND NOT property_value MATCHES "-NOTFOUND$")
      list(APPEND link_entries ${property_value})
    endif()
  endforeach()

  foreach(link_entry IN LISTS link_entries)
    # Reject a forbidden target even when CMake wrapped it in a generator
    # expression such as $<LINK_ONLY:pelican_core>.
    foreach(forbidden_target IN LISTS forbidden_targets)
      string(FIND "${link_entry}" "${forbidden_target}" forbidden_position)
      if(NOT forbidden_position EQUAL -1)
        message(FATAL_ERROR
          "D0 link boundary violation: ${link_path} -> ${current_target} "
          "contains forbidden link entry '${link_entry}'")
      endif()
    endforeach()

    # LINK_LIBRARIES may contain plain target names, directory-id wrappers,
    # or generator expressions. Extract every target-shaped token and follow
    # the in-build targets; imported SDK targets cannot hide an in-tree edge.
    string(REGEX MATCHALL
      "[A-Za-z0-9_.+-]+(::[A-Za-z0-9_.+-]+)*"
      candidate_targets "${link_entry}")
    foreach(candidate_target IN LISTS candidate_targets)
      if(TARGET "${candidate_target}")
        _pelican_walk_link_boundary(
          "${state_key}"
          "${candidate_target}"
          "${link_path} -> ${candidate_target}")
      endif()
    endforeach()
  endforeach()
endfunction()

function(pelican_assert_link_boundary)
  cmake_parse_arguments(
    BOUNDARY
    ""
    "TARGET"
    "REQUIRED_DIRECT_TARGETS;FORBIDDEN_TARGETS"
    ${ARGN}
  )

  if(NOT BOUNDARY_TARGET OR NOT TARGET "${BOUNDARY_TARGET}")
    message(FATAL_ERROR
      "pelican_assert_link_boundary requires an existing TARGET")
  endif()
  if(NOT BOUNDARY_FORBIDDEN_TARGETS)
    message(FATAL_ERROR
      "pelican_assert_link_boundary requires FORBIDDEN_TARGETS")
  endif()

  set(direct_link_entries)
  foreach(link_property IN ITEMS LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
    get_target_property(property_value "${BOUNDARY_TARGET}" "${link_property}")
    if(property_value AND NOT property_value MATCHES "-NOTFOUND$")
      list(APPEND direct_link_entries ${property_value})
    endif()
  endforeach()

  foreach(required_target IN LISTS BOUNDARY_REQUIRED_DIRECT_TARGETS)
    set(required_target_found OFF)
    foreach(link_entry IN LISTS direct_link_entries)
      string(FIND "${link_entry}" "${required_target}" required_position)
      if(NOT required_position EQUAL -1)
        set(required_target_found ON)
        break()
      endif()
    endforeach()
    if(NOT required_target_found)
      message(FATAL_ERROR
        "D0 link boundary violation: ${BOUNDARY_TARGET} must link "
        "'${required_target}' directly")
    endif()
  endforeach()

  get_property(boundary_counter GLOBAL PROPERTY PELICAN_LINK_BOUNDARY_COUNTER)
  if(NOT boundary_counter)
    set(boundary_counter 0)
  endif()
  math(EXPR boundary_counter "${boundary_counter} + 1")
  set_property(GLOBAL PROPERTY PELICAN_LINK_BOUNDARY_COUNTER "${boundary_counter}")
  set(state_key "PELICAN_LINK_BOUNDARY_${boundary_counter}")
  set_property(GLOBAL PROPERTY "${state_key}_VISITED" "")
  set_property(GLOBAL PROPERTY
    "${state_key}_FORBIDDEN" "${BOUNDARY_FORBIDDEN_TARGETS}")

  _pelican_walk_link_boundary(
    "${state_key}" "${BOUNDARY_TARGET}" "${BOUNDARY_TARGET}")
endfunction()
