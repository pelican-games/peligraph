#include "../src/core/shader/shadercompiler.hpp"

#include <catch2/catch_test_macros.hpp>

namespace Pelican {

TEST_CASE("shader compiler injects feature defines into GLSL compilation", "[shader][render-feature]") {
#if PELICAN_RUNTIME_SHADER_COMPILER
    ShaderCompiler compiler;
    const char *source = R"glsl(
#version 450
#ifndef PELICAN_FEATURE_DUMMY
#error PELICAN_FEATURE_DUMMY is required
#endif
void main() {
    gl_Position = vec4(0.0, 0.0, 0.0, 1.0);
}
)glsl";

    const auto without_define =
        compiler.compileSource(source, vk::ShaderStageFlagBits::eVertex, "feature_define_missing.vert");
    REQUIRE_FALSE(without_define.ok);

    ShaderCompileOptions options;
    options.defines = {"PELICAN_FEATURE_DUMMY"};
    const auto with_define =
        compiler.compileSource(source, vk::ShaderStageFlagBits::eVertex, "feature_define_present.vert", options);
    REQUIRE(with_define.ok);
    REQUIRE_FALSE(with_define.spirv.empty());
#else
    SUCCEED("runtime shader compiler disabled");
#endif
}

} // namespace Pelican
