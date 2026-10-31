#pragma once
#include "../src/project/targetrendermemory.hpp"

namespace Pelican::TestSupport {
// Deterministic CPU device: storage images use a different memory type.
inline std::optional<ImageMemoryRequirements> rc7cMemoryRequirements(const VulkanPhysicalResourcePlan &r, ImageUsage usage) {
    if (!r.format) return std::nullopt;
    const auto extent=resolveResourceExtent(r.extent.value_or(ResourceExtentPlan{}),{64,64});
    const auto bytes=vulkanPhysicalResourceBytes(r,{.extent=extent});
    return ImageMemoryRequirements{std::max<std::uint64_t>(256,(bytes+255)/256*256),256,
        bool(usage & ImageUsageBit::storage) ? 2u : 1u};
}
}
