FetchContent_Declare(
  Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG        v3.11.0
)
FetchContent_MakeAvailable(Catch2)
list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)
include(${CMAKE_CURRENT_SOURCE_DIR}/gpu_lease_config.cmake)
find_package(Python3 COMPONENTS Interpreter REQUIRED)

# CPU reference math and fixture generators belong only to the tests that
# consume them; pelican_player whole-archives pelican_core for SDK exports.
add_library(pelican_test_raytracing_oracle STATIC
    ${CMAKE_SOURCE_DIR}/src/core/shader/raytracingoracle.cpp)
set_target_properties(pelican_test_raytracing_oracle PROPERTIES
    CXX_STANDARD 20
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
if(MSVC)
    target_compile_options(pelican_test_raytracing_oracle PRIVATE /fp:strict)
else()
    target_compile_options(pelican_test_raytracing_oracle PRIVATE -ffp-contract=off)
endif()

# Every GPU-labelled CTest test runs through this test-only executable.  The
# target is intentionally independent of pelican_core so a broken production
# build cannot make the lease disappear.
add_executable(pelican_gpu_lease_run gpu_lease_run.cpp)
set_target_properties(pelican_gpu_lease_run PROPERTIES CXX_STANDARD 20)

define_property(GLOBAL PROPERTY PELICAN_GPU_INTRINSIC_TIMEOUTS
  BRIEF_DOCS "Per-test intrinsic timeouts feeding the generated lease policy"
  FULL_DOCS "WP385 decision 12: the CTest TIMEOUT of every gpu-labelled test is "
            "lease_timeout_max + intrinsic + grace, and the inventory gate "
            "reads the same numbers from one generated file.")

function(pelican_gpu_test_properties test_name intrinsic_timeout)
  math(EXPR ctest_timeout
    "${PELICAN_GPU_LEASE_TIMEOUT_MAX} + ${intrinsic_timeout} + ${PELICAN_GPU_CTEST_TIMEOUT_GRACE}")
  set_property(TEST ${test_name} APPEND PROPERTY LABELS gpu)
  set_tests_properties(${test_name} PROPERTIES
    RESOURCE_LOCK pelican_gpu
    TIMEOUT ${ctest_timeout})
  if(NOT intrinsic_timeout EQUAL PELICAN_GPU_INTRINSIC_TIMEOUT_DEFAULT)
    set_property(GLOBAL APPEND PROPERTY PELICAN_GPU_INTRINSIC_TIMEOUTS
      "\"${test_name}\": ${intrinsic_timeout}")
  endif()
endfunction()

add_test(
  NAME pelican_public_api_inventory
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_SOURCE_DIR}/tools/public_api_inventory.py
          --repo-root ${CMAKE_SOURCE_DIR})
set_tests_properties(pelican_public_api_inventory
  PROPERTIES LABELS "WP380b;cpu;contract")

set(PELICAN_CATCH_ADD_TESTS_SCRIPT "${_CATCH_DISCOVER_TESTS_SCRIPT}")
set(PELICAN_CATCH_DISCOVERY_ALLOW_EMPTY_SCRIPT
  "${CMAKE_CURRENT_BINARY_DIR}/run_catch_discovery_allow_empty.cmake")
configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/run_catch_discovery_allow_empty.cmake.in
  ${PELICAN_CATCH_DISCOVERY_ALLOW_EMPTY_SCRIPT}
  @ONLY)

if(POLICY CMP0110)
  cmake_policy(SET CMP0110 NEW)
endif()

if(MSVC AND PELICAN_LEAN_TEST_ARTIFACTS)
  # A separate linker PDB and incremental-link database for every test
  # duplicates pelican_core's Debug symbols hundreds of times. Keep Debug
  # code generation and assertions, but make routine test artifacts lean.
  # This remains complementary to the link-closure grouping below: grouping
  # removes repeated linked code, while this removes per-binary linker state
  # from each executable that remains.
  # Disable PELICAN_LEAN_TEST_ARTIFACTS when an individual test needs to be
  # debugged with full linker symbols.
  add_link_options(
    "$<$<CONFIG:Debug>:/DEBUG:NONE>"
    "$<$<CONFIG:Debug>:/INCREMENTAL:NO>"
    "$<$<CONFIG:RelWithDebInfo>:/DEBUG:NONE>"
    "$<$<CONFIG:RelWithDebInfo>:/INCREMENTAL:NO>"
  )
  message(STATUS
    "Pelican test artifacts: lean MSVC linking "
    "(no linker PDB or incremental-link database)")
endif()

function(pelican_define_test source_name)
cmake_parse_arguments(PELICAN_TEST
  "GOLDEN;GPU;MIXED_GPU;QT;QT_OFFSCREEN;RUNTIME_SHADER;NO_DISCOVERY" "GROUP" "" ${ARGN})
set(source_target pelican_test_${source_name})
set(target_name ${source_target})
set(is_new_executable TRUE)

if(PELICAN_TEST_GROUP)
  # The configured target graph has 157 Catch executables: 127 sources fit 21
  # groups and 30 remain singletons. Its largest exact closure class contains
  # 108 regular plus 14 GPU sources; discovery/runtime properties and
  # contiguous registration-order runs split that class into smaller groups.
  # Tests share a binary only when their exact transitive link closure and
  # discovery properties match.
  # Keep one object target per source so source-specific definitions,
  # dependencies, and the developer's compile-only inner loop remain local.
  set(target_name pelican_test_group_${PELICAN_TEST_GROUP})
  add_library(${source_target} OBJECT ${source_name}.cpp)
else()
  add_executable(${source_target} ${source_name}.cpp)
endif()

set_target_properties(${source_target} PROPERTIES CXX_STANDARD 20)
target_include_directories(${source_target} PRIVATE ${Vulkan_INCLUDE_DIRS})
target_compile_definitions(${source_target} PRIVATE
    "VULKAN_HPP_DISPATCH_LOADER_STATIC_TYPE=::vk::detail::DispatchLoaderStatic"
)
if(PELICAN_TEST_GROUP)
  target_link_libraries(${source_target} PRIVATE Catch2::Catch2 nlohmann_json
    ${PELICAN_TEST_UNPARSED_ARGUMENTS})
  set(discovery_signature
    "${PELICAN_TEST_GOLDEN};${PELICAN_TEST_GPU};${PELICAN_TEST_MIXED_GPU};${PELICAN_TEST_QT};${PELICAN_TEST_QT_OFFSCREEN};${PELICAN_TEST_RUNTIME_SHADER}")
  if(TARGET ${target_name})
    set(is_new_executable FALSE)
    get_target_property(existing_signature ${target_name}
      PELICAN_TEST_DISCOVERY_SIGNATURE)
    if(NOT existing_signature STREQUAL discovery_signature)
      message(FATAL_ERROR
        "Test group ${PELICAN_TEST_GROUP} mixes discovery properties: "
        "${existing_signature} vs ${discovery_signature}")
    endif()
  else()
    add_executable(${target_name})
    set_target_properties(${target_name} PROPERTIES
      CXX_STANDARD 20
      LINKER_LANGUAGE CXX
      PELICAN_TEST_DISCOVERY_SIGNATURE "${discovery_signature}"
    )
    target_link_libraries(${target_name} PRIVATE
      Catch2::Catch2WithMain nlohmann_json)
  endif()
  target_link_libraries(${target_name} PRIVATE ${source_target})
  set_property(TARGET ${target_name} APPEND PROPERTY
    PELICAN_TEST_GROUP_SOURCES ${source_name})
  set_property(TARGET ${source_target} PROPERTY
    PELICAN_TEST_EXECUTABLE_TARGET ${target_name})
else()
  target_link_libraries(${target_name} PRIVATE Catch2::Catch2WithMain nlohmann_json
    ${PELICAN_TEST_UNPARSED_ARGUMENTS})
  set_property(TARGET ${source_target} PROPERTY
    PELICAN_TEST_EXECUTABLE_TARGET ${target_name})
endif()

if(NOT is_new_executable)
  return()
endif()
if(PELICAN_TEST_NO_DISCOVERY)
  return()
endif()
set(discovery_properties)
# A discovered case with zero assertions must not be reported as passing.
set(discovery_arguments EXTRA_ARGS -w NoAssertions)
if(PELICAN_TEST_QT)
  list(APPEND discovery_arguments
    DL_PATHS "$<TARGET_FILE_DIR:Qt6::Core>")
