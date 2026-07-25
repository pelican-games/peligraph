#pragma once

#include "../renderingpass/renderingpasscontainer.hpp"
#include "../renderingpass/rendertargetcontainer.hpp"
#include "render_target_layout_tracker.hpp"
#include "rendertarget.hpp"
#include "util.hpp"
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

} // namespace Pelican
