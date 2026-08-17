include_guard(GLOBAL)

include(CMakeDependentOption)

# The feature validation registry is the single source for CMake option
# defaults, the development ledger, and the build-unit contrast matrix.
# BASELINE is the configuration against which a WP contrast is built.  It is
# also the option default unless OPTION_DEFAULT is explicitly supplied (the
# SPIR-V linker is intentionally enabled by the development baseline while
# remaining opt-in for ordinary consumers).
macro(pelican_register_feature)
  cmake_parse_arguments(
    PELICAN_FEATURE
    ""
    "NAME;DESCRIPTION;BASELINE;CONTRAST;OPTION_DEFAULT;KIND;CONDITION;CONDITION_VALUE;CONDITION_FALLBACK;SMOKE_ID"
    "FORCES_ON_CONTRAST"
    ${ARGN}
  )

  if(NOT PELICAN_FEATURE_NAME)
    message(FATAL_ERROR
      "pelican.feature_registry.invalid_declaration@1: missing NAME")
  endif()
  if(PELICAN_FEATURE_NAME IN_LIST PELICAN_FEATURE_REGISTRY_NAMES)
    message(FATAL_ERROR
      "pelican.feature_registry.duplicate_declaration@1: ${PELICAN_FEATURE_NAME}")
  endif()
  foreach(required_field IN ITEMS DESCRIPTION BASELINE CONTRAST SMOKE_ID)
    if(NOT DEFINED PELICAN_FEATURE_${required_field} OR
       "${PELICAN_FEATURE_${required_field}}" STREQUAL "")
      message(FATAL_ERROR
        "pelican.feature_registry.invalid_declaration@1: "
        "${PELICAN_FEATURE_NAME} is missing ${required_field}")
    endif()
  endforeach()
  foreach(boolean_field IN ITEMS BASELINE CONTRAST)
    if(NOT PELICAN_FEATURE_${boolean_field} MATCHES "^(ON|OFF)$")
      message(FATAL_ERROR
        "pelican.feature_registry.invalid_boolean@1: "
        "${PELICAN_FEATURE_NAME}.${boolean_field}="
        "${PELICAN_FEATURE_${boolean_field}}")
    endif()
  endforeach()
  if(PELICAN_FEATURE_BASELINE STREQUAL PELICAN_FEATURE_CONTRAST)
    message(FATAL_ERROR
      "pelican.feature_registry.identical_contrast@1: ${PELICAN_FEATURE_NAME}")
  endif()

  if(NOT PELICAN_FEATURE_KIND)
    set(PELICAN_FEATURE_KIND OPTION)
  endif()
  if(NOT PELICAN_FEATURE_KIND MATCHES "^(OPTION|DEPENDENT_OPTION)$")
    message(FATAL_ERROR
      "pelican.feature_registry.invalid_kind@1: "
      "${PELICAN_FEATURE_NAME}=${PELICAN_FEATURE_KIND}")
  endif()
  if(PELICAN_FEATURE_KIND STREQUAL "DEPENDENT_OPTION" AND
     NOT PELICAN_FEATURE_CONDITION)
    message(FATAL_ERROR
      "pelican.feature_registry.missing_condition@1: ${PELICAN_FEATURE_NAME}")
  endif()
  if(NOT PELICAN_FEATURE_KIND STREQUAL "DEPENDENT_OPTION" AND
     PELICAN_FEATURE_CONDITION)
    message(FATAL_ERROR
      "pelican.feature_registry.unexpected_condition@1: ${PELICAN_FEATURE_NAME}")
  endif()

  if(NOT DEFINED PELICAN_FEATURE_OPTION_DEFAULT OR
     "${PELICAN_FEATURE_OPTION_DEFAULT}" STREQUAL "")
    set(PELICAN_FEATURE_OPTION_DEFAULT "${PELICAN_FEATURE_BASELINE}")
  endif()
  if(NOT PELICAN_FEATURE_OPTION_DEFAULT MATCHES "^(ON|OFF)$")
    message(FATAL_ERROR
      "pelican.feature_registry.invalid_boolean@1: "
      "${PELICAN_FEATURE_NAME}.OPTION_DEFAULT=${PELICAN_FEATURE_OPTION_DEFAULT}")
  endif()
  if(NOT PELICAN_FEATURE_CONDITION_VALUE)
    set(PELICAN_FEATURE_CONDITION_VALUE ON)
  endif()
  if(NOT DEFINED PELICAN_FEATURE_CONDITION_FALLBACK OR
     "${PELICAN_FEATURE_CONDITION_FALLBACK}" STREQUAL "")
    set(PELICAN_FEATURE_CONDITION_FALLBACK "${PELICAN_FEATURE_BASELINE}")
  endif()
  foreach(boolean_field IN ITEMS CONDITION_VALUE CONDITION_FALLBACK)
    if(NOT PELICAN_FEATURE_${boolean_field} MATCHES "^(ON|OFF)$")
      message(FATAL_ERROR
        "pelican.feature_registry.invalid_boolean@1: "
        "${PELICAN_FEATURE_NAME}.${boolean_field}="
        "${PELICAN_FEATURE_${boolean_field}}")
    endif()
  endforeach()

  list(APPEND PELICAN_FEATURE_REGISTRY_NAMES "${PELICAN_FEATURE_NAME}")
  foreach(field IN ITEMS
      DESCRIPTION BASELINE CONTRAST OPTION_DEFAULT KIND CONDITION
      CONDITION_VALUE CONDITION_FALLBACK SMOKE_ID FORCES_ON_CONTRAST)
    set("PELICAN_FEATURE_${PELICAN_FEATURE_NAME}_${field}"
        "${PELICAN_FEATURE_${field}}")
  endforeach()
