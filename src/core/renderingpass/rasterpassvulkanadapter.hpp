#pragma once

#include "../shader/pipelinefactory.hpp"
#include "../vkcore/runtimecapabilities.hpp"
#include "../../project/rasterpass.hpp"

#include <span>
#include <string_view>

namespace Pelican {

// PSN-1: the one lowering of the portable vocabulary to Vulkan. The material
// pipeline's per-output blend states use the same three.
vk::BlendFactor lowerVulkanBlendFactor(
    MaterialOutputBlendFactor factor);
vk::BlendOp lowerVulkanBlendOperation(
    MaterialOutputBlendOperation operation);
vk::ColorComponentFlags lowerVulkanColorWriteMask(
    std::uint8_t mask);

GraphicsPipelineColorAttachmentState
lowerVulkanRasterColorAttachmentState(
    const RasterColorAttachmentState &state);

// PSN-1: the one mapping of a PipelineState to the desc. It writes every
// fixed-function field of desc except the per-attachment color states (a
// raster or fullscreen pass adds those through applyVulkanRasterPassContract;
// a material resolves its own) and dynamic_front_face (a recording concern).
// It refuses, naming `context`, state the desc's attachments cannot carry:
// depth bias, depth bounds or stencil without a depth format, and stencil
// on a depth format without a stencil component. Call it after depth_format
// is set.
void applyVulkanPipelineState(
    const PipelineState &state,
    GraphicsPipelineDesc &desc,
    std::string_view context = "Vulkan pipeline");

// Mutates only the portable fixed-function portion of desc. Shader modules,
// physical attachment formats, view execution, and resource ABI remain the
// responsibility of the device compiler that owns desc.
void applyVulkanRasterPassContract(
    const RasterPassContract &contract,
    GraphicsPipelineDesc &desc,
    std::span<const std::uint32_t>
        physical_attachment_locations = {},
    std::string_view context = "Vulkan raster pipeline");

// Fullscreen passes own fixed-function state but not draw geometry. This
// overload shares the exact Vulkan lowering without a dummy draw contract.
void applyVulkanRasterPassContract(
    const RasterFixedFunctionState &state,
    GraphicsPipelineDesc &desc,
    std::span<const std::uint32_t>
        physical_attachment_locations = {},
    std::string_view context = "Vulkan raster pipeline");

// PSN-1: the device facts requirePipelineStateSupported checks, from the
// features the device advertised and the engine enabled.
PipelineStateDeviceFacts pipelineStateDeviceFacts(
    const VulkanRuntimeCapabilities &capabilities);

} // namespace Pelican
