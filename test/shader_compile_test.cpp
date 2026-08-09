#include <catch2/catch_test_macros.hpp>
#include <spirv_reflect.h>

#if PELICAN_RUNTIME_SHADER_COMPILER
#include <shaderc/shaderc.hpp>
#endif

#include <cstdint>
#include <vector>

namespace Pelican {

TEST_CASE("shader tool dependencies link and can create compiler/reflection objects", "[shader]") {
    SpvReflectShaderModule empty_module{};
    REQUIRE(spvReflectCreateShaderModule(0, nullptr, &empty_module) != SPV_REFLECT_RESULT_SUCCESS);

#if PELICAN_RUNTIME_SHADER_COMPILER
    shaderc::Compiler compiler;
    shaderc::CompileOptions options;
    options.SetTargetEnvironment(shaderc_target_env_vulkan,
                                 PELICAN_SHADERC_TARGET_ENV_VERSION);

    const char *source = R"glsl(
#version 450
void main() {
    gl_Position = vec4(0.0, 0.0, 0.0, 1.0);
}
)glsl";

    auto result = compiler.CompileGlslToSpv(source, shaderc_vertex_shader, "minimal.vert", options);
    REQUIRE(result.GetCompilationStatus() == shaderc_compilation_status_success);

    std::vector<uint32_t> spirv{result.cbegin(), result.cend()};
    REQUIRE_FALSE(spirv.empty());
    REQUIRE(spirv.size() >= 2);
    REQUIRE(spirv[1] == PELICAN_SPIRV_TARGET_VERSION_WORD);

    SpvReflectShaderModule module{};
    REQUIRE(spvReflectCreateShaderModule(spirv.size() * sizeof(uint32_t), spirv.data(), &module) ==
            SPV_REFLECT_RESULT_SUCCESS);
    spvReflectDestroyShaderModule(&module);
#endif
}

} // namespace Pelican
