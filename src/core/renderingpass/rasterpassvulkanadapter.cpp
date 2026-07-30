#include "rasterpassvulkanadapter.hpp"

#include <algorithm>
#include <stdexcept>
#include <vector>

namespace Pelican {
namespace {

vk::PrimitiveTopology lowerTopology(
    RasterPrimitiveTopology topology) {
    switch (topology) {
    case RasterPrimitiveTopology::point_list:
        return vk::PrimitiveTopology::ePointList;
    case RasterPrimitiveTopology::line_list:
        return vk::PrimitiveTopology::eLineList;
    case RasterPrimitiveTopology::line_strip:
        return vk::PrimitiveTopology::eLineStrip;
    case RasterPrimitiveTopology::triangle_list:
        return vk::PrimitiveTopology::eTriangleList;
    case RasterPrimitiveTopology::triangle_strip:
        return vk::PrimitiveTopology::eTriangleStrip;
    }
    throw std::runtime_error(
        "unknown portable raster topology");
}

vk::CullModeFlags lowerCullMode(
    RasterCullMode mode) {
    switch (mode) {
    case RasterCullMode::none:
        return vk::CullModeFlagBits::eNone;
    case RasterCullMode::front:
        return vk::CullModeFlagBits::eFront;
    case RasterCullMode::back:
        return vk::CullModeFlagBits::eBack;
    }
    throw std::runtime_error(
        "unknown portable raster cull mode");
}

vk::FrontFace lowerFrontFace(
    RasterFrontFace face) {
    switch (face) {
    case RasterFrontFace::counter_clockwise:
        return vk::FrontFace::eCounterClockwise;
    case RasterFrontFace::clockwise:
        return vk::FrontFace::eClockwise;
    }
    throw std::runtime_error(
        "unknown portable raster front face");
}

vk::CompareOp lowerDepthCompare(
    RasterDepthCompare compare) {
    switch (compare) {
    case RasterDepthCompare::never:
        return vk::CompareOp::eNever;
    case RasterDepthCompare::less:
        return vk::CompareOp::eLess;
    case RasterDepthCompare::equal:
        return vk::CompareOp::eEqual;
    case RasterDepthCompare::less_equal:
        return vk::CompareOp::eLessOrEqual;
    case RasterDepthCompare::greater:
        return vk::CompareOp::eGreater;
    case RasterDepthCompare::not_equal:
        return vk::CompareOp::eNotEqual;
    case RasterDepthCompare::greater_equal:
        return vk::CompareOp::eGreaterOrEqual;
    case RasterDepthCompare::always:
        return vk::CompareOp::eAlways;
    }
    throw std::runtime_error(
        "unknown portable raster depth compare");
}

vk::BlendFactor lowerBlendFactor(
    MaterialOutputBlendFactor factor) {
    switch (factor) {
    case MaterialOutputBlendFactor::zero:
        return vk::BlendFactor::eZero;
    case MaterialOutputBlendFactor::one:
        return vk::BlendFactor::eOne;
    case MaterialOutputBlendFactor::source_color:
        return vk::BlendFactor::eSrcColor;
    case MaterialOutputBlendFactor::one_minus_source_color:
        return vk::BlendFactor::eOneMinusSrcColor;
    case MaterialOutputBlendFactor::destination_color:
        return vk::BlendFactor::eDstColor;
    case MaterialOutputBlendFactor::
        one_minus_destination_color:
        return vk::BlendFactor::eOneMinusDstColor;
    case MaterialOutputBlendFactor::source_alpha:
        return vk::BlendFactor::eSrcAlpha;
    case MaterialOutputBlendFactor::
        one_minus_source_alpha:
        return vk::BlendFactor::eOneMinusSrcAlpha;
    case MaterialOutputBlendFactor::destination_alpha:
        return vk::BlendFactor::eDstAlpha;
    case MaterialOutputBlendFactor::
        one_minus_destination_alpha:
        return vk::BlendFactor::eOneMinusDstAlpha;
    case MaterialOutputBlendFactor::
        source_alpha_saturate:
        return vk::BlendFactor::eSrcAlphaSaturate;
    }
    throw std::runtime_error(
        "unknown portable raster blend factor");
}

vk::BlendOp lowerBlendOperation(
    MaterialOutputBlendOperation operation) {
    switch (operation) {
    case MaterialOutputBlendOperation::add:
        return vk::BlendOp::eAdd;
    case MaterialOutputBlendOperation::subtract:
        return vk::BlendOp::eSubtract;
    case MaterialOutputBlendOperation::reverse_subtract:
        return vk::BlendOp::eReverseSubtract;
    case MaterialOutputBlendOperation::minimum:
        return vk::BlendOp::eMin;
    case MaterialOutputBlendOperation::maximum:
        return vk::BlendOp::eMax;
    }
    throw std::runtime_error(
        "unknown portable raster blend operation");
}

vk::ColorComponentFlags lowerWriteMask(
    std::uint8_t mask) {
    if ((mask & ~materialOutputWriteRgba) != 0) {
        throw std::runtime_error(
            "portable raster color write mask has invalid bits");
    }
    vk::ColorComponentFlags result;
    if ((mask & materialOutputWriteRed) != 0)
        result |= vk::ColorComponentFlagBits::eR;
    if ((mask & materialOutputWriteGreen) != 0)
        result |= vk::ColorComponentFlagBits::eG;
    if ((mask & materialOutputWriteBlue) != 0)
        result |= vk::ColorComponentFlagBits::eB;
    if ((mask & materialOutputWriteAlpha) != 0)
        result |= vk::ColorComponentFlagBits::eA;
    return result;
}

} // namespace

GraphicsPipelineColorAttachmentState
lowerVulkanRasterColorAttachmentState(
    const RasterColorAttachmentState &state) {
    return GraphicsPipelineColorAttachmentState{
        .blend_enabled = state.blend.enabled,
        .source_color =
            lowerBlendFactor(
                state.blend.color.source),
        .destination_color =
            lowerBlendFactor(
                state.blend.color.destination),
        .color_operation =
            lowerBlendOperation(
                state.blend.color.operation),
        .source_alpha =
            lowerBlendFactor(
                state.blend.alpha.source),
        .destination_alpha =
            lowerBlendFactor(
                state.blend.alpha.destination),
        .alpha_operation =
            lowerBlendOperation(
                state.blend.alpha.operation),
        .write_mask =
            lowerWriteMask(state.write_mask),
    };
}

void applyVulkanRasterPassContract(
    const RasterPassContract &contract,
    GraphicsPipelineDesc &desc,
    std::span<const std::uint32_t>
        physical_attachment_locations) {
    validateRasterPassContract(
        contract,
        contract.state.color_attachments.size(),
        desc.depth_format.has_value(),
        "Vulkan raster pipeline");
    if (!physical_attachment_locations.empty() &&
        physical_attachment_locations.size() !=
            desc.color_formats.size()) {
        throw std::runtime_error(
            "Vulkan raster attachment-location mapping count "
            "must match physical color formats");
    }
    if (physical_attachment_locations.empty() &&
        contract.state.color_attachments.size() !=
            desc.color_formats.size()) {
        throw std::runtime_error(
            "Vulkan raster color state count must match physical "
            "color formats when no attachment mapping is supplied");
    }
    desc.topology =
        lowerTopology(contract.state.topology);
    desc.cull_mode =
        lowerCullMode(contract.state.cull);
    desc.front_face =
        lowerFrontFace(
            contract.state.front_face);
    desc.depth_test =
        contract.state.depth.test;
    desc.depth_write =
        contract.state.depth.write;
    desc.depth_compare =
        lowerDepthCompare(
            contract.state.depth.compare);
    desc.color_attachment_states.assign(
        desc.color_formats.size(),
        lowerVulkanRasterColorAttachmentState(
            RasterColorAttachmentState{
                .write_mask = 0}));
    if (physical_attachment_locations.empty()) {
        for (std::size_t index = 0;
             index <
             contract.state.color_attachments.size();
             ++index) {
            desc.color_attachment_states[index] =
                lowerVulkanRasterColorAttachmentState(
                    contract.state
                        .color_attachments[index]);
        }
        return;
    }
    std::vector<bool> mapped(
        contract.state.color_attachments.size(),
        false);
    for (std::size_t physical = 0;
         physical <
         physical_attachment_locations.size();
         ++physical) {
        const auto logical =
            physical_attachment_locations[physical];
        if (logical ==
            unusedGraphicsAttachmentMapping) {
            continue;
        }
        if (logical >=
            contract.state.color_attachments.size()) {
            throw std::runtime_error(
                "Vulkan raster attachment-location mapping "
                "references an absent logical color output");
        }
        if (mapped[logical]) {
            throw std::runtime_error(
                "Vulkan raster attachment-location mapping "
                "duplicates a logical color output");
        }
        mapped[logical] = true;
        desc.color_attachment_states[physical] =
            lowerVulkanRasterColorAttachmentState(
                contract.state
                    .color_attachments[logical]);
    }
    if (std::find(
            mapped.begin(), mapped.end(), false) !=
        mapped.end()) {
        throw std::runtime_error(
            "Vulkan raster attachment-location mapping omits a "
            "logical color output");
    }
}

} // namespace Pelican
