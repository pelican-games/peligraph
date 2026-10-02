#include "../src/core/shader/shadercompiler.hpp"
#include "../src/core/loader/engineresources.hpp"
#include "../src/core/shader/pelican_sets.hpp"
#include "../src/core/shader/shaderreflection.hpp"
#include "../src/core/shader/shaderresourceinterface.hpp"
#include "../src/core/shader/pipelinefactory.hpp"
#include "../src/core/ui/gpuabi.hpp"
#include "../src/project/targetrenderplanning.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>

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

TEST_CASE("embedded and runtime shaders share the configured SPIR-V version",
          "[shader][target-environment]") {
    const auto embedded = engineResource("default.vert.spv");
    REQUIRE(embedded.has_value());
    REQUIRE(embedded->size() >= 2 * sizeof(std::uint32_t));

    std::uint32_t embedded_magic = 0;
    std::uint32_t embedded_version = 0;
    std::memcpy(&embedded_magic, embedded->data(), sizeof(embedded_magic));
    std::memcpy(&embedded_version, embedded->data() + sizeof(embedded_magic),
                sizeof(embedded_version));
    REQUIRE(embedded_magic == 0x07230203u);
    REQUIRE(embedded_version == PELICAN_SPIRV_TARGET_VERSION_WORD);

#if PELICAN_RUNTIME_SHADER_COMPILER
    ShaderCompiler compiler;
    const auto runtime =
        compiler.compileFile(sourceRoot() / "src/core/resources/default.vert");
    INFO(runtime.log);
    REQUIRE(runtime.ok);
    REQUIRE(runtime.spirv.size() >= 2);
    REQUIRE(runtime.spirv[1] == embedded_version);
#endif
}

TEST_CASE(
    "rt shadow shader reflects TLAS in frame set and keeps fullscreen input in set one",
    "[shader][wp283][ray-query]") {
    const auto embedded =
        engineResource("rt_shadow_mask.frag.spv");
    REQUIRE(embedded.has_value());
    REQUIRE(embedded->size() % sizeof(std::uint32_t) == 0);
    std::vector<std::uint32_t> words(
        embedded->size() / sizeof(std::uint32_t));
    std::memcpy(
        words.data(), embedded->data(), embedded->size());
    const auto reflection = reflect(words);
    const auto *tlas = findBinding(
        reflection, PELICAN_SET_FRAME,
        PELICAN_RAY_QUERY_TLAS_BINDING);
    REQUIRE(tlas != nullptr);
    REQUIRE(tlas->type ==
            vk::DescriptorType::eAccelerationStructureKHR);
    REQUIRE(tlas->count == 1);
    REQUIRE(shaderReflectionUsesRayQueryFrameSet(
        reflection));
    const auto *world_position = findBinding(
        reflection, PELICAN_SET_PASS_INPUT, 0);
    REQUIRE(world_position != nullptr);
    REQUIRE(world_position->type ==
            vk::DescriptorType::eCombinedImageSampler);
    const auto *normal = findBinding(
        reflection, PELICAN_SET_PASS_INPUT, 1);
    REQUIRE(normal != nullptr);
    REQUIRE(normal->type ==
            vk::DescriptorType::eCombinedImageSampler);
    REQUIRE(std::count_if(
                reflection.bindings.begin(),
                reflection.bindings.end(),
                [](const auto &binding) {
                    return binding.set ==
                           PELICAN_SET_PASS_INPUT;
                }) == 2);

#if PELICAN_RUNTIME_SHADER_COMPILER
    ShaderCompiler compiler;
    compiler.addIncludeDir(
        sourceRoot() /
        "src/core/resources/shaders/include");
    const auto runtime = compiler.compileFile(
        sourceRoot() /
        "src/core/resources/rt_shadow_mask.frag");
    INFO(runtime.log);
    REQUIRE(runtime.ok);
    const auto runtime_reflection = reflect(runtime.spirv);
    const auto *runtime_tlas = findBinding(
        runtime_reflection, PELICAN_SET_FRAME,
        PELICAN_RAY_QUERY_TLAS_BINDING);
    REQUIRE(runtime_tlas != nullptr);
    REQUIRE(runtime_tlas->type == tlas->type);
#endif
}

