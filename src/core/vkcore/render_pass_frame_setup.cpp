#include "render_pass_frame_setup.hpp"

#include <algorithm>
#include <optional>
#include <stdexcept>

namespace Pelican {

namespace {

std::uint32_t sequentialAttachmentLayer(
    const RasterAttachmentView &attachment,
    const RenderTargetContainer &rt_container,
    RenderPassViewInvocation invocation) {
    if (attachment.subresource) {
        if (invocation.logical_view_count == 0 ||
            invocation.view_index >=
                invocation.logical_view_count ||
            attachment.subresource->layer_count !=
                invocation.logical_view_count) {
            throw std::runtime_error(
                "sequential render-pass invocation does not fit "
                "the explicit attachment subresource");
        }
        return attachment.subresource
                   ->base_array_layer +
               invocation.view_index;
    }
    const auto layers =
        rt_container
            .getMetadata(attachment.target)
            .array_layers;
    if (layers == 1) return 0;
    if (invocation.logical_view_count == 0 ||
        invocation.view_index >=
            invocation.logical_view_count ||
        layers < invocation.logical_view_count) {
        throw std::runtime_error(
            "sequential render-pass invocation does not fit "
            "the target array-layer contract");
    }
    return invocation.view_index;
}

vk::ImageView colorAttachmentView(
    const RasterAttachmentView &attachment,
    RenderTargetContainer &rt_container,
    const GraphicsPipelineViewContract &view,
    RenderPassViewInvocation invocation,
    bool resolve) {
    if (!attachment.subresource) {
        if (view.execution ==
            GraphicsPipelineViewExecution::multiview) {
            return resolve
                       ? rt_container
                             .getLayeredImageView(
                                 attachment.target)
                       : rt_container
                             .getLayeredAttachmentImageView(
                                 attachment.target);
        }
        const auto layer =
            sequentialAttachmentLayer(
                attachment, rt_container,
                invocation);
        return resolve
                   ? rt_container.getImageLayerView(
                         attachment.target, layer)
                   : rt_container
                         .getAttachmentImageLayerView(
                             attachment.target,
                             layer);
    }
    const auto metadata =
        rt_container.getMetadata(
            attachment.target);
    ImageSubresourceRange subresource;
    if (view.execution ==
        GraphicsPipelineViewExecution::multiview) {
        subresource =
            *attachment.subresource;
        if (subresource.layer_count !=
                view.view_count ||
            !validImageSubresourceRange(
                subresource,
                metadata.mip_levels,
                metadata.array_layers)) {
            throw std::runtime_error(
                "multiview render-pass attachment subresource "
                "does not match the target/view contract");
        }
        return resolve
                   ? rt_container
                         .getImageSubresourceView(
                             attachment.target,
                             subresource, true)
                   : rt_container
                         .getAttachmentImageSubresourceView(
                             attachment.target,
                             subresource, true);
    }
    const auto layer = sequentialAttachmentLayer(
        attachment, rt_container, invocation);
    subresource = *attachment.subresource;
    subresource.base_array_layer = layer;
    subresource.layer_count = 1;
    return resolve
               ? rt_container
                     .getImageSubresourceView(
                         attachment.target,
                         subresource, false)
               : rt_container
                     .getAttachmentImageSubresourceView(
                         attachment.target,
                         subresource, false);
}

vk::Extent2D attachmentExtent(
    vk::Extent2D base,
    const RasterAttachmentView &attachment) {
    if (!attachment.subresource) return base;
    const auto mip =
        attachment.subresource->base_mip_level;
    return {
        std::max(1u, base.width >> mip),
        std::max(1u, base.height >> mip),
    };
}

} // namespace

vk::Extent2D getRenderPassTargetExtent(const FrameRenderContext &frame, const PassDefinition &pass_def,
                                       RenderTargetContainer &rt_container) {
    std::optional<vk::Extent2D> result;
    const auto include =
        [&](const RasterAttachmentView &attachment) {
            if (isSwapchainRenderTarget(
                    attachment.target)) {
                if (result &&
                    *result != frame.extent) {
                    throw std::runtime_error(
                        "render pass attachments have different "
                        "runtime extents");
                }
                result = frame.extent;
                return;
            }
            if (!isConcreteRenderTarget(
                    attachment.target)) {
                return;
            }
            const auto extent = attachmentExtent(
                rt_container
                    .getMetadata(attachment.target)
                    .extent,
                attachment);
            if (result && *result != extent) {
                throw std::runtime_error(
                    "render pass attachments have different "
                    "runtime extents");
            }
            result = extent;
        };
    for (const auto &attachment :
         pass_def.output_color) {
        include(attachment);
    }
    include(pass_def.output_depth);
    if (result) {
        return *result;
    }
    return frame.extent;
}

void transitionPassOutputsToAttachmentLayouts(vk::CommandBuffer cmd_buf, const PassDefinition &pass_def,
                                              RenderTargetContainer &rt_container, VulkanUtils &vk_utils,
                                              RenderTargetLayoutTracker &layout_tracker) {
    for (std::size_t index = 0;
         index < pass_def.output_color.size();
         ++index) {
        const auto rt_id =
            pass_def.output_color[index]
                .target;
        if (isConcreteRenderTarget(rt_id) &&
            rt_container.hasSeparateAttachment(rt_id) &&
            pass_def
                    .colorAttachmentOperations(index)
                    .load_op ==
                vk::AttachmentLoadOp::eLoad &&
            layout_tracker.currentLayout(
                rt_id, false, &rt_container,
                RenderTargetImageKind::attachment) ==
                vk::ImageLayout::eUndefined) {
            throw std::runtime_error(
                "multisampled color attachment Load requires a preceding "
                "raster access to the multisample surface");
        }
        layout_tracker.transition(cmd_buf, rt_container, vk_utils, rt_id,
                                  vk::ImageLayout::eColorAttachmentOptimal);
        if (isConcreteRenderTarget(rt_id) &&
            rt_container.hasSeparateAttachment(rt_id)) {
            layout_tracker.transition(
                cmd_buf, rt_container, vk_utils, rt_id,
                vk::ImageLayout::eColorAttachmentOptimal, false,
                RenderTargetImageKind::attachment);
        }
    }
    const auto depth_operations =
        pass_def.depthAttachmentOperations();
    if (isConcreteRenderTarget(pass_def.output_depth) &&
        rt_container.hasSeparateAttachment(pass_def.output_depth) &&
        depth_operations.load_op ==
            vk::AttachmentLoadOp::eLoad &&
        layout_tracker.currentLayout(
            pass_def.output_depth, false, &rt_container,
            RenderTargetImageKind::attachment) ==
            vk::ImageLayout::eUndefined) {
        throw std::runtime_error(
            "multisampled depth attachment Load requires a preceding raster "
            "access to the multisample surface");
    }
    layout_tracker.transition(cmd_buf, rt_container, vk_utils, pass_def.output_depth,
                              vk::ImageLayout::eDepthAttachmentOptimal);
    if (isConcreteRenderTarget(pass_def.output_depth) &&
        rt_container.hasSeparateAttachment(pass_def.output_depth)) {
        layout_tracker.transition(
            cmd_buf, rt_container, vk_utils, pass_def.output_depth,
            vk::ImageLayout::eDepthAttachmentOptimal, false,
            RenderTargetImageKind::attachment);
    }
}

void transitionPassInputsToShaderRead(vk::CommandBuffer cmd_buf, const PassDefinition &pass_def,
                                      RenderTargetContainer &rt_container, VulkanUtils &vk_utils,
                                      RenderTargetLayoutTracker &layout_tracker) {
    for (size_t i = 0; i < pass_def.input_targets.size(); ++i) {
        layout_tracker.transition(cmd_buf, rt_container, vk_utils, pass_def.input_targets[i],
                                  vk::ImageLayout::eShaderReadOnlyOptimal,
                                  pass_def.input_target_history.at(i));
    }
}

std::vector<vk::RenderingAttachmentInfo> createColorAttachments(const FrameRenderContext &frame,
                                                                const PassDefinition &pass_def,
                                                                RenderTargetContainer &rt_container,
                                                                const GraphicsPipelineViewContract &view,
                                                                RenderPassViewInvocation invocation) {
    std::vector<vk::RenderingAttachmentInfo> color_attachments;
    color_attachments.reserve(pass_def.output_color.size());

    for (std::size_t index = 0;
         index < pass_def.output_color.size();
         ++index) {
        const auto rt_id =
            pass_def.output_color[index]
                .target;
        const auto &attachment_view =
            pass_def.output_color[index];
        const auto operations =
            pass_def.colorAttachmentOperations(
                index);
        vk::RenderingAttachmentInfo color_att;
        if (isSwapchainRenderTarget(rt_id)) {
            if (view.execution ==
                    GraphicsPipelineViewExecution::single_view &&
                !frame.color_layer_attachments.empty()) {
                if (invocation.view_index >=
                    frame.color_layer_attachments.size()) {
                    throw std::runtime_error(
                        "sequential frame-target layer is out of range");
                }
                color_att.imageView =
                    frame.color_layer_attachments[
                        invocation.view_index];
            } else {
                color_att.imageView = frame.color_attachment;
            }
        } else {
            color_att.imageView =
                colorAttachmentView(
                    attachment_view, rt_container, view,
                    invocation, false);
            if (rt_container.hasSeparateAttachment(rt_id)) {
                color_att.resolveMode = rt_container.resolveMode(rt_id);
                color_att.resolveImageView =
                    colorAttachmentView(
                        attachment_view, rt_container, view,
                        invocation, true);
                color_att.resolveImageLayout =
                    vk::ImageLayout::eColorAttachmentOptimal;
            }
        }

        color_att.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
        color_att.loadOp = operations.load_op;
        color_att.storeOp = operations.store_op;
        color_att.clearValue.color =
            pass_def.colorClearValue(index).vulkan();
        color_attachments.push_back(color_att);
    }

    return color_attachments;
}

vk::RenderingAttachmentInfo createDepthAttachment(const PassDefinition &pass_def,
                                                  RenderTargetContainer &rt_container,
                                                  const GraphicsPipelineViewContract &view,
                                                  RenderPassViewInvocation invocation) {
    vk::RenderingAttachmentInfo depth_attachment;
    depth_attachment.imageView =
        colorAttachmentView(
            pass_def.output_depth, rt_container, view,
            invocation, false);
    depth_attachment.imageLayout = vk::ImageLayout::eDepthAttachmentOptimal;
    if (rt_container.hasSeparateAttachment(pass_def.output_depth)) {
        depth_attachment.resolveMode =
            rt_container.resolveMode(pass_def.output_depth);
        depth_attachment.resolveImageView =
            colorAttachmentView(
                pass_def.output_depth, rt_container,
                view, invocation, true);
        depth_attachment.resolveImageLayout =
            vk::ImageLayout::eDepthAttachmentOptimal;
    }
    const auto operations =
        pass_def.depthAttachmentOperations();
    depth_attachment.loadOp = operations.load_op;
    depth_attachment.storeOp = operations.store_op;
    depth_attachment.clearValue.depthStencil = vk::ClearDepthStencilValue{1.0f, 0};
    return depth_attachment;
}

void setDynamicViewportAndScissor(vk::CommandBuffer cmd_buf, vk::Extent2D extent) {
    vk::Viewport viewport{0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height), 0.0f,
                          1.0f};
    cmd_buf.setViewport(0, viewport);

