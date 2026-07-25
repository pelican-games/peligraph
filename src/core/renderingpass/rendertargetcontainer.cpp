#include "rendertargetcontainer.hpp"
#include "renderingsamplecount.hpp"
#include "../vkcore/deletionqueue.hpp"
#include "../vkcore/core.hpp"
#include "../vkcore/util.hpp"
#include <array>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace Pelican {

namespace {

struct RetiredRenderTargetResources {
    std::array<ImageWrapper, 2> images;
    std::array<std::vector<vk::UniqueImageView>, 2>
        image_layer_views;
    std::array<vk::UniqueImageView, 2>
        layered_image_views;
    std::array<ImageWrapper, 2> attachment_images;
    std::array<std::vector<vk::UniqueImageView>, 2>
        attachment_image_layer_views;
    std::array<vk::UniqueImageView, 2>
        layered_attachment_image_views;
};

vk::SampleCountFlagBits toSampleCount(std::uint32_t samples) {
    switch (samples) {
    case 1: return vk::SampleCountFlagBits::e1;
    case 2: return vk::SampleCountFlagBits::e2;
    case 4: return vk::SampleCountFlagBits::e4;
    case 8: return vk::SampleCountFlagBits::e8;
    case 16: return vk::SampleCountFlagBits::e16;
    case 32: return vk::SampleCountFlagBits::e32;
    case 64: return vk::SampleCountFlagBits::e64;
    default:
        throw std::runtime_error(
            "render target samples must be a power of two between 1 and 64");
    }
}

vk::ResolveModeFlagBits selectDepthResolveMode(
    vk::PhysicalDevice physical_device) {
    vk::PhysicalDeviceDepthStencilResolveProperties resolve;
    vk::PhysicalDeviceProperties2 properties;
    properties.pNext = &resolve;
    physical_device.getProperties2(&properties);
    constexpr std::array preferred{
        vk::ResolveModeFlagBits::eSampleZero,
        vk::ResolveModeFlagBits::eAverage,
        vk::ResolveModeFlagBits::eMin,
        vk::ResolveModeFlagBits::eMax,
    };
    for (const auto mode : preferred) {
        if (resolve.supportedDepthResolveModes & mode) return mode;
    }
    return vk::ResolveModeFlagBits::eNone;
}

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

static vk::UniqueImageView createImageView(
    vk::Device device, const ImageWrapper &image,
    vk::ImageViewType view_type,
    std::uint32_t base_array_layer,
    std::uint32_t layer_count) {
    if (layer_count == 0 ||
        base_array_layer >= image.array_layers ||
        layer_count >
            image.array_layers - base_array_layer) {
        throw std::runtime_error(
            "render target image view has an invalid array-layer range");
    }
    vk::ImageViewCreateInfo ci;
    ci.image = image.image.get();
    ci.viewType = view_type;
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
    ci.subresourceRange.baseArrayLayer =
        base_array_layer;
    ci.subresourceRange.layerCount = layer_count;
    return device.createImageViewUnique(ci);
}

static std::vector<vk::UniqueImageView>
createSequentialImageViews(
    vk::Device device, const ImageWrapper &image) {
    std::vector<vk::UniqueImageView> result;
    result.reserve(image.array_layers);
    for (std::uint32_t layer = 0;
         layer < image.array_layers; ++layer) {
        result.push_back(createImageView(
            device, image, vk::ImageViewType::e2D,
            layer, 1));
    }
    return result;
}

static vk::UniqueImageView createLayeredImageView(
    vk::Device device, const ImageWrapper &image) {
    if (image.array_layers == 1) return {};
    return createImageView(
        device, image, vk::ImageViewType::e2DArray, 0,
        image.array_layers);
}

void nameRenderTargetSurfaces(const std::string &name,
                              const std::array<ImageWrapper, 2> &images,
                              const std::array<std::vector<vk::UniqueImageView>, 2>
                                  &image_layer_views,
                              const std::array<vk::UniqueImageView, 2> &layered_image_views,
                              std::uint32_t surface_count) {
    const auto &debug_utils = GET_MODULE(VulkanManageCore).getDebugUtils();
    for (std::uint32_t surface = 0; surface < surface_count; ++surface) {
        const auto base = "rt/" + name + "/surface/" + std::to_string(surface);
        debug_utils.nameImage(images[surface].image.get(), (base + "/image").c_str());
        for (std::uint32_t layer = 0;
             layer < image_layer_views[surface].size();
             ++layer) {
            debug_utils.nameImageView(
                image_layer_views[surface][layer].get(),
                (base + "/layer/" + std::to_string(layer) +
                 "/view")
                    .c_str());
        }
        if (layered_image_views[surface]) {
            debug_utils.nameImageView(
                layered_image_views[surface].get(),
                (base + "/layered_view").c_str());
        }
    }
}

void nameAttachmentSurfaces(
    const std::string &name,
    const std::array<ImageWrapper, 2> &images,
    const std::array<std::vector<vk::UniqueImageView>, 2>
        &image_layer_views,
    const std::array<vk::UniqueImageView, 2> &layered_image_views,
    std::uint32_t surface_count) {
    const auto &debug_utils = GET_MODULE(VulkanManageCore).getDebugUtils();
    for (std::uint32_t surface = 0; surface < surface_count; ++surface) {
        const auto base = "rt/" + name + "/surface/" +
                          std::to_string(surface) + "/msaa";
        debug_utils.nameImage(images[surface].image.get(),
                              (base + "/image").c_str());
        for (std::uint32_t layer = 0;
             layer < image_layer_views[surface].size();
             ++layer) {
            debug_utils.nameImageView(
                image_layer_views[surface][layer].get(),
                (base + "/layer/" + std::to_string(layer) +
                 "/view")
                    .c_str());
        }
        if (layered_image_views[surface]) {
            debug_utils.nameImageView(
                layered_image_views[surface].get(),
                (base + "/layered_view").c_str());
        }
    }
}

ImageWrapper createRenderTargetImage(const std::string &name, vk::Extent2D base_extent, float extent_scale,
                                     std::optional<vk::Extent2D> fixed_extent,
                                     vk::Format format, vk::ImageUsageFlags usage,
                                     vma::MemoryUsage memory_usage,
                                     vk::SampleCountFlagBits samples =
                                         vk::SampleCountFlagBits::e1,
                                     std::uint32_t array_layers = 1) {
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
                             memory_usage, {}, VulkanProcessType::graphics,
                             {}, 1, samples, array_layers);
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
                const vk::ImageSubresourceRange range{
                    vk::ImageAspectFlagBits::eColor, 0, 1, 0,
                    image.array_layers};
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

RenderTargetContainer::RenderTargetContainer()
    : device{GET_MODULE(VulkanManageCore).getDevice()},
      depth_resolve_mode{selectDepthResolveMode(
          GET_MODULE(VulkanManageCore).getPhysDevice())} {}

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
                                                                 vk::ClearColorValue history_clear_color,
                                                                 std::uint32_t samples,
                                                                 std::uint32_t array_layers) {
    if (array_layers == 0) {
        throw std::runtime_error(
            "Render target array_layers must be greater than zero: " +
            name);
    }
    if (history && !(usage & vk::ImageUsageFlagBits::eSampled)) {
        throw std::runtime_error("History render target requires SAMPLED usage: " + name);
    }
    if (history && !(usage & vk::ImageUsageFlagBits::eColorAttachment)) {
        throw std::runtime_error("History render target requires COLOR_ATTACHMENT usage: " + name);
    }
    if (history) usage |= vk::ImageUsageFlagBits::eTransferDst;

    // Reuse an existing target when config registration is called more than once.
    if (auto it = name_to_id.find(name); it != name_to_id.end()) {
        const auto &existing = render_targets.get(it->second);
        std::string changed;
        const auto note = [&changed](std::string_view field) {
            if (!changed.empty()) changed += ", ";
            changed += field;
        };
        if (existing.format != format) note("format");
        // Variant composition may remove a reader (for example XR excludes
        // TAA) and therefore request fewer usage bits for the same target.
        // Reusing the already-created superset image is valid. Adding a bit
        // that the physical image was not created with is not.
        if ((existing.usage & usage) != usage) note("usage expansion");
        if (existing.extent_scale != extent_scale) note("extent_scale");
        if (existing.fixed_extent != fixed_extent) note("fixed_extent");
        if (existing.history != history) note("history");
        if (existing.samples != samples) note("samples");
        // A previously allocated array image is a safe physical superset for
        // a sequential/flat registration. Expanding a live image during a
        // candidate transaction is not failure-atomic, so it remains an
        // explicit restart boundary.
        if (existing.array_layers < array_layers)
            note("array_layers expansion");
        if (!changed.empty()) {
            throw std::runtime_error(
                "Render target re-registration changed its physical contract: " +
                name + " (" + changed + ")");
        }
        return it->second;
    }

    const auto sample_count = toSampleCount(samples);
    if (samples > 1 && depth_resolve_mode == vk::ResolveModeFlagBits::eNone &&
        (usage & vk::ImageUsageFlagBits::eDepthStencilAttachment)) {
        throw std::runtime_error(
            "multisampled depth render target requires depth resolve support: " +
            name);
    }
    registration_order.reserve(registration_order.size() + 1);
    std::array<ImageWrapper, 2> images;
    std::array<std::vector<vk::UniqueImageView>, 2>
        image_layer_views;
    std::array<vk::UniqueImageView, 2>
        layered_image_views;
    std::array<ImageWrapper, 2> attachment_images;
    std::array<std::vector<vk::UniqueImageView>, 2>
        attachment_image_layer_views;
    std::array<vk::UniqueImageView, 2>
        layered_attachment_image_views;
    const uint32_t surface_count = history ? 2u : 1u;
    for (uint32_t i = 0; i < surface_count; ++i) {
        images[i] = createRenderTargetImage(name, base_extent, extent_scale, fixed_extent,
                                            format, usage, memUsage,
                                            vk::SampleCountFlagBits::e1,
                                            array_layers);
        image_layer_views[i] =
            createSequentialImageViews(device, images[i]);
        layered_image_views[i] =
            createLayeredImageView(device, images[i]);
        if (samples > 1) {
            const auto attachment_usage =
                usage & (vk::ImageUsageFlagBits::eColorAttachment |
                         vk::ImageUsageFlagBits::eDepthStencilAttachment);
            attachment_images[i] = createRenderTargetImage(
                name, base_extent, extent_scale, fixed_extent, format,
                attachment_usage, memUsage, sample_count,
                array_layers);
            attachment_image_layer_views[i] =
                createSequentialImageViews(
                    device, attachment_images[i]);
            layered_attachment_image_views[i] =
                createLayeredImageView(
                    device, attachment_images[i]);
        }
    }
    nameRenderTargetSurfaces(
        name, images, image_layer_views,
        layered_image_views,
        surface_count);
    if (samples > 1) {
        nameAttachmentSurfaces(
            name, attachment_images,
            attachment_image_layer_views,
            layered_attachment_image_views, surface_count);
    }
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
        .samples = samples,
        .array_layers = array_layers,
        .images = std::move(images),
        .image_layer_views =
            std::move(image_layer_views),
        .layered_image_views =
            std::move(layered_image_views),
        .attachment_images = std::move(attachment_images),
        .attachment_image_layer_views =
            std::move(attachment_image_layer_views),
        .layered_attachment_image_views =
            std::move(layered_attachment_image_views),
    });

    try {
        if (!name_to_id.emplace(name, id).second) {
            throw std::runtime_error(
                "Render target name table changed during registration: " +
                name);
        }
    } catch (...) {
        (void)render_targets.extract(id, false);
        throw;
    }
    registration_order.push_back(id);
    return id;
}