TEST_CASE(
    "non-ray frame descriptor layout retains the six-binding ABI",
    "[shader][wp283][ray-query]") {
    const auto base =
        frameDescriptorSetLayoutBindings(false);
    REQUIRE(base.size() == 6);
    REQUIRE(base[0].binding == PELICAN_FRAME_UBO_BINDING);
    REQUIRE(base[0].descriptorType ==
            vk::DescriptorType::eUniformBuffer);
    REQUIRE(base[1].binding == PELICAN_OBJECT_BUFFER_BINDING);
    REQUIRE(base[1].descriptorType ==
            vk::DescriptorType::eStorageBuffer);
    REQUIRE(base[2].binding == PELICAN_LIGHT_UBO_BINDING);
    REQUIRE(base[2].descriptorType ==
            vk::DescriptorType::eUniformBuffer);
    REQUIRE(base[3].binding ==
            PELICAN_PREVIOUS_OBJECT_BUFFER_BINDING);
    REQUIRE(base[3].descriptorType ==
            vk::DescriptorType::eStorageBuffer);
    REQUIRE(base[4].binding ==
            PELICAN_FRAME_RESOLUTION_UBO_BINDING);
    REQUIRE(base[4].descriptorType ==
            vk::DescriptorType::eUniformBuffer);
    REQUIRE(base[5].binding ==
            PELICAN_DIRECTIONAL_SHADOW_DATA_BINDING);
    REQUIRE(base[5].descriptorType ==
            vk::DescriptorType::eStorageBuffer);

    const auto extended =
        frameDescriptorSetLayoutBindings(true);
    REQUIRE(extended.size() == 7);
    REQUIRE(std::equal(
        base.begin(), base.end(), extended.begin(),
        [](const auto &left, const auto &right) {
            return left.binding == right.binding &&
                   left.descriptorType == right.descriptorType &&
                   left.descriptorCount == right.descriptorCount &&
                   left.stageFlags == right.stageFlags;
        }));
    REQUIRE(extended.back().binding ==
            PELICAN_RAY_QUERY_TLAS_BINDING);
    REQUIRE(extended.back().descriptorType ==
            vk::DescriptorType::eAccelerationStructureKHR);

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

    const auto *previous_object_buffer =
        findBinding(merged, PELICAN_SET_FRAME, PELICAN_PREVIOUS_OBJECT_BUFFER_BINDING);
    REQUIRE(previous_object_buffer != nullptr);
    REQUIRE(previous_object_buffer->type == vk::DescriptorType::eStorageBuffer);
    REQUIRE(static_cast<bool>(previous_object_buffer->stages & vk::ShaderStageFlagBits::eVertex));

    const auto *base_color = findBinding(merged, PELICAN_SET_MATERIAL, 0);
    REQUIRE(base_color != nullptr);
    REQUIRE(base_color->type == vk::DescriptorType::eCombinedImageSampler);
    REQUIRE(static_cast<bool>(base_color->stages & vk::ShaderStageFlagBits::eFragment));

    const auto *occlusion = findBinding(
        merged, PELICAN_SET_MATERIAL,
        PELICAN_MATERIAL_OCCLUSION_TEXTURE_BINDING);
    REQUIRE(occlusion != nullptr);
    REQUIRE(occlusion->type == vk::DescriptorType::eCombinedImageSampler);
    REQUIRE(occlusion->stages == vk::ShaderStageFlagBits::eFragment);

    const auto *material_instance = findBinding(
        merged, PELICAN_SET_FREE, PELICAN_MATERIAL_INSTANCE_OVERRIDE_BINDING);
    REQUIRE(material_instance != nullptr);
    REQUIRE(material_instance->type == vk::DescriptorType::eStorageBuffer);
    REQUIRE(material_instance->stages == vk::ShaderStageFlagBits::eFragment);

    const auto *material_absolute_headers = findBinding(
        merged, PELICAN_SET_FREE,
        PELICAN_MATERIAL_INSTANCE_ABSOLUTE_HEADER_BINDING);
    REQUIRE(material_absolute_headers != nullptr);
    REQUIRE(material_absolute_headers->type == vk::DescriptorType::eStorageBuffer);
    REQUIRE(material_absolute_headers->stages ==
            (vk::ShaderStageFlagBits::eVertex |
             vk::ShaderStageFlagBits::eFragment));

    const auto *material_absolute_records = findBinding(
        merged, PELICAN_SET_FREE,
        PELICAN_MATERIAL_INSTANCE_ABSOLUTE_RECORD_BINDING);
    REQUIRE(material_absolute_records != nullptr);
    REQUIRE(material_absolute_records->type == vk::DescriptorType::eStorageBuffer);
    REQUIRE(material_absolute_records->stages ==
            (vk::ShaderStageFlagBits::eVertex |
             vk::ShaderStageFlagBits::eFragment));

    REQUIRE(merged.push_constants.size() == 2);

    REQUIRE(merged.vertex_inputs.size() == 5);
    REQUIRE(merged.vertex_inputs[0].location == 0);
    REQUIRE(merged.vertex_inputs[0].format == vk::Format::eR32G32B32Sfloat);
    REQUIRE(merged.vertex_inputs[2].location == 2);
    REQUIRE(merged.vertex_inputs[2].format == vk::Format::eR32G32Sfloat);

    const auto set0_bindings = makeDescriptorSetLayoutBindings(merged, PELICAN_SET_FRAME);
    REQUIRE(set0_bindings.size() == 5);
    REQUIRE(set0_bindings[0].binding == 0);
    REQUIRE(set0_bindings[0].descriptorType == vk::DescriptorType::eUniformBuffer);
    REQUIRE(set0_bindings[1].binding == 1);
    REQUIRE(set0_bindings[1].descriptorType == vk::DescriptorType::eStorageBuffer);
    REQUIRE(set0_bindings[3].binding == PELICAN_PREVIOUS_OBJECT_BUFFER_BINDING);
    REQUIRE(set0_bindings[3].descriptorType == vk::DescriptorType::eStorageBuffer);
    REQUIRE(
        set0_bindings[4].binding ==
        PELICAN_FRAME_RESOLUTION_UBO_BINDING);
    REQUIRE(
        set0_bindings[4].descriptorType ==
        vk::DescriptorType::eUniformBuffer);

    const auto set2_bindings = makeDescriptorSetLayoutBindings(merged, PELICAN_SET_MATERIAL);
    REQUIRE(set2_bindings.size() == 6);
    REQUIRE(set2_bindings[4].binding == PELICAN_MATERIAL_BUFFER_BINDING);
    REQUIRE(set2_bindings[4].descriptorType == vk::DescriptorType::eStorageBuffer);
    REQUIRE(
        set2_bindings.back().binding ==
        PELICAN_MATERIAL_OCCLUSION_TEXTURE_BINDING);
    REQUIRE(
        set2_bindings.back().descriptorType ==
        vk::DescriptorType::eCombinedImageSampler);

    const auto push_ranges = makePushConstantRanges(merged);
    REQUIRE(push_ranges.size() == 1);
    REQUIRE(push_ranges[0].offset == 0);
    REQUIRE(push_ranges[0].size ==
            PELICAN_PUSH_ENGINE_BYTES + sizeof(uint32_t) * 2);
#endif
}

