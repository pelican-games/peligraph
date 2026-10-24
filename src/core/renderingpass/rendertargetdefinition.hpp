#pragma once

#include "rendertargetstoragemode.hpp"
#include "../../project/imageformat.hpp"
#include "../../project/imagesource.hpp"
#include "../../project/imagesubresource.hpp"
#include "../../project/imageusage.hpp"
#include "../../project/resourceextent.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <vulkan/vulkan.hpp>

namespace Pelican {

// EN-2: a render.image read from a project image file (each generation and
// each extent change uploads its pixels). RC-3c1a: the declaration and its
// defaults are project's (imagesource.hpp).
using RenderTargetSource = ImageSourceDeclaration;

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
    std::optional<ResolvedResourceExtent> fixed_extent;
    ImageFormat format = ImageFormat::r8g8b8a8_unorm;
    // The authored format remains the automatic/default choice. Additional
    // entries are explicit candidates a verified physical fragment may
    // select for this target.
    std::vector<ImageFormat> format_candidates;
    ImageUsage usage;
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
    // PS-1: `lifetime: "persistent"`. The contents are kept from one frame
    // to the next: never an alias, transient or tile-local candidate, and
    // the layout tracker resumes from its last layout instead of discarding.
    // Undefined again once the image is made anew (extent, format, rebuild).
    bool persistent = false;
};

// Resolves the authored clear value against the target's format. Lives with
// the definition rather than with config registration because registration
// is the upper layer.
vk::ClearColorValue physicalRenderTargetHistoryClearColor(
    const RenderTargetDefinition &definition);

// RC-3a1: the usage registration creates a target's images with: the
// declared bits, a scope-local attachment's transient (and tile-local input)
// bits in place of the rest, and the transfer destination a history image is
// cleared through. registerRenderTarget refuses a definition first.
ImageUsage registeredRenderTargetUsage(
    const RenderTargetDefinition &definition);
// (Defined with RenderTargetContainer, which makes the images.)

} // namespace Pelican
