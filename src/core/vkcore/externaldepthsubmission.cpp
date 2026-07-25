#include "externaldepthsubmission.hpp"

#include "../renderingpass/rendertargetcontainer.hpp"
#include "../renderingpass/viewexecutionscheduler.hpp"
#include "render_target_layout_tracker.hpp"
#include "util.hpp"

#include <nlohmann/json.hpp>
#include <stdexcept>

namespace Pelican {
namespace {

vk::ImageAspectFlags depthLayoutAspect(
    vk::Format format) {
    if (format == vk::Format::eD24UnormS8Uint ||
        format == vk::Format::eD32SfloatS8Uint) {
        return vk::ImageAspectFlagBits::eDepth |
               vk::ImageAspectFlagBits::eStencil;
    }
    return vk::ImageAspectFlagBits::eDepth;
}

} // namespace

std::optional<RuntimeExternalDepthExport>
resolveRuntimeExternalDepthExport(
    const CompiledFrameGraphExecution &frame_graph,
    const RenderTargetContainer &render_targets) {
    if (frame_graph.target_plan == nullptr ||
        !frame_graph.target_plan
             ->external_depth_export) {
        return std::nullopt;
    }
    const auto &plan =
        *frame_graph.target_plan
             ->external_depth_export;
    const auto binding =
        frame_graph.render_target_bindings.find(
            plan.source_resource);
    if (binding ==
            frame_graph.render_target_bindings.end() ||
        !isConcreteRenderTarget(binding->second)) {
        throw std::runtime_error(
            "external depth export source is not bound to a "
            "concrete render target: " +
            plan.source_resource);
    }
    const auto source =
        render_targets.getMetadata(binding->second);
    if (plan.format !=
            vk::to_string(source.format) ||
        !(source.usage &
          vk::ImageUsageFlagBits::
              eDepthStencilAttachment) ||
        !(source.usage &
          vk::ImageUsageFlagBits::eTransferSrc) ||
        source.array_layers <
            plan.array_layers) {
        throw std::runtime_error(
            "external depth export source no longer matches its "
            "compiled physical contract: " +
            plan.source_resource);
    }
    return RuntimeExternalDepthExport{
        binding->second, source, &plan};
}

void recordExternalDepthExport(
    const FrameRenderContext &render_ctx,
    const RuntimeExternalDepthExport &export_depth,
    RenderTargetContainer &render_targets,
    VulkanUtils &vulkan,
    RenderTargetLayoutTracker &layout_tracker,
    std::uint32_t view_index,
    std::uint32_t logical_view_count,
    bool view_family,
    nlohmann::json *node_trace) {
    if (!render_ctx.depth_image ||
        render_ctx.depth_format !=
            export_depth.source.format ||
        render_ctx.extent !=
            export_depth.source.extent ||
        render_ctx.depth_copy_layout !=
            vk::ImageLayout::eTransferDstOptimal ||
        render_ctx.depth_required_layout !=
            vk::ImageLayout::
                eDepthStencilAttachmentOptimal) {
        throw std::runtime_error(
            "logical-frame target exposed an incompatible external "
            "depth copy contract");
    }

    const auto layers = planViewLayerCopy(
        export_depth.source.array_layers,
        render_ctx.depth_base_array_layer,
        render_ctx.depth_array_layers,
        view_index, logical_view_count,
        view_family);

    layout_tracker.transition(
        render_ctx.cmd_buf, render_targets,
        vulkan, export_depth.source_id,
        vk::ImageLayout::eTransferSrcOptimal);

    vk::ImageCopy copy;
    copy.srcSubresource = {
        vk::ImageAspectFlagBits::eDepth, 0,
        layers.source_base_array_layer,
        layers.array_layers};
    copy.dstSubresource = {
        vk::ImageAspectFlagBits::eDepth, 0,
        layers.destination_base_array_layer,
        layers.array_layers};
    copy.extent = vk::Extent3D{
        export_depth.source.extent.width,
        export_depth.source.extent.height, 1};
    render_ctx.cmd_buf.copyImage(
        render_targets
            .getImage(export_depth.source_id)
            .image.get(),
        vk::ImageLayout::eTransferSrcOptimal,
        render_ctx.depth_image,
        render_ctx.depth_copy_layout, copy);

    vk::ImageMemoryBarrier barrier;
    barrier.srcAccessMask =
        vk::AccessFlagBits::eTransferWrite;
    barrier.dstAccessMask =
        vk::AccessFlagBits::
            eDepthStencilAttachmentRead |
        vk::AccessFlagBits::
            eDepthStencilAttachmentWrite;
    barrier.oldLayout =
        render_ctx.depth_copy_layout;
    barrier.newLayout =
        render_ctx.depth_required_layout;
    barrier.srcQueueFamilyIndex =
        VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex =
        VK_QUEUE_FAMILY_IGNORED;
    barrier.image = render_ctx.depth_image;
    barrier.subresourceRange = {
        depthLayoutAspect(
            render_ctx.depth_format),
        0, 1,
        layers.destination_base_array_layer,
        layers.array_layers};
    render_ctx.cmd_buf.pipelineBarrier(
        vk::PipelineStageFlagBits::eTransfer,
        vk::PipelineStageFlagBits::
                eEarlyFragmentTests |
            vk::PipelineStageFlagBits::
                eLateFragmentTests,
        {}, {}, {}, {barrier});

    if (node_trace != nullptr) {
        node_trace->push_back({
            {"name",
             "external_composition_depth_export"},
            {"kind", "engine_owned_copy"},
            {"source",
             export_depth.plan
                 ->source_resource},
            {"format",
             export_depth.plan->format},
            {"source_base_array_layer",
             layers.source_base_array_layer},
            {"destination_base_array_layer",
             layers.destination_base_array_layer},
            {"array_layers",
             layers.array_layers},
        });
    }
}

} // namespace Pelican
