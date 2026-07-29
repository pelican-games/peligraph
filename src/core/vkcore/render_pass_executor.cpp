#include "render_pass_executor.hpp"
#include "../renderingpass/rendertargetcontainer.hpp"
#include "core.hpp"
#include "render_pass_dispatch.hpp"
#include "render_pass_frame_setup.hpp"

#include <algorithm>
#include <limits>
#include <optional>
#include <stdexcept>

namespace Pelican {

namespace {

std::uint32_t contiguousViewMask(
    std::uint32_t view_count) {
    return view_count >= 32
               ? std::numeric_limits<
                     std::uint32_t>::max()
               : (std::uint32_t{1} << view_count) -
                     1u;
}

void validateViewExecution(
    const CompiledPass &pass,
    const RenderTargetContainer &targets,
    RenderPassViewInvocation invocation) {
    const auto &view = pass.view;
    if (invocation.logical_view_count == 0 ||
        invocation.view_index >=
            invocation.logical_view_count) {
        throw std::runtime_error(
            "render-pass view invocation is out of range: " +
            pass.definition.name);
    }

    if (view.execution ==
        GraphicsPipelineViewExecution::single_view) {
        if (view.view_count != 1 ||
            view.view_mask != 0) {
            throw std::runtime_error(
                "single-view compiled pass has an invalid "
                "view contract: " +
                pass.definition.name);
        }
        return;
    }

    if (view.view_count < 2 ||
        view.view_count > 32 ||
        view.view_mask !=
            contiguousViewMask(view.view_count)) {
        throw std::runtime_error(
            "multiview compiled pass has an invalid "
            "view contract: " +
            pass.definition.name);
    }
    if (invocation.logical_view_count !=
            view.view_count ||
        invocation.view_index != 0) {
        throw std::runtime_error(
            "multiview compiled pass invocation does not "
            "match its pipeline contract: " +
            pass.definition.name);
    }
    if (pass.definition.isUi()
#if PELICAN_WITH_IMGUI
        || pass.definition.isImGui()
#endif
    ) {
        throw std::runtime_error(
            "UI pass implementation does not support "
            "multiview: " +
            pass.definition.name);
    }

    const auto require_layers =
        [&](GlobalRenderTargetId target,
            const std::optional<
                ImageSubresourceRange>
                &subresource = std::nullopt) {
            if (!isConcreteRenderTarget(target)) return;
            const auto layers =
                targets.getMetadata(target).array_layers;
            const auto valid =
                subresource
                    ? subresource->layer_count ==
                              view.view_count &&
                          subresource
                                  ->base_array_layer <=
                              layers &&
                          subresource->layer_count <=
                              layers -
                                  subresource
                                      ->base_array_layer
                    : layers >= view.view_count;
            if (!valid) {
                throw std::runtime_error(
                    "multiview pass target has too few "
                    "array layers: " +
                    pass.definition.name);
            }
        };
    for (const auto &target :
         pass.definition.output_color) {
        require_layers(
            target.target,
            target.subresource);
    }
    require_layers(
        pass.definition.output_depth.target,
        pass.definition.output_depth.subresource);
    for (std::size_t input_index = 0;
         input_index <
         pass.definition.input_targets.size();
         ++input_index) {
        const auto consumer_independent =
            pass.definition.input_target_views.size() ==
                    pass.definition.input_targets.size() &&
                (pass.definition.input_target_views[input_index] ==
                     PassInputViewDimension::shared_2d ||
                 pass.definition.input_target_views[input_index] ==
                     PassInputViewDimension::
                         family_2d_array);
        if (!consumer_independent) {
            require_layers(
                pass.definition
                    .input_targets[input_index],
                std::nullopt);
        }
    }
}

bool sameScopeAttachmentContract(
    const CompiledPassRenderingContract &left,
    const CompiledPassRenderingContract &right) {
    return left.scope_index == right.scope_index &&
           left.scope_id == right.scope_id &&
           left.color_attachments ==
               right.color_attachments &&
           left.depth_attachment ==
               right.depth_attachment &&
           left.scope_color_attachment_operations ==
               right.scope_color_attachment_operations &&
           left.scope_color_clear_values ==
               right.scope_color_clear_values &&
           left.scope_depth_attachment_operations ==
               right.scope_depth_attachment_operations &&
           left.scope_depth_clear_value ==
               right.scope_depth_clear_value &&
           left.scope_stencil_clear_value ==
               right.scope_stencil_clear_value &&
           left.fused_rendering_scope &&
           right.fused_rendering_scope &&
           left.local_read_scope ==
               right.local_read_scope;
}

void validateRenderingScope(
    std::span<const CompiledPass *const> passes,
    const RenderTargetContainer &targets,
    RenderPassViewInvocation invocation) {
    if (passes.empty() || passes.front() == nullptr) {
        throw std::invalid_argument(
            "fused physical rendering scope has no passes");
    }
    const auto &first = *passes.front();
    if (!first.rendering
             .fused_rendering_scope) {
        throw std::runtime_error(
            "fused rendering execution was requested for a "
            "non-fused physical scope");
    }
    if (first.rendering.local_read_scope &&
        !GET_MODULE(VulkanManageCore)
             .getRuntimeCapabilities()
             .dynamic_rendering_local_read) {
        throw std::runtime_error(
            "tile-local physical scope requires the enabled "
            "dynamic-rendering-local-read feature");
    }
    for (std::size_t pass_index = 0;
         pass_index < passes.size(); ++pass_index) {
        const auto *pass = passes[pass_index];
        if (pass == nullptr) {
            throw std::invalid_argument(
                "fused physical rendering scope contains a null pass");
        }
        validateViewExecution(
            *pass, targets, invocation);
        if (pass->view != first.view ||
            !sameScopeAttachmentContract(
                pass->rendering,
                first.rendering)) {
            throw std::runtime_error(
                "fused rendering passes disagree on their physical "
                "scope contract: " +
                pass->definition.name);
        }
        if (pass->rendering
                    .color_attachment_locations
                    .size() !=
                first.rendering
                    .color_attachments.size() ||
            pass->rendering
                    .color_attachment_input_indices
                    .size() !=
                first.rendering
                    .color_attachments.size()) {
            throw std::runtime_error(
                "fused rendering pass mappings do not match the "
                "physical attachment union: " +
                pass->definition.name);
        }
        if (pass->definition.isUi()
#if PELICAN_WITH_IMGUI
            || pass->definition.isImGui()
#endif
        ) {
            throw std::runtime_error(
                "UI pass implementation cannot execute inside a "
                "fused physical rendering scope: " +
                pass->definition.name);
        }
        if (pass->definition
                .input_target_history.size() !=
            pass->definition
                .input_targets.size()) {
            throw std::runtime_error(
                "fused rendering pass input history metadata is "
                "incomplete: " +
                pass->definition.name);
        }
        for (std::size_t input_index = 0;
             input_index <
             pass->definition.input_targets.size();
             ++input_index) {
            const auto local =
                passInputUsesLocalRead(
                    *pass, input_index);
            if (pass_index == 0 && local) {
                throw std::runtime_error(
                    "first tile-local scope pass reads an attachment "
                    "before a scope producer: " +
                    pass->definition.name);
            }
            const auto target =
                pass->definition
                    .input_targets[input_index];
            const auto same_frame_attachment =
                !pass->definition
                     .input_target_history.at(
                         input_index) &&
                (std::find_if(
                     first.rendering
                         .color_attachments.begin(),
                     first.rendering
                         .color_attachments.end(),
                     [&](const auto &attachment) {
                         return attachment
                                    .target.value ==
                                target.value;
                     }) !=
                     first.rendering
                         .color_attachments.end() ||
                 target.value ==
                     first.rendering
                         .depth_attachment
                         .target.value);
            if (same_frame_attachment && !local) {
                throw std::runtime_error(
                    "fused rendering scope contains a same-frame "
                    "attachment read that was not lowered to an "
                    "input attachment: " +
                    pass->definition.name);
            }
        }
    }
}

void setLocalReadMappings(
    vk::CommandBuffer cmd_buf,
    const CompiledPassRenderingContract
        &rendering) {
    vk::RenderingAttachmentLocationInfoKHR
        locations;
    locations.setColorAttachmentLocations(
        rendering.color_attachment_locations);
    auto &vkcore =
        GET_MODULE(VulkanManageCore);
    vkcore.setRenderingAttachmentLocations(
        cmd_buf, locations);

    auto depth_input =
        rendering.depth_attachment_input_index;
    vk::RenderingInputAttachmentIndexInfoKHR
        input_indices;
    input_indices
        .setColorAttachmentInputIndices(
            rendering
                .color_attachment_input_indices);
    if (depth_input !=
        unusedPhysicalAttachmentMapping) {
        input_indices.pDepthInputAttachmentIndex =
            &depth_input;
    }
    vkcore.setRenderingInputAttachmentIndices(
        cmd_buf, input_indices);
}

} // namespace

void RenderPassExecutor::execute(const FrameRenderContext &frame, const CompiledPass &pass,
                                 const RenderPassExecutorDependencies &dependencies,
                                 RenderTargetLayoutTracker &layout_tracker,
                                 RenderPassViewInvocation invocation) const {
    auto &rt_container = dependencies.render_target_container;
    auto &vk_utils = dependencies.vk_utils;
    const auto cmd_buf = frame.cmd_buf;
    const auto &pass_def = pass.definition;
    validateViewExecution(
        pass, rt_container, invocation);
    const auto target_extent = getRenderPassTargetExtent(frame, pass_def, rt_container);

    transitionPassInputsToShaderRead(cmd_buf, pass_def, rt_container, vk_utils, layout_tracker);
    transitionPassOutputsToAttachmentLayouts(cmd_buf, pass_def, rt_container, vk_utils, layout_tracker);

    if (pass_def.isUi()) {
        renderUiPass(cmd_buf, frame, pass_def, target_extent, rt_container, dependencies.dispatch);
        return;
    }
#if PELICAN_WITH_IMGUI
    if (pass_def.isImGui()) {
        renderImGuiPass(cmd_buf, frame, pass_def, target_extent, rt_container, dependencies.dispatch);
        return;
    }
#endif

    auto color_attachments = createColorAttachments(
        frame, pass_def, rt_container, pass.view,
        invocation);

    vk::RenderingInfo render_info;
    render_info.renderArea = vk::Rect2D{{0, 0}, target_extent};
    render_info.layerCount = 1;
    render_info.viewMask = pass.view.view_mask;
    render_info.setColorAttachments(color_attachments);

    vk::RenderingAttachmentInfo depth_attachment;
    if (isConcreteRenderTarget(pass_def.output_depth)) {
        depth_attachment = createDepthAttachment(
            pass_def, rt_container, pass.view,
            invocation);
        render_info.pDepthAttachment = &depth_attachment;
    }

    cmd_buf.beginRendering(render_info);
    setDynamicViewportAndScissor(cmd_buf, target_extent);

    renderDynamicPassDrawCalls(cmd_buf, pass.pass_id, pass_def, target_extent,
                               dependencies.dispatch, invocation);

    cmd_buf.endRendering();
}

void RenderPassExecutor::beginRenderingScope(
    const FrameRenderContext &frame,
    std::span<const CompiledPass *const> passes,
    const RenderPassExecutorDependencies &dependencies,
    RenderTargetLayoutTracker &layout_tracker,
    RenderPassViewInvocation invocation) const {
    auto &targets =
        dependencies.render_target_container;
    validateRenderingScope(
        passes, targets, invocation);
    const auto &first = *passes.front();
    const auto extent =
        getLocalReadScopeTargetExtent(
            frame, first.rendering, targets);
    const auto local_read_color_attachments =
        localReadScopeColorAttachmentMask(
            passes);
    const auto local_read_depth_attachment =
        localReadScopeUsesDepthInput(
            passes);

    transitionLocalReadScopeInputs(
        frame.cmd_buf, passes, targets,
        dependencies.vk_utils, layout_tracker);
    transitionLocalReadScopeAttachments(
        frame.cmd_buf, first.rendering,
        local_read_color_attachments,
        local_read_depth_attachment, targets,
        dependencies.vk_utils, layout_tracker);
    auto color_attachments =
        createLocalReadScopeColorAttachments(
            frame, first.rendering,
            local_read_color_attachments, targets,
            first.view, invocation);

    vk::RenderingInfo render_info;
    render_info.renderArea =
        vk::Rect2D{{0, 0}, extent};
    render_info.layerCount = 1;
    render_info.viewMask =
        first.view.view_mask;
    render_info.setColorAttachments(
        color_attachments);

    vk::RenderingAttachmentInfo
        depth_attachment;
    if (isConcreteRenderTarget(
            first.rendering.depth_attachment)) {
        depth_attachment =
            createLocalReadScopeDepthAttachment(
                first.rendering,
                local_read_depth_attachment,
                targets,
                first.view, invocation);
        render_info.pDepthAttachment =
            &depth_attachment;
    }

    frame.cmd_buf.beginRendering(render_info);
    setDynamicViewportAndScissor(
        frame.cmd_buf, extent);
}

void RenderPassExecutor::executeRenderingScopePass(
    const FrameRenderContext &frame,
    const CompiledPass &pass,
    const RenderPassExecutorDependencies &dependencies,
    RenderPassViewInvocation invocation) const {
    validateViewExecution(
        pass,
        dependencies.render_target_container,
        invocation);
    if (!pass.rendering.fused_rendering_scope) {
        throw std::runtime_error(
            "fused rendering draw requested for a pass outside a "
            "single-instance physical scope: " +
            pass.definition.name);
    }
    if (pass.rendering.local_read_scope) {
        setLocalReadMappings(
            frame.cmd_buf, pass.rendering);
    }
    const auto extent =
        getLocalReadScopeTargetExtent(
            frame, pass.rendering,
            dependencies.render_target_container);
    renderDynamicPassDrawCalls(
        frame.cmd_buf, pass.pass_id,
        pass.definition, extent,
        dependencies.dispatch, invocation);
}

void RenderPassExecutor::renderingScopeDependency(
    vk::CommandBuffer cmd_buf) const {
    vk::MemoryBarrier barrier;
    barrier.srcAccessMask =
        vk::AccessFlagBits::eColorAttachmentWrite |
        vk::AccessFlagBits::
            eDepthStencilAttachmentWrite;
    barrier.dstAccessMask =
        vk::AccessFlagBits::eInputAttachmentRead |
        vk::AccessFlagBits::eColorAttachmentRead |
        vk::AccessFlagBits::eColorAttachmentWrite |
        vk::AccessFlagBits::eDepthStencilAttachmentRead |
        vk::AccessFlagBits::eDepthStencilAttachmentWrite;
    cmd_buf.pipelineBarrier(
        vk::PipelineStageFlagBits::
                eColorAttachmentOutput |
            vk::PipelineStageFlagBits::
                eEarlyFragmentTests |
            vk::PipelineStageFlagBits::
                eLateFragmentTests,
        vk::PipelineStageFlagBits::eFragmentShader |
            vk::PipelineStageFlagBits::
                eColorAttachmentOutput |
            vk::PipelineStageFlagBits::
                eEarlyFragmentTests |
            vk::PipelineStageFlagBits::
                eLateFragmentTests,
        vk::DependencyFlagBits::eByRegion,
        {barrier}, {}, {});
}

void RenderPassExecutor::endRenderingScope(
    vk::CommandBuffer cmd_buf) const {
    cmd_buf.endRendering();
}

} // namespace Pelican
