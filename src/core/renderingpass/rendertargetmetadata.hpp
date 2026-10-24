#pragma once

#include "rendertargetstoragemode.hpp"
#include "../../project/generationresources.hpp"
#include "../../project/imageformat.hpp"
#include "../../project/imageresourcedimension.hpp"
#include "../../project/imageusage.hpp"
#include "../../project/resourceextent.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace Pelican {

struct RenderTargetMetadata {
    std::string name;
    ImageUsage usage;
    ImageFormat format = ImageFormat::r8g8b8a8_unorm;
    ResolvedResourceExtent extent;
    bool history = false;
    std::uint32_t samples = 1;
    std::uint32_t mip_levels = 1;
    std::uint32_t array_layers = 1;
    ImageResourceDimension dimension =
        ImageResourceDimension::two_d;
    RenderTargetStorageMode storage_mode =
        RenderTargetStorageMode::materialized;
    std::optional<std::string> alias_group;
};

// RC-3c2a: what a resource check reads of an image of the generation's
// resource description (the registry's metadata of a target registered from
// it is the same: its definition was made from it).
RenderTargetMetadata renderTargetMetadataOf(const GenerationImage &image);

// RC-1: the bytes a registered target's images occupy -- every surface
// (two for history), mip and layer, and the multisampled image it renders
// into -- by the planner's arithmetic (imageResourceBytes) over what
// registration made: the image's extent and resolved mip count.
// (Defined with RenderTargetContainer, which makes the images.)
std::uint64_t renderTargetBytes(const RenderTargetMetadata &metadata);

} // namespace Pelican
