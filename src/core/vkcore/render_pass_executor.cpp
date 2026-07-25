#include "render_pass_executor.hpp"
#include "../renderingpass/rendertargetcontainer.hpp"
#include "render_pass_dispatch.hpp"
#include "render_pass_frame_setup.hpp"

#include <limits>
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
        [&](GlobalRenderTargetId target) {
            if (!isConcreteRenderTarget(target)) return;
            if (targets.getMetadata(target).array_layers <
                view.view_count) {
                throw std::runtime_error(
                    "multiview pass target has too few "
                    "array layers: " +
                    pass.definition.name);
            }
        };
    for (const auto target :
         pass.definition.output_color) {
        require_layers(target);
    }
    require_layers(pass.definition.output_depth);
    for (std::size_t input_index = 0;
         input_index <
         pass.definition.input_targets.size();
         ++input_index) {
        const auto shared =
            pass.definition.input_target_views.size() ==
                    pass.definition.input_targets.size() &&
                pass.definition.input_target_views[input_index] ==
                    PassInputViewDimension::shared_2d;
        if (!shared) {
            require_layers(
                pass.definition
                    .input_targets[input_index]);
        }
    }
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

} // namespace Pelican