endmacro()

pelican_register_feature(
  NAME PELICAN_RUNTIME_SHADER_COMPILER
  DESCRIPTION "Enable runtime GLSL shader compilation with shaderc"
  BASELINE ON
  CONTRAST OFF
  SMOKE_ID runtime-shader-compiler
)
pelican_register_feature(
  NAME PELICAN_WITH_SPIRV_LINK
  DESCRIPTION "Enable the experimental pinned SPIRV-Tools linker backend"
  BASELINE ON
  CONTRAST OFF
  OPTION_DEFAULT OFF
  SMOKE_ID spirv-link
)
pelican_register_feature(
  NAME PELICAN_WITH_AUDIO
  DESCRIPTION "Enable WAV sound-effect playback with miniaudio"
  BASELINE ON
  CONTRAST OFF
  SMOKE_ID audio
)
pelican_register_feature(
  NAME PELICAN_WITH_VAT
  DESCRIPTION "Enable pelican.vat GLB playback support"
  BASELINE ON
  CONTRAST OFF
  SMOKE_ID vat
)
pelican_register_feature(
  NAME PELICAN_WITH_EXR
  DESCRIPTION "Enable EXR texture loading with tinyexr"
  BASELINE ON
  CONTRAST OFF
  SMOKE_ID exr
)
pelican_register_feature(
  NAME PELICAN_WITH_RPC
  DESCRIPTION "Enable stdio JSON-RPC control server"
  BASELINE ON
  CONTRAST OFF
  SMOKE_ID rpc
)
pelican_register_feature(
  NAME PELICAN_WITH_SEQPLAYER
  DESCRIPTION "Enable transform sequence playback"
  BASELINE ON
  CONTRAST OFF
  SMOKE_ID seqplayer
)
pelican_register_feature(
  NAME PELICAN_WITH_IMGUI
  DESCRIPTION "Enable the interactive engine developer UI"
  BASELINE ON
  CONTRAST OFF
  SMOKE_ID imgui
)
pelican_register_feature(
  NAME PELICAN_WITH_PHYSICS
  DESCRIPTION "Enable the physics query service and collider world"
  BASELINE ON
  CONTRAST OFF
  SMOKE_ID physics
)
pelican_register_feature(
  NAME PELICAN_WITH_OPENXR
  DESCRIPTION "Enable the private OpenXR runtime unit"
  BASELINE ON
  CONTRAST OFF
  SMOKE_ID openxr
)
pelican_register_feature(
  NAME PELICAN_WITH_RENDERDOC
  DESCRIPTION "Enable passive RenderDoc in-application capture integration"
  BASELINE ON
  CONTRAST OFF
  SMOKE_ID renderdoc
)
pelican_register_feature(
  NAME PELICAN_WITH_STANDARD_RENDER_ALGORITHMS
  DESCRIPTION "Build Pelican's replaceable standard render algorithm package"
  BASELINE ON
  CONTRAST OFF
  SMOKE_ID standard-render-algorithms
)
pelican_register_feature(
  NAME PELICAN_WITH_JOLT_PHYSICS
  DESCRIPTION "Enable the optional Jolt query provider"
  BASELINE OFF
  CONTRAST ON
  KIND DEPENDENT_OPTION
  CONDITION PELICAN_WITH_PHYSICS
  FORCES_ON_CONTRAST PELICAN_WITH_BUILTIN_PHYSICS=OFF
  SMOKE_ID jolt-physics
)
pelican_register_feature(
  NAME PELICAN_WITH_BUILTIN_PHYSICS
  DESCRIPTION "Enable Pelican's built-in sphere/box/capsule query provider"
  BASELINE ON
  CONTRAST OFF
  KIND DEPENDENT_OPTION
  CONDITION PELICAN_WITH_PHYSICS
  CONDITION_FALLBACK OFF
  SMOKE_ID builtin-physics
)
pelican_register_feature(
  NAME SKIP_DEVSTUDIO
  DESCRIPTION "Skip the Qt-based Pelican Studio targets"
  BASELINE OFF
  CONTRAST ON
  SMOKE_ID skip-devstudio
)

