#pragma once

#include "../../project/materialoutput.hpp"

#include <glm/vec3.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <vulkan/vulkan.hpp>

namespace Pelican {

enum class ReflectedImageViewDimension {
    none,
    two_d,
    two_d_array,
    cube,
    three_d,
    other,
};

std::string_view reflectedImageViewDimensionName(
    ReflectedImageViewDimension dimension);

struct ReflectedBinding {
    uint32_t set = 0;
    uint32_t binding = 0;
    vk::DescriptorType type = vk::DescriptorType::eSampler;
    uint32_t count = 1;
    vk::ShaderStageFlags stages;
    std::string name;
    ReflectedImageViewDimension image_view_dimension =
        ReflectedImageViewDimension::none;
};

struct ReflectedFragmentOutput {
    uint32_t location = 0;
    vk::Format format = vk::Format::eUndefined;
    std::string name;

    bool operator==(const ReflectedFragmentOutput &) const =
        default;
};

struct ShaderReflection {
    std::vector<ReflectedBinding> bindings;
    std::vector<vk::PushConstantRange> push_constants;
    std::vector<vk::VertexInputAttributeDescription> vertex_inputs;
    std::vector<ReflectedFragmentOutput> fragment_outputs;
    glm::uvec3 local_size{0, 0, 0};
    bool uses_view_index = false;
};

ShaderReflection reflect(std::span<const uint32_t> spirv);
ShaderReflection merge(std::span<const ShaderReflection> stages);
void validateFragmentOutputSchema(
    const ShaderReflection &reflection,
    const MaterialOutputSchema &schema,
    std::string_view context = "fragment shader");

std::vector<vk::DescriptorSetLayoutBinding> makeDescriptorSetLayoutBindings(const ShaderReflection &reflection,
                                                                            uint32_t set);
vk::DescriptorSetLayoutCreateInfo
makeDescriptorSetLayoutCreateInfo(std::span<const vk::DescriptorSetLayoutBinding> bindings);
std::vector<vk::PushConstantRange> makePushConstantRanges(const ShaderReflection &reflection);
void validatePushConstantContract(const ShaderReflection &reflection);
vk::PipelineLayoutCreateInfo makePipelineLayoutCreateInfo(std::span<const vk::DescriptorSetLayout> layouts,
                                                          std::span<const vk::PushConstantRange> push_constants);

} // namespace Pelican