void RenderTargetContainer::recreateForExtent(vk::Extent2D base_extent) {
    for (const auto id : registration_order) {
        auto &rt = render_targets.get(id);
        std::array<ImageWrapper, 2> next_images;
        std::array<std::vector<vk::UniqueImageView>, 2>
            next_layer_views;
        std::array<vk::UniqueImageView, 2>
            next_layered_views;
        std::array<ImageWrapper, 2> next_attachment_images;
        std::array<std::vector<vk::UniqueImageView>, 2>
            next_attachment_layer_views;
        std::array<vk::UniqueImageView, 2>
            next_layered_attachment_views;
        const uint32_t surface_count = rt.history ? 2u : 1u;
        for (uint32_t i = 0; i < surface_count; ++i) {
            next_images[i] = createRenderTargetImage(rt.name, base_extent, rt.extent_scale,
                                                     rt.fixed_extent, rt.format, rt.usage,
                                                     rt.memory_usage,
                                                     vk::SampleCountFlagBits::e1,
                                                     rt.array_layers);
            next_layer_views[i] =
                createSequentialImageViews(
                    device, next_images[i]);
            next_layered_views[i] =
                createLayeredImageView(
                    device, next_images[i]);
            if (rt.samples > 1) {
                const auto attachment_usage =
                    rt.usage &
                    (vk::ImageUsageFlagBits::eColorAttachment |
                     vk::ImageUsageFlagBits::eDepthStencilAttachment);
                next_attachment_images[i] = createRenderTargetImage(
                    rt.name, base_extent, rt.extent_scale, rt.fixed_extent,
                    rt.format, attachment_usage, rt.memory_usage,
                    toSampleCount(rt.samples), rt.array_layers);
                next_attachment_layer_views[i] =
                    createSequentialImageViews(
                        device, next_attachment_images[i]);
                next_layered_attachment_views[i] =
                    createLayeredImageView(
                        device, next_attachment_images[i]);
            }
        }
        nameRenderTargetSurfaces(
            rt.name, next_images, next_layer_views,
            next_layered_views, surface_count);
        if (rt.samples > 1) {
            nameAttachmentSurfaces(
                rt.name, next_attachment_images,
                next_attachment_layer_views,
                next_layered_attachment_views,
                surface_count);
        }
        if (rt.history) clearHistoryImages(next_images, rt.history_clear_color);

        GET_MODULE(DeletionQueue)
            .defer(RetiredRenderTargetResources{
                .images = std::move(rt.images),
                .image_layer_views =
                    std::move(rt.image_layer_views),
                .layered_image_views =
                    std::move(rt.layered_image_views),
                .attachment_images = std::move(rt.attachment_images),
                .attachment_image_layer_views =
                    std::move(
                        rt.attachment_image_layer_views),
                .layered_attachment_image_views =
                    std::move(
                        rt.layered_attachment_image_views),
            });

        rt.images = std::move(next_images);
        rt.image_layer_views =
            std::move(next_layer_views);
        rt.layered_image_views =
            std::move(next_layered_views);
        rt.attachment_images = std::move(next_attachment_images);
        rt.attachment_image_layer_views =
            std::move(next_attachment_layer_views);
        rt.layered_attachment_image_views =
            std::move(next_layered_attachment_views);
    }
    history_frame_index = 0;
}