function(pelican_validate_feature_registry)
  set(smoke_ids)
  foreach(feature_name IN LISTS PELICAN_FEATURE_REGISTRY_NAMES)
    set(smoke_id "${PELICAN_FEATURE_${feature_name}_SMOKE_ID}")
    if(smoke_id IN_LIST smoke_ids)
      message(FATAL_ERROR
        "pelican.feature_registry.duplicate_smoke_id@1: ${smoke_id}")
    endif()
    list(APPEND smoke_ids "${smoke_id}")

    set(condition "${PELICAN_FEATURE_${feature_name}_CONDITION}")
    if(condition)
      if(NOT condition IN_LIST PELICAN_FEATURE_REGISTRY_NAMES)
        message(FATAL_ERROR
          "pelican.feature_registry.unknown_condition@1: "
          "${feature_name} -> ${condition}")
      endif()
      set(condition_baseline "${PELICAN_FEATURE_${condition}_BASELINE}")
      set(condition_value "${PELICAN_FEATURE_${feature_name}_CONDITION_VALUE}")
      if(NOT condition_baseline STREQUAL condition_value)
        message(FATAL_ERROR
          "pelican.feature_registry.invalid_baseline_dependency@1: "
          "${feature_name} requires ${condition}=${condition_value}, "
          "baseline is ${condition_baseline}")
      endif()
    endif()

    foreach(force_assignment IN LISTS
        PELICAN_FEATURE_${feature_name}_FORCES_ON_CONTRAST)
      if(NOT force_assignment MATCHES "^([A-Z][A-Z0-9_]*)=(ON|OFF)$")
        message(FATAL_ERROR
          "pelican.feature_registry.invalid_force@1: "
          "${feature_name} -> ${force_assignment}")
      endif()
      set(forced_name "${CMAKE_MATCH_1}")
      if(NOT forced_name IN_LIST PELICAN_FEATURE_REGISTRY_NAMES)
        message(FATAL_ERROR
          "pelican.feature_registry.unknown_forced_feature@1: "
          "${feature_name} -> ${forced_name}")
      endif()
    endforeach()
  endforeach()
endfunction()