TEST_CASE(
    "generated shader resource ports preserve descriptor names and image "
    "view dimensions",
    "[shader][reflection][resource-port][wp207a]") {
#if PELICAN_RUNTIME_SHADER_COMPILER
    const std::array bindings{
        ShaderResourceInterfaceBinding{
            .port =
                ShaderResourcePortDefinition{
                    .name = "scene_color",
                    .resource = "scene",
                    .access =
                        ShaderResourcePortAccess::
                            sampled,
                },
            .binding = 0,
            .descriptor =
                ShaderResourceDescriptorKind::
                    combined_image_sampler,
            .image_view_dimension =
                ReflectedImageViewDimension::two_d,
        },
        ShaderResourceInterfaceBinding{
            .port =
                ShaderResourcePortDefinition{
                    .name = "result",
                    .resource = "output",
                    .access =
                        ShaderResourcePortAccess::
                            storage,
                    .view =
                        ShaderResourcePortView::
                            per_view,
                    .subresource =
                        ImageSubresourceRange{
                            .base_mip_level = 3,
                            .level_count = 1,
                            .base_array_layer = 1,
                            .layer_count = 1,
                        },
                },
            .binding = 1,
            .descriptor =
                ShaderResourceDescriptorKind::
                    storage_image,
            .image_view_dimension =
                ReflectedImageViewDimension::
                    two_d_array,
            .storage_format =
                vk::Format::eR8G8B8A8Unorm,
            .readable = false,
            .writable = true,
        },
        ShaderResourceInterfaceBinding{
            .port =
                ShaderResourcePortDefinition{
                    .name = "scene_layers",
                    .resource = "layers",
                    .access =
                        ShaderResourcePortAccess::
                            sampled,
                    .view =
                        ShaderResourcePortView::
                            per_view,
                },
            .binding = 2,
            .descriptor =
                ShaderResourceDescriptorKind::
                    combined_image_sampler,
            .image_view_dimension =
                ReflectedImageViewDimension::
                    two_d_array,
        },
        ShaderResourceInterfaceBinding{
            .port =
                ShaderResourcePortDefinition{
                    .name = "environment",
                    .resource = "environment_probe",
                    .access =
                        ShaderResourcePortAccess::
                            sampled,
                    .view =
                        ShaderResourcePortView::cube,
                },
            .binding = 3,
            .descriptor =
                ShaderResourceDescriptorKind::
                    combined_image_sampler,
            .image_view_dimension =
                ReflectedImageViewDimension::cube,
        },
    };
    ShaderCompiler compiler;
    compiler.addIncludeDir(
        sourceRoot() /
        "src/core/resources/shaders/include");
    ShaderCompileOptions options;
    const auto generated_ports =
        makeShaderResourcePortVirtualInclude(
            bindings);
    REQUIRE(
        generated_ports.second.find(
            "pelican_base_mip_result() { return 3u; }") !=
        std::string::npos);
    REQUIRE(
        generated_ports.second.find(
            "pelican_base_layer_result() { return 1u; }") !=
        std::string::npos);
    REQUIRE(
        generated_ports.second.find(
            "uniform samplerCube "
            "pelican_resource_environment") !=
        std::string::npos);
    options.virtual_includes.push_back(
        generated_ports);
    const auto compiled =
        compiler.compileSource(
            R"glsl(
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "pelican_resource_ports.glsl"
layout(local_size_x = 1, local_size_y = 1) in;
void main() {
    vec4 value = pelican_sample_lod_scene_color(
        vec2(0.5),
        float(pelican_mip_count_scene_color() - 1u));
    value += pelican_sample_lod_scene_color(
        vec2(0.5), 0u, 0.0) *
        float(pelican_view_count_scene_color()) * 0.001;
    value += vec4(pelican_size_lod_scene_color(0), 0, 0) * 0.0;
    value += pelican_sample_lod_scene_layers(
        vec2(0.5), 0u, 0.0) * 0.001;
    value += pelican_sample_lod_environment(
        normalize(vec3(1.0, 0.5, 0.25)), 0.0) *
        float(pelican_view_count_environment()) * 0.001;
    if (pelican_base_mip_result() != 3u ||
        pelican_base_layer_result() != 1u) {
        return;
    }
    pelican_store_result(ivec2(0), 0u, value);
}
)glsl",
            vk::ShaderStageFlagBits::eCompute,
            "typed_resource_ports.comp", options);
    INFO(compiled.log);
    REQUIRE(compiled.ok);
    const auto reflection =
        reflect(compiled.spirv);
    REQUIRE_NOTHROW(
        validateShaderResourceInterfaceReflection(
            bindings, reflection));

    const auto *sampled =
        findBinding(
            reflection, PELICAN_SET_PASS_INPUT, 0);
    const auto *storage =
        findBinding(
            reflection, PELICAN_SET_PASS_INPUT, 1);
    const auto *sampled_array =
        findBinding(
            reflection, PELICAN_SET_PASS_INPUT, 2);
    const auto *sampled_cube =
        findBinding(
            reflection, PELICAN_SET_PASS_INPUT, 3);
    REQUIRE(sampled != nullptr);
    REQUIRE(
        sampled->image_view_dimension ==
        ReflectedImageViewDimension::two_d);
    REQUIRE(storage != nullptr);
    REQUIRE(
        storage->image_view_dimension ==
        ReflectedImageViewDimension::
            two_d_array);
    REQUIRE(sampled_array != nullptr);
    REQUIRE(
        sampled_array->image_view_dimension ==
        ReflectedImageViewDimension::
            two_d_array);
    REQUIRE(sampled_cube != nullptr);
    REQUIRE(
        sampled_cube->image_view_dimension ==
        ReflectedImageViewDimension::cube);

    auto wrong_dimension = reflection;
    wrong_dimension.bindings.at(1)
        .image_view_dimension =
        ReflectedImageViewDimension::two_d;
    REQUIRE_THROWS_WITH(
        validateShaderResourceInterfaceReflection(
            bindings, wrong_dimension),
        Catch::Matchers::ContainsSubstring(
            "resource 'output'"));

    auto wrong_descriptor = reflection;
    wrong_descriptor.bindings.at(0).type =
        vk::DescriptorType::eStorageImage;
    REQUIRE_THROWS_WITH(
        validateShaderResourceInterfaceReflection(
            bindings, wrong_descriptor),
        Catch::Matchers::ContainsSubstring(
            "resource 'scene'"));

    auto wrong_name = reflection;
    wrong_name.bindings.at(0).name =
        "pelican_resource_wrong";
    REQUIRE_THROWS_WITH(
        validateShaderResourceInterfaceReflection(
            bindings, wrong_name),
        Catch::Matchers::ContainsSubstring(
            "resource 'scene'"));