    vk::Rect2D scissor{{0, 0}, extent};
    cmd_buf.setScissor(0, scissor);
}

bool passInputUsesLocalRead(
    const CompiledPass &pass,
    std::size_t input_index) {
    if (input_index >=
        pass.definition.input_targets.size()) {
        throw std::out_of_range(
            "render-pass input index is out of range");
    }
    const auto mapped =
        [&](std::uint32_t candidate) {
            return candidate !=
                       unusedPhysicalAttachmentMapping &&
                   candidate == input_index;
        };
    return std::any_of(
               pass.rendering
                   .color_attachment_input_indices
                   .begin(),
               pass.rendering
                   .color_attachment_input_indices
                   .end(),
               mapped) ||
           mapped(
               pass.rendering
                   .depth_attachment_input_index);
}

std::vector<std::uint8_t>
localReadScopeColorAttachmentMask(
    std::span<const CompiledPass *const> passes) {
    if (passes.empty() ||
        passes.front() == nullptr) {
        throw std::invalid_argument(
            "tile-local physical scope has no passes");
    }
    std::vector<std::uint8_t> result(
        passes.front()
            ->rendering.color_attachments.size(),
        0);
    for (const auto *pass : passes) {
        if (pass == nullptr ||
            pass->rendering
                    .color_attachment_input_indices
                    .size() != result.size()) {
            throw std::runtime_error(
                "tile-local pass input mappings do not match the "
                "physical attachment union");
        }
        for (std::size_t index = 0;
             index < result.size(); ++index) {
            result[index] =
                result[index] ||
                pass->rendering
                        .color_attachment_input_indices[index] !=
                    unusedPhysicalAttachmentMapping;
        }
    }
    return result;
}

