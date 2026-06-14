#include "../src/core/shader/shadercompiler.hpp"
#include "../src/core/shader/shaderreflection.hpp"
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>

namespace Pelican {

namespace {

std::filesystem::path sourceRoot() { return std::filesystem::path{PELICAN_TEST_SOURCE_DIR}; }

const ReflectedBinding *findBinding(const ShaderReflection &reflection, uint32_t set, uint32_t binding) {
    const auto found = std::find_if(reflection.bindings.begin(), reflection.bindings.end(), [&](const auto &item) {
        return item.set == set && item.binding == binding;
    });
    return found == reflection.bindings.end() ? nullptr : &*found;
}

} // namespace

TEST_CASE("shader stage is inferred from GLSL file extensions", "[shader]") {
    REQUIRE(inferShaderStageFromPath("default.vert") == vk::ShaderStageFlagBits::eVertex);
    REQUIRE(inferShaderStageFromPath("default.frag") == vk::ShaderStageFlagBits::eFragment);
    REQUIRE(inferShaderStageFromPath("compute.comp") == vk::ShaderStageFlagBits::eCompute);
    REQUIRE_FALSE(inferShaderStageFromPath("default.spv").has_value());
}

TEST_CASE("shader compiler compiles GLSL and reports failures without throwing", "[shader]") {
    ShaderCompiler compiler;

#if PELICAN_RUNTIME_SHADER_COMPILER
    const auto result = compiler.compileFile(sourceRoot() / "src/core/resources/default.vert");
    REQUIRE(result.ok);
    REQUIRE_FALSE(result.spirv.empty());
    REQUIRE(result.spirv.front() == 0x07230203u);

    const auto bad_result =
        compiler.compileSource("#version 450\nthis is not valid GLSL\n", vk::ShaderStageFlagBits::eFragment,
                               "broken.frag");
    REQUIRE_FALSE(bad_result.ok);
    REQUIRE_FALSE(bad_result.log.empty());
#else
    const auto result = compiler.compileFile(sourceRoot() / "src/core/resources/default.vert");
    REQUIRE_FALSE(result.ok);
    REQUIRE_FALSE(result.log.empty());
#endif
}

TEST_CASE("shader compiler resolves include directories", "[shader]") {
#if PELICAN_RUNTIME_SHADER_COMPILER
    const auto include_dir = std::filesystem::temp_directory_path() / "pelican_shader_include_test";
    std::filesystem::create_directories(include_dir);
    {
        std::ofstream include_file{include_dir / "shared.glsl"};
        include_file << "vec4 sharedColor() { return vec4(1.0, 0.0, 0.0, 1.0); }\n";
    }

    ShaderCompiler compiler;
    compiler.addIncludeDir(include_dir);
    const auto source = R"glsl(
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "shared.glsl"
layout(location = 0) out vec4 outColor;
void main() {
    outColor = sharedColor();
}
)glsl";

    const auto result = compiler.compileSource(source, vk::ShaderStageFlagBits::eFragment, "include_test.frag");
    REQUIRE(result.ok);
    REQUIRE_FALSE(result.spirv.empty());
#endif
}

TEST_CASE("shader reflection reports descriptors, push constants, and vertex inputs", "[shader]") {
#if PELICAN_RUNTIME_SHADER_COMPILER
    ShaderCompiler compiler;
    const auto vert = compiler.compileFile(sourceRoot() / "src/core/resources/default.vert");
    const auto frag = compiler.compileFile(sourceRoot() / "src/core/resources/default.frag");
    REQUIRE(vert.ok);
    REQUIRE(frag.ok);

    const auto vert_reflection = reflect(vert.spirv);
    const auto frag_reflection = reflect(frag.spirv);
    const auto merged = merge(std::array{vert_reflection, frag_reflection});

    const auto *object_buffer = findBinding(merged, 0, 0);
    REQUIRE(object_buffer != nullptr);
    REQUIRE(object_buffer->type == vk::DescriptorType::eStorageBuffer);
    REQUIRE(object_buffer->count == 1);
    REQUIRE(static_cast<bool>(object_buffer->stages & vk::ShaderStageFlagBits::eVertex));

    const auto *base_color = findBinding(merged, 1, 0);
    REQUIRE(base_color != nullptr);
    REQUIRE(base_color->type == vk::DescriptorType::eCombinedImageSampler);
    REQUIRE(static_cast<bool>(base_color->stages & vk::ShaderStageFlagBits::eFragment));

    REQUIRE(merged.push_constant.has_value());
    REQUIRE(merged.push_constant->offset == 0);
    REQUIRE(merged.push_constant->size == 64);
    REQUIRE(static_cast<bool>(merged.push_constant->stageFlags & vk::ShaderStageFlagBits::eVertex));

    REQUIRE(merged.vertex_inputs.size() == 5);
    REQUIRE(merged.vertex_inputs[0].location == 0);
    REQUIRE(merged.vertex_inputs[0].format == vk::Format::eR32G32B32Sfloat);
    REQUIRE(merged.vertex_inputs[2].location == 2);
    REQUIRE(merged.vertex_inputs[2].format == vk::Format::eR32G32Sfloat);

    const auto set0_bindings = makeDescriptorSetLayoutBindings(merged, 0);
    REQUIRE(set0_bindings.size() == 1);
    REQUIRE(set0_bindings[0].binding == 0);
    REQUIRE(set0_bindings[0].descriptorType == vk::DescriptorType::eStorageBuffer);

    const auto set1_bindings = makeDescriptorSetLayoutBindings(merged, 1);
    REQUIRE(set1_bindings.size() == 4);
    REQUIRE(set1_bindings[3].binding == 3);
    REQUIRE(set1_bindings[3].descriptorType == vk::DescriptorType::eCombinedImageSampler);

    const auto push_ranges = makePushConstantRanges(merged);
    REQUIRE(push_ranges.size() == 1);
    REQUIRE(push_ranges[0].size == 64);
#endif
}

} // namespace Pelican
