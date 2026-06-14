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
target_link_libraries(${target_name} PRIVATE Catch2::Catch2WithMain nlohmann_json ${ARGN})
catch_discover_tests(${target_name})
endfunction()

# register tests
pelican_define_test(hoge_test)
pelican_define_test(renderingpass_helpers_test pelican_core)
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