bool localReadScopeUsesDepthInput(
    std::span<const CompiledPass *const> passes) {
    return std::any_of(
        passes.begin(), passes.end(),
        [](const auto *pass) {
            if (pass == nullptr) {
                throw std::invalid_argument(
                    "tile-local physical scope contains a null pass");
            }
            return pass->rendering
                       .depth_attachment_input_index !=
                   unusedPhysicalAttachmentMapping;
        });
}

vk::Extent2D getLocalReadScopeTargetExtent(
    const FrameRenderContext &frame,
    const CompiledPassRenderingContract &rendering,
    RenderTargetContainer &rt_container) {
    std::optional<vk::Extent2D> result;
    const auto include =
        [&](const RasterAttachmentView &target) {
            if (isSwapchainRenderTarget(target)) {
                throw std::runtime_error(
                    "tile-local physical scopes do not yet support "
                    "the frame target");
            }
            if (!isConcreteRenderTarget(target)) {
                return;
            }
            const auto extent =
                attachmentExtent(
                    rt_container
                        .getMetadata(target.target)
                        .extent,
                    target);
            if (result && *result != extent) {
                throw std::runtime_error(
                    "tile-local physical scope attachments have "
                    "different extents");
            }
            result = extent;
        };
    for (const auto target :
         rendering.color_attachments) {
        include(target);
    }
    include(rendering.depth_attachment);
    if (!result) {
        throw std::runtime_error(
            "tile-local physical scope has no concrete "
            "attachment");
    }
    (void)frame;
    return *result;
}

