#include "rasterpassvulkanadapter.hpp"

#include "renderingpassjsonhelpers.hpp"
#include "vulkanimagevocabulary.hpp"
#include "../vkcore/depthstencilformat.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>
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

vk::CompareOp lowerCompare(
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
        "unknown portable raster compare");
}

vk::PolygonMode lowerPolygonMode(
    RasterPolygonMode mode) {
    switch (mode) {
    case RasterPolygonMode::fill:
        return vk::PolygonMode::eFill;
    case RasterPolygonMode::line:
        return vk::PolygonMode::eLine;
    case RasterPolygonMode::point:
        return vk::PolygonMode::ePoint;
    }
    throw std::runtime_error(
        "unknown portable raster polygon mode");
}

vk::ConservativeRasterizationModeEXT lowerConservativeMode(
    RasterConservativeMode mode) {
    switch (mode) {
    case RasterConservativeMode::off:
        return vk::ConservativeRasterizationModeEXT::eDisabled;
    case RasterConservativeMode::overestimate:
        return vk::ConservativeRasterizationModeEXT::eOverestimate;
    case RasterConservativeMode::underestimate:
        return vk::ConservativeRasterizationModeEXT::eUnderestimate;
    }
    throw std::runtime_error(
        "unknown portable raster conservative mode");
}

vk::StencilOp lowerStencilOperation(
    RasterStencilOperation operation) {
    switch (operation) {
    case RasterStencilOperation::keep:
        return vk::StencilOp::eKeep;
    case RasterStencilOperation::zero:
        return vk::StencilOp::eZero;
    case RasterStencilOperation::replace:
        return vk::StencilOp::eReplace;
    case RasterStencilOperation::increment_and_clamp:
        return vk::StencilOp::eIncrementAndClamp;
    case RasterStencilOperation::decrement_and_clamp:
        return vk::StencilOp::eDecrementAndClamp;
    case RasterStencilOperation::invert:
        return vk::StencilOp::eInvert;
    case RasterStencilOperation::increment_and_wrap:
        return vk::StencilOp::eIncrementAndWrap;
    case RasterStencilOperation::decrement_and_wrap:
        return vk::StencilOp::eDecrementAndWrap;
    }
    throw std::runtime_error(
        "unknown portable raster stencil operation");
}

vk::LogicOp lowerLogicOperation(
    RasterLogicOperation operation) {
    switch (operation) {
    case RasterLogicOperation::clear:
        return vk::LogicOp::eClear;
    case RasterLogicOperation::bitwise_and:
        return vk::LogicOp::eAnd;
    case RasterLogicOperation::and_reverse:
        return vk::LogicOp::eAndReverse;
    case RasterLogicOperation::copy:
        return vk::LogicOp::eCopy;
    case RasterLogicOperation::and_inverted:
        return vk::LogicOp::eAndInverted;
    case RasterLogicOperation::no_op:
        return vk::LogicOp::eNoOp;
    case RasterLogicOperation::bitwise_xor:
        return vk::LogicOp::eXor;
    case RasterLogicOperation::bitwise_or:
        return vk::LogicOp::eOr;
    case RasterLogicOperation::nor:
        return vk::LogicOp::eNor;
    case RasterLogicOperation::equivalent:
        return vk::LogicOp::eEquivalent;
    case RasterLogicOperation::invert:
        return vk::LogicOp::eInvert;
    case RasterLogicOperation::or_reverse:
        return vk::LogicOp::eOrReverse;
    case RasterLogicOperation::copy_inverted:
        return vk::LogicOp::eCopyInverted;
    case RasterLogicOperation::or_inverted:
        return vk::LogicOp::eOrInverted;
    case RasterLogicOperation::nand:
        return vk::LogicOp::eNand;
    case RasterLogicOperation::set:
        return vk::LogicOp::eSet;
    }
    throw std::runtime_error(
        "unknown portable raster logic operation");
}

GraphicsPipelineStencilFaceState lowerStencilFace(
    const RasterStencilFaceState &face) {
    return GraphicsPipelineStencilFaceState{
        .fail = lowerStencilOperation(face.fail),
        .pass = lowerStencilOperation(face.pass),
        .depth_fail = lowerStencilOperation(face.depth_fail),
        .compare = lowerCompare(face.compare),
        .compare_mask = face.compare_mask,
        .write_mask = face.write_mask,
        .reference = face.reference,
    };
}

