#include "render_target_layout_tracker.hpp"
#include "../renderingpass/rendertargetcontainer.hpp"
#include <stdexcept>

namespace Pelican {

namespace {

VulkanUtils::ChangeImageLayoutInfo makeTransitionInfo(
    vk::ImageLayout old_layout, vk::ImageLayout new_layout,
    vk::PipelineStageFlags source_shader_stages,
    vk::PipelineStageFlags destination_shader_stages) {
    VulkanUtils::ChangeImageLayoutInfo info{
        .src_stage = vk::PipelineStageFlagBits::eTopOfPipe,
        .dst_stage = vk::PipelineStageFlagBits::eTopOfPipe,
        .src_access = {},
        .dst_access = {},
    };

    if (old_layout == vk::ImageLayout::eShaderReadOnlyOptimal) {
        info.src_stage = source_shader_stages;
        info.src_access = vk::AccessFlagBits::eShaderRead;
    } else if (old_layout == vk::ImageLayout::eColorAttachmentOptimal) {
        info.src_stage = vk::PipelineStageFlagBits::eColorAttachmentOutput;
        info.src_access = vk::AccessFlagBits::eColorAttachmentRead |
                          vk::AccessFlagBits::eColorAttachmentWrite;
    } else if (old_layout == vk::ImageLayout::eDepthAttachmentOptimal ||
               // PSN-1: a depth format with stencil (both aspects).
               old_layout ==
                   vk::ImageLayout::eDepthStencilAttachmentOptimal) {
        // Dynamic-rendering depth resolve completes in
        // COLOR_ATTACHMENT_OUTPUT even though both images use a depth
        // attachment layout. Cover both the depth test access and the
        // single-sample resolve write.
        info.src_stage = vk::PipelineStageFlagBits::eEarlyFragmentTests |
                         vk::PipelineStageFlagBits::eLateFragmentTests |
                         vk::PipelineStageFlagBits::eColorAttachmentOutput;
        info.src_access = vk::AccessFlagBits::eDepthStencilAttachmentRead |
                          vk::AccessFlagBits::eDepthStencilAttachmentWrite |
                          vk::AccessFlagBits::eColorAttachmentWrite;
    } else if (old_layout == vk::ImageLayout::eGeneral) {
        info.src_stage = source_shader_stages;
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
        info.dst_stage = destination_shader_stages;
        info.dst_access = vk::AccessFlagBits::eShaderRead;
    } else if (new_layout == vk::ImageLayout::eColorAttachmentOptimal) {
        info.dst_stage = vk::PipelineStageFlagBits::eColorAttachmentOutput;
        info.dst_access = vk::AccessFlagBits::eColorAttachmentRead |
                          vk::AccessFlagBits::eColorAttachmentWrite;
    } else if (new_layout == vk::ImageLayout::eDepthAttachmentOptimal ||
               new_layout ==
                   vk::ImageLayout::eDepthStencilAttachmentOptimal) {
        info.dst_stage = vk::PipelineStageFlagBits::eEarlyFragmentTests |
                         vk::PipelineStageFlagBits::eLateFragmentTests |
                         vk::PipelineStageFlagBits::eColorAttachmentOutput;
        info.dst_access = vk::AccessFlagBits::eDepthStencilAttachmentRead |
                          vk::AccessFlagBits::eDepthStencilAttachmentWrite |
                          vk::AccessFlagBits::eColorAttachmentWrite;
    } else if (new_layout == vk::ImageLayout::eGeneral) {
        info.dst_stage = destination_shader_stages;
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
    RenderTargetImageKind image_kind,
    vk::PipelineStageFlags shader_stages) {
    recordAccess(cmd_buf, rt_container, vk_utils, rt_id, new_layout,
                 history_read, image_kind, shader_stages, false);
}

void RenderTargetLayoutTracker::recordAccess(
    vk::CommandBuffer cmd_buf, RenderTargetContainer &rt_container,
    VulkanUtils &vk_utils, GlobalRenderTargetId rt_id,
    vk::ImageLayout new_layout, bool history_read,
    RenderTargetImageKind image_kind,
    vk::PipelineStageFlags shader_stages, bool memory_dependency) {
    if (isSpecialRenderTarget(rt_id)) return;
    if (image_kind == RenderTargetImageKind::attachment &&
        !rt_container.hasSeparateAttachment(rt_id)) {
        image_kind = RenderTargetImageKind::resolved;
    }

    const RenderTargetRead read{.previous = history_read};
    const auto surface = rt_container.surfaceIndex(rt_id, read);
    const auto key = layoutKey(rt_id, surface, image_kind);
    forgetRecreatedPersistentImage(rt_container, rt_id, key, image_kind, history_read);
    notePersistentImage(rt_container, rt_id, key, image_kind, history_read);
    std::optional<AliasTransition> observation;
    const auto &physical_image=image_kind == RenderTargetImageKind::attachment
        ? rt_container.getAttachmentImage(rt_id,read) : rt_container.getImage(rt_id,read);
    if (physical_image.region_bound) {
        const auto region=reinterpret_cast<std::uintptr_t>(physical_image.allocation.get());
        auto &current=alias_ranges[key];
        if(current.image!=static_cast<VkImage>(physical_image.image.get())) {
            current={region,physical_image.memory_offset,physical_image.memory_size,0,static_cast<VkImage>(physical_image.image.get())};
            frame_accessed.erase(key); layouts.erase(key); shader_access_stages.erase(key);
        }
        const auto overlaps=[&](const RangeAccess &other) {
            return current.region==other.region && current.offset<other.offset+other.size && other.offset<current.offset+current.size;
        };
        const bool first_access=!frame_accessed.contains(key);
        bool reacquire=false;
        for(const auto &[other_key,other]:alias_ranges)
            if(other_key!=key && overlaps(other) && other.access>current.access) reacquire=true;
        observation=AliasTransition{region,rt_id,false,{},{},{},{},{},new_layout};
        if(first_access || reacquire) {
            observation->dependency=true;
            observation->src_stage=observation->dst_stage=vk::PipelineStageFlagBits::eAllCommands;
            observation->src_access=observation->dst_access=vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite;
            vk::MemoryBarrier barrier{observation->src_access,observation->dst_access};
            cmd_buf.pipelineBarrier(observation->src_stage,observation->dst_stage,{}, {barrier},{},{});
            layouts[key]=vk::ImageLayout::eUndefined; shader_access_stages.erase(key);
            for(const auto &[other_key,other]:alias_ranges) if(other_key!=key && overlaps(other)) {
                layouts[other_key]=vk::ImageLayout::eUndefined; shader_access_stages.erase(other_key);
            }
            ++alias_dependency_count;
            ++frame_alias_dependency_count;
        }
        current.access=++access_serial;
        frame_accessed.insert(key);
    }
    auto [it, inserted] = layouts.try_emplace(
        key, rt_container.initialLayout(
                 rt_id, image_kind == RenderTargetImageKind::attachment));
    (void)inserted;
    const auto old_layout = it->second;
    if (observation && alias_transition_observer) {
        observation->old_layout = old_layout;
        alias_transition_observer(*observation);
    }
    if (old_layout == new_layout && !memory_dependency) return;
    const auto old_shader_stages =
        shader_access_stages.contains(key)
            ? shader_access_stages.at(key)
            : shader_stages;

    const auto &image =
        image_kind == RenderTargetImageKind::attachment
            ? rt_container.getAttachmentImage(rt_id, read)
            : rt_container.getImage(rt_id, read);
    vk_utils.changeImageLayoutCmd(
        cmd_buf, image, old_layout, new_layout,
        makeTransitionInfo(old_layout, new_layout,
                           old_shader_stages,
                           shader_stages));
    it->second = new_layout;
    if (new_layout == vk::ImageLayout::eGeneral ||
        new_layout == vk::ImageLayout::eShaderReadOnlyOptimal) {
        shader_access_stages[key] = shader_stages;
    } else {
        shader_access_stages.erase(key);
    }
}

void RenderTargetLayoutTracker::memoryDependency(
    vk::CommandBuffer cmd_buf, RenderTargetContainer &rt_container,
    VulkanUtils &vk_utils, GlobalRenderTargetId rt_id,
    bool history_read, vk::PipelineStageFlags shader_stages) {
    if (isSpecialRenderTarget(rt_id)) return;

    const RenderTargetRead read{.previous = history_read};
    const auto surface = rt_container.surfaceIndex(rt_id, read);
    const auto dependency = [&](RenderTargetImageKind kind,
                                bool require_preceding_access) {
        const auto key = layoutKey(rt_id, surface, kind);
        forgetRecreatedPersistentImage(rt_container, rt_id, key, kind, history_read);
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
        recordAccess(cmd_buf, rt_container, vk_utils, rt_id, layout,
                     history_read, kind, shader_stages, true);
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
            ? rt_container->surfaceIndex(
                  rt_id, RenderTargetRead{.previous = history_read})
            : 0u;
    const auto key = layoutKey(rt_id, surface, image_kind);
    if (auto it = layouts.find(key); it != layouts.end()) {
        // PS-1: a persistent target made anew since it was tracked.
        const auto persistent = persistent_images.find(key);
        if (rt_container == nullptr || persistent == persistent_images.end() ||
            persistent->second ==
                static_cast<VkImage>(
                    (image_kind == RenderTargetImageKind::attachment
                         ? rt_container->getAttachmentImage(
                               rt_id, RenderTargetRead{.previous = history_read})
                         : rt_container->getImage(
                               rt_id, RenderTargetRead{.previous = history_read}))
                        .image.get())) {
            return it->second;
        }
    }
    return rt_container != nullptr
               ? rt_container->initialLayout(
                     rt_id,
                     image_kind == RenderTargetImageKind::attachment)
               : vk::ImageLayout::eUndefined;
}

void RenderTargetLayoutTracker::assumeLayout(
    RenderTargetContainer &rt_container,
    GlobalRenderTargetId rt_id,
    vk::ImageLayout layout,
    bool history_read,
    RenderTargetImageKind image_kind) {
    if (isSpecialRenderTarget(rt_id)) return;
    if (image_kind == RenderTargetImageKind::attachment &&
        !rt_container.hasSeparateAttachment(rt_id)) {
        image_kind = RenderTargetImageKind::resolved;
    }
    const auto surface =
        rt_container.surfaceIndex(
            rt_id, RenderTargetRead{.previous = history_read});
    const auto key =
        layoutKey(rt_id, surface, image_kind);
    forgetRecreatedPersistentImage(rt_container, rt_id, key, image_kind, history_read);
    notePersistentImage(rt_container, rt_id, key, image_kind, history_read);
    layouts[key] = layout;
    shader_access_stages.erase(key);
}

void RenderTargetLayoutTracker::beginFrame() {
    frame_alias_dependency_count=0;
    frame_accessed.clear();
}

void RenderTargetLayoutTracker::reset() {
    frame_accessed.clear();
    alias_ranges.clear();
    access_serial=0;
    // PS-1: a persistent target keeps its contents through a reset (a
    // generation commit, a history reset, a view-family change). Its layout
    // stays known; a target made anew is caught by its image on next use.
    std::unordered_map<std::uint64_t, vk::ImageLayout> kept_layouts;
    std::unordered_map<std::uint64_t, vk::PipelineStageFlags> kept_stages;
    for (const auto &[key, image] : persistent_images) {
        (void)image;
        if (const auto found = layouts.find(key); found != layouts.end())
            kept_layouts.emplace(key, found->second);
        if (const auto found = shader_access_stages.find(key);
            found != shader_access_stages.end())
            kept_stages.emplace(key, found->second);
    }
    layouts = std::move(kept_layouts);
    shader_access_stages = std::move(kept_stages);
    memory_dependency_count = 0;
    alias_dependency_count = 0;
    frame_alias_dependency_count = 0;
}

void RenderTargetLayoutTracker::forgetRecreatedPersistentImage(
    const RenderTargetContainer &rt_container,
    GlobalRenderTargetId rt_id, std::uint64_t key,
    RenderTargetImageKind image_kind, bool history_read) {
    const auto found = persistent_images.find(key);
    if (found == persistent_images.end()) return;
    const RenderTargetRead read{.previous = history_read};
    const auto image = static_cast<VkImage>(
        (image_kind == RenderTargetImageKind::attachment
             ? rt_container.getAttachmentImage(rt_id, read)
             : rt_container.getImage(rt_id, read))
            .image.get());
    if (found->second == image) return;
    persistent_images.erase(found);
    layouts.erase(key);
    shader_access_stages.erase(key);
}

void RenderTargetLayoutTracker::notePersistentImage(
    const RenderTargetContainer &rt_container,
    GlobalRenderTargetId rt_id, std::uint64_t key,
    RenderTargetImageKind image_kind, bool history_read) {
    if (!rt_container.persistent(rt_id)) return;
    const RenderTargetRead read{.previous = history_read};
    persistent_images[key] = static_cast<VkImage>(
        (image_kind == RenderTargetImageKind::attachment
             ? rt_container.getAttachmentImage(rt_id, read)
             : rt_container.getImage(rt_id, read))
            .image.get());
}

} // namespace Pelican