endif()
if(PELICAN_TEST_QT_OFFSCREEN)
  if(NOT PELICAN_TEST_QT)
    message(FATAL_ERROR "QT_OFFSCREEN test ${source_name} must also specify QT")
  endif()
  list(APPEND discovery_properties
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()
if(PELICAN_TEST_GOLDEN OR
   source_name STREQUAL "debugtext_ui_compat_test")
  # Golden GPU fixtures are part of the one GPU serialization domain.
  list(APPEND discovery_properties RESOURCE_LOCK pelican_gpu)
endif()
if(PELICAN_TEST_GPU)
  math(EXPR gpu_ctest_timeout
    "${PELICAN_GPU_LEASE_TIMEOUT_MAX} + ${PELICAN_GPU_INTRINSIC_TIMEOUT_DEFAULT} + ${PELICAN_GPU_CTEST_TIMEOUT_GRACE}")
  list(APPEND discovery_properties LABELS gpu RESOURCE_LOCK pelican_gpu TIMEOUT ${gpu_ctest_timeout})
endif()
if(PELICAN_TEST_GPU AND PELICAN_TEST_MIXED_GPU)
  message(FATAL_ERROR
    "Test ${source_name} cannot use both GPU and MIXED_GPU discovery")
endif()
if(PELICAN_TEST_RUNTIME_SHADER AND NOT PELICAN_RUNTIME_SHADER_COMPILER)
  # These cases exercise source compilation and deliberately compile to empty
  # Catch2 bodies in the baked-SPIR-V shipping tier.  Keep them visible as
  # disabled tests instead of letting -w NoAssertions turn that tier red (or,
  # worse, dropping the no-assertion guard and reporting false coverage).
  list(APPEND discovery_properties DISABLED TRUE)
endif()
if(PELICAN_TEST_MIXED_GPU)
  # Catch2 returns before writing its discovery file when a TEST_SPEC has no
  # matches.  That exposes a synthetic *_NOT_BUILT test in clean builds and
  # can retain stale names/labels after a tag mutation.  Keep both TEST_SPEC
  # lanes, but make an empty lane an explicit empty inventory.
  set(_CATCH_DISCOVER_TESTS_SCRIPT
    ${PELICAN_CATCH_DISCOVERY_ALLOW_EMPTY_SCRIPT})
  math(EXPR mixed_gpu_ctest_timeout
    "${PELICAN_GPU_LEASE_TIMEOUT_MAX} + ${PELICAN_GPU_INTRINSIC_TIMEOUT_DEFAULT} + ${PELICAN_GPU_CTEST_TIMEOUT_GRACE}")
  set(mixed_gpu_properties ${discovery_properties}
    LABELS gpu RESOURCE_LOCK pelican_gpu TIMEOUT ${mixed_gpu_ctest_timeout})
  if(discovery_properties)
    catch_discover_tests(${target_name}
      ${discovery_arguments}
      TEST_SPEC "~[gpu]"
      PROPERTIES ${discovery_properties})
    catch_discover_tests(${target_name}
      ${discovery_arguments}
      TEST_SPEC "[gpu]"
      PROPERTIES ${mixed_gpu_properties})
  else()
    catch_discover_tests(${target_name}
      ${discovery_arguments}
      TEST_SPEC "~[gpu]")
    catch_discover_tests(${target_name}
      ${discovery_arguments}
      TEST_SPEC "[gpu]"
      PROPERTIES ${mixed_gpu_properties})
  endif()
elseif(discovery_properties)
  catch_discover_tests(${target_name}
    ${discovery_arguments}
    PROPERTIES ${discovery_properties})
else()
  catch_discover_tests(${target_name} ${discovery_arguments})
endif()

if(PELICAN_TEST_GPU OR PELICAN_TEST_MIXED_GPU)
  # Catch writes this file in its POST_BUILD discovery command.  Append our
  # rewrite command afterwards so the discovered [gpu] CTest entries (and only
  # those entries for MIXED_GPU targets) start with the launcher.
  add_dependencies(${target_name} pelican_gpu_lease_run)
  add_custom_command(TARGET ${target_name} POST_BUILD
    COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/patch_catch_gpu_lease_tests.py
      --directory ${CMAKE_CURRENT_BINARY_DIR}
      --target ${target_name}
      --launcher $<TARGET_FILE:pelican_gpu_lease_run>
    VERBATIM)
endif()

endfunction()

function(pelican_copy_test_shaderc_runtime source_target)
  get_target_property(executable_target ${source_target}
    PELICAN_TEST_EXECUTABLE_TARGET)
  if(NOT executable_target)
    set(executable_target ${source_target})
  endif()
  get_target_property(copy_configured ${executable_target}
    PELICAN_TEST_SHADERC_RUNTIME_COPY_CONFIGURED)
  if(NOT copy_configured)
    pelican_copy_shaderc_runtime(${executable_target})
    set_property(TARGET ${executable_target} PROPERTY
      PELICAN_TEST_SHADERC_RUNTIME_COPY_CONFIGURED TRUE)
  endif()
endfunction()

add_test(
  NAME devstudio_d0_boundary_negative
  COMMAND ${CMAKE_COMMAND}
    -DPELICAN_SOURCE_DIR=${CMAKE_SOURCE_DIR}
    -DTEST_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/devstudio_boundary_negative
    -P ${CMAKE_CURRENT_SOURCE_DIR}/run_devstudio_boundary_negative.cmake
)

add_test(
  NAME wp360_raw_authority_compile_contract
  COMMAND ${CMAKE_COMMAND}
    "-DPELICAN_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
    "-DTEST_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/wp360_raw_authority_build"
    "-DNLOHMANN_INCLUDE_DIR=${nlohmann_SOURCE_DIR}/include"
    "-DGENERATOR=${CMAKE_GENERATOR}"
    "-DGENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}"
    "-DGENERATOR_TOOLSET=${CMAKE_GENERATOR_TOOLSET}"
    "-DTEST_CONFIG=$<CONFIG>"
    -P ${CMAKE_CURRENT_SOURCE_DIR}/run_wp360_raw_authority_compile.cmake
)

add_test(
  NAME feature_registry_ledger_contract
  COMMAND ${CMAKE_COMMAND}
    -P ${CMAKE_SOURCE_DIR}/cmake/verify_feature_registry.cmake
)
add_test(
  NAME feature_registry_smoke_matrix_contract
  COMMAND ${CMAKE_COMMAND}
    -DPELICAN_BUILD_UNIT_SMOKE_PARSE_ONLY=ON
    -P ${CMAKE_CURRENT_SOURCE_DIR}/run_build_units_smoke.cmake
)
add_test(
  NAME feature_registry_mutation_contract
  COMMAND ${CMAKE_COMMAND}
    -DPELICAN_MUTATION_DIR=${CMAKE_CURRENT_BINARY_DIR}/feature_registry_mutations
    -P ${CMAKE_SOURCE_DIR}/cmake/test_feature_registry_mutations.cmake
)
add_test(
  NAME wp367a_required_capability_rejects_zero_tests_and_skips
  COMMAND ${CMAKE_COMMAND}
    "-DPELICAN_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
    "-DPELICAN_TEST_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}"
    -P ${CMAKE_CURRENT_SOURCE_DIR}/run_required_capability_ctest_contrast.cmake
)

add_test(
  NAME wp375c_ctest_inventory_contract
  COMMAND ${CMAKE_COMMAND}
    "-DPELICAN_CTEST_COMMAND=${CMAKE_CTEST_COMMAND}"
    "-DPELICAN_BINARY_DIR=${CMAKE_BINARY_DIR}"
    "-DPELICAN_TEST_CONFIG=$<CONFIG>"
    "-DPELICAN_EXPECT_RPC=$<BOOL:${PELICAN_WITH_RPC}>"
    -P ${CMAKE_CURRENT_SOURCE_DIR}/run_wp375c_ctest_inventory.cmake
)

if(TARGET pelican_studio_layout)
  # Qt imported targets are directory-scoped; make the layout library and
  # compatibility test dependencies visible in this sibling directory.
  find_package(Qt6 REQUIRED COMPONENTS Core Widgets)
  pelican_define_test(devstudio_layoutpreset_test QT QT_OFFSCREEN
    pelican_studio_layout
    pelican_studio_view
    Qt6::Core
    Qt6::Widgets)
endif()

if(TARGET pelican_studio_outliner)
  pelican_define_test(devstudio_outliner_test
    pelican_studio_outliner
    pelican_core)
  target_compile_definitions(pelican_test_devstudio_outliner_test PRIVATE
    PELICAN_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
  )
endif()

if(TARGET pelican_studio_view)
  find_package(Qt6 REQUIRED COMPONENTS Core Widgets)
  pelican_define_test(shaderresolution_production_test GPU QT QT_OFFSCREEN
    RUNTIME_SHADER
    pelican_studio_view
    pelican_studio_viewport
    pelican_studio_frame_plan
    pelican_core
    VulkanMemoryAllocator
    VulkanMemoryAllocator-Hpp
    Qt6::Core
    Qt6::Widgets)
  target_compile_definitions(pelican_test_shaderresolution_production_test PRIVATE
    PELICAN_RUNTIME_SHADER_COMPILER=$<BOOL:${PELICAN_RUNTIME_SHADER_COMPILER}>)
  if(MSVC)
    target_compile_options(pelican_test_shaderresolution_production_test PRIVATE /bigobj)
  endif()
  if(PELICAN_RUNTIME_SHADER_COMPILER)
    target_link_libraries(pelican_test_shaderresolution_production_test PRIVATE
      pelican_shaderc)
    if(COMMAND pelican_copy_shaderc_runtime)
      pelican_copy_test_shaderc_runtime(
        pelican_test_shaderresolution_production_test)
    endif()
  endif()
endif()

if(TARGET pelican_studio_inspector_model)
  pelican_define_test(devstudio_graphattachment_test pelican_studio_inspector_model)
  pelican_define_test(devstudio_inspector_test
    pelican_studio_inspector_model
    pelican_studio_gizmo_model
    pelican_core
    glm
  )
endif()

if(TARGET pelican_studio_gizmo_model)
  pelican_define_test(devstudio_gizmo_test pelican_studio_gizmo_model)
endif()

pelican_define_test(schema_vocabulary_test
  pelican_project
  pelican_schema_core_adapter
)

add_test(
  NAME schema_leaf_independent_consumer
  COMMAND ${CMAKE_COMMAND}
    "-DPELICAN_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
    "-DTEST_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/schema_leaf_consumer_build"
    "-DNLOHMANN_INCLUDE_DIR=${nlohmann_SOURCE_DIR}/include"
    "-DGENERATOR=${CMAKE_GENERATOR}"
    "-DGENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}"
    "-DGENERATOR_TOOLSET=${CMAKE_GENERATOR_TOOLSET}"
    "-DTEST_CONFIG=$<CONFIG>"
    -P ${CMAKE_CURRENT_SOURCE_DIR}/run_schema_leaf_consumer.cmake
)

if(PELICAN_WITH_RPC)
pelican_define_test(schema_rpc_test pelican_core pelican_schema_core_adapter picosha2)
target_compile_definitions(pelican_test_schema_rpc_test PRIVATE
  PELICAN_WP358_RPC_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/wp358/get_components.pre_wp358.ndjson"
)
endif()

pelican_define_test(schema_value_oracle_test pelican_core pelican_schema_leaf)
if(MSVC)
  target_compile_options(pelican_test_schema_value_oracle_test PRIVATE
    /constexpr:depth2048)
endif()

if(TARGET pelican_studio_frame_plan)
  pelican_define_test(devstudio_frameplan_test
    pelican_studio_frame_plan
    pelican_core)
  target_compile_definitions(pelican_test_devstudio_frameplan_test PRIVATE
    PELICAN_TEST_FRAME_PLAN_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/devstudio/example_frame_plan.json"
    PELICAN_RUNTIME_SHADER_COMPILER=$<BOOL:${PELICAN_RUNTIME_SHADER_COMPILER}>
  )
endif()

if(TARGET pelican_studio_viewport AND TARGET pelican_studio_frame_plan)
  find_package(Qt6 REQUIRED COMPONENTS Core Test Widgets)
  add_executable(pelican_test_wp351b_live_child process_fixture_child.cpp)
  set_target_properties(pelican_test_wp351b_live_child PROPERTIES CXX_STANDARD 20)
  pelican_define_test(devstudio_engine_failure_test QT GPU GOLDEN
    pelican_studio_viewport
    pelican_studio_frame_plan
    pelican_core
    VulkanMemoryAllocator
    VulkanMemoryAllocator-Hpp
    glm
    Qt6::Core
    Qt6::Widgets
  )
  target_sources(pelican_test_devstudio_engine_failure_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/devstudio/view/frameplanwidget.cpp)
  target_sources(pelican_test_devstudio_engine_failure_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/devstudio/view/frameplangraphics.cpp)
  target_include_directories(pelican_test_devstudio_engine_failure_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/devstudio/view)
  set(pelican_wp298_example_project
      "${CMAKE_SOURCE_DIR}/projects/example")
  if(NOT "${PELICAN_TEST_PROJECTS_DIR}" STREQUAL "" AND
     EXISTS "${PELICAN_TEST_PROJECTS_DIR}/example/project.json")
    set(pelican_wp298_example_project
        "${PELICAN_TEST_PROJECTS_DIR}/example")
  endif()
  target_compile_definitions(pelican_test_devstudio_engine_failure_test PRIVATE
    PELICAN_WP298_PLAYER="$<TARGET_FILE:pelican_player>"
    PELICAN_WP298_EXAMPLE_PROJECT="${pelican_wp298_example_project}"
    PELICAN_WP351B_LIVE_CHILD="$<TARGET_FILE:pelican_test_wp351b_live_child>"
  )
  add_dependencies(pelican_test_devstudio_engine_failure_test
    pelican_player
    pelican_test_wp351b_live_child)

  pelican_define_test(devstudio_frameplan_graph_test QT QT_OFFSCREEN
    pelican_studio_viewport
    pelican_studio_frame_plan
    pelican_core
    Qt6::Core
    Qt6::Test
    Qt6::Widgets
  )
  target_sources(pelican_test_devstudio_frameplan_graph_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/devstudio/view/frameplanwidget.cpp
    ${CMAKE_SOURCE_DIR}/src/devstudio/view/frameplangraphics.cpp
    ${CMAKE_SOURCE_DIR}/src/devstudio/view/taskconnectiongraphics.cpp
  )
  target_include_directories(pelican_test_devstudio_frameplan_graph_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/devstudio/view
  )
  target_compile_definitions(pelican_test_devstudio_frameplan_graph_test PRIVATE
    PELICAN_TEST_FRAME_PLAN_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/devstudio/example_frame_plan.json"
    PELICAN_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    PELICAN_RUNTIME_SHADER_COMPILER=$<BOOL:${PELICAN_RUNTIME_SHADER_COMPILER}>
    PELICAN_WITH_IMGUI=$<BOOL:${PELICAN_WITH_IMGUI}>
    PELICAN_WITH_OPENXR=$<BOOL:${PELICAN_WITH_OPENXR}>
  )

  pelican_define_test(devstudio_fullscreen_pass_test QT QT_OFFSCREEN
    pelican_studio_view
    pelican_studio_viewport
    pelican_studio_frame_plan
    pelican_studio_layout
    pelican_studio_outliner
    pelican_studio_inspector_model
    pelican_studio_gizmo_model
    pelican_project
    pelican_core
    Qt6::Core
    Qt6::Widgets
  )
  target_compile_definitions(pelican_test_devstudio_fullscreen_pass_test PRIVATE
    PELICAN_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
  )
endif()

if(WIN32 AND COMMAND pelican_configure_game_logic_target)
  pelican_define_test(game_logic_abi_test NO_DISCOVERY pelican_core)
  # The frozen-v1 OS control imports this executable by name. CTest's direct
  # Win32 child launch can block while LoadLibrary resolves that self-named
  # EXE import; one execute_process hop preserves the real loader call and its
  # error 127. Keep tests that can reach that path under a source mutation
  # behind one execute_process hop and serial. The remaining reload cases run
  # concurrently and exercise the per-process shadow paths.
  catch_discover_tests(pelican_test_game_logic_abi_test
    TEST_SPEC "~[wp381-os-loader-control]"
    EXTRA_ARGS -w NoAssertions)
  add_test(
    NAME "WP381 legacy entrance inventory is diagnosed before load"
    COMMAND ${CMAKE_COMMAND}
      "-DTEST_EXECUTABLE=$<TARGET_FILE:pelican_test_game_logic_abi_test>"
      "-DTEST_NAME=WP381 legacy entrance inventory is diagnosed before load"
      -P ${CMAKE_CURRENT_SOURCE_DIR}/run_wp381_os_loader_control.cmake)
  add_test(
    NAME wp381_os_loader_control
    COMMAND ${CMAKE_COMMAND}
      "-DTEST_EXECUTABLE=$<TARGET_FILE:pelican_test_game_logic_abi_test>"
      "-DTEST_NAME=WP381 diagnostic and OS enforcement are separate constructor controls"
      -P ${CMAKE_CURRENT_SOURCE_DIR}/run_wp381_os_loader_control.cmake)
  set_tests_properties(
    "WP381 legacy entrance inventory is diagnosed before load"
    wp381_os_loader_control
    PROPERTIES RUN_SERIAL TRUE TIMEOUT 30)
  set_target_properties(pelican_test_game_logic_abi_test PROPERTIES
    ENABLE_EXPORTS TRUE
  )
  if(MSVC)
    target_link_options(pelican_test_game_logic_abi_test PRIVATE
      "/WHOLEARCHIVE:$<TARGET_LINKER_FILE:pelican_core>"
    )
  endif()

  set(wp381_v1_import_dir "${CMAKE_CURRENT_BINARY_DIR}/wp381_v1_import")
  set(wp381_v1_import_library
      "${wp381_v1_import_dir}/pelican_test_game_logic_abi_test_v1.lib")
  add_custom_command(
    OUTPUT "${wp381_v1_import_library}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${wp381_v1_import_dir}"
    COMMAND "${CMAKE_AR}" /nologo
      "/DEF:${CMAKE_CURRENT_SOURCE_DIR}/fixtures/game_logic_abi/pelican_game_logic_v1.def"
      /MACHINE:X64
      /NAME:pelican_test_game_logic_abi_test.exe
      "/OUT:${wp381_v1_import_library}"
    DEPENDS
      "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/game_logic_abi/pelican_game_logic_v1.def"
    VERBATIM
  )
  add_custom_target(pelican_wp381_v1_import_stub
    DEPENDS "${wp381_v1_import_library}")

  set(wp381_epoch_v999_import_library
      "${wp381_v1_import_dir}/pelican_player_epoch_v999.lib")
  add_custom_command(
    OUTPUT "${wp381_epoch_v999_import_library}"
    COMMAND "${CMAKE_AR}" /nologo
      "/DEF:${CMAKE_CURRENT_SOURCE_DIR}/fixtures/game_logic_abi/pelican_game_logic_epoch_v999.def"
      /MACHINE:X64
      /NAME:pelican_player.exe
      "/OUT:${wp381_epoch_v999_import_library}"
    DEPENDS
      "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/game_logic_abi/pelican_game_logic_epoch_v999.def"
    VERBATIM)
  add_custom_target(pelican_wp381_epoch_v999_import_stub
    DEPENDS "${wp381_epoch_v999_import_library}")

  function(pelican_add_wp381_legacy_symbol_fixture
           target_name symbol_define fixture_role)
    add_library(${target_name} SHARED
      fixtures/game_logic_abi/legacy_symbol.cpp)
    set_target_properties(${target_name} PROPERTIES OUTPUT_NAME "${target_name}")
    target_compile_definitions(${target_name} PRIVATE ${symbol_define}=1)
    pelican_configure_game_logic_target(${target_name}
      HOST_TARGET pelican_test_game_logic_abi_test
      ROLE ${fixture_role} NO_INVENTORY)
    add_dependencies(${target_name} pelican_wp381_v1_import_stub)
    target_link_libraries(${target_name} PRIVATE "${wp381_v1_import_library}")
  endfunction()

  pelican_add_wp381_legacy_symbol_fixture(
    pelican_wp381_legacy_behavior WP381_LEGACY_BEHAVIOR legacy-behavior)
  pelican_add_wp381_legacy_symbol_fixture(
    pelican_wp381_legacy_event WP381_LEGACY_EVENT legacy-event)
  pelican_add_wp381_legacy_symbol_fixture(
    pelican_wp381_legacy_emit WP381_LEGACY_EMIT legacy-emit)
  pelican_add_wp381_legacy_symbol_fixture(
    pelican_wp381_legacy_system WP381_LEGACY_SYSTEM legacy-system)

  # Compile four real reader mutants. Each copy disables exactly one legacy
  # recognizer literal; its matching single-import fixture must then be
  # (incorrectly) accepted, proving the production assertion kills the mutant.
  # The mutation copies are written at configure time. Without this, editing the
  # reader leaves the copies stale and the reader mutation tests exercise an old
  # reader.
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/src/core/gamelogic/gamelogicimports.cpp")
  file(READ "${CMAKE_SOURCE_DIR}/src/core/gamelogic/gamelogicimports.cpp"
    wp381_import_reader_source)
  foreach(wp381_mutation IN ITEMS
      "behavior|__registerBehavior|pelican_wp381_legacy_behavior"
      "event|__registerEvent|pelican_wp381_legacy_event"
      "emit|__emit|pelican_wp381_legacy_emit"
      "system|__registerSystem|pelican_wp381_legacy_system")
    string(REPLACE "|" ";" wp381_mutation_fields "${wp381_mutation}")
    list(GET wp381_mutation_fields 0 wp381_mutation_name)
    list(GET wp381_mutation_fields 1 wp381_mutation_symbol)
    list(GET wp381_mutation_fields 2 wp381_mutation_fixture)
    string(FIND "${wp381_import_reader_source}"
      "\"${wp381_mutation_symbol}\"" wp381_mutation_index)
    if(wp381_mutation_index EQUAL -1)
      message(FATAL_ERROR
        "WP381 reader mutation point vanished: ${wp381_mutation_symbol}")
    endif()
    string(REPLACE "\"${wp381_mutation_symbol}\""
      "\"${wp381_mutation_symbol}_removed_by_mutation\""
      wp381_mutated_reader "${wp381_import_reader_source}")
    set(wp381_mutated_source
      "${CMAKE_CURRENT_BINARY_DIR}/wp381_mutations/gamelogicimports_${wp381_mutation_name}.cpp")
    file(WRITE "${wp381_mutated_source}" "${wp381_mutated_reader}")
    set(wp381_mutation_target
      pelican_wp381_reader_mutation_${wp381_mutation_name})
    add_executable(${wp381_mutation_target}
      wp381_legacy_reader_mutation.cpp "${wp381_mutated_source}")
    set_target_properties(${wp381_mutation_target} PROPERTIES CXX_STANDARD 20)
    target_include_directories(${wp381_mutation_target} PRIVATE
      "${CMAKE_SOURCE_DIR}/src/core/gamelogic")
    target_link_libraries(${wp381_mutation_target} PRIVATE nlohmann_json)
    target_compile_definitions(${wp381_mutation_target} PRIVATE
      WP381_MUTATION_FIXTURE="$<TARGET_FILE:${wp381_mutation_fixture}>"
      WP381_MUTATION_HOST="pelican_test_game_logic_abi_test.exe")
    add_dependencies(${wp381_mutation_target} ${wp381_mutation_fixture})
    add_test(NAME wp381_legacy_reader_mutation_${wp381_mutation_name}
      COMMAND $<TARGET_FILE:${wp381_mutation_target}>)
  endforeach()

  add_library(pelican_wp381_stale_behavior SHARED
    fixtures/game_logic_abi/constructor_fixture.cpp)
  set_target_properties(pelican_wp381_stale_behavior PROPERTIES
    CXX_STANDARD 20
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
    OUTPUT_NAME pelican_wp381_stale_behavior
  )
  add_dependencies(pelican_wp381_stale_behavior
    pelican_wp381_v1_import_stub pelican_test_game_logic_abi_test)
  target_link_libraries(pelican_wp381_stale_behavior PRIVATE
    "${wp381_v1_import_library}" glm nlohmann_json)
  target_include_directories(pelican_wp381_stale_behavior PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/userpublic_frozen/v1"
    "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/userpublic_frozen/v1_support"
  )
  target_compile_definitions(pelican_wp381_stale_behavior PRIVATE
    PELICAN_GAME_DLL=1
    WP381_FROZEN_V1_BEHAVIOR=1
  )
  set(wp381_stale_source_dependencies
      "${CMAKE_CURRENT_BINARY_DIR}/wp381_stale_source_dependencies.json")
  target_compile_options(pelican_wp381_stale_behavior PRIVATE
    /sourceDependencies "${wp381_stale_source_dependencies}")
  pelican_classify_game_logic_target(pelican_wp381_stale_behavior
    loader stale-behavior-v1)
  set_property(TARGET pelican_wp381_stale_behavior PROPERTY
    PELICAN_GAME_DLL_HOST_TARGET pelican_test_game_logic_abi_test)

  add_library(pelican_wp381_v2_constructor_control SHARED
    fixtures/game_logic_abi/constructor_fixture.cpp)
  set_target_properties(pelican_wp381_v2_constructor_control PROPERTIES
    OUTPUT_NAME pelican_wp381_v2_constructor_control)
  target_compile_definitions(pelican_wp381_v2_constructor_control PRIVATE
    WP381_CURRENT_V2_CONTROL=1)
  pelican_configure_game_logic_target(pelican_wp381_v2_constructor_control
    HOST_TARGET pelican_test_game_logic_abi_test
    ROLE v2-constructor-control NO_INVENTORY)

  function(pelican_add_wp381_registration_fixture target_name order role)
    add_library(${target_name} SHARED
      fixtures/game_logic_abi/registration_order.cpp)
    set_target_properties(${target_name} PROPERTIES OUTPUT_NAME "${target_name}")
    target_compile_definitions(${target_name} PRIVATE
      WP381_REGISTRATION_ORDER=${order})
    pelican_configure_game_logic_target(${target_name}
      HOST_TARGET pelican_test_game_logic_abi_test ROLE ${role} NO_INVENTORY)
  endfunction()
  pelican_add_wp381_registration_fixture(
    pelican_wp381_duplicate_key 1 duplicate-key)
  pelican_add_wp381_registration_fixture(
    pelican_wp381_reverse_a 2 reverse-order-a)
  pelican_add_wp381_registration_fixture(
    pelican_wp381_reverse_b 3 reverse-order-b)

  add_library(pelican_wp382a_frozen_v2 SHARED
    fixtures/wp382a_schema/frozen_v2_fixture.cpp)
  set_target_properties(pelican_wp382a_frozen_v2 PROPERTIES
    OUTPUT_NAME pelican_wp382a_frozen_v2)
  pelican_configure_game_logic_target(pelican_wp382a_frozen_v2
    HOST_TARGET pelican_test_game_logic_abi_test
    ROLE wp382a-frozen-v2 NO_INVENTORY)
  target_include_directories(pelican_wp382a_frozen_v2 BEFORE PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/userpublic_frozen/v2")

  function(pelican_add_wp382a_v3_fixture target_name variant surface)
    add_library(${target_name} SHARED
      fixtures/wp382a_schema/v3_fixture.cpp)
    set_target_properties(${target_name} PROPERTIES OUTPUT_NAME ${target_name})
    target_compile_definitions(${target_name} PRIVATE
      WP382A_SCHEMA_VARIANT=${variant}
      WP382A_SCHEMA_SURFACE=${surface})
    pelican_configure_game_logic_target(${target_name}
      HOST_TARGET pelican_test_game_logic_abi_test
      ROLE wp382a-v3-${variant} NO_INVENTORY)
  endfunction()

  pelican_add_wp382a_v3_fixture(pelican_wp382a_v3_full 0 3)
  foreach(wp382a_surface IN ITEMS behavior event component)
    if(wp382a_surface STREQUAL "behavior")
      set(wp382a_surface_id 0)
    elseif(wp382a_surface STREQUAL "event")
      set(wp382a_surface_id 1)
    else()
      set(wp382a_surface_id 2)
    endif()
    foreach(wp382a_variant IN ITEMS
        "same|0" "default|1" "order|2" "vocabulary|3" "presence|4")
      string(REPLACE "|" ";" wp382a_variant_fields "${wp382a_variant}")
      list(GET wp382a_variant_fields 0 wp382a_variant_name)
      list(GET wp382a_variant_fields 1 wp382a_variant_id)
      pelican_add_wp382a_v3_fixture(
        pelican_wp382a_v3_${wp382a_surface}_${wp382a_variant_name}
        ${wp382a_variant_id} ${wp382a_surface_id})
    endforeach()
  endforeach()

  foreach(wp382a_negative IN ITEMS
      "flags_composite|FlagsVocabularyNotOneHot"
      "flags_zero|FlagsVocabularyNotOneHot"
      "v2_reordered|FrozenV2FieldOffsetMismatch")
    string(REPLACE "|" ";" wp382a_negative_fields "${wp382a_negative}")
    list(GET wp382a_negative_fields 0 wp382a_negative_name)
    list(GET wp382a_negative_fields 1 wp382a_negative_diagnostic)
    add_test(
      NAME wp382a_negative_compile_${wp382a_negative_name}
      COMMAND ${CMAKE_COMMAND}
        -DPROJECT_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/game_dll_boundary_compile_fixture
        -DBUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}/wp382a_negative/${wp382a_negative_name}
        -DFIXTURE_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/wp382a_schema/${wp382a_negative_name}_negative.cpp
        -DGAME_LOGIC_ENTRY=${CMAKE_SOURCE_DIR}/src/player/gamelogic_entry.cpp
        -DUSERPUBLIC_INCLUDE=${CMAKE_SOURCE_DIR}/src/core/userpublic
        -DCORE_INCLUDE=${CMAKE_SOURCE_DIR}/src/core
        -DNLOHMANN_INCLUDE=${nlohmann_json_SOURCE_DIR}/include
        -DHOST_IMPORT_LIBRARY=$<TARGET_LINKER_FILE:pelican_test_game_logic_abi_test>
        -DCONFIG=$<CONFIG>
        -DTARGET_NAME=pelican_wp382a_negative_${wp382a_negative_name}
        -DEXPECTED_DIAGNOSTIC=${wp382a_negative_diagnostic}
        -DGENERATOR=${CMAKE_GENERATOR}
        -DGENERATOR_INSTANCE=${CMAKE_GENERATOR_INSTANCE}
        -DGENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}
        -DGENERATOR_TOOLSET=${CMAKE_GENERATOR_TOOLSET}
        -DCXX_COMPILER=${CMAKE_CXX_COMPILER}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/run_game_dll_boundary_compile.cmake)
    set_tests_properties(wp382a_negative_compile_${wp382a_negative_name}
      PROPERTIES LABELS "WP382a;cpu;negative-compile")
  endforeach()

  foreach(wp381_role_target IN ITEMS
      pelican_wp381_legacy_behavior
      pelican_wp381_legacy_event
      pelican_wp381_legacy_emit
      pelican_wp381_legacy_system
      pelican_wp381_stale_behavior
      pelican_wp381_v2_constructor_control
      pelican_wp381_duplicate_key
      pelican_wp381_reverse_a
      pelican_wp381_reverse_b)
    set_property(TARGET ${wp381_role_target} PROPERTY
      PELICAN_WP381_ABI_FIXTURE TRUE)
  endforeach()

  # animgraph_demo and vrm_xr_demo call the SDK evaluator, so they are shipped
  # projects only where the animation unit is built.
  set(wp381_projects example sprite_demo)
  if(PELICAN_WITH_ANIMATION)
    set(wp381_projects example animgraph_demo sprite_demo vrm_xr_demo)
  endif()
  foreach(wp381_project IN LISTS wp381_projects)
    string(REPLACE "_" "-" wp381_project_role "${wp381_project}")
    set(wp381_project_target pelican_wp381_project_${wp381_project})
    if(wp381_project STREQUAL "example")
      set(wp381_project_source playercontrol.cpp)
    elseif(wp381_project STREQUAL "animgraph_demo")
      set(wp381_project_source animgraphdemo.cpp)
    elseif(wp381_project STREQUAL "sprite_demo")
      set(wp381_project_source flipbook_demo.cpp)
    else()
      set(wp381_project_source vrmxrdemo.cpp)
    endif()
    add_library(${wp381_project_target} SHARED
      "${CMAKE_SOURCE_DIR}/projects/${wp381_project}/code/${wp381_project_source}")
    set_target_properties(${wp381_project_target} PROPERTIES
      OUTPUT_NAME "${wp381_project_target}")
    target_include_directories(${wp381_project_target} PRIVATE
      "${CMAKE_SOURCE_DIR}/projects/${wp381_project}"
      "${CMAKE_SOURCE_DIR}/projects/${wp381_project}/code")
    pelican_configure_game_logic_target(${wp381_project_target}
      HOST_TARGET pelican_test_game_logic_abi_test
      ROLE shipped-project-${wp381_project_role} NO_INVENTORY)
  endforeach()

  target_compile_definitions(pelican_test_game_logic_abi_test PRIVATE
    PELICAN_WP381_LEGACY_BEHAVIOR="$<TARGET_FILE:pelican_wp381_legacy_behavior>"
    PELICAN_WP381_LEGACY_EVENT="$<TARGET_FILE:pelican_wp381_legacy_event>"
    PELICAN_WP381_LEGACY_EMIT="$<TARGET_FILE:pelican_wp381_legacy_emit>"
    PELICAN_WP381_LEGACY_SYSTEM="$<TARGET_FILE:pelican_wp381_legacy_system>"
    PELICAN_WP381_STALE_BEHAVIOR="$<TARGET_FILE:pelican_wp381_stale_behavior>"
    PELICAN_WP381_V2_CONTROL="$<TARGET_FILE:pelican_wp381_v2_constructor_control>"
    PELICAN_WP381_DUPLICATE_KEY="$<TARGET_FILE:pelican_wp381_duplicate_key>"
    PELICAN_WP381_REVERSE_A="$<TARGET_FILE:pelican_wp381_reverse_a>"
    PELICAN_WP381_REVERSE_B="$<TARGET_FILE:pelican_wp381_reverse_b>"
    PELICAN_WP381_PROJECT_EXAMPLE="$<TARGET_FILE:pelican_wp381_project_example>"
    PELICAN_WP381_PROJECT_SPRITE="$<TARGET_FILE:pelican_wp381_project_sprite_demo>"
    PELICAN_WP381_STALE_SOURCE_DEPENDENCIES="${wp381_stale_source_dependencies}"
    PELICAN_WP381_CURRENT_USERPUBLIC="${CMAKE_SOURCE_DIR}/src/core/userpublic"
    PELICAN_WP382A_FROZEN_V2="$<TARGET_FILE:pelican_wp382a_frozen_v2>"
    PELICAN_WP382A_V3_FULL="$<TARGET_FILE:pelican_wp382a_v3_full>"
    PELICAN_WP382A_V3_BEHAVIOR_SAME="$<TARGET_FILE:pelican_wp382a_v3_behavior_same>"
    PELICAN_WP382A_V3_BEHAVIOR_DEFAULT="$<TARGET_FILE:pelican_wp382a_v3_behavior_default>"
    PELICAN_WP382A_V3_BEHAVIOR_ORDER="$<TARGET_FILE:pelican_wp382a_v3_behavior_order>"
    PELICAN_WP382A_V3_BEHAVIOR_VOCABULARY="$<TARGET_FILE:pelican_wp382a_v3_behavior_vocabulary>"
    PELICAN_WP382A_V3_BEHAVIOR_PRESENCE="$<TARGET_FILE:pelican_wp382a_v3_behavior_presence>"
    PELICAN_WP382A_V3_EVENT_SAME="$<TARGET_FILE:pelican_wp382a_v3_event_same>"
    PELICAN_WP382A_V3_EVENT_DEFAULT="$<TARGET_FILE:pelican_wp382a_v3_event_default>"
    PELICAN_WP382A_V3_EVENT_ORDER="$<TARGET_FILE:pelican_wp382a_v3_event_order>"
    PELICAN_WP382A_V3_EVENT_VOCABULARY="$<TARGET_FILE:pelican_wp382a_v3_event_vocabulary>"
    PELICAN_WP382A_V3_EVENT_PRESENCE="$<TARGET_FILE:pelican_wp382a_v3_event_presence>"
    PELICAN_WP382A_V3_COMPONENT_SAME="$<TARGET_FILE:pelican_wp382a_v3_component_same>"
    PELICAN_WP382A_V3_COMPONENT_DEFAULT="$<TARGET_FILE:pelican_wp382a_v3_component_default>"
    PELICAN_WP382A_V3_COMPONENT_ORDER="$<TARGET_FILE:pelican_wp382a_v3_component_order>"
    PELICAN_WP382A_V3_COMPONENT_VOCABULARY="$<TARGET_FILE:pelican_wp382a_v3_component_vocabulary>"
    PELICAN_WP382A_V3_COMPONENT_PRESENCE="$<TARGET_FILE:pelican_wp382a_v3_component_presence>"
  )
  if(PELICAN_WITH_ANIMATION)
    target_compile_definitions(pelican_test_game_logic_abi_test PRIVATE
      PELICAN_WP381_PROJECT_ANIMGRAPH="$<TARGET_FILE:pelican_wp381_project_animgraph_demo>"
      PELICAN_WP381_PROJECT_VRM_XR="$<TARGET_FILE:pelican_wp381_project_vrm_xr_demo>"
    )
  endif()

  get_filename_component(wp381_msvc_tool_dir "${CMAKE_LINKER}" DIRECTORY)
  add_test(
    NAME game_logic_abi_post_link_audit
    COMMAND ${CMAKE_COMMAND}
      -DDUMPBIN=${wp381_msvc_tool_dir}/dumpbin.exe
      -DSTALE_DLL=$<TARGET_FILE:pelican_wp381_stale_behavior>
      -DHOST_EXE=$<TARGET_FILE:pelican_test_game_logic_abi_test>
      -DSOURCE_DEPENDENCIES=${wp381_stale_source_dependencies}
      -DCURRENT_USERPUBLIC=${CMAKE_SOURCE_DIR}/src/core/userpublic
      -DEXPECTED_HOST=pelican_test_game_logic_abi_test.exe
      -P ${CMAKE_CURRENT_SOURCE_DIR}/wp381_post_link_audit.cmake
  )
  set_tests_properties(game_logic_abi_post_link_audit PROPERTIES TIMEOUT 30)

  add_library(pelican_public_api_link_fixture SHARED
    fixtures/public_api_link/flipbook_client.cpp
  )
  set_target_properties(pelican_public_api_link_fixture PROPERTIES
    OUTPUT_NAME "pelican_public_api_link_fixture"
  )
  pelican_configure_game_logic_target(pelican_public_api_link_fixture
    LINK_ONLY ROLE public-api-link)

  foreach(wp90_version IN ITEMS 1 2)
    set(wp90_target pelican_wp90_fixture_v${wp90_version})
    add_library(${wp90_target} SHARED
      ${CMAKE_SOURCE_DIR}/projects/example/code/playercontrol.cpp
    )
    set_target_properties(${wp90_target} PROPERTIES OUTPUT_NAME "pelican_wp90_fixture_v${wp90_version}")
    target_compile_definitions(${wp90_target} PRIVATE
      WP90_BEHAVIOR_VERSION=${wp90_version}
      WP90_HOT_RELOAD_FIXTURE=1
    )
    pelican_configure_game_logic_target(${wp90_target})
  endforeach()

  add_library(pelican_wp90_fixture_bad_abi SHARED
    ${CMAKE_SOURCE_DIR}/projects/example/code/playercontrol.cpp
  )
  set_target_properties(pelican_wp90_fixture_bad_abi PROPERTIES OUTPUT_NAME "pelican_wp90_fixture_bad_abi")
  target_compile_definitions(pelican_wp90_fixture_bad_abi PRIVATE
    WP90_BEHAVIOR_VERSION=2
    WP90_HOT_RELOAD_FIXTURE=1
    PELICAN_GAME_LOGIC_ABI_VERSION=999
  )
  pelican_configure_game_logic_target(pelican_wp90_fixture_bad_abi
    ROLE epoch-mismatch)
  add_dependencies(pelican_wp90_fixture_bad_abi
    pelican_wp381_epoch_v999_import_stub)
  target_link_libraries(pelican_wp90_fixture_bad_abi PRIVATE
    "${wp381_epoch_v999_import_library}")

  add_library(pelican_wp155_behavior_fixture SHARED
    fixtures/behavior_game_dll/behavior.cpp
  )
  set_target_properties(pelican_wp155_behavior_fixture PROPERTIES
    OUTPUT_NAME "pelican_wp155_behavior_fixture"
  )
  pelican_configure_game_logic_target(pelican_wp155_behavior_fixture)

  if(PELICAN_WITH_PHYSICS)
    add_library(pelican_wp179_trigger_behavior_fixture SHARED
      fixtures/physics_trigger_behavior/behavior.cpp
    )
    set_target_properties(pelican_wp179_trigger_behavior_fixture PROPERTIES
      OUTPUT_NAME "pelican_wp179_trigger_behavior_fixture"
    )
    pelican_configure_game_logic_target(pelican_wp179_trigger_behavior_fixture)

    add_test(
      NAME physics_trigger_behavior_e2e
      COMMAND $<TARGET_FILE:pelican_gpu_lease_run> -- powershell -NoProfile -ExecutionPolicy Bypass
        -File ${CMAKE_CURRENT_SOURCE_DIR}/run_physics_trigger_behavior.ps1
        -Player $<TARGET_FILE:pelican_player>
        -Fixture $<TARGET_FILE:pelican_wp179_trigger_behavior_fixture>
        -ProjectPass ${CMAKE_SOURCE_DIR}/projects/example/passes/main_rendering_config.json
        -OutDir ${CMAKE_BINARY_DIR}/test_artifacts/physics_trigger_behavior
    )
    set_tests_properties(physics_trigger_behavior_e2e PROPERTIES
      LABELS gpu)
    pelican_gpu_test_properties(physics_trigger_behavior_e2e 120)
  endif()

  function(pelican_add_wp162_behavior_fixture target_name variant)
    add_library(${target_name} SHARED
      fixtures/behavior_reload_dll/behavior.cpp
    )
    set_target_properties(${target_name} PROPERTIES OUTPUT_NAME "${target_name}")
    target_compile_definitions(${target_name} PRIVATE WP162_VARIANT=${variant})
    pelican_configure_game_logic_target(${target_name})
  endfunction()

  pelican_add_wp162_behavior_fixture(pelican_wp162_fixture_v1 1)
  pelican_add_wp162_behavior_fixture(pelican_wp162_fixture_code_v2 2)
  pelican_add_wp162_behavior_fixture(pelican_wp162_fixture_drift 3)
  pelican_add_wp162_behavior_fixture(pelican_wp162_fixture_bump_ok 4)
  pelican_add_wp162_behavior_fixture(pelican_wp162_fixture_bump_bad 5)
  pelican_add_wp162_behavior_fixture(pelican_wp162_fixture_removed 6)
  pelican_add_wp162_behavior_fixture(pelican_wp162_fixture_init_fault 7)

  foreach(wp162_case IN ITEMS
      code_only
      schema_drift
      version_bump_success
      version_bump_failure
      type_removed
      candidate_lifecycle_zero
      pending_recovery
      queued_event_purge
      oninit_rollback)
    add_test(
      NAME behavior_dll_reload_${wp162_case}
       COMMAND $<TARGET_FILE:pelican_gpu_lease_run> -- powershell -NoProfile -ExecutionPolicy Bypass
        -File ${CMAKE_CURRENT_SOURCE_DIR}/run_behavior_dll_reload.ps1
        -Case ${wp162_case}
        -Player $<TARGET_FILE:pelican_player>
        -FixtureV1 $<TARGET_FILE:pelican_wp162_fixture_v1>
        -FixtureCodeV2 $<TARGET_FILE:pelican_wp162_fixture_code_v2>
        -FixtureDrift $<TARGET_FILE:pelican_wp162_fixture_drift>
        -FixtureBumpOk $<TARGET_FILE:pelican_wp162_fixture_bump_ok>
        -FixtureBumpBad $<TARGET_FILE:pelican_wp162_fixture_bump_bad>
        -FixtureRemoved $<TARGET_FILE:pelican_wp162_fixture_removed>
        -FixtureInitFault $<TARGET_FILE:pelican_wp162_fixture_init_fault>
        -ProjectPass ${CMAKE_SOURCE_DIR}/projects/example/passes/main_rendering_config.json
        -OutDir ${CMAKE_BINARY_DIR}/test_artifacts/behavior_dll_reload_${wp162_case}
    )
    set_tests_properties(behavior_dll_reload_${wp162_case} PROPERTIES
      LABELS gpu)
    pelican_gpu_test_properties(behavior_dll_reload_${wp162_case} 120)
  endforeach()

  add_test(
    NAME behavior_project_e2e
     COMMAND $<TARGET_FILE:pelican_gpu_lease_run> -- powershell -NoProfile -ExecutionPolicy Bypass
      -File ${CMAKE_CURRENT_SOURCE_DIR}/run_behavior_project.ps1
      -Player $<TARGET_FILE:pelican_player>
      -Fixture $<TARGET_FILE:pelican_wp155_behavior_fixture>
      -ProjectPass ${CMAKE_SOURCE_DIR}/projects/example/passes/main_rendering_config.json
      -OutDir ${CMAKE_BINARY_DIR}/test_artifacts/behavior_project
  )
  set_tests_properties(behavior_project_e2e PROPERTIES
    LABELS gpu)
  pelican_gpu_test_properties(behavior_project_e2e 120)

  add_test(
    NAME game_logic_dll_reload
     COMMAND $<TARGET_FILE:pelican_gpu_lease_run> -- powershell -NoProfile -ExecutionPolicy Bypass
      -File ${CMAKE_CURRENT_SOURCE_DIR}/run_game_logic_reload.ps1
      -Player $<TARGET_FILE:pelican_player>
      -FixtureV1 $<TARGET_FILE:pelican_wp90_fixture_v1>
      -FixtureV2 $<TARGET_FILE:pelican_wp90_fixture_v2>
      -FixtureBadAbi $<TARGET_FILE:pelican_wp90_fixture_bad_abi>
      -ProjectPass ${CMAKE_SOURCE_DIR}/projects/example/passes/main_rendering_config.json
      -FixtureModel ${CMAKE_SOURCE_DIR}/test/fixtures/ground.glb
      -OutDir ${CMAKE_BINARY_DIR}/test_artifacts/game_logic_reload
  )
  set_tests_properties(game_logic_dll_reload PROPERTIES
    LABELS gpu)
  pelican_gpu_test_properties(game_logic_dll_reload 120)

  # The executable exports the real SDK symbol consumed by fixture DLL static
  # registration. Fixtures include only userpublic headers and link the host's
  # import library, matching a shipped game DLL rather than an in-process fake.
  add_executable(pelican_test_render_policy_dll_test
    render_policy_dll_test.cpp
  )
  set_target_properties(pelican_test_render_policy_dll_test PROPERTIES
    CXX_STANDARD 20
    ENABLE_EXPORTS TRUE
  )
  target_include_directories(pelican_test_render_policy_dll_test PRIVATE
    ${Vulkan_INCLUDE_DIRS}
  )
  target_compile_definitions(pelican_test_render_policy_dll_test PRIVATE
    "VULKAN_HPP_DISPATCH_LOADER_STATIC_TYPE=::vk::detail::DispatchLoaderStatic"
  )
  if(MSVC)
    target_link_options(pelican_test_render_policy_dll_test PRIVATE
      "/WHOLEARCHIVE:$<TARGET_LINKER_FILE:pelican_core>"
    )
  endif()
  target_link_libraries(pelican_test_render_policy_dll_test PRIVATE
    Catch2::Catch2WithMain
    nlohmann_json
    pelican_core
    glm
  )

  foreach(render_policy_fixture_version IN ITEMS 1 2)
    set(render_policy_fixture_target
        pelican_render_policy_fixture_v${render_policy_fixture_version})
    add_library(${render_policy_fixture_target} SHARED
      fixtures/render_policy_dll/provider.cpp
    )
    set_target_properties(${render_policy_fixture_target} PROPERTIES
      OUTPUT_NAME "pelican_render_policy_fixture_v${render_policy_fixture_version}"
    )
    pelican_configure_game_logic_target(${render_policy_fixture_target}
      HOST_TARGET pelican_test_render_policy_dll_test
      ROLE render-provider)
    target_compile_definitions(${render_policy_fixture_target} PRIVATE
      PELICAN_RENDER_POLICY_FIXTURE_VERSION=${render_policy_fixture_version}
    )
  endforeach()

  add_library(pelican_render_policy_fixture_bad_abi SHARED
    fixtures/render_policy_dll/provider.cpp
  )
  set_target_properties(pelican_render_policy_fixture_bad_abi PROPERTIES
    CXX_STANDARD 20
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
    OUTPUT_NAME "pelican_render_policy_fixture_bad_abi"
  )
  add_dependencies(pelican_render_policy_fixture_bad_abi
    pelican_test_render_policy_dll_test)
  target_link_libraries(pelican_render_policy_fixture_bad_abi PRIVATE
    "$<TARGET_LINKER_FILE:pelican_test_render_policy_dll_test>"
  )
  target_include_directories(pelican_render_policy_fixture_bad_abi PRIVATE
    ${CMAKE_SOURCE_DIR}/src/core/userpublic
  )
  target_compile_definitions(pelican_render_policy_fixture_bad_abi PRIVATE
    PELICAN_GAME_DLL=1
    PELICAN_RENDER_POLICY_FIXTURE_VERSION=2
  )
  pelican_mark_game_logic_target(pelican_render_policy_fixture_bad_abi
    loader provider-only-stale)
  set_property(TARGET pelican_render_policy_fixture_bad_abi PROPERTY
    PELICAN_GAME_DLL_HOST_TARGET pelican_test_render_policy_dll_test)

  add_test(
    NAME render_policy_game_dll_e2e
    COMMAND ${CMAKE_COMMAND} -E env
      "PELICAN_RENDER_POLICY_FIXTURE_V1=$<TARGET_FILE:pelican_render_policy_fixture_v1>"
      "PELICAN_RENDER_POLICY_FIXTURE_V2=$<TARGET_FILE:pelican_render_policy_fixture_v2>"
      "PELICAN_RENDER_POLICY_FIXTURE_BAD_ABI=$<TARGET_FILE:pelican_render_policy_fixture_bad_abi>"
      $<TARGET_FILE:pelican_test_render_policy_dll_test>
      "[render-policy-dll]"
  )
  set_tests_properties(render_policy_game_dll_e2e PROPERTIES TIMEOUT 30)

  if(PELICAN_WITH_PHYSICS)
    # This executable is the real SDK export host for the fixture DLLs. The
    # fixtures link only its import library and see only userpublic headers.
    add_executable(pelican_test_physics_provider_dll_test
      physics_provider_dll_test.cpp
    )
    set_target_properties(pelican_test_physics_provider_dll_test PROPERTIES
      CXX_STANDARD 20
      ENABLE_EXPORTS TRUE
    )
    target_include_directories(pelican_test_physics_provider_dll_test PRIVATE
      ${Vulkan_INCLUDE_DIRS}
    )
    target_compile_definitions(pelican_test_physics_provider_dll_test PRIVATE
      "VULKAN_HPP_DISPATCH_LOADER_STATIC_TYPE=::vk::detail::DispatchLoaderStatic"
      PELICAN_WITH_BUILTIN_PHYSICS=$<BOOL:${PELICAN_WITH_BUILTIN_PHYSICS}>
      PELICAN_WITH_JOLT_PHYSICS=$<BOOL:${PELICAN_WITH_JOLT_PHYSICS}>
    )
    if(MSVC)
      target_link_options(pelican_test_physics_provider_dll_test PRIVATE
        "/WHOLEARCHIVE:$<TARGET_LINKER_FILE:pelican_core>"
      )
    endif()
    target_link_libraries(pelican_test_physics_provider_dll_test PRIVATE
      Catch2::Catch2WithMain
      nlohmann_json
      pelican_core
      glm
    )

    foreach(physics_fixture_revision IN ITEMS 1 2)
      if(physics_fixture_revision EQUAL 1)
        set(physics_fixture_name initial)
      else()
        set(physics_fixture_name updated)
      endif()
      set(physics_fixture_target
          pelican_physics_provider_fixture_${physics_fixture_name})
      add_library(${physics_fixture_target} SHARED
        fixtures/physics_provider_dll/provider.cpp
      )
      set_target_properties(${physics_fixture_target} PROPERTIES
        OUTPUT_NAME "pelican_physics_provider_fixture_${physics_fixture_name}"
      )
      pelican_configure_game_logic_target(${physics_fixture_target}
        HOST_TARGET pelican_test_physics_provider_dll_test
        ROLE physics-provider)
      target_compile_definitions(${physics_fixture_target} PRIVATE
        PELICAN_PHYSICS_FIXTURE_REVISION=${physics_fixture_revision}
      )
    endforeach()

    add_library(pelican_physics_provider_fixture_bad_abi SHARED
      fixtures/physics_provider_dll/provider.cpp
    )
    set_target_properties(pelican_physics_provider_fixture_bad_abi PROPERTIES
      CXX_STANDARD 20
      MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
      OUTPUT_NAME "pelican_physics_provider_fixture_bad_abi"
    )
    add_dependencies(pelican_physics_provider_fixture_bad_abi
      pelican_test_physics_provider_dll_test)
    target_link_libraries(pelican_physics_provider_fixture_bad_abi PRIVATE
      "$<TARGET_LINKER_FILE:pelican_test_physics_provider_dll_test>"
    )
    target_include_directories(pelican_physics_provider_fixture_bad_abi PRIVATE
      ${CMAKE_SOURCE_DIR}/src/core/userpublic
    )
    target_compile_definitions(pelican_physics_provider_fixture_bad_abi PRIVATE
      PELICAN_GAME_DLL=1
      PELICAN_PHYSICS_FIXTURE_REVISION=2
    )
    pelican_mark_game_logic_target(pelican_physics_provider_fixture_bad_abi
      loader provider-only-stale)
    set_property(TARGET pelican_physics_provider_fixture_bad_abi PROPERTY
      PELICAN_GAME_DLL_HOST_TARGET pelican_test_physics_provider_dll_test)

    add_test(
      NAME physics_provider_game_dll_e2e
      COMMAND ${CMAKE_COMMAND} -E env
        "PELICAN_PHYSICS_FIXTURE_INITIAL=$<TARGET_FILE:pelican_physics_provider_fixture_initial>"
        "PELICAN_PHYSICS_FIXTURE_UPDATED=$<TARGET_FILE:pelican_physics_provider_fixture_updated>"
        "PELICAN_PHYSICS_FIXTURE_BAD_ABI=$<TARGET_FILE:pelican_physics_provider_fixture_bad_abi>"
        $<TARGET_FILE:pelican_test_physics_provider_dll_test>
        "[physics-provider-dll]"
    )
    set_tests_properties(physics_provider_game_dll_e2e PROPERTIES TIMEOUT 30)
  endif()
