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
pelican_define_test(enginetime_test pelican_core)
pelican_define_test(golden_image_test pelican_core VulkanMemoryAllocator VulkanMemoryAllocator-Hpp spirv-reflect-static stb glm)
target_compile_definitions(pelican_test_golden_image_test PRIVATE
    VULKAN_HPP_DISPATCH_LOADER_STATIC_TYPE=::vk::detail::DispatchLoaderStatic
    PELICAN_RUNTIME_SHADER_COMPILER=$<BOOL:${PELICAN_RUNTIME_SHADER_COMPILER}>
    PELICAN_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    PELICAN_TEST_BINARY_DIR="${CMAKE_BINARY_DIR}"
)
if(PELICAN_RUNTIME_SHADER_COMPILER)
    target_link_libraries(pelican_test_golden_image_test PRIVATE pelican_shaderc)
    if(COMMAND pelican_copy_shaderc_runtime)
        pelican_copy_shaderc_runtime(pelican_test_golden_image_test)
    endif()
endif()
pelican_define_test(headless_render_test pelican_core VulkanMemoryAllocator VulkanMemoryAllocator-Hpp)
target_compile_definitions(pelican_test_headless_render_test PRIVATE
    VULKAN_HPP_DISPATCH_LOADER_STATIC_TYPE=::vk::detail::DispatchLoaderStatic
)
pelican_define_test(hoge_test)
pelican_define_test(imageloader_test pelican_core tinyexr)
pelican_define_test(inputstate_test pelican_core)
pelican_define_test(jsonrpc_test pelican_core)
pelican_define_test(pathresolver_test pelican_core)
target_compile_definitions(pelican_test_pathresolver_test PRIVATE
    PELICAN_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
pelican_define_test(projectconfig_test pelican_core glm)
target_compile_definitions(pelican_test_projectconfig_test PRIVATE
    PELICAN_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
pelican_define_test(renderingpass_helpers_test pelican_core)
pelican_define_test(sceneformat_test pelican_core glm)
target_compile_definitions(pelican_test_sceneformat_test PRIVATE
    PELICAN_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
pelican_define_test(seqplayer_test pelican_core glm)
pelican_define_test(vatformat_test pelican_core glm)
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
    NAME compute_buffer_headless_player
    COMMAND ${CMAKE_COMMAND}
        -DPLAYER=$<TARGET_FILE:pelican_player>
        -DPLAYER_DIR=$<TARGET_FILE_DIR:pelican_player>
        -DOUT_DIR=${CMAKE_BINARY_DIR}/test_artifacts/compute_buffer_headless
        -P ${CMAKE_CURRENT_SOURCE_DIR}/run_compute_headless.cmake
)

pelican_define_test(importmanifest_test pelican_core)
target_compile_definitions(pelican_test_importmanifest_test PRIVATE
    PELICAN_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
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
