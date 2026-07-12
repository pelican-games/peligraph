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
        info.src_access = vk::AccessFlagBits::eColorAttachmentRead |
                          vk::AccessFlagBits::eColorAttachmentWrite;
    } else if (old_layout == vk::ImageLayout::eDepthAttachmentOptimal) {
        info.src_stage = vk::PipelineStageFlagBits::eLateFragmentTests;
        info.src_access = vk::AccessFlagBits::eDepthStencilAttachmentRead |
                          vk::AccessFlagBits::eDepthStencilAttachmentWrite;
    } else if (old_layout == vk::ImageLayout::eGeneral) {
        info.src_stage = vk::PipelineStageFlagBits::eComputeShader;
        info.src_access = vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite;
    } else if (old_layout == vk::ImageLayout::eTransferSrcOptimal) {
        info.src_stage = vk::PipelineStageFlagBits::eTransfer;
        info.src_access = vk::AccessFlagBits::eTransferRead;
    } else if (old_layout == vk::ImageLayout::eTransferDstOptimal) {
        info.src_stage = vk::PipelineStageFlagBits::eTransfer;
        info.src_access = vk::AccessFlagBits::eTransferWrite;
    }

    if (new_layout == vk::ImageLayout::eShaderReadOnlyOptimal) {
        info.dst_stage = vk::PipelineStageFlagBits::eFragmentShader;
        info.dst_access = vk::AccessFlagBits::eShaderRead;
    } else if (new_layout == vk::ImageLayout::eColorAttachmentOptimal) {
        info.dst_stage = vk::PipelineStageFlagBits::eColorAttachmentOutput;
        info.dst_access = vk::AccessFlagBits::eColorAttachmentRead |
                          vk::AccessFlagBits::eColorAttachmentWrite;
    } else if (new_layout == vk::ImageLayout::eDepthAttachmentOptimal) {
        info.dst_stage = vk::PipelineStageFlagBits::eEarlyFragmentTests;
        info.dst_access = vk::AccessFlagBits::eDepthStencilAttachmentRead |
                          vk::AccessFlagBits::eDepthStencilAttachmentWrite;
    } else if (new_layout == vk::ImageLayout::eGeneral) {
        info.dst_stage = vk::PipelineStageFlagBits::eComputeShader;
        info.dst_access = vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite;
    } else if (new_layout == vk::ImageLayout::eTransferSrcOptimal) {
        info.dst_stage = vk::PipelineStageFlagBits::eTransfer;
        info.dst_access = vk::AccessFlagBits::eTransferRead;
    } else if (new_layout == vk::ImageLayout::eTransferDstOptimal) {
        info.dst_stage = vk::PipelineStageFlagBits::eTransfer;
        info.dst_access = vk::AccessFlagBits::eTransferWrite;
    }

    return info;
}

} // namespace

void RenderTargetLayoutTracker::transition(vk::CommandBuffer cmd_buf, RenderTargetContainer &rt_container,
                                           VulkanUtils &vk_utils, GlobalRenderTargetId rt_id,
                                           vk::ImageLayout new_layout, bool history_read) {
    if (isSpecialRenderTarget(rt_id)) {
        return;
    }

    const auto surface = rt_container.surfaceIndex(rt_id, history_read);
    const auto key = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(rt_id.value)) << 1u) | surface;
    auto [it, inserted] = layouts.try_emplace(key, rt_container.initialLayout(rt_id));
    const auto old_layout = it->second;
    if (old_layout == new_layout) {
        return;
    }

    const auto &image = rt_container.getImage(rt_id, history_read);
    vk_utils.changeImageLayoutCmd(cmd_buf, image, old_layout, new_layout,
                                  makeTransitionInfo(old_layout, new_layout));
    it->second = new_layout;
}

vk::ImageLayout RenderTargetLayoutTracker::currentLayout(
    GlobalRenderTargetId rt_id, bool history_read,
    const RenderTargetContainer *rt_container) const {
    const auto surface = rt_container != nullptr ? rt_container->surfaceIndex(rt_id, history_read) : 0u;
    const auto key = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(rt_id.value)) << 1u) | surface;
    if (auto it = layouts.find(key); it != layouts.end()) {
        return it->second;
    }
    return rt_container != nullptr ? rt_container->initialLayout(rt_id) : vk::ImageLayout::eUndefined;
}

void RenderTargetLayoutTracker::reset() { layouts.clear(); }

} // namespace Pelican
