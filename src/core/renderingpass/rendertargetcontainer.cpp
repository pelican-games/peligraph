#include "rendertargetcontainer.hpp"
#include "../vkcore/deletionqueue.hpp"
#include "../vkcore/core.hpp"
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <utility>

namespace Pelican {

namespace {

struct RetiredRenderTargetResources {
    ImageWrapper image;
    vk::UniqueImageView image_view;
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
                                                                 vma::MemoryUsage memUsage) {
    // Reuse an existing target when config registration is called more than once.
    if (auto it = name_to_id.find(name); it != name_to_id.end()) {
        return it->second;
    }

    ImageWrapper image =
        createRenderTargetImage(name, base_extent, extent_scale, fixed_extent, format, usage, memUsage);
    auto image_view = createImageView(device, image);

    GlobalRenderTargetId id = render_targets.reg(InternalRenderTarget{
        .name = name,
        .format_class = format_class,
        .role = role,
        .extent_scale = extent_scale,
        .fixed_extent = fixed_extent,
        .format = format,
        .usage = usage,
        .memory_usage = memUsage,
        .image = std::move(image),
        .image_view = std::move(image_view),
    });

    name_to_id.emplace(name, id);
    return id;
}

void RenderTargetContainer::recreateForExtent(vk::Extent2D base_extent) {
    for (const auto &[name, id] : name_to_id) {
        auto &rt = render_targets.get(id);
        auto next_image = createRenderTargetImage(rt.name, base_extent, rt.extent_scale,
                                                  rt.fixed_extent, rt.format, rt.usage, rt.memory_usage);
        auto next_view = createImageView(device, next_image);

        GET_MODULE(DeletionQueue)
            .defer(RetiredRenderTargetResources{
                .image = std::move(rt.image),
                .image_view = std::move(rt.image_view),
            });

        rt.image = std::move(next_image);
        rt.image_view = std::move(next_view);
    }
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
        rt.image.format,
        vk::Extent2D{rt.image.extent.width, rt.image.extent.height},
    };
}

const ImageWrapper &RenderTargetContainer::getImage(GlobalRenderTargetId id) const {
    return render_targets.get(id).image;
}

vk::ImageView RenderTargetContainer::getImageView(GlobalRenderTargetId id) const {
    return render_targets.get(id).image_view.get();
}

} // namespace Pelican