#endif
}

TEST_CASE(
    "RS-5 resource ports bind where a precompiled shader's reflection names "
    "them, whatever order they were listed in",
    "[shader][reflection][resource-port][rs5]") {
    // engine://rt_shadow_mask_pipeline was built with world_position at 0,
    // normal at 1 and shadow_mask at 2. A graph lists its reads by resource
    // name, so normal comes first and is numbered 0.
    ShaderReflection reflection;
    const auto reflected = [&](std::uint32_t binding,
                               vk::DescriptorType type,
                               std::string name) {
        reflection.bindings.push_back(ReflectedBinding{
            .set = PELICAN_SET_PASS_INPUT,
            .binding = binding,
            .type = type,
            .count = 1,
            .stages = vk::ShaderStageFlagBits::eRaygenKHR,
            .name = std::move(name),
            .image_view_dimension =
                ReflectedImageViewDimension::two_d,
        });
    };
    reflected(0, vk::DescriptorType::eCombinedImageSampler,
              "pelican_resource_world_position");
    reflected(1, vk::DescriptorType::eCombinedImageSampler,
              "pelican_resource_normal");
    reflected(2, vk::DescriptorType::eStorageImage,
              "pelican_resource_shadow_mask");
    const auto port = [](std::string name, std::string resource,
                         std::uint32_t binding, bool sampled) {
        return ShaderResourceInterfaceBinding{
            .port = ShaderResourcePortDefinition{
                .name = std::move(name),
                .resource = std::move(resource),
                .access = sampled
                              ? ShaderResourcePortAccess::sampled
                              : ShaderResourcePortAccess::storage,
            },
            .binding = binding,
            .descriptor =
                sampled ? ShaderResourceDescriptorKind::
                              combined_image_sampler
                        : ShaderResourceDescriptorKind::
                              storage_image,
            .image_view_dimension =
                ReflectedImageViewDimension::two_d,
            .readable = sampled,
            .writable = !sampled,
        };
    };
    std::vector bindings{
        port("normal", "gb__normal", 0, true),
        port("world_position", "gb__worldpos", 1, true),
        port("shadow_mask", "mask", 2, false),
    };
    REQUIRE_THROWS_WITH(
        validateShaderResourceInterfaceReflection(
            bindings, reflection),
        Catch::Matchers::ContainsSubstring(
            "reflected unexpected descriptor "
            "'pelican_resource_world_position' at binding 0"));

    bindShaderResourceInterfaceByReflectedName(
        bindings, reflection);
    CHECK(bindings.at(0).binding == 1);
    CHECK(bindings.at(1).binding == 0);
    CHECK(bindings.at(2).binding == 2);
    REQUIRE_NOTHROW(
        validateShaderResourceInterfaceReflection(
            bindings, reflection));

    // The type, count and view checks stay where they were.
    auto storage_normal = reflection;
    storage_normal.bindings.at(1).type =
        vk::DescriptorType::eStorageImage;
    REQUIRE_THROWS_WITH(
        validateShaderResourceInterfaceReflection(
            bindings, storage_normal),
        Catch::Matchers::ContainsSubstring(
            "resource 'gb__normal'"));

    // A port the shader does not name keeps its place and is refused as
    // absent by the check the pipeline runs.
    std::vector missing{
        port("normals", "gb__normal", 3, true)};
    bindShaderResourceInterfaceByReflectedName(
        missing, reflection);
    CHECK(missing.at(0).binding == 3);
    REQUIRE_THROWS_WITH(
        validateShaderResourceInterfaceReflection(
            missing, reflection),
        Catch::Matchers::ContainsSubstring(
            "Shader resource port 'normals' (resource "
            "'gb__normal') is absent from shader reflection"));
}

