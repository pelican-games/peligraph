FetchContent_Declare(
  Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG        v3.11.0
)
FetchContent_MakeAvailable(Catch2)
list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(CTest)
include(Catch)

function(pelican_define_test source_name)
cmake_parse_arguments(PELICAN_TEST "GPU" "" "" ${ARGN})
set(target_name pelican_test_${source_name})
add_executable(${target_name} ${source_name}.cpp)
set_target_properties(${target_name} PROPERTIES CXX_STANDARD 20)
target_include_directories(${target_name} PRIVATE ${Vulkan_INCLUDE_DIRS})
target_compile_definitions(${target_name} PRIVATE
    "VULKAN_HPP_DISPATCH_LOADER_STATIC_TYPE=::vk::detail::DispatchLoaderStatic"
)
target_link_libraries(${target_name} PRIVATE Catch2::Catch2WithMain nlohmann_json
    ${PELICAN_TEST_UNPARSED_ARGUMENTS})
set(discovery_properties)
if(source_name STREQUAL "golden_image_test")
  # Serialize golden cases so deterministic GPU fixtures do not contend for the device.
  list(APPEND discovery_properties RESOURCE_LOCK pelican_golden_gpu)
endif()
if(PELICAN_TEST_GPU)
  list(APPEND discovery_properties LABELS gpu)
endif()
if(discovery_properties)
  catch_discover_tests(${target_name} PROPERTIES ${discovery_properties})
else()
  catch_discover_tests(${target_name})
endif()
endfunction()

