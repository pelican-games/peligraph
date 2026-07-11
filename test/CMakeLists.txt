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
set(target_name pelican_test_${source_name})
add_executable(${target_name} ${source_name}.cpp)
set_target_properties(${target_name} PROPERTIES CXX_STANDARD 20)
target_include_directories(${target_name} PRIVATE ${Vulkan_INCLUDE_DIRS})
target_compile_definitions(${target_name} PRIVATE
    "VULKAN_HPP_DISPATCH_LOADER_STATIC_TYPE=::vk::detail::DispatchLoaderStatic"
)
target_link_libraries(${target_name} PRIVATE Catch2::Catch2WithMain nlohmann_json ${ARGN})
catch_discover_tests(${target_name})
endfunction()

add_executable(pelican_test_vat_fixture_writer vat_fixture_writer.cpp)
set_target_properties(pelican_test_vat_fixture_writer PROPERTIES CXX_STANDARD 20)
target_link_libraries(pelican_test_vat_fixture_writer PRIVATE nlohmann_json)

# register tests
pelican_define_test(deletionqueue_test pelican_core)
pelican_define_test(deterministicrng_test pelican_core)
pelican_define_test(enginetime_test pelican_core)
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
pelican_define_test(golden_image_test pelican_core VulkanMemoryAllocator VulkanMemoryAllocator-Hpp spirv-reflect-static stb glm picosha2)
target_compile_definitions(pelican_test_golden_image_test PRIVATE
    VULKAN_HPP_DISPATCH_LOADER_STATIC_TYPE=::vk::detail::DispatchLoaderStatic
    PELICAN_RUNTIME_SHADER_COMPILER=$<BOOL:${PELICAN_RUNTIME_SHADER_COMPILER}>
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
pelican_define_test(gltf_fragment_test pelican_core VulkanMemoryAllocator VulkanMemoryAllocator-Hpp)
pelican_define_test(headless_render_test pelican_core VulkanMemoryAllocator VulkanMemoryAllocator-Hpp)
target_compile_definitions(pelican_test_headless_render_test PRIVATE
    VULKAN_HPP_DISPATCH_LOADER_STATIC_TYPE=::vk::detail::DispatchLoaderStatic
)
pelican_define_test(gamesystem_test pelican_core)
target_compile_definitions(pelican_test_gamesystem_test PRIVATE
    PELICAN_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
pelican_define_test(camera_test pelican_core glm)
pelican_define_test(color_pipeline_test pelican_core glm)
pelican_define_test(rpc_color_contract_test pelican_core VulkanMemoryAllocator VulkanMemoryAllocator-Hpp spirv-reflect-static stb glm)
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
endif()
pelican_define_test(inputstate_test pelican_core)
if(PELICAN_WITH_RPC)
pelican_define_test(jsonrpc_test pelican_project)
endif()
pelican_define_test(pathresolver_test pelican_core)
target_compile_definitions(pelican_test_pathresolver_test PRIVATE
    PELICAN_WITH_VAT=$<BOOL:${PELICAN_WITH_VAT}>
    PELICAN_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
pelican_define_test(physquery_test pelican_core)
pelican_define_test(physworld_test pelican_core glm)
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
pelican_define_test(vulkan_headless_test pelican_core VulkanMemoryAllocator VulkanMemoryAllocator-Hpp)
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
endif()

if(PELICAN_WITH_RPC)
    add_test(
        NAME rpc_headless_player
        COMMAND ${CMAKE_COMMAND}
            -DPLAYER=$<TARGET_FILE:pelican_player>
            -DPLAYER_DIR=$<TARGET_FILE_DIR:pelican_player>
            -DPROJECT_DIR=${CMAKE_SOURCE_DIR}/projects/example
            -DOUT_DIR=${CMAKE_BINARY_DIR}/test_artifacts/rpc_headless
            -P ${CMAKE_CURRENT_SOURCE_DIR}/run_rpc_headless.cmake
    )
    add_test(
        NAME rpc_set_camera_headless_player
        COMMAND ${CMAKE_COMMAND}
            -DPLAYER=$<TARGET_FILE:pelican_player>
            -DPLAYER_DIR=$<TARGET_FILE_DIR:pelican_player>
            -DPROJECT_DIR=${CMAKE_SOURCE_DIR}/projects/example
            -DOUT_DIR=${CMAKE_BINARY_DIR}/test_artifacts/rpc_set_camera_headless
            -P ${CMAKE_CURRENT_SOURCE_DIR}/run_rpc_set_camera_headless.cmake
    )
    add_test(
        NAME rpc_scene_flow_headless_player
        COMMAND ${CMAKE_COMMAND}
            -DPLAYER=$<TARGET_FILE:pelican_player>
            -DPLAYER_DIR=$<TARGET_FILE_DIR:pelican_player>
            -DPROJECT_DIR=${CMAKE_SOURCE_DIR}/projects/example
            -DOUT_DIR=${CMAKE_BINARY_DIR}/test_artifacts/rpc_scene_flow_headless
            -P ${CMAKE_CURRENT_SOURCE_DIR}/run_rpc_scene_flow_headless.cmake
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
    add_test(
        NAME rpc_inject_input_headless_player
        COMMAND ${CMAKE_COMMAND}
            -DPLAYER=$<TARGET_FILE:pelican_test_rpc_inject_input_player>
            -DPLAYER_DIR=$<TARGET_FILE_DIR:pelican_test_rpc_inject_input_player>
            -DPROJECT_DIR=${CMAKE_SOURCE_DIR}/projects/example
            -DOUT_DIR=${CMAKE_BINARY_DIR}/test_artifacts/rpc_inject_input_headless
            -P ${CMAKE_CURRENT_SOURCE_DIR}/run_rpc_inject_input_headless.cmake
    )
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
endif()

add_test(
    NAME compute_buffer_headless_player
    COMMAND ${CMAKE_COMMAND}
        -DPLAYER=$<TARGET_FILE:pelican_player>
        -DPLAYER_DIR=$<TARGET_FILE_DIR:pelican_player>
        -DOUT_DIR=${CMAKE_BINARY_DIR}/test_artifacts/compute_buffer_headless
        -P ${CMAKE_CURRENT_SOURCE_DIR}/run_compute_headless.cmake
)

pelican_define_test(importmanifest_test pelican_project)
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

add_test(
    NAME devcli_dump_lowered_material
    COMMAND ${CMAKE_COMMAND}
        -DCLI=$<TARGET_FILE:pelican_cli>
        -DSURFACE=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/surface_format/valid/wp76.surface
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

add_test(
    NAME gpu_timing_headless_player
    COMMAND ${CMAKE_COMMAND}
        -DPLAYER=$<TARGET_FILE:pelican_player>
        -DPLAYER_DIR=$<TARGET_FILE_DIR:pelican_player>
        -DOUT_DIR=${CMAKE_BINARY_DIR}/test_artifacts/gpu_timing_headless
        -P ${CMAKE_CURRENT_SOURCE_DIR}/run_gpu_timing_headless.cmake
)

add_test(
    NAME frame_plan_dump_headless_player
    COMMAND ${CMAKE_COMMAND}
        -DPLAYER=$<TARGET_FILE:pelican_player>
        -DPLAYER_DIR=$<TARGET_FILE_DIR:pelican_player>
        -DPROJECT_DIR=${CMAKE_SOURCE_DIR}/projects/example
        -DOUT_DIR=${CMAKE_BINARY_DIR}/test_artifacts/frame_plan_dump_headless
        -P ${CMAKE_CURRENT_SOURCE_DIR}/run_frame_plan_dump_headless.cmake
)

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

pelican_define_test(surfaceformat_test pelican_project)
target_compile_definitions(pelican_test_surfaceformat_test PRIVATE
    PELICAN_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

pelican_define_test(framephase_test pelican_core)

pelican_define_test(ui_foundation_test pelican_core)
target_compile_definitions(pelican_test_ui_foundation_test PRIVATE
    PELICAN_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

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