TEST_CASE(
    "resource port view resolver separates shared consumer and producer-family "
    "contracts",
    "[shader][resource-port][multiview][wp207a]") {
    ShaderResourcePortDefinition shared{
        .name = "shared",
        .resource = "shadow",
    };
    ShaderResourcePortDefinition per_view{
        .name = "eyes",
        .resource = "eye_color",
        .view = ShaderResourcePortView::per_view,
    };
    ShaderResourcePortDefinition family_array{
        .name = "cascades",
        .resource = "shadow",
        .view =
            ShaderResourcePortView::family_array,
    };
    ShaderResourcePortDefinition cube{
        .name = "environment",
        .resource = "environment_probe",
        .view = ShaderResourcePortView::cube,
    };
    REQUIRE(
        resolveShaderResourceImageViewDimension(
            shared,
            VulkanResourceViewLayout::shared_2d,
            ShaderResourceConsumerView::
                graphics_sequential) ==
        ReflectedImageViewDimension::two_d);
    REQUIRE(
        resolveShaderResourceImageViewDimension(
            per_view,
            VulkanResourceViewLayout::
                shared_2d,
            ShaderResourceConsumerView::
                compute_per_view) ==
        ReflectedImageViewDimension::two_d);
    REQUIRE(
        resolveShaderResourceImageViewDimension(
            per_view,
            VulkanResourceViewLayout::
                sequential_2d,
            ShaderResourceConsumerView::
                compute_per_view) ==
        ReflectedImageViewDimension::two_d);
    REQUIRE(
        resolveShaderResourceImageViewDimension(
            per_view,
            VulkanResourceViewLayout::
                layered_2d_array,
            ShaderResourceConsumerView::
                compute_per_view) ==
        ReflectedImageViewDimension::
            two_d_array);
    REQUIRE(
        resolveShaderResourceImageViewDimension(
            per_view,
            VulkanResourceViewLayout::
                sequential_2d,
            ShaderResourceConsumerView::
                graphics_sequential) ==
        ReflectedImageViewDimension::two_d);
    REQUIRE(
        resolveShaderResourceImageViewDimension(
            per_view,
            VulkanResourceViewLayout::
                layered_2d_array,
            ShaderResourceConsumerView::
                graphics_multiview) ==
        ReflectedImageViewDimension::
            two_d_array);
    REQUIRE(
        resolveShaderResourceImageViewDimension(
            per_view,
            VulkanResourceViewLayout::
                sequential_2d,
            ShaderResourceConsumerView::
                compute_once) ==
        ReflectedImageViewDimension::
            two_d_array);
    REQUIRE_THROWS_WITH(
        resolveShaderResourceImageViewDimension(
            shared,
            VulkanResourceViewLayout::
                layered_2d_array,
            ShaderResourceConsumerView::
                graphics_multiview),
        Catch::Matchers::ContainsSubstring(
            "resource 'shadow'"));
    REQUIRE(
        resolveShaderResourceImageViewDimension(
            family_array,
            VulkanResourceViewLayout::
                family_2d_array,
            ShaderResourceConsumerView::
                graphics_multiview) ==
        ReflectedImageViewDimension::
            two_d_array);
    REQUIRE_THROWS_WITH(
        resolveShaderResourceImageViewDimension(
            per_view,
            VulkanResourceViewLayout::
                family_2d_array,
            ShaderResourceConsumerView::
                graphics_sequential),
        Catch::Matchers::ContainsSubstring(
            "must declare family_array"));
    REQUIRE_THROWS_WITH(
        resolveShaderResourceImageViewDimension(
            family_array,
            VulkanResourceViewLayout::
                layered_2d_array,
            ShaderResourceConsumerView::
                graphics_multiview),
        Catch::Matchers::ContainsSubstring(
            "requires family_array"));
    REQUIRE(
        resolveShaderResourceImageViewDimension(
            cube,
            VulkanResourceViewLayout::shared_2d,
            ShaderResourceConsumerView::
                compute_once,
            ImageResourceDimension::cube) ==
        ReflectedImageViewDimension::cube);
    REQUIRE_THROWS_WITH(
        resolveShaderResourceImageViewDimension(
            cube,
            VulkanResourceViewLayout::shared_2d,
            ShaderResourceConsumerView::
                compute_once),
        Catch::Matchers::ContainsSubstring(
            "requires a cube image resource"));
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

// LC-1: pelican_loop.glsl declares the element and count at [112, 120), in
// its own push-constant block or (PELICAN_LOOP_CUSTOM_PUSH) in the shader's;
// the engine pushes them to the stages whose range holds them, and refuses a
// range that covers only part of them.
TEST_CASE("LC-1 pelican_loop.glsl declares the loop index at 112 in its own block or the shader's",
          "[shader][lc1]") {
#if PELICAN_RUNTIME_SHADER_COMPILER
    REQUIRE(engineResource("shaders/include/pelican_loop.glsl").has_value());
    ShaderCompiler compiler;
    compiler.addIncludeDir(sourceRoot() / "src/core/resources/shaders/include");
    const auto compile = [&](const char *source, vk::ShaderStageFlagBits stage, const char *name) {
        const auto compiled = compiler.compileSource(source, stage, name);
        INFO(compiled.log);
        REQUIRE(compiled.ok);
        return reflect(compiled.spirv);
    };
    const auto vertex = compile(R"glsl(
#version 450
#extension GL_GOOGLE_include_directive : require
#include "pelican_loop.glsl"
layout(location = 0) flat out uvec2 outLoop;
void main() {
    outLoop = uvec2(pelican_loop_element(), pelican_loop_count());
    gl_Position = vec4(0.0, 0.0, 0.0, 1.0);
}
)glsl", vk::ShaderStageFlagBits::eVertex, "lc1_loop.vert");
    const auto fragment = compile(R"glsl(
#version 450
#extension GL_GOOGLE_include_directive : require
#include "pelican_loop.glsl"
layout(location = 0) out vec4 outColor;
void main() { outColor = vec4(float(pelican_loop_element()), float(pelican_loop_count()), 0.0, 1.0); }
)glsl", vk::ShaderStageFlagBits::eFragment, "lc1_loop.frag");

    // The include's own block: [112, 120) in the stage that includes it.
    REQUIRE(fragment.push_constants.size() == 1);
    CHECK(fragment.push_constants[0].offset == PELICAN_PUSH_LOOP_OFFSET);
    CHECK(fragment.push_constants[0].size == PELICAN_PUSH_LOOP_BYTES);
    CHECK(loopIndexPushStages(fragment) == vk::ShaderStageFlagBits::eFragment);
    CHECK(readsLoopIndex(fragment));
    REQUIRE_NOTHROW(validatePushConstantContract(fragment));
    REQUIRE_NOTHROW(validateLoopIndexPushRange(fragment, "lc1_loop.frag"));

    // Both stages of a raster pass include it: one push reaches both.
    const std::array stages{vertex, fragment};
    const auto both = merge(stages);
    CHECK(loopIndexPushStages(both) ==
          (vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment));
    const auto both_ranges = makePushConstantRanges(both);
    REQUIRE(both_ranges.size() == 1);
    CHECK(both_ranges[0].stageFlags ==
          (vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment));
    CHECK(both_ranges[0].offset == PELICAN_PUSH_LOOP_OFFSET);
    CHECK(both_ranges[0].size == PELICAN_PUSH_LOOP_BYTES);

    // PELICAN_LOOP_CUSTOM_PUSH: the fields in the shader's own block, next to
    // a projection_view; the functions read them through the block's name.
    const auto custom = compile(R"glsl(
#version 450
#extension GL_GOOGLE_include_directive : require
#define PELICAN_LOOP_CUSTOM_PUSH
#include "pelican_loop.glsl"
layout(push_constant) uniform Push {
    mat4 projection_view;
    vec4 camera_position;
    PELICAN_LOOP_PUSH_FIELDS
} push;
PELICAN_LOOP_FUNCTIONS(push)
layout(location = 0) out vec4 outColor;
void main() {
    outColor = push.projection_view * push.camera_position +
               vec4(float(pelican_loop_element()), float(pelican_loop_count()), 0.0, 0.0);
}
)glsl", vk::ShaderStageFlagBits::eFragment, "lc1_custom.frag");
    REQUIRE(custom.push_constants.size() == 1);
    CHECK(custom.push_constants[0].offset == 0);
    CHECK(custom.push_constants[0].size == PELICAN_PUSH_LOOP_OFFSET + PELICAN_PUSH_LOOP_BYTES);
    CHECK(loopIndexPushStages(custom) == vk::ShaderStageFlagBits::eFragment);
    REQUIRE_NOTHROW(validatePushConstantContract(custom));
    REQUIRE_NOTHROW(validateLoopIndexPushRange(custom, "lc1_custom.frag"));
    // With the default block in the vertex stage: two ranges, and the push to
    // both stages covers [112, 120) in each.
    const std::array mixed_stages{vertex, custom};
    const auto mixed = merge(mixed_stages);
    CHECK(loopIndexPushStages(mixed) ==
          (vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment));
    const auto mixed_ranges = makePushConstantRanges(mixed);
    REQUIRE(mixed_ranges.size() == 2);
    for (const auto &range : mixed_ranges) {
        CHECK(range.offset <= PELICAN_PUSH_LOOP_OFFSET);
        CHECK(range.offset + range.size >= PELICAN_PUSH_LOOP_OFFSET + PELICAN_PUSH_LOOP_BYTES);
    }

    // A shader without it: nothing to push.
    const auto plain = compile(R"glsl(
#version 450
layout(push_constant) uniform ShaderData { layout(offset = 64) vec4 value; } pc;
layout(location = 0) out vec4 outColor;
void main() { outColor = pc.value; }
)glsl", vk::ShaderStageFlagBits::eFragment, "lc1_plain.frag");
    CHECK_FALSE(loopIndexPushStages(plain));
    CHECK_FALSE(readsLoopIndex(plain));
    REQUIRE_NOTHROW(validateLoopIndexPushRange(plain, "lc1_plain.frag"));

    // A range that covers part of [112, 120) is refused by name.
    const auto overlap = compile(R"glsl(
#version 450
layout(push_constant) uniform ShaderData { layout(offset = 104) uvec2 value; uint more; } pc;
layout(location = 0) out vec4 outColor;
void main() { outColor = vec4(pc.value, pc.more, 1.0); }
)glsl", vk::ShaderStageFlagBits::eFragment, "lc1_overlap.frag");
    CHECK_FALSE(loopIndexPushStages(overlap));
    std::string message;
    try {
        validateLoopIndexPushRange(overlap, "project://shaders/lc1_overlap.frag");
    } catch (const std::runtime_error &error) {
        message = error.what();
    }
    const std::string prefix = std::string{shaderLoopPushOverlapCode} + ": ";
    REQUIRE(message.starts_with(prefix));
    CHECK(nlohmann::json::parse(message.substr(prefix.size())) ==
          nlohmann::json{{"shader", "project://shaders/lc1_overlap.frag"},
                         {"stage", "fragment"},
                         {"range", {{"offset", 104}, {"size", 12}}}});
#endif
}

// AB-1: the engine's include for a baked table (bake_audio_analysis) compiles
// in a fragment and a compute stage, the table a sampler, the layout and the
// time from a buffer, as a render.scalar vec4 passes them.
TEST_CASE("AB-1 pelican_sample_table.glsl compiles where a fragment or compute stage reads a baked table",
          "[shader][ab1]") {
#if PELICAN_RUNTIME_SHADER_COMPILER
    REQUIRE(engineResource("shaders/include/pelican_sample_table.glsl").has_value());
    ShaderCompiler compiler;
    compiler.addIncludeDir(sourceRoot() / "src/core/resources/shaders/include");
    const auto fragment = compiler.compileSource(R"glsl(
#version 450
#extension GL_GOOGLE_include_directive : require
#include "pelican_sample_table.glsl"
layout(set = 0, binding = 0) uniform sampler2D analysis;
layout(set = 0, binding = 1) uniform Table { vec4 layout_and_time; } table;
layout(location = 0) out vec4 outColor;
void main() {
    float level = pelican_sample_table(analysis, 0u, uint(table.layout_and_time.x), uint(table.layout_and_time.y),
                                       table.layout_and_time.z, table.layout_and_time.w);
    outColor = vec4(level, level, level, 1.0);
}
)glsl", vk::ShaderStageFlagBits::eFragment, "ab1_table.frag");
    INFO(fragment.log);
    CHECK(fragment.ok);
    const auto compute = compiler.compileSource(R"glsl(
#version 450
#extension GL_GOOGLE_include_directive : require
#include "pelican_sample_table.glsl"
layout(local_size_x = 8) in;
layout(set = 0, binding = 0) uniform sampler2D analysis;
layout(set = 0, binding = 1) buffer Levels { float levels[]; };
void main() {
    levels[gl_GlobalInvocationID.x] =
        pelican_sample_table(analysis, gl_GlobalInvocationID.x, 1u, 101u, 0.01, 0.25);
}
)glsl", vk::ShaderStageFlagBits::eCompute, "ab1_table.comp");
    INFO(compute.log);
    CHECK(compute.ok);
#endif
}

} // namespace Pelican
