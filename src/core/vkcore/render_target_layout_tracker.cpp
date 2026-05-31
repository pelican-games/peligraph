#include "render_target_layout_tracker.hpp"
#include "../renderingpass/rendertargetcontainer.hpp"

namespace Pelican {

namespace {

VulkanUtils::ChangeImageLayoutInfo makeTransitionInfo(vk::ImageLayout old_layout, vk::ImageLayout new_layout) {
    VulkanUtils::ChangeImageLayoutInfo info{
        .src_stage = vk::PipelineStageFlagBits::eTopOfPipe,
        .dst_stage = vk::PipelineStageFlagBits::eTopOfPipe,
        .src_access = {},
        .dst_access = {},
    };

    if (old_layout == vk::ImageLayout::eShaderReadOnlyOptimal) {
        info.src_stage = vk::PipelineStageFlagBits::eFragmentShader;
        info.src_access = vk::AccessFlagBits::eShaderRead;
    } else if (old_layout == vk::ImageLayout::eColorAttachmentOptimal) {
        info.src_stage = vk::PipelineStageFlagBits::eColorAttachmentOutput;
        info.src_access = vk::AccessFlagBits::eColorAttachmentWrite;
    } else if (old_layout == vk::ImageLayout::eDepthAttachmentOptimal) {
        info.src_stage = vk::PipelineStageFlagBits::eLateFragmentTests;
        info.src_access = vk::AccessFlagBits::eDepthStencilAttachmentWrite;
    }

    if (new_layout == vk::ImageLayout::eShaderReadOnlyOptimal) {
        info.dst_stage = vk::PipelineStageFlagBits::eFragmentShader;
        info.dst_access = vk::AccessFlagBits::eShaderRead;
    } else if (new_layout == vk::ImageLayout::eColorAttachmentOptimal) {
        info.dst_stage = vk::PipelineStageFlagBits::eColorAttachmentOutput;
        info.dst_access = vk::AccessFlagBits::eColorAttachmentWrite;
    } else if (new_layout == vk::ImageLayout::eDepthAttachmentOptimal) {
        info.dst_stage = vk::PipelineStageFlagBits::eEarlyFragmentTests;
        info.dst_access = vk::AccessFlagBits::eDepthStencilAttachmentWrite;
    }

    return info;
}

} // namespace

void RenderTargetLayoutTracker::transition(vk::CommandBuffer cmd_buf, RenderTargetContainer &rt_container,
                                           VulkanUtils &vk_utils, GlobalRenderTargetId rt_id,
                                           vk::ImageLayout new_layout) {
    if (rt_id.value < 0) {
        return;
    }

    auto [it, inserted] = layouts.try_emplace(rt_id, vk::ImageLayout::eUndefined);
    const auto old_layout = it->second;
    if (old_layout == new_layout) {
        return;
    }

    const auto &rt = rt_container.get(rt_id);
    vk_utils.changeImageLayoutCmd(cmd_buf, rt.image, old_layout, new_layout,
                                  makeTransitionInfo(old_layout, new_layout));
    it->second = new_layout;
}

vk::ImageLayout RenderTargetLayoutTracker::currentLayout(GlobalRenderTargetId rt_id) const {
    if (auto it = layouts.find(rt_id); it != layouts.end()) {
        return it->second;
    }
    return vk::ImageLayout::eUndefined;
}

void RenderTargetLayoutTracker::reset() { layouts.clear(); }

} // namespace Pelican