void transitionLocalReadScopeInputs(
    vk::CommandBuffer cmd_buf,
    std::span<const CompiledPass *const> passes,
    RenderTargetContainer &rt_container,
    VulkanUtils &vk_utils,
    RenderTargetLayoutTracker &layout_tracker) {
    for (const auto *pass : passes) {
        if (pass == nullptr) {
            throw std::invalid_argument(
                "tile-local physical scope contains a null pass");
        }
        const auto &definition = pass->definition;
        if (definition.input_target_history.size() !=
            definition.input_targets.size()) {
            throw std::runtime_error(
                "tile-local pass input history metadata is "
                "incomplete: " +
                definition.name);
        }
        for (std::size_t input_index = 0;
             input_index <
             definition.input_targets.size();
             ++input_index) {
            if (passInputUsesLocalRead(
                    *pass, input_index)) {
                if (definition
                        .input_target_history[input_index]) {
                    throw std::runtime_error(
                        "tile-local pass input cannot read history: " +
                        definition.name);
                }
                continue;
            }
            layout_tracker.transition(
                cmd_buf, rt_container, vk_utils,
                definition.input_targets[input_index],
                vk::ImageLayout::
                    eShaderReadOnlyOptimal,
                definition
                    .input_target_history[input_index]);
        }
    }
}

