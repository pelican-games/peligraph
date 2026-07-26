#pragma once

#include "rendertargetstoragemode.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include <vulkan/vulkan.hpp>

namespace Pelican {

struct RenderTargetDefinition {
    std::string name;
    std::string format_class;
    std::string role;
    float extent_scale = 1.0f;
    std::optional<vk::Extent2D> fixed_extent;
    vk::Format format;
    // The authored format remains the automatic/default choice. Additional
    // entries are explicit candidates a verified physical fragment may
    // select for this target.
    std::vector<vk::Format> format_candidates;
    vk::ImageUsageFlags usage;
    bool history = false;
    vk::ClearColorValue history_clear_color =
        vk::ClearColorValue{std::array{0.0f, 0.0f, 0.0f, 0.0f}};
    std::uint32_t samples = 1;
    std::uint32_t array_layers = 1;
    RenderTargetStorageMode storage_mode =
        RenderTargetStorageMode::materialized;
    // Filled by physical-plan compilation. Targets with the same value may
    // share one allocation when their compiled lifetimes do not overlap.
    std::optional<std::string> alias_group;
};

} // namespace Pelican
