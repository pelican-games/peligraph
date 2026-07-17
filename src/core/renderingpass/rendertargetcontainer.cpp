#include "rendertargetcontainer.hpp"
#include "../vkcore/deletionqueue.hpp"
#include "../vkcore/core.hpp"
#include "../vkcore/util.hpp"
#include <array>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <utility>

namespace Pelican {

namespace {

struct RetiredRenderTargetResources {
    std::array<ImageWrapper, 2> images;
    std::array<vk::UniqueImageView, 2> image_views;
};

vk::Extent2D resolveRenderTargetExtent(const std::string &name, vk::Extent2D base_extent, float extent_scale,
                                       std::optional<vk::Extent2D> fixed_extent) {
    const vk::Extent2D extent = fixed_extent.value_or(vk::Extent2D{
        static_cast<uint32_t>(base_extent.width * extent_scale),
        static_cast<uint32_t>(base_extent.height * extent_scale),
    });

    if (extent.width == 0 || extent.height == 0) {
        throw std::runtime_error("Render target extent became zero-sized: " + name);
    }
    return extent;
}

static vk::UniqueImageView createImageView(vk::Device device, const ImageWrapper &image) {
    vk::ImageViewCreateInfo ci;
    ci.image = image.image.get();
    ci.viewType = vk::ImageViewType::e2D;
    ci.format = image.format;
    ci.components = {
        vk::ComponentSwizzle::eR,
        vk::ComponentSwizzle::eG,
        vk::ComponentSwizzle::eB,
        vk::ComponentSwizzle::eA,
    };

    // Pick the aspect mask from the target format.
    if (image.format == vk::Format::eD32Sfloat ||
        image.format == vk::Format::eD16Unorm ||
        image.format == vk::Format::eD24UnormS8Uint ||
        image.format == vk::Format::eD32SfloatS8Uint) {
        ci.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eDepth;
    } else {
        ci.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
    }

    ci.subresourceRange.baseMipLevel = 0;
    ci.subresourceRange.levelCount = 1;
    ci.subresourceRange.baseArrayLayer = 0;
    ci.subresourceRange.layerCount = 1;
    return device.createImageViewUnique(ci);
}

void nameRenderTargetSurfaces(const std::string &name,
                              const std::array<ImageWrapper, 2> &images,
                              const std::array<vk::UniqueImageView, 2> &image_views,
                              std::uint32_t surface_count) {
    const auto &debug_utils = GET_MODULE(VulkanManageCore).getDebugUtils();
    for (std::uint32_t surface = 0; surface < surface_count; ++surface) {
        const auto base = "rt/" + name + "/surface/" + std::to_string(surface);
        debug_utils.nameImage(images[surface].image.get(), (base + "/image").c_str());
        debug_utils.nameImageView(image_views[surface].get(), (base + "/view").c_str());
    }
}

ImageWrapper createRenderTargetImage(const std::string &name, vk::Extent2D base_extent, float extent_scale,
                                     std::optional<vk::Extent2D> fixed_extent,
                                     vk::Format format, vk::ImageUsageFlags usage,
                                     vma::MemoryUsage memory_usage) {
    const auto &vkcore = GET_MODULE(VulkanManageCore);
    const auto features = vkcore.getPhysDevice().getFormatProperties(format).optimalTilingFeatures;
    vk::FormatFeatureFlags required;
    if (usage & vk::ImageUsageFlagBits::eColorAttachment) {
        required |= vk::FormatFeatureFlagBits::eColorAttachment;
        if (format == vk::Format::eR8G8B8A8Srgb || format == vk::Format::eB8G8R8A8Srgb) {
            required |= vk::FormatFeatureFlagBits::eColorAttachmentBlend;
        }
    }
    if (usage & vk::ImageUsageFlagBits::eSampled) {
        required |= vk::FormatFeatureFlagBits::eSampledImage |
                    vk::FormatFeatureFlagBits::eSampledImageFilterLinear;
    }
    if ((features & required) != required) {
        throw std::runtime_error("Render target format lacks required color capability: " + name);
    }
    const auto extent = resolveRenderTargetExtent(name, base_extent, extent_scale, fixed_extent);
    return vkcore.allocImage(vk::Extent3D{extent.width, extent.height, 1}, format, usage,
                             memory_usage, {});
}

void clearHistoryImages(const std::array<ImageWrapper, 2> &images,
                        const vk::ClearColorValue &clear_color) {
    GET_MODULE(VulkanUtils).executeOneTimeCmd(
        [&](vk::CommandBuffer cmd_buf) {
            for (const auto &image : images) {
                VulkanUtils::ChangeImageLayoutInfo to_clear{
                    .src_stage = vk::PipelineStageFlagBits::eTopOfPipe,
                    .dst_stage = vk::PipelineStageFlagBits::eTransfer,
                    .src_access = {},
                    .dst_access = vk::AccessFlagBits::eTransferWrite,
                };
                GET_MODULE(VulkanUtils).changeImageLayoutCmd(
                    cmd_buf, image, vk::ImageLayout::eUndefined,
                    vk::ImageLayout::eTransferDstOptimal, to_clear);
                const vk::ImageSubresourceRange range{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
                cmd_buf.clearColorImage(image.image.get(), vk::ImageLayout::eTransferDstOptimal,
                                        clear_color, range);
                VulkanUtils::ChangeImageLayoutInfo to_read{
                    .src_stage = vk::PipelineStageFlagBits::eTransfer,
                    .dst_stage = vk::PipelineStageFlagBits::eFragmentShader,
                    .src_access = vk::AccessFlagBits::eTransferWrite,
                    .dst_access = vk::AccessFlagBits::eShaderRead,
                };
                GET_MODULE(VulkanUtils).changeImageLayoutCmd(
                    cmd_buf, image, vk::ImageLayout::eTransferDstOptimal,
                    vk::ImageLayout::eShaderReadOnlyOptimal, to_read);
            }
        },
        true);
}

} // namespace

RenderTargetContainer::RenderTargetContainer() : device{GET_MODULE(VulkanManageCore).getDevice()} {}

RenderTargetContainer::~RenderTargetContainer() {}

GlobalRenderTargetId RenderTargetContainer::registerRenderTarget(const std::string &name,
                                                                 vk::Extent2D base_extent,
                                                                 const std::string &format_class,
                                                                 const std::string &role,
                                                                 float extent_scale,
                                                                 std::optional<vk::Extent2D> fixed_extent,
                                                                 vk::Format format,
                                                                 vk::ImageUsageFlags usage,
                                                                 vma::MemoryUsage memUsage,
                                                                 bool history,
                                                                 vk::ClearColorValue history_clear_color) {
    // Reuse an existing target when config registration is called more than once.
    if (auto it = name_to_id.find(name); it != name_to_id.end()) {
        return it->second;
    }

    if (history && !(usage & vk::ImageUsageFlagBits::eSampled)) {
        throw std::runtime_error("History render target requires SAMPLED usage: " + name);
    }
    if (history && !(usage & vk::ImageUsageFlagBits::eColorAttachment)) {
        throw std::runtime_error("History render target requires COLOR_ATTACHMENT usage: " + name);
    }
    if (history) usage |= vk::ImageUsageFlagBits::eTransferDst;
    std::array<ImageWrapper, 2> images;
    std::array<vk::UniqueImageView, 2> image_views;
    const uint32_t surface_count = history ? 2u : 1u;
    for (uint32_t i = 0; i < surface_count; ++i) {
        images[i] = createRenderTargetImage(name, base_extent, extent_scale, fixed_extent,
                                            format, usage, memUsage);
        image_views[i] = createImageView(device, images[i]);
    }
    nameRenderTargetSurfaces(name, images, image_views, surface_count);
    if (history) clearHistoryImages(images, history_clear_color);

    GlobalRenderTargetId id = render_targets.reg(InternalRenderTarget{
        .name = name,
        .format_class = format_class,
        .role = role,
        .extent_scale = extent_scale,
        .fixed_extent = fixed_extent,
        .format = format,
        .usage = usage,
        .memory_usage = memUsage,
        .history = history,
        .history_clear_color = history_clear_color,
        .images = std::move(images),
        .image_views = std::move(image_views),
    });

    name_to_id.emplace(name, id);
    return id;
}

void RenderTargetContainer::recreateForExtent(vk::Extent2D base_extent) {
    for (const auto &[name, id] : name_to_id) {
        auto &rt = render_targets.get(id);
        std::array<ImageWrapper, 2> next_images;
        std::array<vk::UniqueImageView, 2> next_views;
        const uint32_t surface_count = rt.history ? 2u : 1u;
        for (uint32_t i = 0; i < surface_count; ++i) {
            next_images[i] = createRenderTargetImage(rt.name, base_extent, rt.extent_scale,
                                                     rt.fixed_extent, rt.format, rt.usage,
                                                     rt.memory_usage);
            next_views[i] = createImageView(device, next_images[i]);
        }
        nameRenderTargetSurfaces(rt.name, next_images, next_views, surface_count);
        if (rt.history) clearHistoryImages(next_images, rt.history_clear_color);

        GET_MODULE(DeletionQueue)
            .defer(RetiredRenderTargetResources{
                .images = std::move(rt.images),
                .image_views = std::move(rt.image_views),
            });

        rt.images = std::move(next_images);
        rt.image_views = std::move(next_views);
    }
    history_frame_index = 0;
}

void RenderTargetContainer::resetHistory() {
    GET_MODULE(VulkanManageCore).waitIdle();
    for (const auto &[name, id] : name_to_id) {
        (void)name;
        auto &rt = render_targets.get(id);
        if (rt.history) clearHistoryImages(rt.images, rt.history_clear_color);
    }
    history_frame_index = 0;
}

void RenderTargetContainer::advanceHistoryFrame() {
    history_frame_index ^= 1u;
}

uint32_t RenderTargetContainer::surfaceIndex(GlobalRenderTargetId id, bool history_read) const {
    const auto &rt = render_targets.get(id);
    if (!rt.history) return 0;
    return history_read ? (history_frame_index ^ 1u) : history_frame_index;
}

GlobalRenderTargetId RenderTargetContainer::getRenderTargetIdByName(const std::string &name) const {
    if (auto it = name_to_id.find(name); it != name_to_id.end()) {
        return it->second;
    }
    return noRenderTargetId();
}

RenderTargetMetadata RenderTargetContainer::getMetadata(GlobalRenderTargetId id) const {
    const auto &rt = render_targets.get(id);
    return RenderTargetMetadata{
        rt.name,
        rt.usage,
        rt.format,
        vk::Extent2D{rt.images[0].extent.width, rt.images[0].extent.height},
        rt.history,
    };
}

const ImageWrapper &RenderTargetContainer::getImage(GlobalRenderTargetId id, bool history_read) const {
    return render_targets.get(id).images[surfaceIndex(id, history_read)];
}

const ImageWrapper &RenderTargetContainer::getImageForFrame(GlobalRenderTargetId id, bool history_read,
                                                            uint32_t frame_index) const {
    const auto &rt = render_targets.get(id);
    const uint32_t index = rt.history ? ((frame_index & 1u) ^ (history_read ? 1u : 0u)) : 0u;
    return rt.images[index];
}

vk::ImageView RenderTargetContainer::getImageView(GlobalRenderTargetId id, bool history_read) const {
    return render_targets.get(id).image_views[surfaceIndex(id, history_read)].get();
}

vk::ImageView RenderTargetContainer::getImageViewForFrame(GlobalRenderTargetId id, bool history_read,
                                                          uint32_t frame_index) const {
    const auto &rt = render_targets.get(id);
    const uint32_t index = rt.history ? ((frame_index & 1u) ^ (history_read ? 1u : 0u)) : 0u;
    return rt.image_views[index].get();
}

vk::ImageLayout RenderTargetContainer::initialLayout(GlobalRenderTargetId id) const {
    return render_targets.get(id).history ? vk::ImageLayout::eShaderReadOnlyOptimal
                                          : vk::ImageLayout::eUndefined;
}

} // namespace Pelican