void transitionLocalReadScopeAttachments(
    vk::CommandBuffer cmd_buf,
    const CompiledPassRenderingContract &rendering,
    std::span<const std::uint8_t>
        local_read_color_attachments,
    bool local_read_depth_attachment,
    RenderTargetContainer &rt_container,
    VulkanUtils &vk_utils,
    RenderTargetLayoutTracker &layout_tracker) {
    if (rendering
            .scope_color_attachment_operations
            .size() !=
            rendering.color_attachments.size() ||
        local_read_color_attachments.size() !=
            rendering.color_attachments.size()) {
        throw std::runtime_error(
            "tile-local color attachment operations do not match "
            "the physical scope");
    }
    for (std::size_t index = 0;
         index < rendering.color_attachments.size();
         ++index) {
        const auto target =
            rendering.color_attachments[index];
        if (isSwapchainRenderTarget(target)) {
            throw std::runtime_error(
                "tile-local physical scopes do not yet support "
                "the frame target");
        }
        if (!isConcreteRenderTarget(target)) {
            throw std::runtime_error(
                "tile-local physical scope has an invalid color "
                "attachment");
        }
        if (rt_container.hasSeparateAttachment(target)) {
            throw std::runtime_error(
                "tile-local physical scopes currently require "
                "single-sample color attachments");
        }
        if (local_read_color_attachments[index] &&
            !(rt_container.getMetadata(target).usage &
              vk::ImageUsageFlagBits::
                  eInputAttachment)) {
            throw std::runtime_error(
                "tile-local color attachment lacks Vulkan "
                "INPUT_ATTACHMENT usage");
        }
        if (rendering
                    .scope_color_attachment_operations[index]
                    .load_op ==
                vk::AttachmentLoadOp::eLoad &&
            layout_tracker.currentLayout(
                target, false, &rt_container) ==
                vk::ImageLayout::eUndefined) {
            throw std::runtime_error(
                "tile-local color attachment Load has no "
                "preceding image contents");
        }
        layout_tracker.transition(
            cmd_buf, rt_container, vk_utils, target,
            local_read_color_attachments[index]
                ? vk::ImageLayout::
                      eRenderingLocalReadKHR
                : vk::ImageLayout::
                      eColorAttachmentOptimal);
    }

    if (!isConcreteRenderTarget(
            rendering.depth_attachment)) {
        if (rendering
                .scope_depth_attachment_operations) {
            throw std::runtime_error(
                "tile-local physical scope maps depth operations "
                "without a depth attachment");
        }
        return;
    }
    const auto depth = rendering.depth_attachment;
    if (rt_container.hasSeparateAttachment(depth)) {
        throw std::runtime_error(
            "tile-local physical scopes currently require a "
            "single-sample depth attachment");
    }
    if (!rendering
            .scope_depth_attachment_operations) {
        throw std::runtime_error(
            "tile-local depth attachment lacks physical "
            "operations");
    }
    if (local_read_depth_attachment &&
        !(rt_container.getMetadata(depth).usage &
          vk::ImageUsageFlagBits::
              eInputAttachment)) {
        throw std::runtime_error(
            "tile-local depth attachment lacks Vulkan "
            "INPUT_ATTACHMENT usage");
    }
    if (rendering
                .scope_depth_attachment_operations
                ->load_op ==
            vk::AttachmentLoadOp::eLoad &&
        layout_tracker.currentLayout(
            depth, false, &rt_container) ==
            vk::ImageLayout::eUndefined) {
        throw std::runtime_error(
            "tile-local depth attachment Load has no preceding "
            "image contents");
    }
    layout_tracker.transition(
        cmd_buf, rt_container, vk_utils, depth,
        local_read_depth_attachment
            ? vk::ImageLayout::
                  eRenderingLocalReadKHR
            : vk::ImageLayout::
                  eDepthAttachmentOptimal);
}