endif()

add_executable(pelican_test_vat_fixture_writer vat_fixture_writer.cpp)
set_target_properties(pelican_test_vat_fixture_writer PROPERTIES CXX_STANDARD 20)
target_link_libraries(pelican_test_vat_fixture_writer PRIVATE nlohmann_json)

add_executable(pelican_test_gltf_scene_fixture_writer gltf_scene_fixture_writer.cpp)
set_target_properties(pelican_test_gltf_scene_fixture_writer PROPERTIES CXX_STANDARD 20)
target_link_libraries(pelican_test_gltf_scene_fixture_writer PRIVATE nlohmann_json)

add_executable(pelican_test_vrm_fixture_writer vrm_fixture_writer.cpp)
set_target_properties(pelican_test_vrm_fixture_writer PROPERTIES CXX_STANDARD 20)
target_link_libraries(pelican_test_vrm_fixture_writer PRIVATE nlohmann_json)

add_executable(pelican_test_vrma_fixture_writer vrma_fixture_writer.cpp)
set_target_properties(pelican_test_vrma_fixture_writer PROPERTIES CXX_STANDARD 20)
target_link_libraries(pelican_test_vrma_fixture_writer PRIVATE nlohmann_json)

add_executable(pelican_test_vrm_expression_preview_writer
  vrm_expression_preview_writer.cpp)
set_target_properties(pelican_test_vrm_expression_preview_writer
  PROPERTIES CXX_STANDARD 20)
target_link_libraries(pelican_test_vrm_expression_preview_writer PRIVATE
  nlohmann_json glm)
