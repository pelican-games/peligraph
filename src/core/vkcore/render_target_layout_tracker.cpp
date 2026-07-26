#include "render_target_layout_tracker.hpp"
#include "../renderingpass/rendertargetcontainer.hpp"
#include <stdexcept>

namespace Pelican {

namespace {

VulkanUtils::ChangeImageLayoutInfo makeTransitionInfo(
    vk::ImageLayout old_layout, vk::ImageLayout new_layout) {
    VulkanUtils::ChangeImageLayoutInfo info{
        .src_stage = vk::PipelineStageFlagBits::eTopOfPipe,
        .dst_stage = vk::PipelineStageFlagBits::eTopOfPipe,
        .src_access = {},
        .dst_access = {},
    };

    if (old_layout == vk::ImageLayout::eShaderReadOnlyOptimal) {
        info.src_stage =
            vk::PipelineStageFlagBits::eFragmentShader |
            vk::PipelineStageFlagBits::eComputeShader;
        info.src_access = vk::AccessFlagBits::eShaderRead;
    } else if (old_layout == vk::ImageLayout::eColorAttachmentOptimal) {
        info.src_stage = vk::PipelineStageFlagBits::eColorAttachmentOutput;
        info.src_access = vk::AccessFlagBits::eColorAttachmentRead |
                          vk::AccessFlagBits::eColorAttachmentWrite;
    } else if (old_layout == vk::ImageLayout::eDepthAttachmentOptimal) {
        // Dynamic-rendering depth resolve completes in
        // COLOR_ATTACHMENT_OUTPUT even though both images use a depth
        // attachment layout. Cover both the depth test access and the
        // single-sample resolve write.
        info.src_stage = vk::PipelineStageFlagBits::eLateFragmentTests |
                         vk::PipelineStageFlagBits::eColorAttachmentOutput;
        info.src_access = vk::AccessFlagBits::eDepthStencilAttachmentRead |
                          vk::AccessFlagBits::eDepthStencilAttachmentWrite |
                          vk::AccessFlagBits::eColorAttachmentWrite;
    } else if (old_layout == vk::ImageLayout::eGeneral) {
        info.src_stage = vk::PipelineStageFlagBits::eComputeShader;
        info.src_access = vk::AccessFlagBits::eShaderRead |
                          vk::AccessFlagBits::eShaderWrite;
    } else if (old_layout == vk::ImageLayout::eTransferSrcOptimal) {
        info.src_stage = vk::PipelineStageFlagBits::eTransfer;
        info.src_access = vk::AccessFlagBits::eTransferRead;
    } else if (old_layout == vk::ImageLayout::eTransferDstOptimal) {
        info.src_stage = vk::PipelineStageFlagBits::eTransfer;
        info.src_access = vk::AccessFlagBits::eTransferWrite;
    } else if (old_layout ==
               vk::ImageLayout::eRenderingLocalReadKHR) {
        info.src_stage =
            vk::PipelineStageFlagBits::eFragmentShader |
            vk::PipelineStageFlagBits::
                eColorAttachmentOutput |
            vk::PipelineStageFlagBits::
                eEarlyFragmentTests |
            vk::PipelineStageFlagBits::
                eLateFragmentTests;
        info.src_access =
            vk::AccessFlagBits::eInputAttachmentRead |
            vk::AccessFlagBits::eColorAttachmentRead |
            vk::AccessFlagBits::eColorAttachmentWrite |
            vk::AccessFlagBits::
                eDepthStencilAttachmentRead |
            vk::AccessFlagBits::
                eDepthStencilAttachmentWrite;
    }

    if (new_layout == vk::ImageLayout::eShaderReadOnlyOptimal) {
        info.dst_stage =
            vk::PipelineStageFlagBits::eFragmentShader |
            vk::PipelineStageFlagBits::eComputeShader;
        info.dst_access = vk::AccessFlagBits::eShaderRead;
    } else if (new_layout == vk::ImageLayout::eColorAttachmentOptimal) {
        info.dst_stage = vk::PipelineStageFlagBits::eColorAttachmentOutput;
        info.dst_access = vk::AccessFlagBits::eColorAttachmentRead |
                          vk::AccessFlagBits::eColorAttachmentWrite;
    } else if (new_layout == vk::ImageLayout::eDepthAttachmentOptimal) {
        info.dst_stage = vk::PipelineStageFlagBits::eEarlyFragmentTests |
                         vk::PipelineStageFlagBits::eColorAttachmentOutput;
        info.dst_access = vk::AccessFlagBits::eDepthStencilAttachmentRead |
                          vk::AccessFlagBits::eDepthStencilAttachmentWrite |
                          vk::AccessFlagBits::eColorAttachmentWrite;
    } else if (new_layout == vk::ImageLayout::eGeneral) {
        info.dst_stage = vk::PipelineStageFlagBits::eComputeShader;
        info.dst_access = vk::AccessFlagBits::eShaderRead |
                          vk::AccessFlagBits::eShaderWrite;
    } else if (new_layout == vk::ImageLayout::eTransferSrcOptimal) {
        info.dst_stage = vk::PipelineStageFlagBits::eTransfer;
        info.dst_access = vk::AccessFlagBits::eTransferRead;
    } else if (new_layout == vk::ImageLayout::eTransferDstOptimal) {
        info.dst_stage = vk::PipelineStageFlagBits::eTransfer;
        info.dst_access = vk::AccessFlagBits::eTransferWrite;
    } else if (new_layout ==
               vk::ImageLayout::eRenderingLocalReadKHR) {
        info.dst_stage =
            vk::PipelineStageFlagBits::eFragmentShader |
            vk::PipelineStageFlagBits::
                eColorAttachmentOutput |
            vk::PipelineStageFlagBits::
                eEarlyFragmentTests |
            vk::PipelineStageFlagBits::
                eLateFragmentTests;
        info.dst_access =
            vk::AccessFlagBits::eInputAttachmentRead |
            vk::AccessFlagBits::eColorAttachmentRead |
            vk::AccessFlagBits::eColorAttachmentWrite |
            vk::AccessFlagBits::
                eDepthStencilAttachmentRead |
            vk::AccessFlagBits::
                eDepthStencilAttachmentWrite;
    }

    return info;
}

std::uint64_t layoutKey(GlobalRenderTargetId rt_id, std::uint32_t surface,
                        RenderTargetImageKind kind) {
    return (static_cast<std::uint64_t>(
                static_cast<std::uint32_t>(rt_id.value))
            << 2u) |
           (static_cast<std::uint64_t>(surface) << 1u) |
           static_cast<std::uint64_t>(kind);
}

} // namespace

void RenderTargetLayoutTracker::transition(
    vk::CommandBuffer cmd_buf, RenderTargetContainer &rt_container,
    VulkanUtils &vk_utils, GlobalRenderTargetId rt_id,
    vk::ImageLayout new_layout, bool history_read,
    RenderTargetImageKind image_kind) {
    if (isSpecialRenderTarget(rt_id)) return;
    if (image_kind == RenderTargetImageKind::attachment &&
        !rt_container.hasSeparateAttachment(rt_id)) {
        image_kind = RenderTargetImageKind::resolved;
    }

    const auto surface = rt_container.surfaceIndex(rt_id, history_read);
    const auto key = layoutKey(rt_id, surface, image_kind);
    if (const auto alias_group =
            rt_container.aliasGroup(rt_id)) {
        const auto pending =
            alias_groups_requiring_dependency.erase(
                *alias_group) != 0;
        auto [active, inserted] =
            active_alias_resources.try_emplace(
                *alias_group, key);
        const auto switched =
            !inserted && active->second != key;
        if (pending || switched) {
            vk::MemoryBarrier barrier;
            barrier.srcAccessMask =
                vk::AccessFlagBits::eMemoryRead |
                vk::AccessFlagBits::eMemoryWrite;
            barrier.dstAccessMask =
                vk::AccessFlagBits::eMemoryRead |
                vk::AccessFlagBits::eMemoryWrite;
            cmd_buf.pipelineBarrier(
                vk::PipelineStageFlagBits::eAllCommands,
                vk::PipelineStageFlagBits::eAllCommands,
                {}, {barrier}, {}, {});
            if (switched) {
                layouts[active->second] =
                    vk::ImageLayout::eUndefined;
            }
            layouts[key] =
                vk::ImageLayout::eUndefined;
            ++alias_dependency_count;
        }
        active->second = key;
    }
    auto [it, inserted] = layouts.try_emplace(
        key, rt_container.initialLayout(
                 rt_id, image_kind == RenderTargetImageKind::attachment));
    (void)inserted;
    const auto old_layout = it->second;
    if (old_layout == new_layout) return;

    const auto &image =
        image_kind == RenderTargetImageKind::attachment
            ? rt_container.getAttachmentImage(rt_id, history_read)
            : rt_container.getImage(rt_id, history_read);
    vk_utils.changeImageLayoutCmd(
        cmd_buf, image, old_layout, new_layout,
        makeTransitionInfo(old_layout, new_layout));
    it->second = new_layout;
}

void RenderTargetLayoutTracker::memoryDependency(
    vk::CommandBuffer cmd_buf, RenderTargetContainer &rt_container,
    VulkanUtils &vk_utils, GlobalRenderTargetId rt_id, bool history_read) {
    if (isSpecialRenderTarget(rt_id)) return;

    const auto surface = rt_container.surfaceIndex(rt_id, history_read);
    const auto dependency = [&](RenderTargetImageKind kind,
                                bool require_preceding_access) {
        const auto key = layoutKey(rt_id, surface, kind);
        auto it = layouts.find(key);
        if (it == layouts.end()) {
            const auto initial_layout = rt_container.initialLayout(
                rt_id, kind == RenderTargetImageKind::attachment);
            if (!require_preceding_access &&
                initial_layout == vk::ImageLayout::eUndefined) {
                return;
            }
            it = layouts.emplace(key, initial_layout).first;
        }
        const auto layout = it->second;
        if (layout == vk::ImageLayout::eUndefined) {
            if (!require_preceding_access) return;
            throw std::logic_error(
                "frame graph image dependency has no preceding tracked image access");
        }
        const auto &image =
            kind == RenderTargetImageKind::attachment
                ? rt_container.getAttachmentImage(rt_id, history_read)
                : rt_container.getImage(rt_id, history_read);
        vk_utils.changeImageLayoutCmd(cmd_buf, image, layout, layout,
                                      makeTransitionInfo(layout, layout));
        ++memory_dependency_count;
    };

    dependency(RenderTargetImageKind::resolved, true);
    if (rt_container.hasSeparateAttachment(rt_id)) {
        // Compute and transfer work can access only the resolved surface.
        // Barrier the multisample surface when a preceding raster access
        // actually created it, but do not invent an attachment dependency.
        dependency(RenderTargetImageKind::attachment, false);
    }
}

vk::ImageLayout RenderTargetLayoutTracker::currentLayout(
    GlobalRenderTargetId rt_id, bool history_read,
    const RenderTargetContainer *rt_container,
    RenderTargetImageKind image_kind) const {
    if (rt_container != nullptr &&
        image_kind == RenderTargetImageKind::attachment &&
        !rt_container->hasSeparateAttachment(rt_id)) {
        image_kind = RenderTargetImageKind::resolved;
    }
    const auto surface =
        rt_container != nullptr
            ? rt_container->surfaceIndex(rt_id, history_read)
            : 0u;
    const auto key = layoutKey(rt_id, surface, image_kind);
    if (auto it = layouts.find(key); it != layouts.end()) {
        return it->second;
    }
    return rt_container != nullptr
               ? rt_container->initialLayout(
                     rt_id,
                     image_kind == RenderTargetImageKind::attachment)
               : vk::ImageLayout::eUndefined;
}

void RenderTargetLayoutTracker::reset() {
    for (const auto &[group, resource] :
         active_alias_resources) {
        (void)resource;
        alias_groups_requiring_dependency.insert(group);
    }
    active_alias_resources.clear();
    layouts.clear();
    memory_dependency_count = 0;
    alias_dependency_count = 0;
}

} // namespace Pelican
