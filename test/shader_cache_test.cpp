#include <catch2/catch_test_macros.hpp>

#include "../src/core/shader/shadercompiler.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace Pelican {

namespace {
struct TempDirectory {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("pelican_shader_cache_" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));

    TempDirectory() { std::filesystem::create_directories(path); }
    ~TempDirectory() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

constexpr std::string_view vertexSource = R"glsl(#version 450
void main() { gl_Position = vec4(0.0, 0.0, 0.0, 1.0); }
)glsl";
} // namespace

#if PELICAN_RUNTIME_SHADER_COMPILER
TEST_CASE("shader disk cache is byte-identical and recovers from corruption", "[shader][startup][cache]") {
    TempDirectory temp;
    const auto cache = temp.path / "cache";
    ShaderCompiler compiler;
    compiler.setCacheDirectory(cache);

    const auto first = compiler.compileSource(vertexSource, vk::ShaderStageFlagBits::eVertex, "same.vert");
    REQUIRE(first.ok);
    REQUIRE_FALSE(first.cache_hit);
    REQUIRE_FALSE(first.cache_key.empty());

    const auto second = compiler.compileSource(vertexSource, vk::ShaderStageFlagBits::eVertex, "same.vert");
    REQUIRE(second.ok);
    REQUIRE(second.cache_hit);
    REQUIRE(second.cache_key == first.cache_key);
    REQUIRE(second.spirv == first.spirv);

    const auto entry = cache / (first.cache_key + ".spv-cache");
    {
        std::ofstream corrupt{entry, std::ios::binary | std::ios::trunc};
        corrupt << "corrupt";
    }
    ShaderCompiler recovery_compiler;
    recovery_compiler.setCacheDirectory(cache);
    const auto recovered = recovery_compiler.compileSource(vertexSource, vk::ShaderStageFlagBits::eVertex,
                                                           "same.vert");
    REQUIRE(recovered.ok);
    REQUIRE_FALSE(recovered.cache_hit);
    REQUIRE(recovered.spirv == first.spirv);
    REQUIRE(std::filesystem::file_size(entry) > 7);
}

TEST_CASE("shader cache key covers defines and transitive source bytes", "[shader][startup][cache]") {
    TempDirectory temp;
    const auto include = temp.path / "common.glsl";
    {
        std::ofstream file{include};
        file << "const float kValue = 0.0;\n";
    }
    const std::string source = R"glsl(#version 450
#include "common.glsl"
void main() { gl_Position = vec4(kValue, 0.0, 0.0, 1.0); }
)glsl";

    ShaderCompiler compiler;
    compiler.addIncludeDir(temp.path);
    compiler.setCacheDirectory(temp.path / "cache");
    const auto base = compiler.compileSource(source, vk::ShaderStageFlagBits::eVertex, "include.vert");
    REQUIRE(base.ok);

    ShaderCompileOptions defined_options;
    defined_options.defines = {"UNUSED_VARIANT=1"};
    const auto defined = compiler.compileSource(source, vk::ShaderStageFlagBits::eVertex,
                                                "include.vert", defined_options);
    REQUIRE(defined.ok);
    REQUIRE(defined.cache_key != base.cache_key);

    {
        std::ofstream file{include, std::ios::trunc};
        file << "const float kValue = 1.0;\n";
    }
    const auto changed = compiler.compileSource(source, vk::ShaderStageFlagBits::eVertex, "include.vert");
    REQUIRE(changed.ok);
    REQUIRE_FALSE(changed.cache_hit);
    REQUIRE(changed.cache_key != base.cache_key);

    const auto absolute_source = std::string{"#version 450\n#include \""} +
        include.generic_string() +
        "\"\nvoid main() { gl_Position = vec4(kValue, 0.0, 0.0, 1.0); }\n";
    ShaderCompiler absolute_compiler;
    absolute_compiler.setCacheDirectory(temp.path / "absolute-cache");
    const auto absolute_first = absolute_compiler.compileSource(
        absolute_source, vk::ShaderStageFlagBits::eVertex, "absolute.vert");
    REQUIRE(absolute_first.ok);
    {
        std::ofstream file{include, std::ios::trunc};
        file << "const float kValue = 2.0;\n";
    }
    const auto absolute_changed = absolute_compiler.compileSource(
        absolute_source, vk::ShaderStageFlagBits::eVertex, "absolute.vert");
    REQUIRE(absolute_changed.ok);
    REQUIRE_FALSE(absolute_changed.cache_hit);
    REQUIRE(absolute_changed.cache_key != absolute_first.cache_key);
}

TEST_CASE("unwritable shader cache never prevents compilation", "[shader][startup][cache]") {
    TempDirectory temp;
    const auto not_a_directory = temp.path / "regular-file";
    {
        std::ofstream file{not_a_directory};
        file << "occupied";
    }
    ShaderCompiler compiler;
    compiler.setCacheDirectory(not_a_directory);
    const auto result = compiler.compileSource(vertexSource, vk::ShaderStageFlagBits::eVertex,
                                               "fallback.vert");
    REQUIRE(result.ok);
    REQUIRE_FALSE(result.cache_hit);
    REQUIRE_FALSE(result.spirv.empty());
}
#endif

} // namespace Pelican