target_compile_definitions(pelican_test_vrm_expression_preview_writer PRIVATE
  PELICAN_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

# register tests
pelican_define_test(module_container_test GROUP core_foundation pelican_core)
target_compile_definitions(pelican_test_module_container_test PRIVATE
    PELICAN_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
pelican_define_test(log_test GROUP core_foundation pelican_core)
pelican_define_test(deletionqueue_test GROUP core_foundation pelican_core)
pelican_define_test(lifetime_teardown_test GROUP core_foundation pelican_core)
pelican_define_test(deterministicrng_test GROUP core_foundation pelican_core)
pelican_define_test(scene_transaction_contract_test GROUP core_foundation pelican_core)
target_sources(pelican_test_scene_transaction_contract_test PRIVATE
    sceneloadfault_testing.cpp)
pelican_define_test(enginetime_test GROUP core_foundation pelican_core)
pelican_define_test(skeletalanimation_test GROUP core_foundation pelican_core glm)
pelican_define_test(vrmsemantic_test GROUP core_foundation pelican_core tinygltf)
target_compile_definitions(pelican_test_vrmsemantic_test PRIVATE
    PELICAN_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
pelican_define_test(vrmadecoder_test GROUP core_foundation pelican_core tinygltf picosha2)
# The animation unit's own tests exist only when it is built. The common-graph
# tests beside them (value / render / audio) stay in the group either way.
if(PELICAN_WITH_ANIMATION)
pelican_define_test(vrmaretarget_test GROUP core_foundation pelican_core glm)
pelican_define_test(animation_probe_test GROUP core_foundation pelican_core glm)
pelican_define_test(animation_jobs_test GROUP core_foundation pelican_core glm)
pelican_define_test(animgraph_test GROUP core_foundation pelican_core glm)
pelican_define_test(posegraph_test GROUP core_foundation pelican_core glm)
target_compile_definitions(pelican_test_animation_probe_test PRIVATE
    PELICAN_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
else()
# The OFF build's own acceptance: a scene that animates is refused by name at
# prepare, and the scene that was already live survives the refusal.
pelican_define_test(animation_off_scene_test GROUP core_foundation pelican_core)
endif()
pelican_define_test(curve_test GROUP core_foundation pelican_core)
# The test-only graph module lives with the test that needs it; the production
# archives are checked for its symbol below.
target_sources(pelican_test_curve_test PRIVATE graphtestopaquemodule.cpp)
if(PELICAN_WITH_ANIMATION)
pelican_define_test(vrmasource_test GPU RUNTIME_SHADER pelican_core VulkanMemoryAllocator
                    VulkanMemoryAllocator-Hpp glm)
endif()

if(WIN32 AND PELICAN_WITH_ANIMATION)
  add_library(pelican_animation_abi_old SHARED fixtures/animation_abi/old_client.cpp)
  add_library(pelican_animation_abi_new SHARED fixtures/animation_abi/new_client.cpp)
  add_library(pelican_animation_public_evaluator SHARED fixtures/animation_abi/public_evaluator.cpp)
  add_library(pelican_vrm_application_public_evaluator SHARED
    fixtures/animation_abi/vrm_application_evaluator.cpp)
  foreach(animation_abi_fixture IN ITEMS pelican_animation_abi_old pelican_animation_abi_new
                                         pelican_animation_public_evaluator
                                         pelican_vrm_application_public_evaluator)
    set_target_properties(${animation_abi_fixture} PROPERTIES
      CXX_STANDARD 20
      MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
    )
    target_include_directories(${animation_abi_fixture} PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/animation_abi
      ${CMAKE_SOURCE_DIR}/src/core/userpublic
    )
  endforeach()
  pelican_define_test(animation_abi_dll_test pelican_core)
  add_dependencies(pelican_test_animation_abi_dll_test pelican_animation_abi_old pelican_animation_abi_new)
  add_dependencies(pelican_test_animation_abi_dll_test pelican_animation_public_evaluator)
  target_compile_definitions(pelican_test_animation_abi_dll_test PRIVATE
    PELICAN_ANIM_OLD_DLL="$<TARGET_FILE:pelican_animation_abi_old>"
    PELICAN_ANIM_NEW_DLL="$<TARGET_FILE:pelican_animation_abi_new>"
    PELICAN_ANIM_EVALUATOR_DLL="$<TARGET_FILE:pelican_animation_public_evaluator>"
  )
  pelican_define_test(vrmapplication_test GPU RUNTIME_SHADER pelican_core VulkanMemoryAllocator
                      VulkanMemoryAllocator-Hpp glm)
  add_dependencies(pelican_test_vrmapplication_test
                   pelican_vrm_application_public_evaluator)
  target_compile_definitions(pelican_test_vrmapplication_test PRIVATE
    PELICAN_VRM_APPLICATION_EVALUATOR_DLL="$<TARGET_FILE:pelican_vrm_application_public_evaluator>"
  )
endif()
pelican_define_test(ecs_lifecycle_test GROUP ecs_editor pelican_core)
pelican_define_test(ecs_migration_test GROUP ecs_editor pelican_core)
pelican_define_test(ecs_scheduler_test GROUP ecs_editor pelican_core)
pelican_define_test(registration_lifetime_test GROUP ecs_editor pelican_core)
pelican_define_test(component_namespace_test GROUP ecs_editor pelican_core)
pelican_define_test(eventlayer_test GROUP ecs_editor pelican_core)
pelican_define_test(eventpayloadschema_test GROUP ecs_editor pelican_core)
pelican_define_test(structfieldschema_test GROUP ecs_editor pelican_core)
pelican_define_test(wp382a_schema_test pelican_core pelican_project)
add_executable(pelican_test_wp382a_descriptor_allocation_probe
  wp382a_descriptor_allocation_probe.cpp)
set_target_properties(pelican_test_wp382a_descriptor_allocation_probe
  PROPERTIES CXX_STANDARD 20)
target_include_directories(pelican_test_wp382a_descriptor_allocation_probe
  PRIVATE "${CMAKE_SOURCE_DIR}/src/core/userpublic")
target_link_libraries(pelican_test_wp382a_descriptor_allocation_probe
  PRIVATE nlohmann_json)
add_test(NAME wp382a_descriptor_static_initialization_allocation_zero
  COMMAND $<TARGET_FILE:pelican_test_wp382a_descriptor_allocation_probe>)
set_tests_properties(wp382a_descriptor_static_initialization_allocation_zero
  PROPERTIES LABELS "WP382a;cpu;schema-v3")
pelican_define_test(behaviorarena_test GROUP ecs_editor pelican_core)

add_executable(pelican_test_behavior_determinism_probe behavior_determinism_probe.cpp)
set_target_properties(pelican_test_behavior_determinism_probe PROPERTIES CXX_STANDARD 20)
target_include_directories(pelican_test_behavior_determinism_probe PRIVATE ${Vulkan_INCLUDE_DIRS})
target_compile_definitions(pelican_test_behavior_determinism_probe PRIVATE
  "VULKAN_HPP_DISPATCH_LOADER_STATIC_TYPE=::vk::detail::DispatchLoaderStatic"
)
target_link_libraries(pelican_test_behavior_determinism_probe PRIVATE pelican_core nlohmann_json)
add_test(
  NAME behavior_determinism_four_processes
  COMMAND ${CMAKE_COMMAND}
    -DPROBE=$<TARGET_FILE:pelican_test_behavior_determinism_probe>
    -P ${CMAKE_CURRENT_SOURCE_DIR}/run_behavior_determinism.cmake
)
pelican_define_test(componentcodec_test GROUP ecs_editor pelican_core glm)
pelican_define_test(wp378_resolution_test GROUP ecs_editor pelican_core glm)
pelican_define_test(wp375_editor_projection_test GROUP ecs_editor pelican_core glm)
pelican_define_test(wp382b_editor_test GROUP ecs_editor pelican_core)
pelican_define_test(editorprojectiontransaction_test GROUP ecs_editor pelican_core glm
  VulkanMemoryAllocator VulkanMemoryAllocator-Hpp)
pelican_define_test(editorpreviewprojection_test GROUP ecs_editor pelican_core glm)
pelican_define_test(editorpreview_test GROUP ecs_editor pelican_core glm)
target_compile_definitions(pelican_test_componentcodec_test PRIVATE
  PELICAN_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
target_compile_definitions(pelican_test_wp378_resolution_test PRIVATE
  PELICAN_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
if(WIN32)
  add_library(pelican_wp378_structfield_abi_fixture SHARED
    fixtures/wp378_abi/old_client.cpp
  )
  set_target_properties(pelican_wp378_structfield_abi_fixture PROPERTIES
    CXX_STANDARD 20
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
    OUTPUT_NAME "pelican_wp378_structfield_abi_fixture"
  )
  target_include_directories(pelican_wp378_structfield_abi_fixture PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/wp378_abi
  )
  add_dependencies(pelican_test_wp378_resolution_test
    pelican_wp378_structfield_abi_fixture)
  target_compile_definitions(pelican_test_wp378_resolution_test PRIVATE
    PELICAN_WP378_STRUCTFIELD_ABI_DLL="$<TARGET_FILE:pelican_wp378_structfield_abi_fixture>"
  )
endif()
pelican_define_test(editorcommandservice_test GROUP ecs_editor pelican_core glm picosha2)
target_compile_definitions(pelican_test_editorcommandservice_test PRIVATE
  PELICAN_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
pelican_define_test(editorjournal_test GROUP ecs_editor pelican_core glm)
pelican_define_test(wp360_resolvedscene_test pelican_core glm picosha2)
target_compile_definitions(pelican_test_wp360_resolvedscene_test PRIVATE
  PELICAN_WP360_RPC_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/wp360/get_components.parent.ndjson"
  PELICAN_WP360_SKIP_DEVSTUDIO=$<BOOL:${SKIP_DEVSTUDIO}>
)
pelican_define_test(wp361_prefab_test pelican_core glm picosha2)
target_compile_definitions(pelican_test_wp361_prefab_test PRIVATE
  PELICAN_WITH_IMGUI=$<BOOL:${PELICAN_WITH_IMGUI}>
)
if(MSVC)
  target_compile_options(pelican_test_wp361_prefab_test PRIVATE /bigobj)
endif()

function(pelican_event_schema_compile_fixture fixture expect_failure)
    set(target_name pelican_event_schema_${fixture})
    add_test(
        NAME event_schema_compile_${fixture}
        COMMAND ${CMAKE_COMMAND}
            -DPROJECT_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/schema_compile_fixture
            -DBUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}/schema_compile/${target_name}
            -DFIXTURE_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/event_payload_schema/${fixture}.cpp
            -DCONFIG=$<CONFIG>
            -DTARGET_NAME=${target_name}
            -DEXPECT_FAILURE=${expect_failure}
            -DGENERATOR=${CMAKE_GENERATOR}
            -DGENERATOR_INSTANCE=${CMAKE_GENERATOR_INSTANCE}
            -DGENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}
            -DGENERATOR_TOOLSET=${CMAKE_GENERATOR_TOOLSET}
            -DTOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}
            -DCXX_COMPILER=${CMAKE_CXX_COMPILER}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/run_event_schema_compile.cmake
    )
endfunction()

pelican_event_schema_compile_fixture(compile_pass FALSE)
pelican_event_schema_compile_fixture(fail_empty_name TRUE)
pelican_event_schema_compile_fixture(fail_duplicate_name TRUE)
pelican_event_schema_compile_fixture(fail_range_kind TRUE)
pelican_event_schema_compile_fixture(fail_quat_range TRUE)
pelican_event_schema_compile_fixture(fail_min_max TRUE)
pelican_event_schema_compile_fixture(fail_nonfinite TRUE)
pelican_event_schema_compile_fixture(fail_f32_bound TRUE)
pelican_event_schema_compile_fixture(fail_unsupported_type TRUE)

function(pelican_struct_schema_compile_fixture fixture expect_failure)
    set(target_name pelican_struct_schema_${fixture})
    add_test(
        NAME struct_schema_compile_${fixture}
        COMMAND ${CMAKE_COMMAND}
            -DPROJECT_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/schema_compile_fixture
            -DBUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}/schema_compile/${target_name}
            -DFIXTURE_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/struct_schema/${fixture}.cpp
            -DCONFIG=$<CONFIG>
            -DTARGET_NAME=${target_name}
            -DEXPECT_FAILURE=${expect_failure}
            -DGENERATOR=${CMAKE_GENERATOR}
            -DGENERATOR_INSTANCE=${CMAKE_GENERATOR_INSTANCE}
            -DGENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}
            -DGENERATOR_TOOLSET=${CMAKE_GENERATOR_TOOLSET}
            -DTOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}
            -DCXX_COMPILER=${CMAKE_CXX_COMPILER}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/run_event_schema_compile.cmake
    )
endfunction()

pelican_struct_schema_compile_fixture(event_policy_pass FALSE)
pelican_struct_schema_compile_fixture(behavior_policy_pass FALSE)
pelican_struct_schema_compile_fixture(component_policy_pass FALSE)
pelican_struct_schema_compile_fixture(fail_policy_omitted TRUE)
pelican_struct_schema_compile_fixture(fail_event_default TRUE)
pelican_struct_schema_compile_fixture(fail_behavior_required TRUE)
pelican_struct_schema_compile_fixture(fail_behavior_enum_values TRUE)
pelican_struct_schema_compile_fixture(fail_component_implicit TRUE)
pelican_struct_schema_compile_fixture(fail_component_codec TRUE)
add_library(pelican_bloom_upsample_test_support STATIC
    bloom_upsample_oracle.cpp)
set_target_properties(pelican_bloom_upsample_test_support
    PROPERTIES CXX_STANDARD 20)
target_include_directories(pelican_bloom_upsample_test_support PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR})
add_library(pelican_golden_harness STATIC
    golden_harness.cpp
    golden_bloom_upsample_harness.cpp)
set(pelican_wp357_shader_sources
    fixtures/wp357/wp357_seed_V0.frag
    fixtures/wp357/wp357_seed_V1.frag
    fixtures/wp357/wp357_seed_V2.frag
    fixtures/wp357/wp357_seed_V3.frag
    fixtures/wp357/wp357_destination.frag
    fixtures/wp357/wp357_source.frag
    fixtures/wp357/wp357_zero.frag
    fixtures/wp357/wp357_half.frag)
set(pelican_wp357_shader_output_dir
    "${CMAKE_CURRENT_BINARY_DIR}/wp357_precompiled_shaders")
set(pelican_wp357_shader_outputs)
foreach(wp357_shader IN LISTS pelican_wp357_shader_sources)
    get_filename_component(wp357_shader_name "${wp357_shader}" NAME)
    set(wp357_shader_output
        "${pelican_wp357_shader_output_dir}/${wp357_shader_name}.spv")
    set(wp357_shader_hash_output
        "${pelican_wp357_shader_output_dir}/${wp357_shader_name}.sha256")
    add_custom_command(
        OUTPUT "${wp357_shader_output}" "${wp357_shader_hash_output}"
        COMMAND "${CMAKE_COMMAND}"
                "-DGLSLC_EXECUTABLE=${Vulkan_GLSLC_EXECUTABLE}"
                "-DSHADER_TARGET_ENV=${PELICAN_SHADER_TARGET_ENV}"
                "-DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/${wp357_shader}"
                "-DOUTPUT=${wp357_shader_output}"
                "-DHASH_OUTPUT=${wp357_shader_hash_output}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/compile_wp357_shader.cmake"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${wp357_shader}"
                "${CMAKE_CURRENT_SOURCE_DIR}/compile_wp357_shader.cmake"
        VERBATIM)
    list(APPEND pelican_wp357_shader_outputs
        "${wp357_shader_output}" "${wp357_shader_hash_output}")
endforeach()
add_custom_target(pelican_wp357_precompiled_shaders
    DEPENDS ${pelican_wp357_shader_outputs})
add_dependencies(pelican_golden_harness pelican_wp357_precompiled_shaders)
set_target_properties(pelican_golden_harness PROPERTIES CXX_STANDARD 20)
target_include_directories(pelican_golden_harness PRIVATE ${Vulkan_INCLUDE_DIRS})
if(MSVC)
    # The shared golden harness intentionally keeps all generated-project
    # fixtures in one translation unit. Its Catch assertions exceed COFF's
    # default section count before they approach compiler memory limits.
    target_compile_options(pelican_golden_harness PRIVATE /bigobj)
endif()
target_compile_definitions(pelican_golden_harness PRIVATE
    VULKAN_HPP_DISPATCH_LOADER_STATIC_TYPE=::vk::detail::DispatchLoaderStatic
    PELICAN_RUNTIME_SHADER_COMPILER=$<BOOL:${PELICAN_RUNTIME_SHADER_COMPILER}>
    PELICAN_WITH_OPENXR=$<BOOL:${PELICAN_WITH_OPENXR}>
    PELICAN_WITH_STANDARD_RENDER_ALGORITHMS=$<BOOL:${PELICAN_WITH_STANDARD_RENDER_ALGORITHMS}>
    PELICAN_WITH_VAT=$<BOOL:${PELICAN_WITH_VAT}>
    PELICAN_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    PELICAN_TEST_BINARY_DIR="${CMAKE_BINARY_DIR}"
    PELICAN_WP357_PRECOMPILED_SHADER_DIR="${pelican_wp357_shader_output_dir}"
)
target_link_libraries(pelican_golden_harness PRIVATE
    Catch2::Catch2
    nlohmann_json
    pelican_core
    VulkanMemoryAllocator
    VulkanMemoryAllocator-Hpp
    spirv-reflect-static
    stb
    glm
    picosha2
    pelican_bloom_upsample_test_support
)
if(PELICAN_RUNTIME_SHADER_COMPILER)
    target_link_libraries(pelican_golden_harness PRIVATE pelican_shaderc)
endif()

set(pelican_golden_test_sources
    golden_cases_test
    golden_temporal_test
    golden_timing_test
    golden_framegraph_test
    golden_ssao_test
    golden_ssao_blur_test
)
foreach(golden_test_source IN LISTS pelican_golden_test_sources)
    pelican_define_test(${golden_test_source} GROUP golden_runtime
        GOLDEN GPU RUNTIME_SHADER pelican_golden_harness)
    if(PELICAN_RUNTIME_SHADER_COMPILER AND COMMAND pelican_copy_shaderc_runtime)
        pelican_copy_test_shaderc_runtime(pelican_test_${golden_test_source})
    endif()
endforeach()
pelican_define_test(golden_bloom_upsample_test GROUP golden_bloom_upsample_runtime
    GOLDEN GPU pelican_golden_harness)
if(PELICAN_RUNTIME_SHADER_COMPILER AND COMMAND pelican_copy_shaderc_runtime)
    pelican_copy_test_shaderc_runtime(pelican_test_golden_bloom_upsample_test)
