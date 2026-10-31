#pragma once
#include "../src/core/renderingpass/renderingsamplecount.hpp"
#include "../src/core/renderingpass/vulkanimagevocabulary.hpp"
#include "rc7c_test_facts.hpp"
namespace Pelican::TestSupport {
inline std::optional<ImageMemoryRequirements> rc7cDeviceRequirements(const vk::ImageCreateInfo &info) {
    const auto format=imageFormatFromVulkan(info.format);
    if (!format) return std::nullopt;
    const auto bytes=imageResourceBytes({.format=*format,.extent={info.extent.width,info.extent.height},
        .mip_levels=info.mipLevels,.array_layers=info.arrayLayers});
    return ImageMemoryRequirements{std::max<std::uint64_t>(256,(bytes+255)/256*256),256,
        bool(info.usage & vk::ImageUsageFlagBits::eStorage) ? 2u : 1u};
}
inline RenderingTargetPlanDeviceFacts rc7cDeviceFacts() {
    RenderingTargetPlanDeviceFacts facts;
    facts.image_memory_requirements=rc7cDeviceRequirements;
    return facts;
}
}
