#include "shaderreflection.hpp"
#include "pelican_sets.hpp"
#include <algorithm>
#include <map>
#include <limits>
#include <spirv_reflect.h>
#include <stdexcept>

namespace Pelican {

namespace {

class ReflectedModule {
    SpvReflectShaderModule module{};

  public:
    explicit ReflectedModule(std::span<const uint32_t> spirv) {
        if (spirv.empty()) {
            throw std::runtime_error("Cannot reflect empty SPIR-V");
        }

        const auto result = spvReflectCreateShaderModule(spirv.size_bytes(), spirv.data(), &module);
        if (result != SPV_REFLECT_RESULT_SUCCESS) {
            throw std::runtime_error("Failed to create SPIR-V reflection module");
        }
    }

    ReflectedModule(const ReflectedModule &) = delete;
    ReflectedModule &operator=(const ReflectedModule &) = delete;

    ~ReflectedModule() { spvReflectDestroyShaderModule(&module); }

    const SpvReflectShaderModule *operator->() const { return &module; }
};

vk::ShaderStageFlags stageFlags(SpvReflectShaderStageFlagBits stage) {
    return vk::ShaderStageFlags{static_cast<vk::ShaderStageFlagBits>(stage)};
}

uint32_t descriptorCount(const SpvReflectDescriptorBinding &binding) {
    return binding.count == 0 ? 1 : binding.count;
}

bool hasLocalSize(const glm::uvec3 &value) { return value.x != 0 || value.y != 0 || value.z != 0; }

void sortReflection(ShaderReflection &reflection) {
    std::sort(reflection.bindings.begin(), reflection.bindings.end(), [](const auto &lhs, const auto &rhs) {
        if (lhs.set != rhs.set) {
            return lhs.set < rhs.set;
        }
        return lhs.binding < rhs.binding;
    });
    std::sort(reflection.vertex_inputs.begin(), reflection.vertex_inputs.end(), [](const auto &lhs, const auto &rhs) {
        return lhs.location < rhs.location;
    });
}

} // namespace

ShaderReflection reflect(std::span<const uint32_t> spirv) {
    ReflectedModule module{spirv};
    ShaderReflection reflection;

    uint32_t binding_count = 0;
    auto result = spvReflectEnumerateDescriptorBindings(module.operator->(), &binding_count, nullptr);
    if (result != SPV_REFLECT_RESULT_SUCCESS) {
        throw std::runtime_error("Failed to enumerate descriptor bindings");
    }

    std::vector<SpvReflectDescriptorBinding *> descriptor_bindings(binding_count);
    result = spvReflectEnumerateDescriptorBindings(module.operator->(), &binding_count, descriptor_bindings.data());
    if (result != SPV_REFLECT_RESULT_SUCCESS) {
        throw std::runtime_error("Failed to read descriptor bindings");
    }

    for (const auto *binding : descriptor_bindings) {
        reflection.bindings.push_back({
            binding->set,
            binding->binding,
            static_cast<vk::DescriptorType>(binding->descriptor_type),
            descriptorCount(*binding),
            stageFlags(module->shader_stage),
            binding->name != nullptr ? binding->name : "",
        });
    }

    uint32_t push_constant_count = 0;
    result = spvReflectEnumeratePushConstantBlocks(module.operator->(), &push_constant_count, nullptr);
    if (result != SPV_REFLECT_RESULT_SUCCESS) {
        throw std::runtime_error("Failed to enumerate push constants");
    }
    std::vector<SpvReflectBlockVariable *> push_constants(push_constant_count);
    result = spvReflectEnumeratePushConstantBlocks(module.operator->(), &push_constant_count, push_constants.data());
    if (result != SPV_REFLECT_RESULT_SUCCESS) {
        throw std::runtime_error("Failed to read push constants");
    }

    for (const auto *block : push_constants) {
        const uint32_t offset = block->offset;
        if (block->size < offset) {
            throw std::runtime_error("Invalid reflected push constant block size");
        }
        reflection.push_constants.push_back(
            vk::PushConstantRange{stageFlags(module->shader_stage), offset, block->size - offset});
    }

    uint32_t input_count = 0;
    result = spvReflectEnumerateInputVariables(module.operator->(), &input_count, nullptr);
    if (result != SPV_REFLECT_RESULT_SUCCESS) {
        throw std::runtime_error("Failed to enumerate vertex inputs");
    }

    std::vector<SpvReflectInterfaceVariable *> input_variables(input_count);
    result = spvReflectEnumerateInputVariables(module.operator->(), &input_count, input_variables.data());
    if (result != SPV_REFLECT_RESULT_SUCCESS) {
        throw std::runtime_error("Failed to read vertex inputs");
    }

    if (module->shader_stage == SPV_REFLECT_SHADER_STAGE_VERTEX_BIT) {
        for (const auto *input : input_variables) {
            if (input->built_in != -1) {
                reflection.uses_view_index =
                    reflection.uses_view_index ||
                    input->built_in == SpvBuiltInViewIndex;
                continue;
            }
            reflection.vertex_inputs.push_back(vk::VertexInputAttributeDescription{
                input->location,
                0,
                static_cast<vk::Format>(input->format),
                0,
            });
        }
    } else {
        for (const auto *input : input_variables) {
            reflection.uses_view_index =
                reflection.uses_view_index ||
                input->built_in == SpvBuiltInViewIndex;
        }
    }

    if (module->shader_stage == SPV_REFLECT_SHADER_STAGE_COMPUTE_BIT && module->entry_point_count > 0) {
        const auto &entry = module->entry_points[0];
        reflection.local_size = {entry.local_size.x, entry.local_size.y, entry.local_size.z};
    }

    sortReflection(reflection);
    return reflection;
}

ShaderReflection merge(std::span<const ShaderReflection> stages) {
    ShaderReflection merged;
    std::map<std::pair<uint32_t, uint32_t>, size_t> binding_indices;

    for (const auto &stage : stages) {
        merged.uses_view_index =
            merged.uses_view_index || stage.uses_view_index;
        for (const auto &binding : stage.bindings) {
            const auto key = std::make_pair(binding.set, binding.binding);
            auto found = binding_indices.find(key);
            if (found == binding_indices.end()) {
                binding_indices.emplace(key, merged.bindings.size());
                merged.bindings.push_back(binding);
                continue;
            }

            auto &existing = merged.bindings[found->second];
            if (existing.type != binding.type || existing.count != binding.count) {
                throw std::runtime_error("Shader descriptor binding mismatch");
            }
            existing.stages |= binding.stages;
            if (existing.name.empty()) {
                existing.name = binding.name;
            }
        }

        merged.push_constants.insert(merged.push_constants.end(), stage.push_constants.begin(),
                                     stage.push_constants.end());

        merged.vertex_inputs.insert(merged.vertex_inputs.end(), stage.vertex_inputs.begin(), stage.vertex_inputs.end());

        if (hasLocalSize(stage.local_size)) {
            if (hasLocalSize(merged.local_size) && merged.local_size != stage.local_size) {
                throw std::runtime_error("Shader compute local size mismatch");
            }
            merged.local_size = stage.local_size;
        }
    }

    sortReflection(merged);
    return merged;
}

std::vector<vk::DescriptorSetLayoutBinding> makeDescriptorSetLayoutBindings(const ShaderReflection &reflection,
                                                                            uint32_t set) {
    std::vector<vk::DescriptorSetLayoutBinding> bindings;
    for (const auto &reflected : reflection.bindings) {
        if (reflected.set != set) {
            continue;
        }
        bindings.push_back(vk::DescriptorSetLayoutBinding{
            reflected.binding,
            reflected.type,
            reflected.count,
            reflected.stages,
        });
    }
    return bindings;
}

vk::DescriptorSetLayoutCreateInfo
makeDescriptorSetLayoutCreateInfo(std::span<const vk::DescriptorSetLayoutBinding> bindings) {
    vk::DescriptorSetLayoutCreateInfo create_info;
    create_info.setBindings(bindings);
    return create_info;
}

std::vector<vk::PushConstantRange> makePushConstantRanges(const ShaderReflection &reflection) {
    if (reflection.push_constants.empty()) {
        return {};
    }

    for (const auto &range : reflection.push_constants) {
        if (range.size == 0 || range.offset % 4 != 0 || range.size % 4 != 0) {
            throw std::runtime_error("Shader push constant range must be non-empty and 4-byte aligned");
        }
        if (range.offset > PELICAN_PUSH_TOTAL_BYTES ||
            range.size > PELICAN_PUSH_TOTAL_BYTES - range.offset) {
            throw std::runtime_error("Shader push constant range exceeds the 128-byte Pelican contract");
        }
        const auto end = range.offset + range.size;
        if (range.offset < PELICAN_PUSH_ENGINE_BYTES &&
            (range.offset != 0 || end < PELICAN_PUSH_ENGINE_BYTES)) {
            throw std::runtime_error(
                "Shader push constant engine region must be exactly the leading 64-byte MVP");
        }
    }

    std::map<std::pair<uint32_t, uint32_t>, vk::ShaderStageFlags> grouped_ranges;
    for (uint32_t stage_bit = 1; stage_bit != 0; stage_bit <<= 1) {
        const vk::ShaderStageFlags stage{static_cast<vk::ShaderStageFlagBits>(stage_bit)};
        uint32_t begin = std::numeric_limits<uint32_t>::max();
        uint32_t end = 0;
        for (const auto &range : reflection.push_constants) {
            if (range.stageFlags & stage) {
                begin = std::min(begin, range.offset);
                end = std::max(end, range.offset + range.size);
            }
        }
        if (begin != std::numeric_limits<uint32_t>::max()) {
            grouped_ranges[{begin, end}] |= stage;
        }
    }

    std::vector<vk::PushConstantRange> result;
    result.reserve(grouped_ranges.size());
    for (const auto &[extent, stages] : grouped_ranges) {
        result.push_back(vk::PushConstantRange{stages, extent.first, extent.second - extent.first});
    }
    return result;
}

void validatePushConstantContract(const ShaderReflection &reflection) {
    (void)makePushConstantRanges(reflection);
}

vk::PipelineLayoutCreateInfo makePipelineLayoutCreateInfo(std::span<const vk::DescriptorSetLayout> layouts,
                                                          std::span<const vk::PushConstantRange> push_constants) {
    vk::PipelineLayoutCreateInfo create_info;
    create_info.setSetLayouts(layouts);
    create_info.setPushConstantRanges(push_constants);
    return create_info;
}

} // namespace Pelican
