#pragma once

#include "../shader/pipelinefactory.hpp"
#include "../../project/rasterpass.hpp"

#include <span>

namespace Pelican {

GraphicsPipelineColorAttachmentState
lowerVulkanRasterColorAttachmentState(
    const RasterColorAttachmentState &state);

// Mutates only the portable fixed-function portion of desc. Shader modules,
// physical attachment formats, view execution, and resource ABI remain the
// responsibility of the device compiler that owns desc.
void applyVulkanRasterPassContract(
    const RasterPassContract &contract,
    GraphicsPipelineDesc &desc,
    std::span<const std::uint32_t>
        physical_attachment_locations = {});

// Fullscreen passes own fixed-function state but not draw geometry. This
// overload shares the exact Vulkan lowering without a dummy draw contract.
void applyVulkanRasterPassContract(
    const RasterFixedFunctionState &state,
    GraphicsPipelineDesc &desc,
    std::span<const std::uint32_t>
        physical_attachment_locations = {});

} // namespace Pelican