std::string depthFormatName(vk::Format format) {
    return vulkanFormatName(format);
}

} // namespace

vk::BlendFactor lowerVulkanBlendFactor(
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
    case MaterialOutputBlendFactor::constant_color:
        return vk::BlendFactor::eConstantColor;
    case MaterialOutputBlendFactor::one_minus_constant_color:
        return vk::BlendFactor::eOneMinusConstantColor;
    case MaterialOutputBlendFactor::constant_alpha:
        return vk::BlendFactor::eConstantAlpha;
    case MaterialOutputBlendFactor::one_minus_constant_alpha:
        return vk::BlendFactor::eOneMinusConstantAlpha;
    case MaterialOutputBlendFactor::source1_color:
        return vk::BlendFactor::eSrc1Color;
    case MaterialOutputBlendFactor::one_minus_source1_color:
        return vk::BlendFactor::eOneMinusSrc1Color;
    case MaterialOutputBlendFactor::source1_alpha:
        return vk::BlendFactor::eSrc1Alpha;
    case MaterialOutputBlendFactor::one_minus_source1_alpha:
        return vk::BlendFactor::eOneMinusSrc1Alpha;
    }
    throw std::runtime_error(
        "unknown portable raster blend factor");
}

vk::BlendOp lowerVulkanBlendOperation(
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

vk::ColorComponentFlags lowerVulkanColorWriteMask(
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

GraphicsPipelineColorAttachmentState
lowerVulkanRasterColorAttachmentState(
    const RasterColorAttachmentState &state) {
    return GraphicsPipelineColorAttachmentState{
        .blend_enabled = state.blend.enabled,
        .source_color =
            lowerVulkanBlendFactor(
                state.blend.color.source),
        .destination_color =
            lowerVulkanBlendFactor(
                state.blend.color.destination),
        .color_operation =
            lowerVulkanBlendOperation(
                state.blend.color.operation),
        .source_alpha =
            lowerVulkanBlendFactor(
                state.blend.alpha.source),
        .destination_alpha =
            lowerVulkanBlendFactor(
                state.blend.alpha.destination),
        .alpha_operation =
            lowerVulkanBlendOperation(
                state.blend.alpha.operation),
        .write_mask =
            lowerVulkanColorWriteMask(state.write_mask),
    };
}

void applyVulkanPipelineState(
    const PipelineState &state,
    GraphicsPipelineDesc &desc,
    std::string_view context) {
    validatePipelineState(
        state, desc.depth_format.has_value(), context);
    if (desc.depth_format) {
        validatePipelineStateStencilFormat(
            state, vulkanFormatHasStencil(*desc.depth_format),
            depthFormatName(*desc.depth_format), context);
    }
    // input assembly
    desc.topology = lowerTopology(state.topology);
    desc.primitive_restart = state.primitive_restart;
    // rasterization
    desc.cull_mode = lowerCullMode(state.cull);
    desc.front_face = lowerFrontFace(state.front_face);
    desc.polygon_mode = lowerPolygonMode(state.polygon_mode);
    desc.depth_clamp = state.depth_clamp;
    desc.depth_bias = std::nullopt;
    if (state.depth_bias) {
        desc.depth_bias = GraphicsPipelineDepthBias{
            .constant_factor =
                static_cast<float>(state.depth_bias->constant),
            .clamp = static_cast<float>(state.depth_bias->clamp),
            .slope_factor =
                static_cast<float>(state.depth_bias->slope),
        };
    }
    desc.line_width = static_cast<float>(state.line_width);
    desc.rasterizer_discard = state.rasterizer_discard;
    desc.conservative_rasterization =
        lowerConservativeMode(state.conservative);
    // multisample
    desc.min_sample_shading = std::nullopt;
    if (state.sample_shading) {
        desc.min_sample_shading =
            static_cast<float>(*state.sample_shading);
    }
    desc.sample_mask = state.sample_mask;
    desc.alpha_to_coverage = state.alpha_to_coverage;
    desc.alpha_to_one = state.alpha_to_one;
    // depth / stencil
    desc.depth_test = state.depth.test;
    desc.depth_write = state.depth.write;
    desc.depth_compare = lowerCompare(state.depth.compare);
    desc.depth_bounds = std::nullopt;
    if (state.depth.bounds) {
        desc.depth_bounds = std::array<float, 2>{
            static_cast<float>(state.depth.bounds->min),
            static_cast<float>(state.depth.bounds->max)};
    }
    desc.stencil = std::nullopt;
    if (state.stencil) {
        desc.stencil = GraphicsPipelineStencilState{
            .front = lowerStencilFace(state.stencil->front),
            .back = lowerStencilFace(state.stencil->back),
        };
    }
    // color blend (the attachments are the caller's)
    desc.logic_op = std::nullopt;
    if (state.logic_op) {
        desc.logic_op = lowerLogicOperation(*state.logic_op);
    }
    for (std::size_t index = 0; index < 4; ++index) {
        desc.blend_constants[index] =
            static_cast<float>(state.blend_constants[index]);
    }
}

void applyVulkanRasterPassContract(
    const RasterPassContract &contract,
    GraphicsPipelineDesc &desc,
    std::span<const std::uint32_t>
        physical_attachment_locations,
    std::string_view context) {
    validateRasterPassContract(
        contract,
        contract.state.color_attachments.size(),
        desc.depth_format.has_value(),
        context);
    applyVulkanRasterPassContract(
        contract.state, desc,
        physical_attachment_locations, context);
    // CC-2: clip_y up is recorded with a negative-height viewport
    // (render_pass_dispatch.cpp) and nothing else. Vulkan judges a face by
    // its winding in framebuffer coordinates, after the viewport, so
    // front_face keeps meaning the winding seen on the finished image.
}

void applyVulkanRasterPassContract(
    const RasterFixedFunctionState &state,
    GraphicsPipelineDesc &desc,
    std::span<const std::uint32_t>
        physical_attachment_locations,
    std::string_view context) {
    validateRasterFixedFunctionState(
        state, state.color_attachments.size(),
        desc.depth_format.has_value(),
        context);
    if (!physical_attachment_locations.empty() &&
        physical_attachment_locations.size() !=
            desc.color_formats.size()) {
        throw std::runtime_error(
            "Vulkan raster attachment-location mapping count "
            "must match physical color formats");
    }
    if (physical_attachment_locations.empty() &&
        state.color_attachments.size() !=
            desc.color_formats.size()) {
        throw std::runtime_error(
            "Vulkan raster color state count must match physical "
            "color formats when no attachment mapping is supplied");
    }
    applyVulkanPipelineState(state, desc, context);
    desc.color_attachment_states.assign(
        desc.color_formats.size(),
        lowerVulkanRasterColorAttachmentState(
            RasterColorAttachmentState{
                .write_mask = 0}));
    if (physical_attachment_locations.empty()) {
        for (std::size_t index = 0;
             index <
             state.color_attachments.size();
             ++index) {
            desc.color_attachment_states[index] =
                lowerVulkanRasterColorAttachmentState(
                    state.color_attachments[index]);
        }
        return;
    }
    std::vector<bool> mapped(
        state.color_attachments.size(),
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
            state.color_attachments.size()) {
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
                state.color_attachments[logical]);
    }
    if (std::find(
            mapped.begin(), mapped.end(), false) !=
        mapped.end()) {
        throw std::runtime_error(
            "Vulkan raster attachment-location mapping omits a "
            "logical color output");
    }
}

PipelineStateDeviceFacts pipelineStateDeviceFacts(
    const VulkanRuntimeCapabilities &capabilities) {
    return PipelineStateDeviceFacts{
        .fill_mode_non_solid = capabilities.fill_mode_non_solid,
        .wide_lines = capabilities.wide_lines,
        .depth_clamp = capabilities.depth_clamp,
        .depth_bias_clamp = capabilities.depth_bias_clamp,
        .depth_bounds = capabilities.depth_bounds,
        .sample_rate_shading = capabilities.sample_rate_shading,
        .alpha_to_one = capabilities.alpha_to_one,
        .logic_op = capabilities.logic_op,
        .dual_src_blend = capabilities.dual_src_blend,
        .max_fragment_dual_src_attachments =
            capabilities.max_fragment_dual_src_attachments,
        .conservative_rasterization =
            capabilities.conservative_rasterization,
        .primitive_underestimation =
            capabilities.primitive_underestimation,
        .conservative_point_and_line_rasterization =
            capabilities.conservative_point_and_line_rasterization,
    };
}

} // namespace Pelican
