#pragma once

#include "rendertargetstoragemode.hpp"
#include "../../project/imagesubresource.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <vulkan/vulkan.hpp>

namespace Pelican {

// EN-2: a render.image read from a project image file. The image is read
// only: its pixels are uploaded whenever the target's image is created (each
// generation and each extent change) and it is never cleared, aliased or
// written by a pass.
struct RenderTargetSource {
    std::string asset;
    // "srgb": the image has an sRGB view and shaders read linear values.
    // "linear": UNORM, read as stored.
    std::string color_space = "srgb";
    // "straight": the file's values; "premultiply": RGB times alpha, in
    // linear space, when the file is read.
    std::string alpha = "straight";
    // "none" or "full" (every mip generated at upload).
    std::string mips = "none";
};

// The decoded RGBA8 pixels of a RenderTargetSource, in the target's format
// (premultiplied when asked), shared by every variant that registers it.
struct RenderTargetSourcePixels {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::byte> rgba8;
};

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
    // Kept logical until the physical format is selected so an alternate
    // SINT/UINT target does not retain a float-tagged Vulkan union.
    std::array<double, 4> history_clear_color{
        0.0, 0.0, 0.0, 0.0};
    std::uint32_t samples = 1;
    ImageMipLevelCount mip_levels;
    std::uint32_t array_layers = 1;
    ImageResourceDimension dimension =
        ImageResourceDimension::two_d;
    RenderTargetStorageMode storage_mode =
        RenderTargetStorageMode::materialized;
    // Filled by physical-plan compilation. Targets with the same value may
    // share one allocation when their compiled lifetimes do not overlap.
    std::optional<std::string> alias_group;
    // EN-2: set for a render.image read from a file. The pixels are attached
    // by registration (graph image sources); the parser fills only `source`.
    std::optional<RenderTargetSource> source;
    std::shared_ptr<const RenderTargetSourcePixels> source_pixels;
};

// Resolves the authored clear value against the target's format. Lives with
// the definition rather than with config registration because registration
// is the upper layer.
vk::ClearColorValue physicalRenderTargetHistoryClearColor(
    const RenderTargetDefinition &definition);

} // namespace Pelican