function(pelican_declare_feature_options)
  pelican_validate_feature_registry()

  foreach(feature_name IN LISTS PELICAN_FEATURE_REGISTRY_NAMES)
    set(description "${PELICAN_FEATURE_${feature_name}_DESCRIPTION}")
    set(option_default "${PELICAN_FEATURE_${feature_name}_OPTION_DEFAULT}")
    set(kind "${PELICAN_FEATURE_${feature_name}_KIND}")
    if(kind STREQUAL "DEPENDENT_OPTION")
      set(condition "${PELICAN_FEATURE_${feature_name}_CONDITION}")
      set(condition_value
          "${PELICAN_FEATURE_${feature_name}_CONDITION_VALUE}")
      set(condition_fallback
          "${PELICAN_FEATURE_${feature_name}_CONDITION_FALLBACK}")
      if(condition_value STREQUAL "ON")
        set(condition_expression "${condition}")
      else()
        set(condition_expression "NOT ${condition}")
      endif()
      cmake_dependent_option(
        "${feature_name}"
        "${description}"
        "${option_default}"
        "${condition_expression}"
        "${condition_fallback}"
      )
    else()
      option("${feature_name}" "${description}" "${option_default}")
    endif()
    # cmake_dependent_option uses a normal variable for its fallback.  Export
    # every resolved value so the directory calling this function sees the
    # same value whether the dependency is enabled or disabled.
    set("${feature_name}" "${${feature_name}}" PARENT_SCOPE)
  endforeach()

  foreach(feature_name IN LISTS PELICAN_FEATURE_REGISTRY_NAMES)
    set(contrast "${PELICAN_FEATURE_${feature_name}_CONTRAST}")
    if("${${feature_name}}" STREQUAL contrast)
      foreach(force_assignment IN LISTS
          PELICAN_FEATURE_${feature_name}_FORCES_ON_CONTRAST)
        string(REPLACE "=" ";" force_parts "${force_assignment}")
        list(GET force_parts 0 forced_name)
        list(GET force_parts 1 forced_value)
        if(NOT "${${forced_name}}" STREQUAL forced_value)
          message(STATUS
            "${feature_name}=${contrast}; forcing "
            "${forced_name}=${forced_value}")
          set("${forced_name}" "${forced_value}" CACHE BOOL
              "${PELICAN_FEATURE_${forced_name}_DESCRIPTION}" FORCE)
          set("${forced_name}" "${forced_value}")
          set("${forced_name}" "${forced_value}" PARENT_SCOPE)
        endif()
      endforeach()
    endif()
  endforeach()
endfunction()

function(pelican_feature_contrast_overrides feature_name out_variable)
  if(NOT feature_name IN_LIST PELICAN_FEATURE_REGISTRY_NAMES)
    message(FATAL_ERROR
      "pelican.feature_registry.unknown_feature@1: ${feature_name}")
  endif()

  set(overrides
      ${PELICAN_FEATURE_${feature_name}_FORCES_ON_CONTRAST})
  set(contrast "${PELICAN_FEATURE_${feature_name}_CONTRAST}")
  foreach(candidate IN LISTS PELICAN_FEATURE_REGISTRY_NAMES)
    set(condition "${PELICAN_FEATURE_${candidate}_CONDITION}")
    if(condition STREQUAL feature_name)
      set(condition_value
          "${PELICAN_FEATURE_${candidate}_CONDITION_VALUE}")
      if(NOT contrast STREQUAL condition_value)
        list(APPEND overrides
          "${candidate}=${PELICAN_FEATURE_${candidate}_CONDITION_FALLBACK}")
      endif()
    endif()
  endforeach()
  if(overrides)
    list(REMOVE_DUPLICATES overrides)
  endif()
  set("${out_variable}" "${overrides}" PARENT_SCOPE)
endfunction()

function(pelican_feature_contrast_arguments feature_name out_variable)
  if(NOT feature_name IN_LIST PELICAN_FEATURE_REGISTRY_NAMES)
    message(FATAL_ERROR
      "pelican.feature_registry.unknown_feature@1: ${feature_name}")
  endif()

  foreach(candidate IN LISTS PELICAN_FEATURE_REGISTRY_NAMES)
    set("contrast_value_${candidate}"
        "${PELICAN_FEATURE_${candidate}_BASELINE}")
  endforeach()
  set("contrast_value_${feature_name}"
      "${PELICAN_FEATURE_${feature_name}_CONTRAST}")

  pelican_feature_contrast_overrides("${feature_name}" overrides)
  set(allowed_changes "${feature_name}")
  foreach(override IN LISTS overrides)
    string(REPLACE "=" ";" override_parts "${override}")
    list(GET override_parts 0 override_name)
    list(GET override_parts 1 override_value)
    set("contrast_value_${override_name}" "${override_value}")
    list(APPEND allowed_changes "${override_name}")
  endforeach()

  set(arguments)
  foreach(candidate IN LISTS PELICAN_FEATURE_REGISTRY_NAMES)
    set(candidate_value "${contrast_value_${candidate}}")
    set(candidate_baseline "${PELICAN_FEATURE_${candidate}_BASELINE}")
    if(NOT candidate_value STREQUAL candidate_baseline AND
       NOT candidate IN_LIST allowed_changes)
      message(FATAL_ERROR
        "pelican.feature_registry.smoke_unregistered_change@1: "
        "${feature_name} changes ${candidate}=${candidate_value}")
    endif()
    list(APPEND arguments "-D${candidate}=${candidate_value}")
  endforeach()
  set("${out_variable}" "${arguments}" PARENT_SCOPE)
