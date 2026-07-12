#include "../src/core/shader/shadercompiler.hpp"
#include "../src/core/shader/pelican_sets.hpp"
#include "../src/core/shader/shaderreflection.hpp"
#include "../src/core/ui/gpuabi.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
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

    const auto *frame_ubo = findBinding(merged, PELICAN_SET_FRAME, PELICAN_FRAME_UBO_BINDING);
    REQUIRE(frame_ubo != nullptr);
    REQUIRE(frame_ubo->type == vk::DescriptorType::eUniformBuffer);

    const auto *object_buffer = findBinding(merged, PELICAN_SET_FRAME, PELICAN_OBJECT_BUFFER_BINDING);
    REQUIRE(object_buffer != nullptr);
    REQUIRE(object_buffer->type == vk::DescriptorType::eStorageBuffer);
    REQUIRE(object_buffer->count == 1);
    REQUIRE(static_cast<bool>(object_buffer->stages & vk::ShaderStageFlagBits::eVertex));

    const auto *base_color = findBinding(merged, PELICAN_SET_MATERIAL, 0);
    REQUIRE(base_color != nullptr);
    REQUIRE(base_color->type == vk::DescriptorType::eCombinedImageSampler);
    REQUIRE(static_cast<bool>(base_color->stages & vk::ShaderStageFlagBits::eFragment));

    REQUIRE(merged.push_constants.size() == 2);

    REQUIRE(merged.vertex_inputs.size() == 5);
    REQUIRE(merged.vertex_inputs[0].location == 0);
    REQUIRE(merged.vertex_inputs[0].format == vk::Format::eR32G32B32Sfloat);
    REQUIRE(merged.vertex_inputs[2].location == 2);
    REQUIRE(merged.vertex_inputs[2].format == vk::Format::eR32G32Sfloat);

    const auto set0_bindings = makeDescriptorSetLayoutBindings(merged, PELICAN_SET_FRAME);
    REQUIRE(set0_bindings.size() == 3);
    REQUIRE(set0_bindings[0].binding == 0);
    REQUIRE(set0_bindings[0].descriptorType == vk::DescriptorType::eUniformBuffer);
    REQUIRE(set0_bindings[1].binding == 1);
    REQUIRE(set0_bindings[1].descriptorType == vk::DescriptorType::eStorageBuffer);

    const auto set2_bindings = makeDescriptorSetLayoutBindings(merged, PELICAN_SET_MATERIAL);
    REQUIRE(set2_bindings.size() == 5);
    REQUIRE(set2_bindings.back().binding == PELICAN_MATERIAL_BUFFER_BINDING);
    REQUIRE(set2_bindings.back().descriptorType == vk::DescriptorType::eStorageBuffer);

    const auto push_ranges = makePushConstantRanges(merged);
    REQUIRE(push_ranges.size() == 1);
    REQUIRE(push_ranges[0].offset == 0);
    REQUIRE(push_ranges[0].size == PELICAN_PUSH_ENGINE_BYTES + sizeof(uint32_t));
#endif
}

TEST_CASE("UI shader reflection and QuadVertex pipeline layout preserve the 20-byte ABI", "[shader][ui][u1]") {
#if PELICAN_RUNTIME_SHADER_COMPILER
    ShaderCompiler compiler;
    compiler.addIncludeDir(sourceRoot() / "src/core/resources/shaders/include");
    const auto compiled = compiler.compileFile(sourceRoot() / "src/core/resources/ui.vert");
    REQUIRE(compiled.ok);
    const auto reflection = reflect(compiled.spirv);
    REQUIRE(reflection.vertex_inputs.size() == 3);
    REQUIRE(reflection.vertex_inputs[0].location == 0);
    REQUIRE(reflection.vertex_inputs[0].format == vk::Format::eR32G32Sfloat);
    REQUIRE(reflection.vertex_inputs[1].location == 1);
    REQUIRE(reflection.vertex_inputs[1].format == vk::Format::eR32G32Sfloat);
    REQUIRE(reflection.vertex_inputs[2].location == 2);
    REQUIRE(reflection.vertex_inputs[2].format == vk::Format::eR32G32B32A32Sfloat);

    const auto layout = ui::quadVertexLayout();
    REQUIRE(layout.binding.stride == 20);
    REQUIRE(layout.attributes[0].offset == 0);
    REQUIRE(layout.attributes[0].format == vk::Format::eR32G32Sfloat);
    REQUIRE(layout.attributes[1].offset == 8);
    REQUIRE(layout.attributes[1].format == vk::Format::eR32G32Sfloat);
    REQUIRE(layout.attributes[2].offset == 16);
    REQUIRE(layout.attributes[2].format == vk::Format::eR8G8B8A8Unorm);
#endif
}

TEST_CASE("push constant reflection enforces engine and shader regions", "[shader]") {
#if PELICAN_RUNTIME_SHADER_COMPILER
    ShaderCompiler compiler;
    const auto invalid_engine = compiler.compileSource(R"glsl(
#version 450
layout(push_constant) uniform InvalidEngine { vec4 value; } pc;
void main() { gl_Position = pc.value; }
)glsl", vk::ShaderStageFlagBits::eVertex, "invalid_engine.vert");
    REQUIRE(invalid_engine.ok);
    REQUIRE_THROWS_WITH(validatePushConstantContract(reflect(invalid_engine.spirv)),
                        Catch::Matchers::ContainsSubstring("leading 64-byte MVP"));

    const auto valid_shader = compiler.compileSource(R"glsl(
#version 450
layout(push_constant) uniform ShaderData { layout(offset = 64) vec4 value; } pc;
layout(location = 0) out vec4 outColor;
void main() { outColor = pc.value; }
)glsl", vk::ShaderStageFlagBits::eFragment, "valid_shader.frag");
    REQUIRE(valid_shader.ok);
    REQUIRE_NOTHROW(validatePushConstantContract(reflect(valid_shader.spirv)));

    ShaderReflection asymmetric;
    asymmetric.push_constants = {
        vk::PushConstantRange{vk::ShaderStageFlagBits::eVertex, 0, 80},
        vk::PushConstantRange{vk::ShaderStageFlagBits::eFragment, 64, 16},
    };
    const auto asymmetric_ranges = makePushConstantRanges(asymmetric);
    REQUIRE(asymmetric_ranges.size() == 2);
    REQUIRE_FALSE(asymmetric_ranges[0].stageFlags & asymmetric_ranges[1].stageFlags);
#endif
}

} // namespace Pelican