endif()
pelican_define_test(gltf_fragment_test GPU RUNTIME_SHADER pelican_core pelican_project VulkanMemoryAllocator VulkanMemoryAllocator-Hpp)
target_compile_definitions(pelican_test_gltf_fragment_test PRIVATE
    PELICAN_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
pelican_define_test(gltfimage_test pelican_core tinygltf)
pelican_define_test(wp382c_occurrence_test pelican_core glm)
if(PELICAN_WITH_ANIMATION)
pelican_define_test(modelhotreload_test GPU RUNTIME_SHADER pelican_core VulkanMemoryAllocator VulkanMemoryAllocator-Hpp glm)
target_sources(pelican_test_modelhotreload_test PRIVATE
    modelreloadfault_testing.cpp)
endif()
pelican_define_test(model_resource_release_test GPU pelican_core
    VulkanMemoryAllocator VulkanMemoryAllocator-Hpp glm)
target_sources(pelican_test_model_resource_release_test PRIVATE
    modelreleasefault_testing.cpp)
pelican_define_test(drawqueuebuilder_test GROUP renderer_registry pelican_core)
pelican_define_test(accelerationstructurepolicy_test GROUP renderer_registry pelican_core
                    VulkanMemoryAllocator VulkanMemoryAllocator-Hpp)
pelican_define_test(renderpolicyregistry_test GROUP renderer_registry pelican_core)
pelican_define_test(passimplementationregistry_test GROUP renderer_registry pelican_core)
pelican_define_test(subgraphreplacementregistry_test GROUP renderer_registry pelican_core)
pelican_define_test(graphtransformregistry_test GROUP renderer_registry pelican_core)
pelican_define_test(renderstrategyregistry_test GROUP renderer_registry pelican_core)
pelican_define_test(modelinstance_slotmap_test GPU pelican_core VulkanMemoryAllocator VulkanMemoryAllocator-Hpp glm)
if(PELICAN_WITH_ANIMATION)
pelican_define_test(skeletal_gltf_test GROUP skeletal_gltf_runtime GPU RUNTIME_SHADER
                    pelican_core VulkanMemoryAllocator VulkanMemoryAllocator-Hpp glm)
pelican_define_test(morph_gltf_test GROUP skeletal_gltf_runtime GPU RUNTIME_SHADER
                    pelican_core VulkanMemoryAllocator VulkanMemoryAllocator-Hpp glm)
endif()
pelican_define_test(vrmfirstperson_test pelican_core glm)
if(PELICAN_WITH_ANIMATION)
pelican_define_test(vrm_xr_demo_test GPU RUNTIME_SHADER pelican_core VulkanMemoryAllocator VulkanMemoryAllocator-Hpp glm)
target_compile_definitions(pelican_test_vrm_xr_demo_test PRIVATE
    PELICAN_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
endif()
pelican_define_test(materialinstanceoverride_test GPU pelican_core VulkanMemoryAllocator VulkanMemoryAllocator-Hpp glm)
# SceneLoader roundtrip case が Vulkan instance を要求するため GPU
# (validation layer なしの CI runner では ErrorLayerNotPresent)。
pelican_define_test(gltf_scene_extract_test GPU pelican_core VulkanMemoryAllocator VulkanMemoryAllocator-Hpp argparse tinygltf glm)
target_sources(pelican_test_gltf_scene_extract_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/devcli/gltfsceneextract.cpp
)
pelican_define_test(headless_render_test GROUP headless_gpu GPU pelican_core
                    pelican_test_raytracing_oracle
                    VulkanMemoryAllocator VulkanMemoryAllocator-Hpp)
target_compile_definitions(pelican_test_headless_render_test PRIVATE
    VULKAN_HPP_DISPATCH_LOADER_STATIC_TYPE=::vk::detail::DispatchLoaderStatic
    PELICAN_RUNTIME_SHADER_COMPILER=$<BOOL:${PELICAN_RUNTIME_SHADER_COMPILER}>
    PELICAN_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
if(MSVC)
    # This integration translation unit intentionally aggregates the complete
    # headless renderer matrix; WP281 adds another GPU lifecycle scenario.
    target_compile_options(pelican_test_headless_render_test PRIVATE /bigobj)
endif()
target_sources(pelican_test_headless_render_test PRIVATE
    headless_native_scope_test.cpp
)
if(PELICAN_RUNTIME_SHADER_COMPILER)
    target_link_libraries(pelican_test_headless_render_test PRIVATE pelican_shaderc)
    if(COMMAND pelican_copy_shaderc_runtime)
        pelican_copy_test_shaderc_runtime(pelican_test_headless_render_test)
    endif()
endif()
pelican_define_test(multiview_execution_test GROUP headless_gpu GPU pelican_core VulkanMemoryAllocator
                    VulkanMemoryAllocator-Hpp)
target_compile_definitions(pelican_test_multiview_execution_test PRIVATE
    PELICAN_RUNTIME_SHADER_COMPILER=$<BOOL:${PELICAN_RUNTIME_SHADER_COMPILER}>
)
if(PELICAN_RUNTIME_SHADER_COMPILER)
    target_link_libraries(pelican_test_multiview_execution_test PRIVATE pelican_shaderc)
    if(COMMAND pelican_copy_shaderc_runtime)
        pelican_copy_test_shaderc_runtime(pelican_test_multiview_execution_test)
    endif()
endif()
pelican_define_test(gamesystem_test GROUP view_temporal pelican_core)
pelican_define_test(lightpolicy_test GROUP view_temporal pelican_core VulkanMemoryAllocator VulkanMemoryAllocator-Hpp)
target_compile_definitions(pelican_test_gamesystem_test PRIVATE
    PELICAN_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
pelican_define_test(camera_test GROUP view_temporal pelican_core glm)
pelican_define_test(temporal_test GROUP view_temporal pelican_core glm)
pelican_define_test(projectionjitter_test GROUP view_temporal pelican_core glm)
pelican_define_test(viewfamily_test GROUP view_temporal pelican_core glm)
pelican_define_test(taa_resolve_test GROUP view_temporal pelican_core glm)
target_compile_definitions(pelican_test_taa_resolve_test PRIVATE
    PELICAN_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
target_compile_definitions(pelican_test_projectionjitter_test PRIVATE
    PELICAN_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
target_compile_definitions(pelican_test_temporal_test PRIVATE
    PELICAN_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
pelican_define_test(color_pipeline_test GROUP view_temporal pelican_core glm
                    pelican_bloom_upsample_test_support)
if(PELICAN_WITH_RPC)
pelican_define_test(rpc_color_contract_test GPU RUNTIME_SHADER pelican_core VulkanMemoryAllocator VulkanMemoryAllocator-Hpp spirv-reflect-static stb glm)
target_include_directories(pelican_test_rpc_color_contract_test PRIVATE ${stb_SOURCE_DIR})
target_compile_definitions(pelican_test_rpc_color_contract_test PRIVATE
    PELICAN_RUNTIME_SHADER_COMPILER=$<BOOL:${PELICAN_RUNTIME_SHADER_COMPILER}>
)
if(PELICAN_RUNTIME_SHADER_COMPILER)
    target_link_libraries(pelican_test_rpc_color_contract_test PRIVATE pelican_shaderc)
    if(COMMAND pelican_copy_shaderc_runtime)
        pelican_copy_test_shaderc_runtime(pelican_test_rpc_color_contract_test)
    endif()
endif()
endif()
if(PELICAN_WITH_EXR)
pelican_define_test(imageloader_test GROUP core_assets_input pelican_core tinyexr)
target_compile_definitions(pelican_test_imageloader_test PRIVATE
    PELICAN_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
endif()
pelican_define_test(inputstate_test GROUP core_assets_input pelican_core)
pelican_define_test(xractivation_test quill)
if(PELICAN_WITH_OPENXR)
pelican_define_test(xrdiscovery_test GROUP core_xr_tools pelican_core pelican_openxr)
if(PELICAN_WITH_RPC)
# This source also verifies the RPC get_status projection, whose implementation
# is intentionally absent from a PELICAN_WITH_RPC=OFF build.
pelican_define_test(xrsession_test GROUP core_xr_tools pelican_core pelican_openxr)
endif()
pelican_define_test(xraction_test GROUP core_xr_tools pelican_core pelican_openxr)
pelican_define_test(xrcompositiontarget_test GROUP core_xr_tools pelican_core pelican_openxr
    VulkanMemoryAllocator VulkanMemoryAllocator-Hpp)
pelican_define_test(xrviewspace_test GROUP core_xr_tools pelican_core pelican_openxr
    VulkanMemoryAllocator VulkanMemoryAllocator-Hpp)
pelican_define_test(xrfeaturepolicy_test GROUP core_xr_tools pelican_core pelican_openxr)
endif()
pelican_define_test(inputsequence_test GROUP core_xr_tools pelican_core)
if(PELICAN_WITH_IMGUI)
pelican_define_test(imgui_isolation_test GROUP core_xr_tools pelican_core)
pelican_define_test(planviewer_test GROUP core_xr_tools pelican_core)
pelican_define_test(assetbrowser_test GROUP core_xr_tools pelican_core glm)
target_compile_definitions(pelican_test_assetbrowser_test PRIVATE
  PELICAN_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
pelican_define_test(inspector_test GROUP core_xr_tools pelican_core glm)
pelican_define_test(compiledplanviewer_test GROUP core_xr_tools
  pelican_core pelican_project glm)
FetchContent_GetProperties(imgui_vendor)
target_include_directories(pelican_test_compiledplanviewer_test PRIVATE
  ${imgui_vendor_SOURCE_DIR}
)
endif()
if(PELICAN_WITH_RPC)
pelican_define_test(jsonrpc_test pelican_project)
pelican_define_test(windowedrpchost_test GROUP runtime_services pelican_core)
endif()
if(PELICAN_WITH_RENDERDOC)
pelican_define_test(renderdoccapture_test GROUP runtime_services pelican_core)
endif()
pelican_define_test(pathresolver_test GROUP runtime_services pelican_core)
target_compile_definitions(pelican_test_pathresolver_test PRIVATE
    PELICAN_WITH_VAT=$<BOOL:${PELICAN_WITH_VAT}>
    PELICAN_RUNTIME_SHADER_COMPILER=$<BOOL:${PELICAN_RUNTIME_SHADER_COMPILER}>
    PELICAN_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
pelican_define_test(shaderresolution_test GROUP runtime_services pelican_core
    VulkanMemoryAllocator VulkanMemoryAllocator-Hpp)
target_compile_definitions(pelican_test_shaderresolution_test PRIVATE
    PELICAN_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    PELICAN_RUNTIME_SHADER_COMPILER=$<BOOL:${PELICAN_RUNTIME_SHADER_COMPILER}>
)
if(PELICAN_WITH_PHYSICS AND (PELICAN_WITH_BUILTIN_PHYSICS OR PELICAN_WITH_JOLT_PHYSICS))
pelican_define_test(trianglemesh_test GROUP runtime_services pelican_core)
endif()
if(PELICAN_WITH_PHYSICS AND PELICAN_WITH_BUILTIN_PHYSICS)
pelican_define_test(physquery_test GROUP runtime_services pelican_core)
pelican_define_test(physworld_test GROUP runtime_services pelican_core glm)
pelican_define_test(platformer_controller_test GROUP runtime_services pelican_core)
endif()
if(PELICAN_WITH_PHYSICS)
pelican_define_test(physicsservice_test GROUP runtime_services pelican_core glm)
target_compile_definitions(pelican_test_physicsservice_test PRIVATE
    PELICAN_WITH_BUILTIN_PHYSICS=$<BOOL:${PELICAN_WITH_BUILTIN_PHYSICS}>
    PELICAN_WITH_JOLT_PHYSICS=$<BOOL:${PELICAN_WITH_JOLT_PHYSICS}>
)
endif()

add_executable(pelican_test_physics_feature_probe physics_feature_probe.cpp)
set_target_properties(pelican_test_physics_feature_probe PROPERTIES CXX_STANDARD 20)
target_link_libraries(pelican_test_physics_feature_probe PRIVATE pelican_core)
add_test(NAME physics_feature_probe COMMAND pelican_test_physics_feature_probe)

# Keep the purge configuration compile- and behavior-checked even when the
# main test build enables physics. This target intentionally does not link
# pelican_core; it exercises the standalone unavailable-service stub.
add_executable(pelican_test_physics_off_feature_probe
    physics_feature_probe.cpp
    ${CMAKE_SOURCE_DIR}/src/core/phys/physicsservice_stub.cpp
)
set_target_properties(pelican_test_physics_off_feature_probe PROPERTIES CXX_STANDARD 20)
target_compile_definitions(pelican_test_physics_off_feature_probe PRIVATE
    PELICAN_WITH_PHYSICS=0
)
add_test(NAME physics_off_feature_probe COMMAND pelican_test_physics_off_feature_probe)

pelican_define_test(projectconfig_test GROUP runtime_services pelican_core glm)
target_compile_definitions(pelican_test_projectconfig_test PRIVATE
    PELICAN_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
pelican_define_test(renderingpass_helpers_test GROUP runtime_services pelican_core
                    VulkanMemoryAllocator VulkanMemoryAllocator-Hpp)
pelican_define_test(sceneformat_test GROUP runtime_services pelican_core glm)
target_compile_definitions(pelican_test_sceneformat_test PRIVATE
    PELICAN_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
if(PELICAN_WITH_SEQPLAYER)
pelican_define_test(seqplayer_test GROUP runtime_services pelican_core glm)
endif()
if(PELICAN_WITH_VAT)
pelican_define_test(vatformat_test GROUP runtime_services pelican_core glm)
endif()
pelican_define_test(shader_compile_test pelican_target_environment spirv-reflect-static stb)
pelican_define_test(parallel_prepare_test)
if(PELICAN_RUNTIME_SHADER_COMPILER)
    pelican_define_test(shader_cache_test GROUP shader_cache_runtime RUNTIME_SHADER
                        pelican_core spirv-reflect-static glm)
    target_compile_definitions(pelican_test_shader_cache_test PRIVATE
        PELICAN_RUNTIME_SHADER_COMPILER=1
    )
    target_link_libraries(pelican_test_shader_cache_test PRIVATE pelican_shaderc)
    if(COMMAND pelican_copy_shaderc_runtime)
        pelican_copy_test_shaderc_runtime(pelican_test_shader_cache_test)
    endif()
    pelican_define_test(shader_cache_wp254_test GROUP shader_cache_runtime RUNTIME_SHADER
                        pelican_core spirv-reflect-static glm)
    target_compile_definitions(pelican_test_shader_cache_wp254_test PRIVATE
        PELICAN_RUNTIME_SHADER_COMPILER=1
    )
    target_link_libraries(pelican_test_shader_cache_wp254_test PRIVATE pelican_shaderc)
    if(COMMAND pelican_copy_shaderc_runtime)
        pelican_copy_test_shaderc_runtime(pelican_test_shader_cache_wp254_test)
    endif()
endif()
target_compile_definitions(pelican_test_shader_compile_test PRIVATE
    PELICAN_RUNTIME_SHADER_COMPILER=$<BOOL:${PELICAN_RUNTIME_SHADER_COMPILER}>
)
if(PELICAN_RUNTIME_SHADER_COMPILER)
    target_link_libraries(pelican_test_shader_compile_test PRIVATE pelican_shaderc)
    if(COMMAND pelican_copy_shaderc_runtime)
        pelican_copy_test_shaderc_runtime(pelican_test_shader_compile_test)
    endif()
endif()
pelican_define_test(shader_compiler_reflection_test GROUP shader_tools RUNTIME_SHADER pelican_core
                    pelican_target_environment spirv-reflect-static glm)
target_compile_definitions(pelican_test_shader_compiler_reflection_test PRIVATE
    PELICAN_RUNTIME_SHADER_COMPILER=$<BOOL:${PELICAN_RUNTIME_SHADER_COMPILER}>
    PELICAN_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
if(PELICAN_RUNTIME_SHADER_COMPILER)
    target_link_libraries(pelican_test_shader_compiler_reflection_test PRIVATE pelican_shaderc)
    if(COMMAND pelican_copy_shaderc_runtime)
        pelican_copy_test_shaderc_runtime(pelican_test_shader_compiler_reflection_test)
    endif()
endif()
pelican_define_test(raytracing_rt1a_test pelican_core spirv-reflect-static picosha2
                    pelican_test_raytracing_oracle)
pelican_define_test(raytracing_rt1b_test pelican_core picosha2
                    pelican_test_raytracing_oracle
                    VulkanMemoryAllocator VulkanMemoryAllocator-Hpp)
pelican_define_test(raytracing_rt2b_test pelican_core picosha2
                    VulkanMemoryAllocator VulkanMemoryAllocator-Hpp)
pelican_define_test(raytracing_rt3_1_test pelican_core picosha2
                    VulkanMemoryAllocator VulkanMemoryAllocator-Hpp)
pelican_define_test(raytracing_rt3_2a_test pelican_core picosha2
                    VulkanMemoryAllocator VulkanMemoryAllocator-Hpp)
target_compile_definitions(pelican_test_raytracing_rt1b_test PRIVATE
    PELICAN_RUNTIME_SHADER_COMPILER=$<BOOL:${PELICAN_RUNTIME_SHADER_COMPILER}>
    PELICAN_WITH_SPIRV_LINK=$<BOOL:${PELICAN_WITH_SPIRV_LINK}>
    PELICAN_WITH_OPENXR=$<BOOL:${PELICAN_WITH_OPENXR}>
    PELICAN_WITH_IMGUI=$<BOOL:${PELICAN_WITH_IMGUI}>
    PELICAN_WP366_SKIP_DEVSTUDIO=$<BOOL:${SKIP_DEVSTUDIO}>
    PELICAN_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
target_compile_definitions(pelican_test_raytracing_rt2b_test PRIVATE
    PELICAN_RUNTIME_SHADER_COMPILER=$<BOOL:${PELICAN_RUNTIME_SHADER_COMPILER}>
    PELICAN_WITH_SPIRV_LINK=$<BOOL:${PELICAN_WITH_SPIRV_LINK}>
    PELICAN_WITH_OPENXR=$<BOOL:${PELICAN_WITH_OPENXR}>
    PELICAN_WITH_IMGUI=$<BOOL:${PELICAN_WITH_IMGUI}>
    PELICAN_WP368_SKIP_DEVSTUDIO=$<BOOL:${SKIP_DEVSTUDIO}>
    PELICAN_WP369_SKIP_DEVSTUDIO=$<BOOL:${SKIP_DEVSTUDIO}>
    PELICAN_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
target_compile_definitions(pelican_test_raytracing_rt3_1_test PRIVATE
    PELICAN_RUNTIME_SHADER_COMPILER=$<BOOL:${PELICAN_RUNTIME_SHADER_COMPILER}>
    PELICAN_WITH_OPENXR=$<BOOL:${PELICAN_WITH_OPENXR}>
    PELICAN_WITH_IMGUI=$<BOOL:${PELICAN_WITH_IMGUI}>
    PELICAN_WP374_SKIP_DEVSTUDIO=$<BOOL:${SKIP_DEVSTUDIO}>
)
# WP380's production executor is intentionally independent of RPC, while the
# WP375 cases in this source are RPC-gated.  Split discovery in both builds so
# the executor remains buildable with RPC OFF and its GPU case never leaks
# into the non-GPU lane.
pelican_define_test(raytracing_rt3_1_production_test
                    MIXED_GPU
                    pelican_core picosha2 pelican_test_raytracing_oracle
                    VulkanMemoryAllocator VulkanMemoryAllocator-Hpp)
if(MSVC)
    target_compile_options(pelican_test_raytracing_rt3_1_production_test PRIVATE /bigobj)
endif()
target_sources(pelican_test_raytracing_rt3_1_production_test PRIVATE
    sceneloadfault_testing.cpp
    modelreleasefault_testing.cpp)
target_compile_definitions(pelican_test_raytracing_rt3_1_production_test PRIVATE
    PELICAN_WITH_RPC=$<BOOL:${PELICAN_WITH_RPC}>
    PELICAN_RUNTIME_SHADER_COMPILER=$<BOOL:${PELICAN_RUNTIME_SHADER_COMPILER}>
    PELICAN_WITH_OPENXR=$<BOOL:${PELICAN_WITH_OPENXR}>
    PELICAN_WITH_IMGUI=$<BOOL:${PELICAN_WITH_IMGUI}>
    PELICAN_WP374_SKIP_DEVSTUDIO=$<BOOL:${SKIP_DEVSTUDIO}>
    PELICAN_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
if(WIN32)
    get_filename_component(wp380c_toolchain_bin
        "${CMAKE_CXX_COMPILER}" DIRECTORY)
    add_test(
        NAME wp380c_production_has_no_fault_setter_symbol
        COMMAND ${CMAKE_COMMAND}
            -DPRODUCTION_ARCHIVE=$<TARGET_FILE:pelican_core>
            -DPROJECT_ARCHIVE=$<TARGET_FILE:pelican_project>
            -DWP380C_DUMPBIN=${wp380c_toolchain_bin}/dumpbin.exe
            -DPRODUCTION_SOURCES="$<JOIN:$<TARGET_PROPERTY:pelican_core,SOURCES>,|>"
            -DPROJECT_SOURCES="$<JOIN:$<TARGET_PROPERTY:pelican_project,SOURCES>,|>"
            -DGRAPH_TEST_MODULE_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/graphtestopaquemodule.cpp
            -DSCENE_FAULT_TEST_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/sceneloadfault_testing.cpp
            -DMODEL_RELEASE_FAULT_TEST_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/modelreleasefault_testing.cpp
            -DMODEL_RELOAD_FAULT_TEST_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/modelreloadfault_testing.cpp
            -DINSTANCE_HEADER=${CMAKE_SOURCE_DIR}/src/core/renderer/polygoninstancecontainer.hpp
            -DSCENE_HEADER=${CMAKE_SOURCE_DIR}/src/core/loader/scene.hpp
            -DRELEASE_HEADER=${CMAKE_SOURCE_DIR}/src/core/model/vertbufcontainer.hpp
            -P ${CMAKE_CURRENT_SOURCE_DIR}/run_wp380c_no_production_fault_setter.cmake)
    set_tests_properties(wp380c_production_has_no_fault_setter_symbol
        PROPERTIES LABELS "WP380c;WP380d;cpu")

    # This production acceptance executable is the real SDK export host for
    # its game-logic fixture.  The DLL sees only the shipped userpublic API and
    # links the host import library, matching the actual player composition.
    set_target_properties(pelican_test_raytracing_rt3_1_production_test
        PROPERTIES ENABLE_EXPORTS TRUE)
    if(MSVC)
        target_link_options(pelican_test_raytracing_rt3_1_production_test PRIVATE
            "/WHOLEARCHIVE:$<TARGET_LINKER_FILE:pelican_core>")
    endif()

    add_library(pelican_wp375_behavior_fixture SHARED
        fixtures/behavior_game_dll/behavior.cpp
    )
    set_target_properties(pelican_wp375_behavior_fixture PROPERTIES
        OUTPUT_NAME "pelican_wp375_behavior_fixture"
    )
    pelican_configure_game_logic_target(pelican_wp375_behavior_fixture
        HOST_TARGET pelican_test_raytracing_rt3_1_production_test
        ROLE production-behavior NO_INVENTORY)
    target_compile_definitions(
        pelican_test_raytracing_rt3_1_production_test PRIVATE
        PELICAN_WP375_BEHAVIOR_FIXTURE="$<TARGET_FILE:pelican_wp375_behavior_fixture>")

    add_library(pelican_wp380_behavior_fixture SHARED
        fixtures/wp380_scene_transaction/behavior.cpp
        ${CMAKE_SOURCE_DIR}/src/player/gamelogic_entry.cpp
    )
    set_target_properties(pelican_wp380_behavior_fixture PROPERTIES
        CXX_STANDARD 20
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
        OUTPUT_NAME "pelican_wp380_behavior_fixture"
    )
    add_dependencies(pelican_wp380_behavior_fixture
        pelican_test_raytracing_rt3_1_production_test)
    target_link_libraries(pelican_wp380_behavior_fixture PRIVATE
        "$<TARGET_LINKER_FILE:pelican_test_raytracing_rt3_1_production_test>"
        glm
        nlohmann_json)
    target_include_directories(pelican_wp380_behavior_fixture PRIVATE
        ${CMAKE_SOURCE_DIR}/src/core/userpublic
        ${CMAKE_SOURCE_DIR}/src/core
        ${Vulkan_INCLUDE_DIRS})
    target_compile_definitions(pelican_wp380_behavior_fixture PRIVATE
        PELICAN_GAME_DLL=1
        PELICAN_WP380_FIXTURE_BUILD=1)
    target_compile_definitions(
        pelican_test_raytracing_rt3_1_production_test PRIVATE
        PELICAN_WP380_BEHAVIOR_FIXTURE="$<TARGET_FILE:pelican_wp380_behavior_fixture>")

    add_library(pelican_wp380b_effect_fixture SHARED
        fixtures/wp380b_effect/behavior.cpp
        ${CMAKE_SOURCE_DIR}/src/player/gamelogic_entry.cpp)
    set_target_properties(pelican_wp380b_effect_fixture PROPERTIES
        CXX_STANDARD 20
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
        OUTPUT_NAME "pelican_wp380b_effect_fixture")
    add_dependencies(pelican_wp380b_effect_fixture
        pelican_test_raytracing_rt3_1_production_test)
    target_link_libraries(pelican_wp380b_effect_fixture PRIVATE
        "$<TARGET_LINKER_FILE:pelican_test_raytracing_rt3_1_production_test>"
        glm nlohmann_json)
    target_include_directories(pelican_wp380b_effect_fixture PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/userpublic_frozen/v2
        ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/userpublic_frozen/v1_support
        ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/wp380b_effect
        ${Vulkan_INCLUDE_DIRS})
    target_compile_definitions(pelican_wp380b_effect_fixture PRIVATE
        PELICAN_GAME_DLL=1 PELICAN_WP380B_FIXTURE_BUILD=1)
    set(wp380b_frozen_source_dependencies
        "${CMAKE_CURRENT_BINARY_DIR}/wp380b_frozen_source_dependencies.json")
    target_compile_options(pelican_wp380b_effect_fixture PRIVATE
        /sourceDependencies "${wp380b_frozen_source_dependencies}")
    target_compile_definitions(
        pelican_test_raytracing_rt3_1_production_test PRIVATE
        PELICAN_WP380B_EFFECT_FIXTURE="$<TARGET_FILE:pelican_wp380b_effect_fixture>")
    add_test(NAME wp380b_frozen_header_audit
        COMMAND ${CMAKE_COMMAND}
            -DSOURCE_DEPENDENCIES=${wp380b_frozen_source_dependencies}
            -DCURRENT_USERPUBLIC=${CMAKE_SOURCE_DIR}/src/core/userpublic
            -DFROZEN_USERPUBLIC=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/userpublic_frozen/v2
            -P ${CMAKE_CURRENT_SOURCE_DIR}/wp380b_frozen_header_audit.cmake)
    set_tests_properties(wp380b_frozen_header_audit
        PROPERTIES LABELS "WP380b;cpu;abi")

    add_library(pelican_wp379_ray_instance_fixture SHARED
        fixtures/ray_namespace_component/component.cpp
    )
    set_target_properties(pelican_wp379_ray_instance_fixture PROPERTIES
        OUTPUT_NAME "pelican_wp379_ray_instance_fixture"
    )
    pelican_configure_game_logic_target(pelican_wp379_ray_instance_fixture
        HOST_TARGET pelican_test_raytracing_rt3_1_production_test
        ROLE production-component NO_INVENTORY)
    target_compile_definitions(
        pelican_test_raytracing_rt3_1_production_test PRIVATE
        PELICAN_WP379_RAY_INSTANCE_FIXTURE="$<TARGET_FILE:pelican_wp379_ray_instance_fixture>")

    foreach(wp379a_probe IN ITEMS engine_owner owner_allocator)
        if(wp379a_probe STREQUAL "engine_owner")
            set(wp379a_source engine_owner_claim.cpp)
            set(wp379a_diagnostic registerEngineComponent)
        else()
            set(wp379a_source owner_allocator_claim.cpp)
            set(wp379a_diagnostic allocateRegistrationOwner)
        endif()
        add_test(
            NAME wp379a_game_dll_rejects_${wp379a_probe}
            COMMAND ${CMAKE_COMMAND}
                -DPROJECT_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/game_dll_boundary_compile_fixture
                -DBUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}/game_dll_boundary/${wp379a_probe}
                -DFIXTURE_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/ray_namespace_component/${wp379a_source}
                -DGAME_LOGIC_ENTRY=${CMAKE_SOURCE_DIR}/src/player/gamelogic_entry.cpp
                -DUSERPUBLIC_INCLUDE=${CMAKE_SOURCE_DIR}/src/core/userpublic
                -DCORE_INCLUDE=${CMAKE_SOURCE_DIR}/src/core
                -DNLOHMANN_INCLUDE=${nlohmann_json_SOURCE_DIR}/include
                -DHOST_IMPORT_LIBRARY=$<TARGET_LINKER_FILE:pelican_test_raytracing_rt3_1_production_test>
                -DCONFIG=$<CONFIG>
                -DTARGET_NAME=pelican_wp379a_${wp379a_probe}_probe
                -DEXPECTED_DIAGNOSTIC=${wp379a_diagnostic}
                -DGENERATOR=${CMAKE_GENERATOR}
                -DGENERATOR_INSTANCE=${CMAKE_GENERATOR_INSTANCE}
                -DGENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}
                -DGENERATOR_TOOLSET=${CMAKE_GENERATOR_TOOLSET}
                -DCXX_COMPILER=${CMAKE_CXX_COMPILER}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/run_game_dll_boundary_compile.cmake)
        set_tests_properties(wp379a_game_dll_rejects_${wp379a_probe}
            PROPERTIES LABELS "WP379a;cpu")
    endforeach()
endif()
if(PELICAN_RUNTIME_SHADER_COMPILER)
    target_link_libraries(pelican_test_raytracing_rt1b_test PRIVATE pelican_shaderc)
    target_link_libraries(pelican_test_raytracing_rt2b_test PRIVATE pelican_shaderc)
    if(COMMAND pelican_copy_shaderc_runtime)
        pelican_copy_shaderc_runtime(pelican_test_raytracing_rt1b_test)
        pelican_copy_shaderc_runtime(pelican_test_raytracing_rt2b_test)
    endif()
endif()
find_package(Git QUIET)
if(Git_FOUND)
    file(TO_CMAKE_PATH "${GIT_EXECUTABLE}" PELICAN_TEST_GIT_EXECUTABLE)
else()
    set(PELICAN_TEST_GIT_EXECUTABLE "")
endif()
target_compile_definitions(pelican_test_raytracing_rt1a_test PRIVATE
    PELICAN_RUNTIME_SHADER_COMPILER=$<BOOL:${PELICAN_RUNTIME_SHADER_COMPILER}>
    PELICAN_WITH_SPIRV_LINK=$<BOOL:${PELICAN_WITH_SPIRV_LINK}>
    PELICAN_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    PELICAN_TEST_GIT_EXECUTABLE="${PELICAN_TEST_GIT_EXECUTABLE}"
)
if(PELICAN_RUNTIME_SHADER_COMPILER)
    target_link_libraries(pelican_test_raytracing_rt1a_test PRIVATE pelican_shaderc)
    if(COMMAND pelican_copy_shaderc_runtime)
        pelican_copy_test_shaderc_runtime(pelican_test_raytracing_rt1a_test)
    endif()
endif()
pelican_define_test(surfacecompiler_test GROUP shader_tools RUNTIME_SHADER pelican_core spirv-reflect-static glm)
target_compile_definitions(pelican_test_surfacecompiler_test PRIVATE
    PELICAN_RUNTIME_SHADER_COMPILER=$<BOOL:${PELICAN_RUNTIME_SHADER_COMPILER}>
    PELICAN_WITH_SPIRV_LINK=$<BOOL:${PELICAN_WITH_SPIRV_LINK}>
    PELICAN_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
if(PELICAN_RUNTIME_SHADER_COMPILER)
    target_link_libraries(pelican_test_surfacecompiler_test PRIVATE pelican_shaderc)
    if(COMMAND pelican_copy_shaderc_runtime)
        pelican_copy_test_shaderc_runtime(pelican_test_surfacecompiler_test)
    endif()
endif()
if(PELICAN_WITH_SPIRV_LINK)
    pelican_define_test(spvlink_test GROUP shader_tools RUNTIME_SHADER pelican_core spirv-reflect-static glm)
    target_compile_definitions(pelican_test_spvlink_test PRIVATE
        PELICAN_RUNTIME_SHADER_COMPILER=$<BOOL:${PELICAN_RUNTIME_SHADER_COMPILER}>
        PELICAN_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    )
    if(PELICAN_RUNTIME_SHADER_COMPILER)
        target_link_libraries(pelican_test_spvlink_test PRIVATE pelican_shaderc)
        if(COMMAND pelican_copy_shaderc_runtime)
            pelican_copy_test_shaderc_runtime(pelican_test_spvlink_test)
        endif()
    endif()
else()
    # This is the OFF-tier contract itself, not a runtime-compiler consumer.
    pelican_define_test(spvlink_disabled_test pelican_core)
endif()
if(PELICAN_WITH_SPIRV_LINK)
    find_program(PELICAN_SLANGC_EXECUTABLE slangc HINTS $ENV{VULKAN_SDK}/Bin)
endif()
if(PELICAN_WITH_SPIRV_LINK AND PELICAN_SLANGC_EXECUTABLE)
    set(PELICAN_SLANG_COOL_FIXTURE ${CMAKE_CURRENT_BINARY_DIR}/fixtures/spvlink/surface_slang_cool.spv)
    set(PELICAN_SLANG_WARM_FIXTURE ${CMAKE_CURRENT_BINARY_DIR}/fixtures/spvlink/surface_slang_warm.spv)
    add_custom_command(
        OUTPUT ${PELICAN_SLANG_COOL_FIXTURE} ${PELICAN_SLANG_WARM_FIXTURE}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/fixtures/spvlink
        COMMAND ${PELICAN_SLANGC_EXECUTABLE}
            -target spirv
            -profile ps_6_0+${PELICAN_SLANG_SPIRV_PROFILE}
            -entry main
            -stage fragment
            -O0
            -emit-spirv-directly
            -fvk-use-entrypoint-name
            ${CMAKE_SOURCE_DIR}/experiments/spvlink/shaders/surface.slang
            -o ${PELICAN_SLANG_COOL_FIXTURE}
        COMMAND ${PELICAN_SLANGC_EXECUTABLE}
            -target spirv
            -profile ps_6_0+${PELICAN_SLANG_SPIRV_PROFILE}
            -entry main
            -stage fragment
            -O0
            -emit-spirv-directly
            -fvk-use-entrypoint-name
            -DPELICAN_VARIANT_WARM
            ${CMAKE_SOURCE_DIR}/experiments/spvlink/shaders/surface.slang
            -o ${PELICAN_SLANG_WARM_FIXTURE}
        DEPENDS ${CMAKE_SOURCE_DIR}/experiments/spvlink/shaders/surface.slang
        VERBATIM
    )
    add_custom_target(pelican_spvlink_slang_fixture
        DEPENDS ${PELICAN_SLANG_COOL_FIXTURE} ${PELICAN_SLANG_WARM_FIXTURE})
    add_dependencies(pelican_test_spvlink_test pelican_spvlink_slang_fixture)
    target_compile_definitions(pelican_test_spvlink_test PRIVATE
        PELICAN_TEST_SLANG_COOL_FIXTURE="${PELICAN_SLANG_COOL_FIXTURE}"
        PELICAN_TEST_SLANG_WARM_FIXTURE="${PELICAN_SLANG_WARM_FIXTURE}"
    )
endif()
if(PELICAN_WITH_SPIRV_LINK AND Vulkan_GLSLANG_VALIDATOR_EXECUTABLE)
    add_test(
        NAME spvlink_cli_linked_golden
        COMMAND ${CMAKE_COMMAND}
            -DGLSLANG=${Vulkan_GLSLANG_VALIDATOR_EXECUTABLE}
            -DSPIRV_VAL=$ENV{VULKAN_SDK}/Bin/spirv-val.exe
            -DLINKER=$<TARGET_FILE:pelican-spv-link>
            -DSOURCE_DIR=${CMAKE_SOURCE_DIR}
            -DOUT_DIR=${CMAKE_BINARY_DIR}/test_artifacts/spvlink_cli_golden
            -DTARGET_ENV=${PELICAN_SHADER_TARGET_ENV}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/run_spvlink_golden.cmake
    )
endif()
pelican_define_test(shader_library_test GROUP shader_tools RUNTIME_SHADER pelican_core spirv-reflect-static glm)
target_compile_definitions(pelican_test_shader_library_test PRIVATE
    PELICAN_RUNTIME_SHADER_COMPILER=$<BOOL:${PELICAN_RUNTIME_SHADER_COMPILER}>
    PELICAN_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
if(PELICAN_RUNTIME_SHADER_COMPILER)
    target_link_libraries(pelican_test_shader_library_test PRIVATE pelican_shaderc)
    if(COMMAND pelican_copy_shaderc_runtime)
        pelican_copy_test_shaderc_runtime(pelican_test_shader_library_test)
    endif()
endif()
pelican_define_test(vulkan_headless_test GPU pelican_core VulkanMemoryAllocator VulkanMemoryAllocator-Hpp)
target_compile_definitions(pelican_test_vulkan_headless_test PRIVATE
    VULKAN_HPP_DISPATCH_LOADER_STATIC_TYPE=::vk::detail::DispatchLoaderStatic
)
pelican_define_test(vulkan_device_feature_policy_test GROUP vulkan_policy pelican_core
                    VulkanMemoryAllocator VulkanMemoryAllocator-Hpp)
pelican_define_test(shaderbindingtable_test GROUP vulkan_policy pelican_core)
pelican_define_test(atlas_descriptor_pool_test GPU pelican_core VulkanMemoryAllocator VulkanMemoryAllocator-Hpp)
target_compile_definitions(pelican_test_atlas_descriptor_pool_test PRIVATE
    VULKAN_HPP_DISPATCH_LOADER_STATIC_TYPE=::vk::detail::DispatchLoaderStatic
)

add_executable(pelican_test_process_fixture_child process_fixture_child.cpp)
set_target_properties(pelican_test_process_fixture_child PROPERTIES CXX_STANDARD 20)
if(TARGET pelican_test_devstudio_frameplan_graph_test)
  target_compile_definitions(pelican_test_devstudio_frameplan_graph_test PRIVATE
    PELICAN_PROCESS_FIXTURE_PATH="$<TARGET_FILE:pelican_test_process_fixture_child>"
  )
  add_dependencies(pelican_test_devstudio_frameplan_graph_test
    pelican_test_process_fixture_child)
endif()

if(TARGET pelican_studio_viewport)
  # Qt imported targets are directory-scoped; make the public viewport
  # interface visible in this sibling test directory.
  find_package(Qt6 REQUIRED COMPONENTS Core Widgets)
  add_executable(pelican_test_devstudio_engine_owner_fixture
    devstudio_engine_owner_fixture.cpp)
  set_target_properties(pelican_test_devstudio_engine_owner_fixture PROPERTIES
    CXX_STANDARD 20)
  target_link_libraries(pelican_test_devstudio_engine_owner_fixture PRIVATE
    pelican_studio_viewport Qt6::Core)
  pelican_define_test(devstudio_viewport_test QT pelican_studio_viewport Qt6::Core Qt6::Widgets)
  target_compile_definitions(pelican_test_devstudio_viewport_test PRIVATE
    PELICAN_PROCESS_FIXTURE_PATH="$<TARGET_FILE:pelican_test_process_fixture_child>"
    PELICAN_ENGINE_OWNER_FIXTURE_PATH="$<TARGET_FILE:pelican_test_devstudio_engine_owner_fixture>"
  )
  add_dependencies(pelican_test_devstudio_viewport_test
    pelican_test_process_fixture_child
    pelican_test_devstudio_engine_owner_fixture)
endif()

pelican_define_test(processrunner_test)
target_sources(pelican_test_processrunner_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/devcli/processrunner.cpp
)
target_compile_definitions(pelican_test_processrunner_test PRIVATE
    PELICAN_PROCESS_FIXTURE_PATH="$<TARGET_FILE:pelican_test_process_fixture_child>"
)
add_dependencies(pelican_test_processrunner_test pelican_test_process_fixture_child)
add_test(
    NAME devcli_import_process_timeout_log
    COMMAND ${CMAKE_COMMAND}
        -DCLI=$<TARGET_FILE:pelican_cli>
        -DCHILD=$<TARGET_FILE:pelican_test_process_fixture_child>
        -DOUT_DIR=${CMAKE_BINARY_DIR}/test_artifacts/devcli_import_process
        -P ${CMAKE_CURRENT_SOURCE_DIR}/run_devcli_import_process.cmake
)

if(PELICAN_WITH_SEQPLAYER)
    add_test(
        NAME seqplayer_headless_player
        COMMAND $<TARGET_FILE:pelican_gpu_lease_run> -- ${CMAKE_COMMAND}
            -DPLAYER=$<TARGET_FILE:pelican_player>
            -DPLAYER_DIR=$<TARGET_FILE_DIR:pelican_player>
            -DPROJECT_DIR=${CMAKE_SOURCE_DIR}/projects/example
            -DSEQ_FILE=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/seqplayer_two_objects.jsonl
            -DOUT_DIR=${CMAKE_BINARY_DIR}/test_artifacts/seqplayer_headless
            -P ${CMAKE_CURRENT_SOURCE_DIR}/run_seqplayer_headless.cmake
    )
    pelican_gpu_test_properties(seqplayer_headless_player ${PELICAN_GPU_INTRINSIC_TIMEOUT_DEFAULT})
endif()

if(PELICAN_WITH_VAT)
    add_test(
        NAME vatplayer_headless_player
            COMMAND $<TARGET_FILE:pelican_gpu_lease_run> -- ${CMAKE_COMMAND}
            -DPLAYER=$<TARGET_FILE:pelican_player>
            -DPLAYER_DIR=$<TARGET_FILE_DIR:pelican_player>
            -DWRITER=$<TARGET_FILE:pelican_test_vat_fixture_writer>
            -DEXAMPLE_PASS=${CMAKE_SOURCE_DIR}/projects/example/passes/main_rendering_config.json
            -DOUT_DIR=${CMAKE_BINARY_DIR}/test_artifacts/vatplayer_headless
            -P ${CMAKE_CURRENT_SOURCE_DIR}/run_vatplayer_headless.cmake
    )
    pelican_gpu_test_properties(vatplayer_headless_player ${PELICAN_GPU_INTRINSIC_TIMEOUT_DEFAULT})
endif()

add_test(
    NAME free_camera_runtime_overlay_player
    COMMAND $<TARGET_FILE:pelican_gpu_lease_run> -- ${CMAKE_COMMAND}
        -DPLAYER=$<TARGET_FILE:pelican_player>
        -DPLAYER_DIR=$<TARGET_FILE_DIR:pelican_player>
        -DEXAMPLE_PASS=${CMAKE_SOURCE_DIR}/projects/example/passes/main_rendering_config.json
        -DOUT_DIR=${CMAKE_BINARY_DIR}/test_artifacts/free_camera_runtime_overlay
        -P ${CMAKE_CURRENT_SOURCE_DIR}/run_free_camera_runtime_overlay.cmake
)
pelican_gpu_test_properties(free_camera_runtime_overlay_player ${PELICAN_GPU_INTRINSIC_TIMEOUT_DEFAULT})

if(PELICAN_WITH_RPC)
    add_test(
        NAME modal_transform_record_replay_player
        COMMAND $<TARGET_FILE:pelican_gpu_lease_run> -- ${CMAKE_COMMAND}
            -DPLAYER=$<TARGET_FILE:pelican_player>
            -DPLAYER_DIR=$<TARGET_FILE_DIR:pelican_player>
            -DEXAMPLE_PASS=${CMAKE_SOURCE_DIR}/projects/example/passes/main_rendering_config.json
            -DOUT_DIR=${CMAKE_BINARY_DIR}/test_artifacts/modal_transform_record_replay
            -P ${CMAKE_CURRENT_SOURCE_DIR}/run_modal_transform_record_replay.cmake
    )
    pelican_gpu_test_properties(modal_transform_record_replay_player 180)
    if(PELICAN_WITH_ANIMATION)
    add_executable(
        pelican_test_vrm_xr_demo_player
        ${CMAKE_SOURCE_DIR}/src/player/main.cpp
        ${CMAKE_SOURCE_DIR}/projects/vrm_xr_demo/code/vrmxrdemo.cpp
    )
    set_target_properties(pelican_test_vrm_xr_demo_player PROPERTIES CXX_STANDARD 20)
    target_link_libraries(pelican_test_vrm_xr_demo_player PRIVATE pelican_core argparse)
    target_include_directories(pelican_test_vrm_xr_demo_player PRIVATE
        ${Vulkan_INCLUDE_DIRS}
        ${CMAKE_SOURCE_DIR}/projects/vrm_xr_demo
        ${CMAKE_SOURCE_DIR}/projects/vrm_xr_demo/code
    )
    add_test(
        NAME vrm_xr_demo_rpc_player
        COMMAND $<TARGET_FILE:pelican_gpu_lease_run> -- ${CMAKE_COMMAND}
            -DPLAYER=$<TARGET_FILE:pelican_test_vrm_xr_demo_player>
            -DPROJECT_DIR=${CMAKE_SOURCE_DIR}/projects/vrm_xr_demo
            -DOUT_DIR=${CMAKE_BINARY_DIR}/test_artifacts/vrm_xr_demo_rpc
            -P ${CMAKE_CURRENT_SOURCE_DIR}/run_vrm_xr_demo_rpc.cmake
    )
    pelican_gpu_test_properties(vrm_xr_demo_rpc_player 180)
    endif()
    add_test(
        NAME rpc_headless_player
        COMMAND $<TARGET_FILE:pelican_gpu_lease_run> -- ${CMAKE_COMMAND}
            -DPLAYER=$<TARGET_FILE:pelican_player>
            -DPLAYER_DIR=$<TARGET_FILE_DIR:pelican_player>
            -DPROJECT_DIR=${CMAKE_SOURCE_DIR}/projects/example
            -DOUT_DIR=${CMAKE_BINARY_DIR}/test_artifacts/rpc_headless
            -DRENDERDOC_STATUS=$<IF:$<BOOL:${PELICAN_WITH_RENDERDOC}>,absent,disabled>
            -DRENDERDOC_REASON=$<IF:$<BOOL:${PELICAN_WITH_RENDERDOC}>,renderdoc_not_injected,renderdoc_build_disabled>
            -P ${CMAKE_CURRENT_SOURCE_DIR}/run_rpc_headless.cmake
    )
    pelican_gpu_test_properties(rpc_headless_player ${PELICAN_GPU_INTRINSIC_TIMEOUT_DEFAULT})
    add_test(
        NAME rpc_set_camera_headless_player
        COMMAND $<TARGET_FILE:pelican_gpu_lease_run> -- ${CMAKE_COMMAND}
            -DPLAYER=$<TARGET_FILE:pelican_player>
            -DPLAYER_DIR=$<TARGET_FILE_DIR:pelican_player>
            -DPROJECT_DIR=${CMAKE_SOURCE_DIR}/projects/example
            -DOUT_DIR=${CMAKE_BINARY_DIR}/test_artifacts/rpc_set_camera_headless
            -P ${CMAKE_CURRENT_SOURCE_DIR}/run_rpc_set_camera_headless.cmake
    )
    pelican_gpu_test_properties(rpc_set_camera_headless_player ${PELICAN_GPU_INTRINSIC_TIMEOUT_DEFAULT})
    add_test(
        NAME rpc_scene_flow_headless_player
        COMMAND $<TARGET_FILE:pelican_gpu_lease_run> -- ${CMAKE_COMMAND}
            -DPLAYER=$<TARGET_FILE:pelican_player>
            -DPLAYER_DIR=$<TARGET_FILE_DIR:pelican_player>
            -DPROJECT_DIR=${CMAKE_SOURCE_DIR}/projects/example
            -DOUT_DIR=${CMAKE_BINARY_DIR}/test_artifacts/rpc_scene_flow_headless
            -P ${CMAKE_CURRENT_SOURCE_DIR}/run_rpc_scene_flow_headless.cmake
    )
    pelican_gpu_test_properties(rpc_scene_flow_headless_player ${PELICAN_GPU_INTRINSIC_TIMEOUT_DEFAULT})
    add_test(
        NAME rpc_scene_flow_normalization_fixtures
        COMMAND ${CMAKE_COMMAND}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/run_rpc_scene_flow_normalization_fixtures.cmake
    )
    add_executable(
        pelican_test_rpc_inject_input_player
        ${CMAKE_SOURCE_DIR}/src/player/main.cpp
        ${CMAKE_SOURCE_DIR}/projects/example/code/playercontrol.cpp
    )
    set_target_properties(pelican_test_rpc_inject_input_player PROPERTIES CXX_STANDARD 20)
    target_link_libraries(pelican_test_rpc_inject_input_player PRIVATE pelican_core argparse)
    target_include_directories(pelican_test_rpc_inject_input_player PRIVATE
        ${Vulkan_INCLUDE_DIRS}
        ${CMAKE_SOURCE_DIR}/projects/example
        ${CMAKE_SOURCE_DIR}/projects/example/code
    )
    add_executable(
        pelican_test_camera_replay_player
        ${CMAKE_SOURCE_DIR}/src/player/main.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/camera_replay_probe.cpp
    )
    set_target_properties(pelican_test_camera_replay_player PROPERTIES CXX_STANDARD 20)
    target_link_libraries(pelican_test_camera_replay_player PRIVATE pelican_core argparse)
    target_include_directories(pelican_test_camera_replay_player PRIVATE
        ${Vulkan_INCLUDE_DIRS}
        ${CMAKE_SOURCE_DIR}/projects/example
        ${CMAKE_SOURCE_DIR}/projects/example/code
    )
    add_test(
        NAME rpc_inject_input_headless_player
        COMMAND $<TARGET_FILE:pelican_gpu_lease_run> -- ${CMAKE_COMMAND}
            -DPLAYER=$<TARGET_FILE:pelican_test_rpc_inject_input_player>
            -DPLAYER_DIR=$<TARGET_FILE_DIR:pelican_test_rpc_inject_input_player>
            -DPROJECT_DIR=${CMAKE_SOURCE_DIR}/projects/example
            -DOUT_DIR=${CMAKE_BINARY_DIR}/test_artifacts/rpc_inject_input_headless
            -P ${CMAKE_CURRENT_SOURCE_DIR}/run_rpc_inject_input_headless.cmake
    )
    pelican_gpu_test_properties(rpc_inject_input_headless_player ${PELICAN_GPU_INTRINSIC_TIMEOUT_DEFAULT})
    add_test(
        NAME input_record_replay_headless_player
        COMMAND $<TARGET_FILE:pelican_gpu_lease_run> -- ${CMAKE_COMMAND}
            -DPLAYER=$<TARGET_FILE:pelican_test_rpc_inject_input_player>
            -DPLAYER_DIR=$<TARGET_FILE_DIR:pelican_test_rpc_inject_input_player>
            -DPROJECT_DIR=${CMAKE_SOURCE_DIR}/projects/example
            -DOUT_DIR=${CMAKE_BINARY_DIR}/test_artifacts/input_record_replay_headless
            -P ${CMAKE_CURRENT_SOURCE_DIR}/run_input_record_replay_headless.cmake
    )
    pelican_gpu_test_properties(input_record_replay_headless_player ${PELICAN_GPU_INTRINSIC_TIMEOUT_DEFAULT})
    add_test(
        NAME devcli_bake_camera_replay
        COMMAND $<TARGET_FILE:pelican_gpu_lease_run> -- ${CMAKE_COMMAND}
            -DCLI=$<TARGET_FILE:pelican_cli>
            -DPLAYER=$<TARGET_FILE:pelican_test_camera_replay_player>
            -DPLAYER_DIR=$<TARGET_FILE_DIR:pelican_test_camera_replay_player>
            -DSETUP_PLAYER=$<TARGET_FILE:pelican_test_rpc_inject_input_player>
            -DPROJECT_DIR=${CMAKE_SOURCE_DIR}/projects/example
            -DOUT_DIR=${CMAKE_BINARY_DIR}/test_artifacts/devcli_bake_camera
            -P ${CMAKE_CURRENT_SOURCE_DIR}/run_devcli_bake_camera.cmake
    )
    pelican_gpu_test_properties(devcli_bake_camera_replay ${PELICAN_GPU_INTRINSIC_TIMEOUT_DEFAULT})
    add_executable(
        pelican_test_rpc_inject_event_player
        ${CMAKE_SOURCE_DIR}/src/player/main.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/event_layer_scene_probe.cpp
    )
    set_target_properties(pelican_test_rpc_inject_event_player PROPERTIES CXX_STANDARD 20)
    target_link_libraries(pelican_test_rpc_inject_event_player PRIVATE pelican_core argparse)
    target_include_directories(pelican_test_rpc_inject_event_player PRIVATE
        ${Vulkan_INCLUDE_DIRS}
        ${CMAKE_SOURCE_DIR}/projects/example
    )
    add_test(
        NAME rpc_inject_event_headless_player
        COMMAND $<TARGET_FILE:pelican_gpu_lease_run> -- ${CMAKE_COMMAND}
            -DPLAYER=$<TARGET_FILE:pelican_test_rpc_inject_event_player>
            -DPLAYER_DIR=$<TARGET_FILE_DIR:pelican_test_rpc_inject_event_player>
            -DPROJECT_DIR=${CMAKE_SOURCE_DIR}/projects/example
            -DOUT_DIR=${CMAKE_BINARY_DIR}/test_artifacts/rpc_inject_event_headless
            -P ${CMAKE_CURRENT_SOURCE_DIR}/run_rpc_inject_event_headless.cmake
    )
    pelican_gpu_test_properties(rpc_inject_event_headless_player ${PELICAN_GPU_INTRINSIC_TIMEOUT_DEFAULT})
    add_executable(
        pelican_test_ui_u2_player
        ${CMAKE_SOURCE_DIR}/src/player/main.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/ui_u2_probe.cpp
    )
    set_target_properties(pelican_test_ui_u2_player PROPERTIES CXX_STANDARD 20)
    target_link_libraries(pelican_test_ui_u2_player PRIVATE pelican_core argparse)
    target_include_directories(pelican_test_ui_u2_player PRIVATE
        ${Vulkan_INCLUDE_DIRS}
        ${CMAKE_SOURCE_DIR}/projects/example
    )
    add_test(
        NAME ui_u2_rpc_replay_player
        COMMAND $<TARGET_FILE:pelican_gpu_lease_run> -- ${CMAKE_COMMAND}
            -DPLAYER=$<TARGET_FILE:pelican_test_ui_u2_player>
            -DPLAYER_DIR=$<TARGET_FILE_DIR:pelican_test_ui_u2_player>
            -DPROJECT_DIR=${CMAKE_SOURCE_DIR}/projects/example
            -DOUT_DIR=${CMAKE_BINARY_DIR}/test_artifacts/ui_u2_rpc_replay
            -P ${CMAKE_CURRENT_SOURCE_DIR}/run_ui_u2_rpc_replay.cmake
    )
    pelican_gpu_test_properties(ui_u2_rpc_replay_player ${PELICAN_GPU_INTRINSIC_TIMEOUT_DEFAULT})
endif()

add_test(
    NAME compute_buffer_headless_player
    COMMAND $<TARGET_FILE:pelican_gpu_lease_run> -- ${CMAKE_COMMAND}
        -DPLAYER=$<TARGET_FILE:pelican_player>
        -DPLAYER_DIR=$<TARGET_FILE_DIR:pelican_player>
        -DOUT_DIR=${CMAKE_BINARY_DIR}/test_artifacts/compute_buffer_headless
        -P ${CMAKE_CURRENT_SOURCE_DIR}/run_compute_headless.cmake
)
pelican_gpu_test_properties(compute_buffer_headless_player ${PELICAN_GPU_INTRINSIC_TIMEOUT_DEFAULT})

pelican_define_test(importmanifest_test GROUP project_formats pelican_project)
pelican_define_test(importrules_test GROUP project_formats pelican_project)
pelican_define_test(assetdataformat_test GROUP project_formats pelican_project)
pelican_define_test(projectformat_test GROUP project_formats pelican_project)
target_compile_definitions(pelican_test_importrules_test PRIVATE PELICAN_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_compile_definitions(pelican_test_importmanifest_test PRIVATE
    PELICAN_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

# This executable deliberately links no product library other than
# pelican_project. It is the build-level D0 proof that project.json, scene data,
# and asset data can be opened without pulling in pelican_core.
add_executable(pelican_test_project_library_boundary project_library_boundary.cpp)
set_target_properties(pelican_test_project_library_boundary PROPERTIES
    CXX_STANDARD 20
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)
target_compile_definitions(pelican_test_project_library_boundary PRIVATE
    PELICAN_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
target_link_libraries(pelican_test_project_library_boundary PRIVATE pelican_project)
add_test(NAME project_library_boundary COMMAND pelican_test_project_library_boundary)

pelican_define_test(materialformat_test GROUP project_formats pelican_project)
target_compile_definitions(pelican_test_materialformat_test PRIVATE
    PELICAN_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

pelican_define_test(materiallowering_test GROUP project_formats pelican_project)
pelican_define_test(rasterpass_test GROUP project_formats pelican_project)
target_compile_definitions(pelican_test_materiallowering_test PRIVATE
    PELICAN_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

pelican_define_test(materialbinding_test GROUP render_planning_core pelican_core)

add_test(
    NAME devcli_dump_lowered_material
    COMMAND ${CMAKE_COMMAND}
        -DCLI=$<TARGET_FILE:pelican_cli>
        -DSURFACE=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/surface_format/valid/wp76.surface
        -P ${CMAKE_CURRENT_SOURCE_DIR}/run_dump_lowered_material.cmake
)

add_test(
    NAME devcli_dump_lowered_material_wp116
    COMMAND ${CMAKE_COMMAND}
        -DCLI=$<TARGET_FILE:pelican_cli>
        -DSURFACE=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/surface_format/valid/wp116_mask_double.surface
        -DMATERIAL=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/material_format/valid/wp116_mask_double.json
        -P ${CMAKE_CURRENT_SOURCE_DIR}/run_dump_lowered_material.cmake
)

add_test(
    NAME devcli_graph_catalog
    COMMAND ${CMAKE_COMMAND}
        -DCLI=$<TARGET_FILE:pelican_cli>
        -DWITH_ANIMATION=$<BOOL:${PELICAN_WITH_ANIMATION}>
        -P ${CMAKE_CURRENT_SOURCE_DIR}/run_devcli_graph_catalog.cmake
)

add_test(
    NAME devcli_import_command
    COMMAND ${CMAKE_COMMAND}
        -DCLI=$<TARGET_FILE:pelican_cli>
        -DFIXTURE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/import_manifest
        -DOUT_DIR=${CMAKE_BINARY_DIR}/test_artifacts/devcli_import
        -P ${CMAKE_CURRENT_SOURCE_DIR}/run_devcli_import.cmake
)

add_test(
    NAME devcli_gltf_scene_extract
    COMMAND ${CMAKE_COMMAND}
        -DCLI=$<TARGET_FILE:pelican_cli>
        -DWRITER=$<TARGET_FILE:pelican_test_gltf_scene_fixture_writer>
        -DOUT_DIR=${CMAKE_BINARY_DIR}/test_artifacts/devcli_gltf_extract
        -P ${CMAKE_CURRENT_SOURCE_DIR}/run_devcli_gltf_extract.cmake
)

add_test(
    NAME devcli_vrm_dump
    COMMAND ${CMAKE_COMMAND}
        -DCLI=$<TARGET_FILE:pelican_cli>
        -DWRITER=$<TARGET_FILE:pelican_test_vrm_fixture_writer>
        -DEXPECTED=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/vrm_semantic/canonical.json
        -DALICIA=${CMAKE_SOURCE_DIR}/projects/example/assets/models/AliciaSolid.vrm
        -DOUT_DIR=${CMAKE_BINARY_DIR}/test_artifacts/devcli_vrm_dump
        -P ${CMAKE_CURRENT_SOURCE_DIR}/run_devcli_vrm_dump.cmake
)

add_test(
    NAME devcli_import_rules
    COMMAND ${CMAKE_COMMAND}
        -DCLI=$<TARGET_FILE:pelican_cli>
        -DWRITER=$<TARGET_FILE:pelican_test_gltf_scene_fixture_writer>
        -DOUT_DIR=${CMAKE_BINARY_DIR}/test_artifacts/devcli_import_rules
        -P ${CMAKE_CURRENT_SOURCE_DIR}/run_devcli_rules_import.cmake
)

pelican_define_test(frameplanner_test GROUP render_planning_core pelican_core)
target_compile_definitions(pelican_test_frameplanner_test PRIVATE
    PELICAN_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
pelican_define_test(frameexecutionplan_test GROUP render_planning_core pelican_core)

pelican_define_test(renderpipelinetransaction_test GROUP render_planning_core pelican_core)
pelican_define_test(logicalrendergraph_test GROUP render_planning_core pelican_core)
pelican_define_test(targetplanning_test GROUP project_planning pelican_project)
pelican_define_test(targetrenderplanning_test GROUP project_planning pelican_project)
pelican_define_test(samplecountplanning_test GROUP project_planning pelican_project)
pelican_define_test(graphvariantpolicy_test GROUP project_planning pelican_project)
pelican_define_test(passfieldownership_test GROUP project_planning
  pelican_project pelican_core
  VulkanMemoryAllocator VulkanMemoryAllocator-Hpp)
target_compile_definitions(pelican_test_passfieldownership_test PRIVATE
    PELICAN_RUNTIME_SHADER_COMPILER=$<BOOL:${PELICAN_RUNTIME_SHADER_COMPILER}>
    PELICAN_WITH_IMGUI=$<BOOL:${PELICAN_WITH_IMGUI}>
    PELICAN_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
pelican_define_test(passshapepolicy_test GROUP project_planning pelican_project)
pelican_define_test(gpudrawtiming_test GROUP project_planning pelican_project)
target_compile_definitions(pelican_test_gpudrawtiming_test PRIVATE
    PELICAN_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

pelican_define_test(featurecompose_test GROUP render_pipeline pelican_core
                    VulkanMemoryAllocator VulkanMemoryAllocator-Hpp)
target_compile_definitions(pelican_test_featurecompose_test PRIVATE
    PELICAN_RUNTIME_SHADER_COMPILER=$<BOOL:${PELICAN_RUNTIME_SHADER_COMPILER}>
    PELICAN_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

pelican_define_test(renderconfigeditor_test GROUP render_pipeline pelican_core)
target_compile_definitions(pelican_test_renderconfigeditor_test PRIVATE
    PELICAN_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    PELICAN_RUNTIME_SHADER_COMPILER=$<BOOL:${PELICAN_RUNTIME_SHADER_COMPILER}>
)

pelican_define_test(renderpipeline_resolve_test GROUP render_pipeline pelican_core)
pelican_define_test(rendercompilerprogram_test GROUP render_pipeline pelican_core)
target_compile_definitions(pelican_test_rendercompilerprogram_test PRIVATE
    PELICAN_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
pelican_define_test(vulkannativescopeexecutor_test GROUP render_pipeline pelican_core)
pelican_define_test(renderingsamplecount_test GROUP render_pipeline pelican_core)

pelican_define_test(shader_defines_test GROUP render_pipeline pelican_core)
target_compile_definitions(pelican_test_shader_defines_test PRIVATE
    PELICAN_RUNTIME_SHADER_COMPILER=$<BOOL:${PELICAN_RUNTIME_SHADER_COMPILER}>
)
if(PELICAN_RUNTIME_SHADER_COMPILER)
    target_link_libraries(pelican_test_shader_defines_test PRIVATE pelican_shaderc)
    if(COMMAND pelican_copy_shaderc_runtime)
        pelican_copy_test_shaderc_runtime(pelican_test_shader_defines_test)
    endif()
endif()

pelican_define_test(debugdraw_test GROUP render_pipeline pelican_core glm
                    VulkanMemoryAllocator VulkanMemoryAllocator-Hpp)
pelican_define_test(gizmo_test GROUP render_pipeline pelican_core glm
                    VulkanMemoryAllocator VulkanMemoryAllocator-Hpp)
pelican_define_test(debugtext_test GROUP render_pipeline pelican_core glm
                    VulkanMemoryAllocator VulkanMemoryAllocator-Hpp)
pelican_define_test(debugtext_ui_compat_test GPU pelican_core glm VulkanMemoryAllocator VulkanMemoryAllocator-Hpp)
pelican_define_test(debugutils_test GROUP core_diagnostics pelican_core)
pelican_define_test(rendertiming_test GROUP core_diagnostics pelican_core)
pelican_define_test(memorydiagnostics_test GROUP core_diagnostics pelican_core
                    VulkanMemoryAllocator VulkanMemoryAllocator-Hpp)
target_compile_definitions(pelican_test_rendertiming_test PRIVATE
    PELICAN_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

add_test(
    NAME gpu_timing_headless_player
    COMMAND $<TARGET_FILE:pelican_gpu_lease_run> -- ${CMAKE_COMMAND}
        -DPLAYER=$<TARGET_FILE:pelican_player>
        -DPLAYER_DIR=$<TARGET_FILE_DIR:pelican_player>
        -DOUT_DIR=${CMAKE_BINARY_DIR}/test_artifacts/gpu_timing_headless
        -P ${CMAKE_CURRENT_SOURCE_DIR}/run_gpu_timing_headless.cmake
)
pelican_gpu_test_properties(gpu_timing_headless_player ${PELICAN_GPU_INTRINSIC_TIMEOUT_DEFAULT})

add_test(
    NAME frame_plan_dump_headless_player
    COMMAND $<TARGET_FILE:pelican_gpu_lease_run> -- ${CMAKE_COMMAND}
        -DPLAYER=$<TARGET_FILE:pelican_player>
        -DPLAYER_DIR=$<TARGET_FILE_DIR:pelican_player>
        -DPROJECT_DIR=${CMAKE_SOURCE_DIR}/projects/example
        -DOUT_DIR=${CMAKE_BINARY_DIR}/test_artifacts/frame_plan_dump_headless
        -P ${CMAKE_CURRENT_SOURCE_DIR}/run_frame_plan_dump_headless.cmake
)
pelican_gpu_test_properties(frame_plan_dump_headless_player ${PELICAN_GPU_INTRINSIC_TIMEOUT_DEFAULT})

pelican_define_test(inputactions_test GROUP core_diagnostics pelican_core)
target_compile_definitions(pelican_test_inputactions_test PRIVATE
    PELICAN_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

add_test(
    NAME devcli_dist_config_command
    COMMAND ${CMAKE_COMMAND}
        -DCLI=$<TARGET_FILE:pelican_cli>
        -DVAT_WRITER=$<TARGET_FILE:pelican_test_vat_fixture_writer>
        -DOUT_DIR=${CMAKE_BINARY_DIR}/test_artifacts/devcli_dist_config
        -P ${CMAKE_CURRENT_SOURCE_DIR}/run_devcli_dist_config.cmake
)

add_test(
    NAME devcli_project_init_command
    COMMAND $<TARGET_FILE:pelican_gpu_lease_run> -- ${CMAKE_COMMAND}
        -DCLI=$<TARGET_FILE:pelican_cli>
        -DPLAYER=$<TARGET_FILE:pelican_player>
        -DOUT_DIR=${CMAKE_BINARY_DIR}/test_artifacts/devcli_project_init
        -P ${CMAKE_CURRENT_SOURCE_DIR}/run_devcli_project_init.cmake
)
pelican_gpu_test_properties(devcli_project_init_command ${PELICAN_GPU_INTRINSIC_TIMEOUT_DEFAULT})

add_test(
    NAME animgraph_demo_preset_headless_player
    COMMAND $<TARGET_FILE:pelican_gpu_lease_run> -- ${CMAKE_COMMAND}
        -DPLAYER=$<TARGET_FILE:pelican_player>
        -DPROJECT_DIR=${CMAKE_SOURCE_DIR}/projects/animgraph_demo
        -DRENDER_CONFIG=${CMAKE_SOURCE_DIR}/projects/animgraph_demo/passes/main.json
        -DOUT_DIR=${CMAKE_BINARY_DIR}/test_artifacts/animgraph_demo_preset
        -P ${CMAKE_CURRENT_SOURCE_DIR}/run_preset_project_headless.cmake
)
pelican_gpu_test_properties(animgraph_demo_preset_headless_player 120)

if(NOT SKIP_DEVSTUDIO)
    add_executable(pelican_test_studio_player_arguments
        studio_player_arguments_probe.cpp
        ${CMAKE_SOURCE_DIR}/src/devstudio/viewport/studioplayerarguments.cpp)
    set_target_properties(pelican_test_studio_player_arguments PROPERTIES
        CXX_STANDARD 20)
    target_link_libraries(pelican_test_studio_player_arguments PRIVATE
        nlohmann_json)

    if(MSVC)
        set(pelican_validation_build_default "$<CONFIG:Debug>")
    else()
        # _DEBUG is an MSVC convention. Preserve the engine's existing exact
        # preprocessor default on other toolchains instead of assuming that a
        # configuration named Debug defines it.
        set(pelican_validation_build_default 0)
    endif()
    add_test(
        NAME vulkan_validation_runtime_player
        COMMAND $<TARGET_FILE:pelican_gpu_lease_run> -- ${CMAKE_COMMAND}
            -DPLAYER=$<TARGET_FILE:pelican_player>
            -DPROJECT_DIR=${CMAKE_SOURCE_DIR}/projects/animgraph_demo
            -DSTUDIO_ARGUMENTS_PROBE=$<TARGET_FILE:pelican_test_studio_player_arguments>
            -DEXPECT_DEFAULT_ENABLED=${pelican_validation_build_default}
            -DOUT_DIR=${CMAKE_BINARY_DIR}/test_artifacts/vulkan_validation_runtime
            -P ${CMAKE_CURRENT_SOURCE_DIR}/run_vulkan_validation_runtime.cmake
    )
    set_tests_properties(vulkan_validation_runtime_player PROPERTIES
        SKIP_REGULAR_EXPRESSION PELICAN_VULKAN_VALIDATION_RUNTIME_SKIP_NO_DEVICE)
    pelican_gpu_test_properties(vulkan_validation_runtime_player 600)
endif()

add_executable(pelican_test_png_nonuniform_check png_nonuniform_check.cpp)
set_target_properties(pelican_test_png_nonuniform_check PROPERTIES CXX_STANDARD 20)
target_link_libraries(pelican_test_png_nonuniform_check PRIVATE stb)

set(PELICAN_TEST_PROJECTS_DIR "" CACHE PATH
    "Project corpus scanned by project_catalog_headless_smoke; empty skips the test")

function(pelican_add_catalog_game_logic project_dir target_stem output_var)
    set(PELICAN_PROJECT_ROOT "${project_dir}")
    set(PELICAN_PROJECT_CODE_DIR "${project_dir}/code")
    set(PELICAN_GAME_LOGIC_TARGET_OVERRIDE
        "pelican_catalog_${target_stem}_game_logic")
    include("${PELICAN_PROJECT_CODE_DIR}/CMakeLists.txt")
    set(${output_var} "${PELICAN_GAME_LOGIC_TARGET_OVERRIDE}" PARENT_SCOPE)
endfunction()

# A catalog project with code is run with that code, rather than with a
# data-only generic player. Discovery still comes from project.json; this
# configure-time pass only creates the DLL artifacts that compiled code needs.
set(pelican_catalog_game_logic_targets)
if(NOT "${PELICAN_TEST_PROJECTS_DIR}" STREQUAL "")
    get_filename_component(pelican_catalog_projects_dir
        "${PELICAN_TEST_PROJECTS_DIR}" ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")
    file(GLOB_RECURSE pelican_catalog_project_manifests
        CONFIGURE_DEPENDS LIST_DIRECTORIES false
        "${pelican_catalog_projects_dir}/*/project.json")
    foreach(project_manifest IN LISTS pelican_catalog_project_manifests)
        get_filename_component(project_dir "${project_manifest}" DIRECTORY)
        if(EXISTS "${project_dir}/code/CMakeLists.txt")
            file(RELATIVE_PATH project_relative
                "${pelican_catalog_projects_dir}" "${project_dir}")
            string(REGEX REPLACE "[^A-Za-z0-9_]" "_"
                target_stem "${project_relative}")
            pelican_add_catalog_game_logic(
                "${project_dir}" "${target_stem}" game_logic_target)
            list(APPEND pelican_catalog_game_logic_targets
                "${game_logic_target}")
        endif()
    endforeach()
endif()

add_dependencies(pelican_test_png_nonuniform_check
    ${pelican_catalog_game_logic_targets})
if(TARGET pelican_test_studio_player_arguments)
    add_dependencies(pelican_test_png_nonuniform_check
        pelican_test_studio_player_arguments)
endif()

if(TARGET pelican_test_studio_player_arguments)
    add_test(
        NAME project_catalog_headless_smoke
        COMMAND $<TARGET_FILE:pelican_gpu_lease_run> -- ${CMAKE_COMMAND}
            -DPLAYER=$<TARGET_FILE:pelican_player>
            -DPNG_NONUNIFORM_CHECK=$<TARGET_FILE:pelican_test_png_nonuniform_check>
            -DSTUDIO_ARGUMENTS_PROBE=$<TARGET_FILE:pelican_test_studio_player_arguments>
            -DGAME_LOGIC_DIR=$<TARGET_FILE_DIR:pelican_test_png_nonuniform_check>
            -DGAME_LOGIC_PREFIX=${CMAKE_SHARED_LIBRARY_PREFIX}
            -DGAME_LOGIC_SUFFIX=${CMAKE_SHARED_LIBRARY_SUFFIX}
            -DPROJECTS_DIR=${PELICAN_TEST_PROJECTS_DIR}
            -DOUT_DIR=${CMAKE_BINARY_DIR}/test_artifacts/project_catalog_smoke
            -P ${CMAKE_CURRENT_SOURCE_DIR}/run_project_catalog_smoke.cmake
    )
    set_tests_properties(project_catalog_headless_smoke PROPERTIES
        SKIP_REGULAR_EXPRESSION PELICAN_PROJECT_CATALOG_SMOKE_SKIP_NO_PROJECTS_DIR)
    pelican_gpu_test_properties(project_catalog_headless_smoke 600)
endif()

if(PELICAN_WITH_AUDIO)
pelican_define_test(audio_test GROUP core_diagnostics pelican_core)
endif()

add_executable(pelican_test_audio_disabled_probe audio_disabled_probe.cpp)
set_target_properties(pelican_test_audio_disabled_probe PROPERTIES CXX_STANDARD 20)
target_link_libraries(pelican_test_audio_disabled_probe PRIVATE pelican_core)

pelican_define_test(persistence_test GROUP core_diagnostics pelican_core)
pelican_define_test(effecttransaction_persistence_test GROUP core_diagnostics pelican_core)
pelican_define_test(effecttransaction_allocation_test pelican_core)
if(PELICAN_WITH_AUDIO)
pelican_define_test(effecttransaction_test GROUP core_diagnostics pelican_core)
endif()
pelican_define_test(assetsmanifest_test pelican_project)
pelican_define_test(assetsverification_test pelican_core)

add_test(
    NAME devcli_assets_command
    COMMAND $<TARGET_FILE:pelican_gpu_lease_run> -- ${CMAKE_COMMAND}
        -DCLI=$<TARGET_FILE:pelican_cli>
        -DPLAYER=$<TARGET_FILE:pelican_player>
        -DOUT_DIR=${CMAKE_BINARY_DIR}/test_artifacts/devcli_assets
        -P ${CMAKE_CURRENT_SOURCE_DIR}/run_devcli_assets.cmake
)
pelican_gpu_test_properties(devcli_assets_command ${PELICAN_GPU_INTRINSIC_TIMEOUT_DEFAULT})

pelican_define_test(surfaceformat_test GROUP project_surface pelican_project)
target_compile_definitions(pelican_test_surfaceformat_test PRIVATE
    PELICAN_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

pelican_define_test(openpbrmapping_test GROUP project_surface pelican_project)
target_compile_definitions(pelican_test_openpbrmapping_test PRIVATE
    PELICAN_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

pelican_define_test(framephase_test GROUP frame_lifecycle pelican_core)

pelican_define_test(filewatcher_test GROUP frame_lifecycle pelican_core)
target_link_libraries(pelican_test_filewatcher_test PRIVATE picosha2)

pelican_define_test(reloadtransaction_test GROUP frame_lifecycle pelican_core)
pelican_define_test(submissionlifetime_test GROUP frame_lifecycle pelican_core)
pelican_define_test(outputcompilefacts_test GROUP frame_lifecycle pelican_core)
pelican_define_test(frametarget_test GROUP frame_lifecycle pelican_core)
pelican_define_test(swapchainrecovery_test GROUP frame_lifecycle pelican_core)
if(WIN32)
    add_test(
        NAME wsi_fault_window_player
        COMMAND $<TARGET_FILE:pelican_gpu_lease_run> -- ${CMAKE_COMMAND}
            -DPLAYER=$<TARGET_FILE:pelican_player>
            -DPLAYER_DIR=$<TARGET_FILE_DIR:pelican_player>
            -DPROJECT_DIR=${CMAKE_SOURCE_DIR}/projects/animgraph_demo
            -DOUT_DIR=${CMAKE_BINARY_DIR}/test_artifacts/wsi_fault_window
            -DCONFIG=$<CONFIG>
            -P ${CMAKE_CURRENT_SOURCE_DIR}/run_wsi_fault_window.cmake
    )
    pelican_gpu_test_properties(wsi_fault_window_player 180)
endif()
pelican_define_test(texturereload_test GPU pelican_core VulkanMemoryAllocator VulkanMemoryAllocator-Hpp)
pelican_define_test(materialvaluesreload_test GPU RUNTIME_SHADER pelican_core VulkanMemoryAllocator VulkanMemoryAllocator-Hpp)
target_compile_definitions(pelican_test_materialvaluesreload_test PRIVATE
    PELICAN_RUNTIME_SHADER_COMPILER=$<BOOL:${PELICAN_RUNTIME_SHADER_COMPILER}>
    PELICAN_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
if(PELICAN_RUNTIME_SHADER_COMPILER)
    target_link_libraries(pelican_test_materialvaluesreload_test PRIVATE pelican_shaderc)
    if(COMMAND pelican_copy_shaderc_runtime)
        pelican_copy_test_shaderc_runtime(pelican_test_materialvaluesreload_test)
    endif()
endif()

pelican_define_test(ui_foundation_test GROUP ui_foundation pelican_core
                    VulkanMemoryAllocator VulkanMemoryAllocator-Hpp)
target_compile_definitions(pelican_test_ui_foundation_test PRIVATE
    PELICAN_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

pelican_define_test(sprite_foundation_test GROUP ui_foundation pelican_core)

# UI semantic fixture coverage gate (design_ui_2d_foundation.md v8 §7).
# Dependency-free node script; skipped when node is unavailable.
find_program(PELICAN_NODE_EXECUTABLE node)
if(PELICAN_NODE_EXECUTABLE)
    add_test(
        NAME ui_semantic_coverage_gate
        COMMAND ${PELICAN_NODE_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/ui_semantic/normative/check_coverage.mjs
            ${CMAKE_SOURCE_DIR}
    )
endif()

# The OFF tier consumes already-baked SPIR-V. Until dist-bake emits every
# project stem/define variant, process tests that launch source-driven sample
# projects are ON-only. Keep the supported OFF test surface registered and
# running; disable only these known source-variant consumers.
set(PELICAN_RUNTIME_SHADER_PROCESS_TESTS
    physics_trigger_behavior_e2e
    behavior_dll_reload_code_only
    behavior_dll_reload_schema_drift
    behavior_dll_reload_version_bump_success
    behavior_dll_reload_version_bump_failure
    behavior_dll_reload_type_removed
    behavior_dll_reload_candidate_lifecycle_zero
    behavior_dll_reload_pending_recovery
    behavior_dll_reload_queued_event_purge
    behavior_dll_reload_oninit_rollback
    behavior_project_e2e
    game_logic_dll_reload
    seqplayer_headless_player
    vatplayer_headless_player
    free_camera_runtime_overlay_player
    vrm_xr_demo_rpc_player
    rpc_headless_player
    rpc_set_camera_headless_player
    rpc_scene_flow_headless_player
    rpc_inject_input_headless_player
    input_record_replay_headless_player
    devcli_bake_camera_replay
    rpc_inject_event_headless_player
    ui_u2_rpc_replay_player
    compute_buffer_headless_player
    gpu_timing_headless_player
    frame_plan_dump_headless_player
    devcli_project_init_command
    animgraph_demo_preset_headless_player
    vulkan_validation_runtime_player
    project_catalog_headless_smoke
    devcli_assets_command
    wsi_fault_window_player
)
if(NOT PELICAN_RUNTIME_SHADER_COMPILER)
    foreach(runtime_shader_test IN LISTS PELICAN_RUNTIME_SHADER_PROCESS_TESTS)
        if(TEST ${runtime_shader_test})
            set_tests_properties(${runtime_shader_test} PROPERTIES DISABLED TRUE)
        endif()
    endforeach()
endif()

set(PELICAN_PYTHON_TESTS "OFF" CACHE STRING
    "Python-backed test gates: OFF (default), AUTO, or ON (required)")
set_property(CACHE PELICAN_PYTHON_TESTS PROPERTY STRINGS AUTO ON OFF)
string(TOUPPER "${PELICAN_PYTHON_TESTS}" PELICAN_PYTHON_TESTS_MODE)
set(_pelican_python_test_modes AUTO ON OFF)
list(FIND _pelican_python_test_modes "${PELICAN_PYTHON_TESTS_MODE}"
     _pelican_python_test_mode_index)
if(_pelican_python_test_mode_index EQUAL -1)
    message(FATAL_ERROR
        "PELICAN_PYTHON_TESTS must be AUTO, ON, or OFF (got '${PELICAN_PYTHON_TESTS}')")
endif()

set(PELICAN_PYTHON_TESTS_AVAILABLE OFF)
if(PELICAN_PYTHON_TESTS_MODE STREQUAL "ON")
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    set(PELICAN_PYTHON_TESTS_AVAILABLE ON)
elseif(PELICAN_PYTHON_TESTS_MODE STREQUAL "AUTO")
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        set(PELICAN_PYTHON_TESTS_AVAILABLE ON)
    else()
        message(STATUS
            "Python 3 was not found; Python-backed tests are omitted "
            "(use -DPELICAN_PYTHON_TESTS=ON to require them)")
    endif()
else()
    message(STATUS "Python-backed tests are disabled")
endif()

if(PELICAN_PYTHON_TESTS_AVAILABLE)
    message(STATUS
        "Python-backed tests are enabled with ${Python3_EXECUTABLE}")

    function(pelican_define_python_test test_name script)
        add_test(
            NAME ${test_name}
            COMMAND ${CMAKE_COMMAND} -E env
                PYTHONDONTWRITEBYTECODE=1
                ${Python3_EXECUTABLE} -B
                ${script} ${ARGN}
        )
        set_tests_properties(${test_name} PROPERTIES LABELS python)
    endfunction()

    function(pelican_define_gpu_python_test test_name script)
        add_test(
            NAME ${test_name}
            COMMAND $<TARGET_FILE:pelican_gpu_lease_run> -- ${CMAKE_COMMAND} -E env
                PYTHONDONTWRITEBYTECODE=1
                ${Python3_EXECUTABLE} -B
                ${script} ${ARGN}
        )
        set_tests_properties(${test_name} PROPERTIES LABELS "gpu;python")
        pelican_gpu_test_properties(${test_name} 120)
    endfunction()

    # WP175 boundary contracts are source/fixture checks and require no GPU.
    foreach(contract_gate IN ITEMS openpbr projection engine-mvp)
        pelican_define_python_test(
            contract_${contract_gate}_gate
            ${CMAKE_CURRENT_SOURCE_DIR}/contract_boundary_gate.py
            --repo-root ${CMAKE_SOURCE_DIR}
            --gate ${contract_gate}
        )
    endforeach()
    pelican_define_python_test(
        contract_boundary_negative_fixtures
        ${CMAKE_CURRENT_SOURCE_DIR}/ci/test_contract_boundary_gate.py
    )
    pelican_define_python_test(
        ci_skip_policy_unit
        ${CMAKE_CURRENT_SOURCE_DIR}/ci/test_skip_policy.py
    )
    pelican_define_python_test(
        gpu_lease_inventory
        ${CMAKE_CURRENT_SOURCE_DIR}/ci/verify_gpu_lease_inventory.py
        --build-dir ${CMAKE_BINARY_DIR}
        --launcher $<TARGET_FILE:pelican_gpu_lease_run>
        --timeouts ${CMAKE_CURRENT_BINARY_DIR}/gpu_lease_timeouts.json
        --ctest ${CMAKE_CTEST_COMMAND}
        --config $<CONFIG>
    )
    # The lease's own runtime acceptance drives the real launcher on a
    # test-scoped lease name, so it needs no GPU and must never carry the gpu
    # label (that label is exactly the set the launcher wraps).  Serialise the
    # cases against each other with their own lock.
    pelican_define_python_test(
        gpu_lease_runtime
        ${CMAKE_CURRENT_SOURCE_DIR}/ci/test_gpu_lease_runtime.py
        --launcher $<TARGET_FILE:pelican_gpu_lease_run>
    )
    set_tests_properties(gpu_lease_runtime PROPERTIES
        TIMEOUT 300 RESOURCE_LOCK pelican_gpu_lease_selftest)

    if(PELICAN_WITH_RPC)
        pelican_define_gpu_python_test(
            pelican_rpc_smoke
            ${CMAKE_CURRENT_SOURCE_DIR}/pelican_rpc_smoke.py
            --repo-root ${CMAKE_SOURCE_DIR}
            --player $<TARGET_FILE:pelican_player>
        )
    endif()

    # Golden inventory integrity is deliberately GPU-free and therefore runs
    # in the CPU gate.
    pelican_define_python_test(
        golden_inventory
        ${CMAKE_CURRENT_SOURCE_DIR}/golden_inventory.py
        --repo-root ${CMAKE_SOURCE_DIR}
    )
    pelican_define_python_test(
        golden_inventory_negative_fixtures
        ${CMAKE_CURRENT_SOURCE_DIR}/ci/test_golden_inventory.py
    )
endif()

if(MSVC)
  string(SHA256 crash_symbol_test_id "${CMAKE_BINARY_DIR}")
  string(SUBSTRING "${crash_symbol_test_id}" 0 12 crash_symbol_test_id)
  file(TO_CMAKE_PATH "$ENV{TEMP}" crash_symbol_test_temp)
  add_test(
    NAME crash_symbol_archive_contract
    COMMAND ${CMAKE_COMMAND}
      "-DTEST_FIXTURE_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/crash_symbol_archive"
      "-DTEST_MODULE=${CMAKE_SOURCE_DIR}/cmake/pelican_crash_symbols.cmake"
      "-DTEST_WORK_ROOT=${crash_symbol_test_temp}/pelican_wp333_${crash_symbol_test_id}"
      "-DTEST_GENERATOR=${CMAKE_GENERATOR}"
      "-DTEST_GENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}"
      "-DTEST_GENERATOR_TOOLSET=${CMAKE_GENERATOR_TOOLSET}"
      -P ${CMAKE_CURRENT_SOURCE_DIR}/run_crash_symbol_archive.cmake
  )
  set_tests_properties(crash_symbol_archive_contract PROPERTIES TIMEOUT 180)

  # The contract test above drives the module through its own fixture, so it
  # stays green even if pelican_studio stops calling it. This one reads what
  # the real studio build left behind.
  if(TARGET pelican_studio)
    add_test(
      NAME crash_symbol_production_wiring
      COMMAND ${CMAKE_COMMAND}
        "-DARCHIVE_ROOT=${PELICAN_CRASH_SYMBOL_ARCHIVE_DIR}"
        "-DEXPECTED_TARGET=pelican_studio"
        -P ${CMAKE_CURRENT_SOURCE_DIR}/run_crash_symbol_production_wiring.cmake
    )
    set_tests_properties(crash_symbol_production_wiring
                         PROPERTIES TIMEOUT 60)
  endif()
endif()

# WP385 decision 12: one generated timeout policy.  The CTest TIMEOUT of every
# gpu-labelled test is lease_timeout_max + intrinsic + grace, and the inventory
# gate reads these same three numbers instead of repeating them.
get_property(pelican_gpu_intrinsic_entries GLOBAL
  PROPERTY PELICAN_GPU_INTRINSIC_TIMEOUTS)
list(JOIN pelican_gpu_intrinsic_entries ",\n    " pelican_gpu_intrinsic_json)
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/gpu_lease_timeouts.json
  "{\n"
  "  \"lease_timeout_max\": ${PELICAN_GPU_LEASE_TIMEOUT_MAX},\n"
  "  \"default_intrinsic\": ${PELICAN_GPU_INTRINSIC_TIMEOUT_DEFAULT},\n"
  "  \"grace\": ${PELICAN_GPU_CTEST_TIMEOUT_GRACE},\n"
  "  \"intrinsic\": {\n"
  "    ${pelican_gpu_intrinsic_json}\n"
  "  }\n"
  "}\n")