void RenderTargetContainer::resetHistory() {
    GET_MODULE(VulkanManageCore).waitIdle();
    for (const auto id : registration_order) {
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
        rt.samples,
        rt.array_layers,
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

const ImageWrapper &RenderTargetContainer::getAttachmentImage(
    GlobalRenderTargetId id, bool history_read) const {
    const auto &rt = render_targets.get(id);
    const auto surface = surfaceIndex(id, history_read);
    return rt.samples > 1 ? rt.attachment_images[surface]
                          : rt.images[surface];
}

vk::ImageView RenderTargetContainer::getImageView(GlobalRenderTargetId id, bool history_read) const {
    return getImageLayerView(id, 0, history_read);
}

vk::ImageView RenderTargetContainer::getImageViewForFrame(GlobalRenderTargetId id, bool history_read,
                                                          uint32_t frame_index) const {
    return getImageLayerViewForFrame(
        id, 0, history_read, frame_index);
}

vk::ImageView RenderTargetContainer::getImageLayerView(
    GlobalRenderTargetId id, std::uint32_t array_layer,
    bool history_read) const {
    const auto &rt = render_targets.get(id);
    const auto surface = surfaceIndex(id, history_read);
    if (array_layer >=
        rt.image_layer_views[surface].size()) {
        throw std::out_of_range(
            "render target array layer is out of range: " +
            rt.name);
    }
    return rt.image_layer_views[surface][array_layer]
        .get();
}

vk::ImageView
RenderTargetContainer::getImageLayerViewForFrame(
    GlobalRenderTargetId id, std::uint32_t array_layer,
    bool history_read, uint32_t frame_index) const {
    const auto &rt = render_targets.get(id);
    const uint32_t index = rt.history ? ((frame_index & 1u) ^ (history_read ? 1u : 0u)) : 0u;
    if (array_layer >= rt.image_layer_views[index].size()) {
        throw std::out_of_range(
            "render target array layer is out of range: " +
            rt.name);
    }
    return rt.image_layer_views[index][array_layer]
        .get();
}

vk::ImageView RenderTargetContainer::getAttachmentImageView(
    GlobalRenderTargetId id, bool history_read) const {
    return getAttachmentImageLayerView(
        id, 0, history_read);
}

vk::ImageView
RenderTargetContainer::getAttachmentImageLayerView(
    GlobalRenderTargetId id, std::uint32_t array_layer,
    bool history_read) const {
    const auto &rt = render_targets.get(id);
    const auto surface = surfaceIndex(id, history_read);
    const auto &views =
        rt.samples > 1
            ? rt.attachment_image_layer_views[surface]
            : rt.image_layer_views[surface];
    if (array_layer >= views.size()) {
        throw std::out_of_range(
            "render target attachment array layer is out of range: " +
            rt.name);
    }
    return views[array_layer].get();
}

vk::ImageView RenderTargetContainer::getLayeredImageView(
    GlobalRenderTargetId id, bool history_read) const {
    const auto &rt = render_targets.get(id);
    const auto surface = surfaceIndex(id, history_read);
    return rt.layered_image_views[surface]
               ? rt.layered_image_views[surface].get()
               : rt.image_layer_views[surface]
                     .front()
                     .get();
}

vk::ImageView
RenderTargetContainer::getLayeredAttachmentImageView(
    GlobalRenderTargetId id, bool history_read) const {
    const auto &rt = render_targets.get(id);
    const auto surface = surfaceIndex(id, history_read);
    if (rt.samples > 1) {
        return rt.layered_attachment_image_views[surface]
                   ? rt.layered_attachment_image_views[surface]
                         .get()
                   : rt.attachment_image_layer_views[surface]
                         .front()
                         .get();
    }
    return getLayeredImageView(id, history_read);
}

bool RenderTargetContainer::hasSeparateAttachment(
    GlobalRenderTargetId id) const {
    return render_targets.get(id).samples > 1;
}

vk::SampleCountFlagBits RenderTargetContainer::sampleCount(
    GlobalRenderTargetId id) const {
    return toSampleCount(render_targets.get(id).samples);
}

vk::ResolveModeFlagBits RenderTargetContainer::resolveMode(
    GlobalRenderTargetId id) const {
    const auto &rt = render_targets.get(id);
    if (rt.samples == 1) return vk::ResolveModeFlagBits::eNone;
    if (rt.usage & vk::ImageUsageFlagBits::eDepthStencilAttachment) {
        return depth_resolve_mode;
    }
    return colorAttachmentResolveMode(rt.format);
}

vk::ImageLayout RenderTargetContainer::initialLayout(
    GlobalRenderTargetId id, bool attachment) const {
    const auto &rt = render_targets.get(id);
    if (attachment && rt.samples > 1) {
        return vk::ImageLayout::eUndefined;
    }
    return rt.history ? vk::ImageLayout::eShaderReadOnlyOptimal
                      : vk::ImageLayout::eUndefined;
}

RenderTargetContainer::RegistrationCheckpoint
RenderTargetContainer::checkpointRegistrations() const {
    return RegistrationCheckpoint{
        registration_order.size(), name_to_id};
}

void RenderTargetContainer::rollbackRegistrations(
    RegistrationCheckpoint checkpoint) {
    if (checkpoint.registration_count >
        registration_order.size()) {
        throw std::runtime_error(
            "Render target registration checkpoint is invalid");
    }
    while (registration_order.size() >
           checkpoint.registration_count) {
        const auto id = registration_order.back();
        (void)render_targets.extract(id, false);
        registration_order.pop_back();
    }
    name_to_id = std::move(checkpoint.name_to_id);
}

std::vector<std::pair<std::string, GlobalRenderTargetId>>
RenderTargetContainer::registrationsSince(
    RegistrationCheckpoint checkpoint) const {
    if (checkpoint.registration_count >
        registration_order.size()) {
        throw std::runtime_error(
            "Render target registration checkpoint is invalid");
    }
    std::vector<std::pair<std::string, GlobalRenderTargetId>>
        result;
    result.reserve(registration_order.size() -
                   checkpoint.registration_count);
    for (std::size_t index = checkpoint.registration_count;
         index < registration_order.size(); ++index) {
        const auto id = registration_order[index];
        result.emplace_back(render_targets.get(id).name, id);
    }
    return result;
}

void RenderTargetContainer::hideRegistrationName(
    const std::string &name, GlobalRenderTargetId expected) {
    const auto found = name_to_id.find(name);
    if (found != name_to_id.end() &&
        found->second == expected) {
        name_to_id.erase(found);
    }
}

void RenderTargetContainer::retireRegistrations(
    const std::vector<GlobalRenderTargetId> &ids) noexcept {
    for (const auto id : ids) {
        if (!render_targets.contains(id)) continue;
        const auto name = render_targets.get(id).name;
        hideRegistrationName(name, id);
        auto retired = render_targets.extract(id, false);
        std::erase(registration_order, id);
        if (!retired) continue;
        try {
            auto *queue =
                FastModuleContainer::tryGet<DeletionQueue>();
            if (queue != nullptr &&
                queue->acceptingResources()) {
                queue->defer(std::move(*retired));
            }
        } catch (...) {
        }
    }
}

} // namespace Pelican
