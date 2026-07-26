#pragma once

#include "../renderingpass/renderingpasscontainer.hpp"
#include "../renderingpass/rendertargetcontainer.hpp"
#include "render_target_layout_tracker.hpp"
#include "rendertarget.hpp"
#include "util.hpp"
#include <cstdint>
#include <span>
#include <vector>
#include <vulkan/vulkan.hpp>

namespace Pelican {

vk::Extent2D getRenderPassTargetExtent(const FrameRenderContext &frame, const PassDefinition &pass_def,
                                       RenderTargetContainer &rt_container);
void transitionPassOutputsToAttachmentLayouts(vk::CommandBuffer cmd_buf, const PassDefinition &pass_def,
                                              RenderTargetContainer &rt_container, VulkanUtils &vk_utils,
                                              RenderTargetLayoutTracker &layout_tracker);
void transitionPassInputsToShaderRead(vk::CommandBuffer cmd_buf, const PassDefinition &pass_def,
                                      RenderTargetContainer &rt_container, VulkanUtils &vk_utils,
                                      RenderTargetLayoutTracker &layout_tracker);
std::vector<vk::RenderingAttachmentInfo> createColorAttachments(const FrameRenderContext &frame,
                                                                const PassDefinition &pass_def,
                                                                RenderTargetContainer &rt_container,
                                                                const GraphicsPipelineViewContract &view,
                                                                RenderPassViewInvocation invocation);
vk::RenderingAttachmentInfo createDepthAttachment(const PassDefinition &pass_def,
                                                  RenderTargetContainer &rt_container,
                                                  const GraphicsPipelineViewContract &view,
                                                  RenderPassViewInvocation invocation);
void setDynamicViewportAndScissor(vk::CommandBuffer cmd_buf, vk::Extent2D extent);

bool passInputUsesLocalRead(
    const CompiledPass &pass,
    std::size_t input_index);
std::vector<std::uint8_t>
localReadScopeColorAttachmentMask(
    std::span<const CompiledPass *const> passes);
bool localReadScopeUsesDepthInput(
    std::span<const CompiledPass *const> passes);
vk::Extent2D getLocalReadScopeTargetExtent(
    const FrameRenderContext &frame,
    const CompiledPassRenderingContract &rendering,
    RenderTargetContainer &rt_container);
void transitionLocalReadScopeInputs(
    vk::CommandBuffer cmd_buf,
    std::span<const CompiledPass *const> passes,
    RenderTargetContainer &rt_container,
    VulkanUtils &vk_utils,
    RenderTargetLayoutTracker &layout_tracker);
void transitionLocalReadScopeAttachments(
    vk::CommandBuffer cmd_buf,
    const CompiledPassRenderingContract &rendering,
    std::span<const std::uint8_t>
        local_read_color_attachments,
    bool local_read_depth_attachment,
    RenderTargetContainer &rt_container,
    VulkanUtils &vk_utils,
    RenderTargetLayoutTracker &layout_tracker);
std::vector<vk::RenderingAttachmentInfo>
createLocalReadScopeColorAttachments(
    const FrameRenderContext &frame,
    const CompiledPassRenderingContract &rendering,
    std::span<const std::uint8_t>
        local_read_color_attachments,
    RenderTargetContainer &rt_container,
    const GraphicsPipelineViewContract &view,
    RenderPassViewInvocation invocation);
vk::RenderingAttachmentInfo
createLocalReadScopeDepthAttachment(
    const CompiledPassRenderingContract &rendering,
    bool local_read_depth_attachment,
    RenderTargetContainer &rt_container,
    const GraphicsPipelineViewContract &view,
    RenderPassViewInvocation invocation);

} // namespace Pelican