if(WIN32 AND COMMAND pelican_configure_game_logic_target)
  add_library(pelican_public_api_link_fixture SHARED
    fixtures/public_api_link/flipbook_client.cpp
  )
  set_target_properties(pelican_public_api_link_fixture PROPERTIES
    OUTPUT_NAME "pelican_public_api_link_fixture"
  )
  pelican_configure_game_logic_target(pelican_public_api_link_fixture)

  foreach(wp90_version IN ITEMS 1 2)
    set(wp90_target pelican_wp90_fixture_v${wp90_version})
    add_library(${wp90_target} SHARED
      ${CMAKE_SOURCE_DIR}/projects/example/code/playercontrol.cpp
      ${CMAKE_SOURCE_DIR}/src/player/gamelogic_entry.cpp
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
    ${CMAKE_SOURCE_DIR}/src/player/gamelogic_entry.cpp
  )
  set_target_properties(pelican_wp90_fixture_bad_abi PROPERTIES OUTPUT_NAME "pelican_wp90_fixture_bad_abi")
  target_compile_definitions(pelican_wp90_fixture_bad_abi PRIVATE
    WP90_BEHAVIOR_VERSION=2
    WP90_HOT_RELOAD_FIXTURE=1
    PELICAN_GAME_LOGIC_ABI_VERSION=999
  )
  pelican_configure_game_logic_target(pelican_wp90_fixture_bad_abi)

  add_test(
    NAME game_logic_dll_reload
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass
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
    TIMEOUT 120 RESOURCE_LOCK pelican_gpu LABELS gpu)

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

    foreach(physics_fixture_version IN ITEMS 1 2)
      set(physics_fixture_target
          pelican_physics_provider_fixture_v${physics_fixture_version})
      add_library(${physics_fixture_target} SHARED
        fixtures/physics_provider_dll/provider.cpp
        ${CMAKE_SOURCE_DIR}/src/player/gamelogic_entry.cpp
      )
      set_target_properties(${physics_fixture_target} PROPERTIES
        CXX_STANDARD 20
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
        OUTPUT_NAME "pelican_physics_provider_fixture_v${physics_fixture_version}"
      )
      add_dependencies(${physics_fixture_target}
        pelican_test_physics_provider_dll_test)
      target_link_libraries(${physics_fixture_target} PRIVATE
        "$<TARGET_LINKER_FILE:pelican_test_physics_provider_dll_test>"
      )
      target_include_directories(${physics_fixture_target} PRIVATE
        ${CMAKE_SOURCE_DIR}/src/core/userpublic
      )
      target_compile_definitions(${physics_fixture_target} PRIVATE
        PELICAN_GAME_DLL=1
        PELICAN_PHYSICS_FIXTURE_VERSION=${physics_fixture_version}
      )
    endforeach()

    add_library(pelican_physics_provider_fixture_bad_abi SHARED
      fixtures/physics_provider_dll/provider.cpp
      ${CMAKE_SOURCE_DIR}/src/player/gamelogic_entry.cpp
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
      PELICAN_PHYSICS_FIXTURE_VERSION=2
      PELICAN_GAME_LOGIC_ABI_VERSION=999
    )

    add_test(
      NAME physics_provider_game_dll_e2e
      COMMAND ${CMAKE_COMMAND} -E env
        "PELICAN_PHYSICS_FIXTURE_V1=$<TARGET_FILE:pelican_physics_provider_fixture_v1>"
        "PELICAN_PHYSICS_FIXTURE_V2=$<TARGET_FILE:pelican_physics_provider_fixture_v2>"
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
pelican_define_test(module_container_test pelican_core)
target_compile_definitions(pelican_test_module_container_test PRIVATE
    PELICAN_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
pelican_define_test(deletionqueue_test pelican_core)
pelican_define_test(deterministicrng_test pelican_core)
pelican_define_test(enginetime_test pelican_core)
pelican_define_test(skeletalanimation_test pelican_core glm)
pelican_define_test(vrmsemantic_test pelican_core tinygltf)
target_compile_definitions(pelican_test_vrmsemantic_test PRIVATE
    PELICAN_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
pelican_define_test(animation_probe_test pelican_core glm)
pelican_define_test(animation_jobs_test pelican_core glm)
pelican_define_test(animgraph_test pelican_core glm)
target_compile_definitions(pelican_test_animation_probe_test PRIVATE
    PELICAN_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

if(WIN32)
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
  pelican_define_test(vrmapplication_test GPU pelican_core VulkanMemoryAllocator
                      VulkanMemoryAllocator-Hpp glm)
  add_dependencies(pelican_test_vrmapplication_test
                   pelican_vrm_application_public_evaluator)
  target_compile_definitions(pelican_test_vrmapplication_test PRIVATE
    PELICAN_VRM_APPLICATION_EVALUATOR_DLL="$<TARGET_FILE:pelican_vrm_application_public_evaluator>"
  )
endif()
pelican_define_test(ecs_lifecycle_test pelican_core)
pelican_define_test(eventlayer_test pelican_core)
pelican_define_test(eventpayloadschema_test pelican_core)

function(pelican_event_schema_compile_fixture fixture expect_failure)
    set(target_name pelican_event_schema_${fixture})
    add_executable(${target_name} EXCLUDE_FROM_ALL fixtures/event_payload_schema/${fixture}.cpp)
    set_target_properties(${target_name} PROPERTIES CXX_STANDARD 20)
    add_test(
        NAME event_schema_compile_${fixture}
        COMMAND ${CMAKE_COMMAND}
            -DBUILD_DIR=${CMAKE_BINARY_DIR}
            -DCONFIG=$<CONFIG>
            -DTARGET_NAME=${target_name}
            -DEXPECT_FAILURE=${expect_failure}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/run_event_schema_compile.cmake
    )
    # These tests drive msbuild on the shared build tree; serialize them so a
    # parallel ctest run cannot race two builds in the same directory.
    set_tests_properties(event_schema_compile_${fixture} PROPERTIES
        RESOURCE_LOCK event_schema_compile_build
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
pelican_define_test(golden_image_test GPU pelican_core VulkanMemoryAllocator VulkanMemoryAllocator-Hpp spirv-reflect-static stb glm picosha2)
target_compile_definitions(pelican_test_golden_image_test PRIVATE
    VULKAN_HPP_DISPATCH_LOADER_STATIC_TYPE=::vk::detail::DispatchLoaderStatic
    PELICAN_RUNTIME_SHADER_COMPILER=$<BOOL:${PELICAN_RUNTIME_SHADER_COMPILER}>
    PELICAN_WITH_OPENXR=$<BOOL:${PELICAN_WITH_OPENXR}>
    PELICAN_WITH_VAT=$<BOOL:${PELICAN_WITH_VAT}>
    PELICAN_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    PELICAN_TEST_BINARY_DIR="${CMAKE_BINARY_DIR}"
)
if(PELICAN_RUNTIME_SHADER_COMPILER)
    target_link_libraries(pelican_test_golden_image_test PRIVATE pelican_shaderc)
    if(COMMAND pelican_copy_shaderc_runtime)
        pelican_copy_shaderc_runtime(pelican_test_golden_image_test)
    endif()
endif()
pelican_define_test(gltf_fragment_test GPU pelican_core pelican_project VulkanMemoryAllocator VulkanMemoryAllocator-Hpp)
target_compile_definitions(pelican_test_gltf_fragment_test PRIVATE
    PELICAN_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
pelican_define_test(modelhotreload_test GPU pelican_core VulkanMemoryAllocator VulkanMemoryAllocator-Hpp glm)
pelican_define_test(skeletal_gltf_test GPU pelican_core VulkanMemoryAllocator VulkanMemoryAllocator-Hpp glm)
pelican_define_test(morph_gltf_test GPU pelican_core VulkanMemoryAllocator VulkanMemoryAllocator-Hpp glm)
pelican_define_test(vrmfirstperson_test pelican_core glm)
pelican_define_test(vrm_xr_demo_test GPU pelican_core VulkanMemoryAllocator VulkanMemoryAllocator-Hpp glm)
target_compile_definitions(pelican_test_vrm_xr_demo_test PRIVATE
    PELICAN_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
pelican_define_test(materialinstanceoverride_test GPU pelican_core VulkanMemoryAllocator VulkanMemoryAllocator-Hpp glm)
# SceneLoader roundtrip case が Vulkan instance を要求するため GPU
# (validation layer なしの CI runner では ErrorLayerNotPresent)。
pelican_define_test(gltf_scene_extract_test GPU pelican_core VulkanMemoryAllocator VulkanMemoryAllocator-Hpp argparse tinygltf glm)
target_sources(pelican_test_gltf_scene_extract_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src/devcli/gltfsceneextract.cpp
)
pelican_define_test(headless_render_test GPU pelican_core VulkanMemoryAllocator VulkanMemoryAllocator-Hpp)
target_compile_definitions(pelican_test_headless_render_test PRIVATE
    VULKAN_HPP_DISPATCH_LOADER_STATIC_TYPE=::vk::detail::DispatchLoaderStatic
)
pelican_define_test(gamesystem_test pelican_core)
pelican_define_test(lightpolicy_test pelican_core VulkanMemoryAllocator VulkanMemoryAllocator-Hpp)
target_compile_definitions(pelican_test_gamesystem_test PRIVATE
    PELICAN_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
pelican_define_test(camera_test pelican_core glm)
pelican_define_test(temporal_test pelican_core glm)
pelican_define_test(projectionjitter_test pelican_core glm)
pelican_define_test(taa_resolve_test pelican_core glm)
target_compile_definitions(pelican_test_taa_resolve_test PRIVATE
    PELICAN_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
target_compile_definitions(pelican_test_projectionjitter_test PRIVATE
    PELICAN_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
target_compile_definitions(pelican_test_temporal_test PRIVATE
    PELICAN_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
pelican_define_test(color_pipeline_test pelican_core glm)
pelican_define_test(rpc_color_contract_test GPU pelican_core VulkanMemoryAllocator VulkanMemoryAllocator-Hpp spirv-reflect-static stb glm)
target_include_directories(pelican_test_rpc_color_contract_test PRIVATE ${stb_SOURCE_DIR})
target_compile_definitions(pelican_test_rpc_color_contract_test PRIVATE
    PELICAN_RUNTIME_SHADER_COMPILER=$<BOOL:${PELICAN_RUNTIME_SHADER_COMPILER}>
)
if(PELICAN_RUNTIME_SHADER_COMPILER)
    target_link_libraries(pelican_test_rpc_color_contract_test PRIVATE pelican_shaderc)
    if(COMMAND pelican_copy_shaderc_runtime)
        pelican_copy_shaderc_runtime(pelican_test_rpc_color_contract_test)
    endif()
endif()
if(PELICAN_WITH_EXR)
pelican_define_test(imageloader_test pelican_core tinyexr)
target_compile_definitions(pelican_test_imageloader_test PRIVATE
    PELICAN_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
endif()
pelican_define_test(inputstate_test pelican_core)
pelican_define_test(xractivation_test quill)
if(PELICAN_WITH_OPENXR)
pelican_define_test(xrdiscovery_test pelican_core pelican_openxr)
pelican_define_test(xrsession_test pelican_core pelican_openxr)
pelican_define_test(xraction_test pelican_core pelican_openxr)
pelican_define_test(xrcompositiontarget_test pelican_core pelican_openxr
    VulkanMemoryAllocator VulkanMemoryAllocator-Hpp)
pelican_define_test(xrviewspace_test pelican_core pelican_openxr
    VulkanMemoryAllocator VulkanMemoryAllocator-Hpp)
pelican_define_test(xrfeaturepolicy_test pelican_core pelican_openxr)
endif()
pelican_define_test(inputsequence_test pelican_core)
if(PELICAN_WITH_IMGUI)
pelican_define_test(imgui_isolation_test pelican_core)
pelican_define_test(planviewer_test pelican_core)
endif()
if(PELICAN_WITH_RPC)
pelican_define_test(jsonrpc_test pelican_project)
endif()
if(PELICAN_WITH_RENDERDOC)
pelican_define_test(renderdoccapture_test pelican_core)
endif()
pelican_define_test(pathresolver_test pelican_core)
target_compile_definitions(pelican_test_pathresolver_test PRIVATE
    PELICAN_WITH_VAT=$<BOOL:${PELICAN_WITH_VAT}>
    PELICAN_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
if(PELICAN_WITH_PHYSICS AND PELICAN_WITH_BUILTIN_PHYSICS)
pelican_define_test(physquery_test pelican_core)
pelican_define_test(physworld_test pelican_core glm)
pelican_define_test(platformer_controller_test pelican_core)
endif()
if(PELICAN_WITH_PHYSICS)
pelican_define_test(physicsservice_test pelican_core glm)
target_compile_definitions(pelican_test_physicsservice_test PRIVATE
    PELICAN_WITH_BUILTIN_PHYSICS=$<BOOL:${PELICAN_WITH_BUILTIN_PHYSICS}>
    PELICAN_WITH_JOLT_PHYSICS=$<BOOL:${PELICAN_WITH_JOLT_PHYSICS}>
)
endif()

add_executable(pelican_test_physics_feature_probe physics_feature_probe.cpp)
set_target_properties(pelican_test_physics_feature_probe PROPERTIES CXX_STANDARD 20)
target_link_libraries(pelican_test_physics_feature_probe PRIVATE pelican_core)
add_test(NAME physics_feature_probe COMMAND pelican_test_physics_feature_probe)
pelican_define_test(projectconfig_test pelican_core glm)
target_compile_definitions(pelican_test_projectconfig_test PRIVATE
    PELICAN_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
pelican_define_test(renderingpass_helpers_test pelican_core)
pelican_define_test(sceneformat_test pelican_core glm)
target_compile_definitions(pelican_test_sceneformat_test PRIVATE
    PELICAN_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
if(PELICAN_WITH_SEQPLAYER)
pelican_define_test(seqplayer_test pelican_core glm)
endif()
if(PELICAN_WITH_VAT)
pelican_define_test(vatformat_test pelican_core glm)
endif()
pelican_define_test(shader_compile_test spirv-reflect-static stb)
pelican_define_test(parallel_prepare_test)
pelican_define_test(shader_cache_test pelican_core spirv-reflect-static glm)
target_compile_definitions(pelican_test_shader_cache_test PRIVATE
    PELICAN_RUNTIME_SHADER_COMPILER=$<BOOL:${PELICAN_RUNTIME_SHADER_COMPILER}>
)
if(PELICAN_RUNTIME_SHADER_COMPILER)
    target_link_libraries(pelican_test_shader_cache_test PRIVATE pelican_shaderc)
    if(COMMAND pelican_copy_shaderc_runtime)
        pelican_copy_shaderc_runtime(pelican_test_shader_cache_test)
    endif()
endif()
target_compile_definitions(pelican_test_shader_compile_test PRIVATE
    PELICAN_RUNTIME_SHADER_COMPILER=$<BOOL:${PELICAN_RUNTIME_SHADER_COMPILER}>
)
if(PELICAN_RUNTIME_SHADER_COMPILER)
    target_link_libraries(pelican_test_shader_compile_test PRIVATE pelican_shaderc)
    if(COMMAND pelican_copy_shaderc_runtime)
        pelican_copy_shaderc_runtime(pelican_test_shader_compile_test)
    endif()
endif()
pelican_define_test(shader_compiler_reflection_test pelican_core spirv-reflect-static glm)
target_compile_definitions(pelican_test_shader_compiler_reflection_test PRIVATE
    PELICAN_RUNTIME_SHADER_COMPILER=$<BOOL:${PELICAN_RUNTIME_SHADER_COMPILER}>
    PELICAN_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
if(PELICAN_RUNTIME_SHADER_COMPILER)
    target_link_libraries(pelican_test_shader_compiler_reflection_test PRIVATE pelican_shaderc)
    if(COMMAND pelican_copy_shaderc_runtime)
        pelican_copy_shaderc_runtime(pelican_test_shader_compiler_reflection_test)
    endif()
endif()
pelican_define_test(surfacecompiler_test pelican_core spirv-reflect-static glm)
target_compile_definitions(pelican_test_surfacecompiler_test PRIVATE
    PELICAN_RUNTIME_SHADER_COMPILER=$<BOOL:${PELICAN_RUNTIME_SHADER_COMPILER}>
    PELICAN_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
if(PELICAN_RUNTIME_SHADER_COMPILER)
    target_link_libraries(pelican_test_surfacecompiler_test PRIVATE pelican_shaderc)
    if(COMMAND pelican_copy_shaderc_runtime)
        pelican_copy_shaderc_runtime(pelican_test_surfacecompiler_test)
    endif()
endif()
pelican_define_test(spvlink_test pelican_core spirv-reflect-static glm)
target_compile_definitions(pelican_test_spvlink_test PRIVATE
    PELICAN_RUNTIME_SHADER_COMPILER=$<BOOL:${PELICAN_RUNTIME_SHADER_COMPILER}>
    PELICAN_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
if(PELICAN_RUNTIME_SHADER_COMPILER)
    target_link_libraries(pelican_test_spvlink_test PRIVATE pelican_shaderc)
    if(COMMAND pelican_copy_shaderc_runtime)
        pelican_copy_shaderc_runtime(pelican_test_spvlink_test)
    endif()
endif()
find_program(PELICAN_SLANGC_EXECUTABLE slangc HINTS $ENV{VULKAN_SDK}/Bin)
if(PELICAN_SLANGC_EXECUTABLE)
    set(PELICAN_SLANG_COOL_FIXTURE ${CMAKE_CURRENT_BINARY_DIR}/fixtures/spvlink/surface_slang_cool.spv)
    set(PELICAN_SLANG_WARM_FIXTURE ${CMAKE_CURRENT_BINARY_DIR}/fixtures/spvlink/surface_slang_warm.spv)
    add_custom_command(
        OUTPUT ${PELICAN_SLANG_COOL_FIXTURE} ${PELICAN_SLANG_WARM_FIXTURE}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/fixtures/spvlink
        COMMAND ${PELICAN_SLANGC_EXECUTABLE}
            -target spirv
            -profile ps_6_0+spirv_1_5
            -entry main
            -stage fragment
            -O0
            -emit-spirv-directly
            -fvk-use-entrypoint-name
            ${CMAKE_SOURCE_DIR}/experiments/spvlink/shaders/surface.slang
            -o ${PELICAN_SLANG_COOL_FIXTURE}
        COMMAND ${PELICAN_SLANGC_EXECUTABLE}
            -target spirv
            -profile ps_6_0+spirv_1_5
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
if(Vulkan_GLSLANG_VALIDATOR_EXECUTABLE)
    add_test(
        NAME spvlink_cli_linked_golden
        COMMAND ${CMAKE_COMMAND}
            -DGLSLANG=${Vulkan_GLSLANG_VALIDATOR_EXECUTABLE}
            -DSPIRV_VAL=$ENV{VULKAN_SDK}/Bin/spirv-val.exe
            -DLINKER=$<TARGET_FILE:pelican-spv-link>
            -DSOURCE_DIR=${CMAKE_SOURCE_DIR}
            -DOUT_DIR=${CMAKE_BINARY_DIR}/test_artifacts/spvlink_cli_golden
            -P ${CMAKE_CURRENT_SOURCE_DIR}/run_spvlink_golden.cmake
    )
endif()
pelican_define_test(shader_library_test pelican_core spirv-reflect-static glm)
target_compile_definitions(pelican_test_shader_library_test PRIVATE
    PELICAN_RUNTIME_SHADER_COMPILER=$<BOOL:${PELICAN_RUNTIME_SHADER_COMPILER}>
    PELICAN_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
if(PELICAN_RUNTIME_SHADER_COMPILER)
    target_link_libraries(pelican_test_shader_library_test PRIVATE pelican_shaderc)
    if(COMMAND pelican_copy_shaderc_runtime)
        pelican_copy_shaderc_runtime(pelican_test_shader_library_test)
    endif()
endif()
pelican_define_test(vulkan_headless_test GPU pelican_core VulkanMemoryAllocator VulkanMemoryAllocator-Hpp)
target_compile_definitions(pelican_test_vulkan_headless_test PRIVATE
    VULKAN_HPP_DISPATCH_LOADER_STATIC_TYPE=::vk::detail::DispatchLoaderStatic
)

if(PELICAN_WITH_SEQPLAYER)
    add_test(
        NAME seqplayer_headless_player
        COMMAND ${CMAKE_COMMAND}
            -DPLAYER=$<TARGET_FILE:pelican_player>
            -DPLAYER_DIR=$<TARGET_FILE_DIR:pelican_player>
            -DPROJECT_DIR=${CMAKE_SOURCE_DIR}/projects/example
            -DSEQ_FILE=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/seqplayer_two_objects.jsonl
            -DOUT_DIR=${CMAKE_BINARY_DIR}/test_artifacts/seqplayer_headless
            -P ${CMAKE_CURRENT_SOURCE_DIR}/run_seqplayer_headless.cmake
    )
    set_tests_properties(seqplayer_headless_player PROPERTIES LABELS gpu)
endif()

if(PELICAN_WITH_VAT)
    add_test(
        NAME vatplayer_headless_player
        COMMAND ${CMAKE_COMMAND}
            -DPLAYER=$<TARGET_FILE:pelican_player>
            -DPLAYER_DIR=$<TARGET_FILE_DIR:pelican_player>
            -DWRITER=$<TARGET_FILE:pelican_test_vat_fixture_writer>
            -DEXAMPLE_PASS=${CMAKE_SOURCE_DIR}/projects/example/passes/main_rendering_config.json
            -DOUT_DIR=${CMAKE_BINARY_DIR}/test_artifacts/vatplayer_headless
            -P ${CMAKE_CURRENT_SOURCE_DIR}/run_vatplayer_headless.cmake
    )
    set_tests_properties(vatplayer_headless_player PROPERTIES LABELS gpu)
endif()

if(PELICAN_WITH_RPC)
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
        COMMAND ${CMAKE_COMMAND}
            -DPLAYER=$<TARGET_FILE:pelican_test_vrm_xr_demo_player>
            -DPROJECT_DIR=${CMAKE_SOURCE_DIR}/projects/vrm_xr_demo
            -DOUT_DIR=${CMAKE_BINARY_DIR}/test_artifacts/vrm_xr_demo_rpc
            -P ${CMAKE_CURRENT_SOURCE_DIR}/run_vrm_xr_demo_rpc.cmake
    )
    set_tests_properties(vrm_xr_demo_rpc_player PROPERTIES
        TIMEOUT 180 RESOURCE_LOCK pelican_gpu LABELS gpu)
    add_test(
        NAME rpc_headless_player
        COMMAND ${CMAKE_COMMAND}
            -DPLAYER=$<TARGET_FILE:pelican_player>
            -DPLAYER_DIR=$<TARGET_FILE_DIR:pelican_player>
            -DPROJECT_DIR=${CMAKE_SOURCE_DIR}/projects/example
            -DOUT_DIR=${CMAKE_BINARY_DIR}/test_artifacts/rpc_headless
            -DRENDERDOC_STATUS=$<IF:$<BOOL:${PELICAN_WITH_RENDERDOC}>,absent,disabled>
            -DRENDERDOC_REASON=$<IF:$<BOOL:${PELICAN_WITH_RENDERDOC}>,renderdoc_not_injected,renderdoc_build_disabled>
            -P ${CMAKE_CURRENT_SOURCE_DIR}/run_rpc_headless.cmake
    )
    set_tests_properties(rpc_headless_player PROPERTIES LABELS gpu)
    add_test(
        NAME rpc_set_camera_headless_player
        COMMAND ${CMAKE_COMMAND}
            -DPLAYER=$<TARGET_FILE:pelican_player>
            -DPLAYER_DIR=$<TARGET_FILE_DIR:pelican_player>
            -DPROJECT_DIR=${CMAKE_SOURCE_DIR}/projects/example
            -DOUT_DIR=${CMAKE_BINARY_DIR}/test_artifacts/rpc_set_camera_headless
            -P ${CMAKE_CURRENT_SOURCE_DIR}/run_rpc_set_camera_headless.cmake
    )
    set_tests_properties(rpc_set_camera_headless_player PROPERTIES LABELS gpu)
    add_test(
        NAME rpc_scene_flow_headless_player
        COMMAND ${CMAKE_COMMAND}
            -DPLAYER=$<TARGET_FILE:pelican_player>
            -DPLAYER_DIR=$<TARGET_FILE_DIR:pelican_player>
            -DPROJECT_DIR=${CMAKE_SOURCE_DIR}/projects/example
            -DOUT_DIR=${CMAKE_BINARY_DIR}/test_artifacts/rpc_scene_flow_headless
            -P ${CMAKE_CURRENT_SOURCE_DIR}/run_rpc_scene_flow_headless.cmake
    )
    set_tests_properties(rpc_scene_flow_headless_player PROPERTIES LABELS gpu)
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
        COMMAND ${CMAKE_COMMAND}
            -DPLAYER=$<TARGET_FILE:pelican_test_rpc_inject_input_player>
            -DPLAYER_DIR=$<TARGET_FILE_DIR:pelican_test_rpc_inject_input_player>
            -DPROJECT_DIR=${CMAKE_SOURCE_DIR}/projects/example
            -DOUT_DIR=${CMAKE_BINARY_DIR}/test_artifacts/rpc_inject_input_headless
            -P ${CMAKE_CURRENT_SOURCE_DIR}/run_rpc_inject_input_headless.cmake
    )
    set_tests_properties(rpc_inject_input_headless_player PROPERTIES LABELS gpu)
    add_test(
        NAME input_record_replay_headless_player
        COMMAND ${CMAKE_COMMAND}
            -DPLAYER=$<TARGET_FILE:pelican_test_rpc_inject_input_player>
            -DPLAYER_DIR=$<TARGET_FILE_DIR:pelican_test_rpc_inject_input_player>
            -DPROJECT_DIR=${CMAKE_SOURCE_DIR}/projects/example
            -DOUT_DIR=${CMAKE_BINARY_DIR}/test_artifacts/input_record_replay_headless
            -P ${CMAKE_CURRENT_SOURCE_DIR}/run_input_record_replay_headless.cmake
    )
    set_tests_properties(input_record_replay_headless_player PROPERTIES LABELS gpu)
    add_test(
        NAME devcli_bake_camera_replay
        COMMAND ${CMAKE_COMMAND}
            -DCLI=$<TARGET_FILE:pelican_cli>
            -DPLAYER=$<TARGET_FILE:pelican_test_camera_replay_player>
            -DPLAYER_DIR=$<TARGET_FILE_DIR:pelican_test_camera_replay_player>
            -DSETUP_PLAYER=$<TARGET_FILE:pelican_test_rpc_inject_input_player>
            -DPROJECT_DIR=${CMAKE_SOURCE_DIR}/projects/example
            -DOUT_DIR=${CMAKE_BINARY_DIR}/test_artifacts/devcli_bake_camera
            -P ${CMAKE_CURRENT_SOURCE_DIR}/run_devcli_bake_camera.cmake
    )
    set_tests_properties(devcli_bake_camera_replay PROPERTIES LABELS gpu)
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
        COMMAND ${CMAKE_COMMAND}
            -DPLAYER=$<TARGET_FILE:pelican_test_rpc_inject_event_player>
            -DPLAYER_DIR=$<TARGET_FILE_DIR:pelican_test_rpc_inject_event_player>
            -DPROJECT_DIR=${CMAKE_SOURCE_DIR}/projects/example
            -DOUT_DIR=${CMAKE_BINARY_DIR}/test_artifacts/rpc_inject_event_headless
            -P ${CMAKE_CURRENT_SOURCE_DIR}/run_rpc_inject_event_headless.cmake
    )
    set_tests_properties(rpc_inject_event_headless_player PROPERTIES LABELS gpu)
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
        COMMAND ${CMAKE_COMMAND}
            -DPLAYER=$<TARGET_FILE:pelican_test_ui_u2_player>
            -DPLAYER_DIR=$<TARGET_FILE_DIR:pelican_test_ui_u2_player>
            -DPROJECT_DIR=${CMAKE_SOURCE_DIR}/projects/example
            -DOUT_DIR=${CMAKE_BINARY_DIR}/test_artifacts/ui_u2_rpc_replay
            -P ${CMAKE_CURRENT_SOURCE_DIR}/run_ui_u2_rpc_replay.cmake
    )
    set_tests_properties(ui_u2_rpc_replay_player PROPERTIES LABELS gpu)
endif()

add_test(
    NAME compute_buffer_headless_player
    COMMAND ${CMAKE_COMMAND}
        -DPLAYER=$<TARGET_FILE:pelican_player>
        -DPLAYER_DIR=$<TARGET_FILE_DIR:pelican_player>
        -DOUT_DIR=${CMAKE_BINARY_DIR}/test_artifacts/compute_buffer_headless
        -P ${CMAKE_CURRENT_SOURCE_DIR}/run_compute_headless.cmake
)
set_tests_properties(compute_buffer_headless_player PROPERTIES LABELS gpu)

pelican_define_test(importmanifest_test pelican_project)
pelican_define_test(importrules_test pelican_project)
target_compile_definitions(pelican_test_importrules_test PRIVATE PELICAN_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_compile_definitions(pelican_test_importmanifest_test PRIVATE
    PELICAN_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

pelican_define_test(materialformat_test pelican_project)
target_compile_definitions(pelican_test_materialformat_test PRIVATE
    PELICAN_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

pelican_define_test(materiallowering_test pelican_project)
target_compile_definitions(pelican_test_materiallowering_test PRIVATE
    PELICAN_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

pelican_define_test(materialbinding_test pelican_core)

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

pelican_define_test(frameplanner_test pelican_core)
target_compile_definitions(pelican_test_frameplanner_test PRIVATE
    PELICAN_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

pelican_define_test(featurecompose_test pelican_core)
target_compile_definitions(pelican_test_featurecompose_test PRIVATE
    PELICAN_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

pelican_define_test(shader_defines_test pelican_core)
target_compile_definitions(pelican_test_shader_defines_test PRIVATE
    PELICAN_RUNTIME_SHADER_COMPILER=$<BOOL:${PELICAN_RUNTIME_SHADER_COMPILER}>
)
if(PELICAN_RUNTIME_SHADER_COMPILER)
    target_link_libraries(pelican_test_shader_defines_test PRIVATE pelican_shaderc)
    if(COMMAND pelican_copy_shaderc_runtime)
        pelican_copy_shaderc_runtime(pelican_test_shader_defines_test)
    endif()
endif()

pelican_define_test(debugdraw_test pelican_core glm VulkanMemoryAllocator VulkanMemoryAllocator-Hpp)
pelican_define_test(debugtext_test pelican_core glm VulkanMemoryAllocator VulkanMemoryAllocator-Hpp)
pelican_define_test(debugutils_test pelican_core)
pelican_define_test(rendertiming_test pelican_core)
target_compile_definitions(pelican_test_rendertiming_test PRIVATE
    PELICAN_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

add_test(
    NAME gpu_timing_headless_player
    COMMAND ${CMAKE_COMMAND}
        -DPLAYER=$<TARGET_FILE:pelican_player>
        -DPLAYER_DIR=$<TARGET_FILE_DIR:pelican_player>
        -DOUT_DIR=${CMAKE_BINARY_DIR}/test_artifacts/gpu_timing_headless
        -P ${CMAKE_CURRENT_SOURCE_DIR}/run_gpu_timing_headless.cmake
)
set_tests_properties(gpu_timing_headless_player PROPERTIES LABELS gpu)

add_test(
    NAME frame_plan_dump_headless_player
    COMMAND ${CMAKE_COMMAND}
        -DPLAYER=$<TARGET_FILE:pelican_player>
        -DPLAYER_DIR=$<TARGET_FILE_DIR:pelican_player>
        -DPROJECT_DIR=${CMAKE_SOURCE_DIR}/projects/example
        -DOUT_DIR=${CMAKE_BINARY_DIR}/test_artifacts/frame_plan_dump_headless
        -P ${CMAKE_CURRENT_SOURCE_DIR}/run_frame_plan_dump_headless.cmake
)
set_tests_properties(frame_plan_dump_headless_player PROPERTIES LABELS gpu)

pelican_define_test(inputactions_test pelican_core)
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
    COMMAND ${CMAKE_COMMAND}
        -DCLI=$<TARGET_FILE:pelican_cli>
        -DPLAYER=$<TARGET_FILE:pelican_player>
        -DOUT_DIR=${CMAKE_BINARY_DIR}/test_artifacts/devcli_project_init
        -P ${CMAKE_CURRENT_SOURCE_DIR}/run_devcli_project_init.cmake
)
set_tests_properties(devcli_project_init_command PROPERTIES LABELS gpu)

if(PELICAN_WITH_AUDIO)
pelican_define_test(audio_test pelican_core)
endif()

add_executable(pelican_test_audio_disabled_probe audio_disabled_probe.cpp)
set_target_properties(pelican_test_audio_disabled_probe PROPERTIES CXX_STANDARD 20)
target_link_libraries(pelican_test_audio_disabled_probe PRIVATE pelican_core)

pelican_define_test(persistence_test pelican_core)
pelican_define_test(assetsmanifest_test pelican_project)
pelican_define_test(assetsverification_test pelican_core)

add_test(
    NAME devcli_assets_command
    COMMAND ${CMAKE_COMMAND}
        -DCLI=$<TARGET_FILE:pelican_cli>
        -DPLAYER=$<TARGET_FILE:pelican_player>
        -DOUT_DIR=${CMAKE_BINARY_DIR}/test_artifacts/devcli_assets
        -P ${CMAKE_CURRENT_SOURCE_DIR}/run_devcli_assets.cmake
)
set_tests_properties(devcli_assets_command PROPERTIES LABELS gpu)

pelican_define_test(surfaceformat_test pelican_project)
target_compile_definitions(pelican_test_surfaceformat_test PRIVATE
    PELICAN_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

pelican_define_test(openpbrmapping_test pelican_project)
target_compile_definitions(pelican_test_openpbrmapping_test PRIVATE
    PELICAN_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

pelican_define_test(framephase_test pelican_core)

pelican_define_test(filewatcher_test pelican_core)
target_link_libraries(pelican_test_filewatcher_test PRIVATE picosha2)

pelican_define_test(reloadtransaction_test pelican_core)
pelican_define_test(texturereload_test GPU pelican_core VulkanMemoryAllocator VulkanMemoryAllocator-Hpp)
pelican_define_test(materialvaluesreload_test GPU pelican_core VulkanMemoryAllocator VulkanMemoryAllocator-Hpp)
target_compile_definitions(pelican_test_materialvaluesreload_test PRIVATE
    PELICAN_RUNTIME_SHADER_COMPILER=$<BOOL:${PELICAN_RUNTIME_SHADER_COMPILER}>
    PELICAN_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
if(PELICAN_RUNTIME_SHADER_COMPILER)
    target_link_libraries(pelican_test_materialvaluesreload_test PRIVATE pelican_shaderc)
    if(COMMAND pelican_copy_shaderc_runtime)
        pelican_copy_shaderc_runtime(pelican_test_materialvaluesreload_test)
    endif()
endif()

pelican_define_test(ui_foundation_test pelican_core VulkanMemoryAllocator VulkanMemoryAllocator-Hpp)
target_compile_definitions(pelican_test_ui_foundation_test PRIVATE
    PELICAN_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

pelican_define_test(sprite_foundation_test pelican_core)

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

# Golden inventory integrity is deliberately GPU-free and therefore runs in the CPU gate.
find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_test(
    NAME golden_inventory
    COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/golden_inventory.py
        --repo-root ${CMAKE_SOURCE_DIR}
)
add_test(
    NAME golden_inventory_negative_fixtures
    COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/ci/test_golden_inventory.py
)
