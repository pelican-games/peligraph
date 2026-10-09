#pragma once

#include "../shader/shaderreflection.hpp"
#include "../shader/pelican_sets.hpp"
#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace Pelican {

// Compute descriptors alternate with history parity, independently of the
// number of submitted frames. Keep the allocation and its budget in sync.
inline constexpr std::uint32_t computeDescriptorParities = 2;

struct ComputeDescriptorBudget {
    std::uint64_t sets = 0;
    std::map<vk::DescriptorType, std::uint64_t> descriptors;

    void add(const ShaderReflection &reflection, std::uint32_t views) {
        bool has_bindings = false;
        const auto copies = std::uint64_t{views} * computeDescriptorParities;
        for (const auto &binding : reflection.bindings) {
            if (binding.set != PELICAN_SET_PASS_INPUT) continue;
            has_bindings = true;
            constexpr auto limit = std::numeric_limits<std::uint32_t>::max();
            if (binding.count && copies > limit / binding.count)
                refuse("descriptor_copies", copies, limit / binding.count);
            const auto count = std::uint64_t{binding.count} * copies;
            if (descriptors[binding.type] + count > limit)
                refuse("descriptor_count", descriptors[binding.type] + count, limit);
            descriptors[binding.type] += count;
        }
        if (has_bindings) {
            sets += copies;
            if (sets > std::numeric_limits<std::uint32_t>::max())
                refuse("set_count", sets, std::numeric_limits<std::uint32_t>::max());
        }
    }

    // One headroom policy for both sets and each descriptor type. Vulkan's
    // maxDescriptorSet* limits apply to pipeline layouts, not pool totals.
    static std::uint32_t capacity(std::uint64_t required) {
        constexpr std::uint64_t extra = 16;
        constexpr auto limit = std::numeric_limits<std::uint32_t>::max();
        if (required > limit)
            refuse("pool_count", required, limit);
        const auto padded = required + (required + 3) / 4 + extra;
        return static_cast<std::uint32_t>(std::min<std::uint64_t>(padded, limit));
    }

    [[noreturn]] static void refuse(const std::string &field,
                                    std::uint64_t count, std::uint64_t limit,
                                    const std::string &task = {}) {
        throw std::runtime_error("compute.descriptor_budget@1: " +
            nlohmann::json{{"field", field}, {"count", count},
                           {"limit", limit}, {"task", task}}.dump());
    }
};

inline void validateComputeDescriptorLayout(const ShaderReflection &reflection,
                                           const vk::PhysicalDeviceLimits &limits,
                                           const std::string &task) {
    std::map<vk::DescriptorType, std::uint64_t> counts;
    for (const auto &binding : reflection.bindings)
        counts[binding.type] += binding.count;
    const auto check = [&](const char *name, std::uint64_t count, std::uint32_t limit) {
        if (count > limit) ComputeDescriptorBudget::refuse(name, count, limit, task);
    };
    check("maxDescriptorSetStorageBuffers", counts[vk::DescriptorType::eStorageBuffer],
          limits.maxDescriptorSetStorageBuffers);
    check("maxDescriptorSetStorageImages", counts[vk::DescriptorType::eStorageImage],
          limits.maxDescriptorSetStorageImages);
    check("maxDescriptorSetSampledImages", counts[vk::DescriptorType::eCombinedImageSampler] +
          counts[vk::DescriptorType::eSampledImage], limits.maxDescriptorSetSampledImages);
    check("maxDescriptorSetSamplers", counts[vk::DescriptorType::eCombinedImageSampler] +
          counts[vk::DescriptorType::eSampler], limits.maxDescriptorSetSamplers);
}

} // namespace Pelican