endfunction()

function(pelican_feature_from_smoke_id smoke_id out_variable)
  foreach(feature_name IN LISTS PELICAN_FEATURE_REGISTRY_NAMES)
    if("${PELICAN_FEATURE_${feature_name}_SMOKE_ID}" STREQUAL smoke_id)
      set("${out_variable}" "${feature_name}" PARENT_SCOPE)
      return()
    endif()
  endforeach()
  message(FATAL_ERROR
    "pelican.feature_registry.unknown_smoke_id@1: ${smoke_id}")
endfunction()

function(pelican_feature_dependency_markdown feature_name out_variable)
  pelican_feature_contrast_overrides("${feature_name}" overrides)
  if(NOT overrides)
    set("${out_variable}" "—" PARENT_SCOPE)
    return()
  endif()

  set(parts)
  foreach(override IN LISTS overrides)
    list(APPEND parts "`${override}`")
  endforeach()
  list(JOIN parts "<br>" rendered)
  set("${out_variable}" "${rendered}" PARENT_SCOPE)
endfunction()

function(pelican_render_feature_ledger_row feature_name out_variable)
  if(NOT feature_name IN_LIST PELICAN_FEATURE_REGISTRY_NAMES)
    message(FATAL_ERROR
      "pelican.feature_registry.unknown_feature@1: ${feature_name}")
  endif()
  pelican_feature_dependency_markdown("${feature_name}" dependencies)
  set(row
      "| `${feature_name}` | `${PELICAN_FEATURE_${feature_name}_BASELINE}` | "
      "`${PELICAN_FEATURE_${feature_name}_CONTRAST}` | ${dependencies} |")
  string(JOIN "" row ${row})
  set("${out_variable}" "${row}" PARENT_SCOPE)
endfunction()

function(pelican_render_feature_ledger out_variable)
  set(rendered "<!-- PELICAN_FEATURE_REGISTRY_BEGIN -->\n")
  string(APPEND rendered
    "| フラグ | 基準値 | 対照値 | 対照で連動するフラグ |\n"
    "|---|---:|---:|---|\n")
  foreach(feature_name IN LISTS PELICAN_FEATURE_REGISTRY_NAMES)
    pelican_render_feature_ledger_row("${feature_name}" rendered_row)
    string(APPEND rendered "${rendered_row}\n")
  endforeach()
  string(APPEND rendered "<!-- PELICAN_FEATURE_REGISTRY_END -->")
  set("${out_variable}" "${rendered}" PARENT_SCOPE)
endfunction()