std::vector<vk::RenderingAttachmentInfo>
createLocalReadScopeColorAttachments(
    const FrameRenderContext &frame,
    const CompiledPassRenderingContract &rendering,
    std::span<const std::uint8_t>
        local_read_color_attachments,
    RenderTargetContainer &rt_container,
    const GraphicsPipelineViewContract &view,
    RenderPassViewInvocation invocation) {
    if (rendering
                .scope_color_attachment_operations
                .size() !=
            rendering.color_attachments.size() ||
        rendering.scope_color_clear_values.size() !=
            rendering.color_attachments.size() ||
        local_read_color_attachments.size() !=
            rendering.color_attachments.size()) {
        throw std::runtime_error(
            "tile-local color attachment state does not match "
            "the physical scope");
    }
    std::vector<vk::RenderingAttachmentInfo> result;
    result.reserve(rendering.color_attachments.size());
    for (std::size_t index = 0;
         index < rendering.color_attachments.size();
         ++index) {
        const auto target =
            rendering.color_attachments[index];
        if (isSwapchainRenderTarget(target)) {
            throw std::runtime_error(
                "tile-local physical scopes do not yet support "
                "the frame target");
        }
        if (!isConcreteRenderTarget(target) ||
            rt_container.hasSeparateAttachment(target)) {
            throw std::runtime_error(
                "tile-local color attachment must be a concrete "
                "single-sample image");
        }
        vk::RenderingAttachmentInfo attachment;
        attachment.imageView = colorAttachmentView(
            target, rt_container, view, invocation,
            false);
        attachment.imageLayout =
            local_read_color_attachments[index]
                ? vk::ImageLayout::
                      eRenderingLocalReadKHR
                : vk::ImageLayout::
                      eColorAttachmentOptimal;
        const auto operations =
            rendering
                .scope_color_attachment_operations[index];
        attachment.loadOp = operations.load_op;
        attachment.storeOp = operations.store_op;
        attachment.clearValue.color =
            rendering
                .scope_color_clear_values[index]
                .vulkan();
        result.push_back(attachment);
    }
    (void)frame;
    return result;
}

vk::RenderingAttachmentInfo
createLocalReadScopeDepthAttachment(
    const CompiledPassRenderingContract &rendering,
    bool local_read_depth_attachment,
    RenderTargetContainer &rt_container,
    const GraphicsPipelineViewContract &view,
    RenderPassViewInvocation invocation) {
    if (!isConcreteRenderTarget(
            rendering.depth_attachment) ||
        !rendering
             .scope_depth_attachment_operations) {
        throw std::runtime_error(
            "tile-local physical scope has no depth attachment "
            "contract");
    }
    if (rt_container.hasSeparateAttachment(
            rendering.depth_attachment)) {
        throw std::runtime_error(
            "tile-local depth attachment must be single-sample");
    }
    vk::RenderingAttachmentInfo result;
    result.imageView = colorAttachmentView(
        rendering.depth_attachment, rt_container,
        view, invocation, false);
    result.imageLayout =
        local_read_depth_attachment
            ? vk::ImageLayout::
                  eRenderingLocalReadKHR
            : vk::ImageLayout::
                  eDepthAttachmentOptimal;
    result.loadOp =
        rendering
            .scope_depth_attachment_operations
            ->load_op;
    result.storeOp =
        rendering
            .scope_depth_attachment_operations
            ->store_op;
    result.clearValue.depthStencil =
        vk::ClearDepthStencilValue{
            rendering.scope_depth_clear_value,
            rendering.scope_stencil_clear_value};
    return result;
}

} // namespace Pelican