function(pelican_verify_feature_ledger ledger_path)
  pelican_validate_feature_registry()
  if(NOT EXISTS "${ledger_path}")
    message(FATAL_ERROR
      "pelican.feature_registry.ledger_missing_file@1: ${ledger_path}")
  endif()

  file(STRINGS "${ledger_path}" ledger_lines ENCODING UTF-8)
  set(in_registry_block FALSE)
  set(saw_begin FALSE)
  set(saw_end FALSE)
  set(saw_header FALSE)
  set(saw_separator FALSE)
  set(ledger_names)
  foreach(ledger_line IN LISTS ledger_lines)
    if(ledger_line STREQUAL "<!-- PELICAN_FEATURE_REGISTRY_BEGIN -->")
      if(saw_begin)
        message(FATAL_ERROR
          "pelican.feature_registry.ledger_duplicate_begin@1: ${ledger_path}")
      endif()
      set(saw_begin TRUE)
      set(in_registry_block TRUE)
      continue()
    endif()
    if(ledger_line STREQUAL "<!-- PELICAN_FEATURE_REGISTRY_END -->")
      if(NOT in_registry_block)
        message(FATAL_ERROR
          "pelican.feature_registry.ledger_unmatched_end@1: ${ledger_path}")
      endif()
      set(saw_end TRUE)
      set(in_registry_block FALSE)
      continue()
    endif()
    if(NOT in_registry_block)
      continue()
    endif()

    if(ledger_line STREQUAL
       "| フラグ | 基準値 | 対照値 | 対照で連動するフラグ |")
      set(saw_header TRUE)
      continue()
    endif()
    if(ledger_line STREQUAL "|---|---:|---:|---|")
      set(saw_separator TRUE)
      continue()
    endif()

    string(REPLACE "`" "" normalized_line "${ledger_line}")
    if(normalized_line MATCHES
       "^\\| ([A-Z][A-Z0-9_]*) \\| (ON|OFF) \\| (ON|OFF) \\| (.*) \\|$")
      set(ledger_name "${CMAKE_MATCH_1}")
      set(ledger_baseline "${CMAKE_MATCH_2}")
      set(ledger_contrast "${CMAKE_MATCH_3}")
      set(ledger_dependencies "${CMAKE_MATCH_4}")

      if(NOT ledger_name IN_LIST PELICAN_FEATURE_REGISTRY_NAMES)
        message(FATAL_ERROR
          "pelican.feature_registry.ledger_unknown@1: ${ledger_name}")
      endif()
      if(ledger_name IN_LIST ledger_names)
        message(FATAL_ERROR
          "pelican.feature_registry.ledger_duplicate@1: ${ledger_name}")
      endif()
      if(NOT ledger_baseline STREQUAL
          "${PELICAN_FEATURE_${ledger_name}_BASELINE}")
        message(FATAL_ERROR
          "pelican.feature_registry.ledger_baseline_mismatch@1: "
          "${ledger_name} expected ${PELICAN_FEATURE_${ledger_name}_BASELINE}, "
          "found ${ledger_baseline}")
      endif()
      if(NOT ledger_contrast STREQUAL
          "${PELICAN_FEATURE_${ledger_name}_CONTRAST}")
        message(FATAL_ERROR
          "pelican.feature_registry.ledger_contrast_mismatch@1: "
          "${ledger_name} expected ${PELICAN_FEATURE_${ledger_name}_CONTRAST}, "
          "found ${ledger_contrast}")
      endif()

      pelican_feature_dependency_markdown("${ledger_name}"
                                          expected_dependencies)
      string(REPLACE "`" "" expected_dependencies
                     "${expected_dependencies}")
      if(NOT ledger_dependencies STREQUAL expected_dependencies)
        message(FATAL_ERROR
          "pelican.feature_registry.ledger_dependency_mismatch@1: "
          "${ledger_name} expected '${expected_dependencies}', "
          "found '${ledger_dependencies}'")
      endif()
      list(APPEND ledger_names "${ledger_name}")
    elseif(normalized_line MATCHES "^[;]?\\| [A-Z][A-Z0-9_]* \\|" OR
           normalized_line MATCHES "^[;]?\\| フラグ \\|" OR
           normalized_line MATCHES "^[;]?\\|---")
      message(FATAL_ERROR
        "pelican.feature_registry.ledger_malformed@1: ${ledger_line}")
    endif()
  endforeach()

  if(NOT saw_begin)
    message(FATAL_ERROR
      "pelican.feature_registry.ledger_begin_missing@1: ${ledger_path}")
  endif()
  if(NOT saw_end OR in_registry_block)
    message(FATAL_ERROR
      "pelican.feature_registry.ledger_end_missing@1: ${ledger_path}")
  endif()
  if(NOT saw_header)
    message(FATAL_ERROR
      "pelican.feature_registry.ledger_header_missing@1: ${ledger_path}")
  endif()
  if(NOT saw_separator)
    message(FATAL_ERROR
      "pelican.feature_registry.ledger_separator_missing@1: ${ledger_path}")
  endif()
  foreach(feature_name IN LISTS PELICAN_FEATURE_REGISTRY_NAMES)
    if(NOT feature_name IN_LIST ledger_names)
      message(FATAL_ERROR
        "pelican.feature_registry.ledger_missing@1: ${feature_name}")
    endif()
  endforeach()
  if(NOT "${ledger_names}" STREQUAL "${PELICAN_FEATURE_REGISTRY_NAMES}")
    message(FATAL_ERROR
      "pelican.feature_registry.ledger_order_mismatch@1: "
      "expected ${PELICAN_FEATURE_REGISTRY_NAMES}; found ${ledger_names}")
  endif()
endfunction()

pelican_validate_feature_registry()
