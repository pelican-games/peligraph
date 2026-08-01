#include "rendertargetcontainer.hpp"
#include "renderingsamplecount.hpp"
#include "../vkcore/deletionqueue.hpp"
#include "../vkcore/core.hpp"
#include "../vkcore/util.hpp"
#include <array>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace Pelican {

namespace {

struct RetiredRenderTargetResourceBatch {
    std::vector<std::unique_ptr<RenderTargetResourceSet>>
        targets;
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
    ImageSubresourceRange subresource) {
    subresource =
        resolveImageSubresourceRange(
            subresource, image.mip_levels,
            image.array_layers);
    if (view_type == vk::ImageViewType::e2D &&
        subresource.layer_count != 1) {
        throw std::runtime_error(
            "2D render target image view requires one array layer");
    }
    if (view_type == vk::ImageViewType::eCube &&
        (!(image.create_flags &
           vk::ImageCreateFlagBits::eCubeCompatible) ||
         subresource.layer_count != 6 ||
         subresource.base_array_layer % 6 != 0)) {
        throw std::runtime_error(
            "Cube render target image view requires a "
            "cube-compatible image and one aligned six-layer "
            "range");
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

    ci.subresourceRange.baseMipLevel =
        subresource.base_mip_level;
    ci.subresourceRange.levelCount =
        subresource.level_count;
    ci.subresourceRange.baseArrayLayer =
        subresource.base_array_layer;
    ci.subresourceRange.layerCount =
        subresource.layer_count;
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
            ImageSubresourceRange{
                .base_array_layer = layer,
            }));
    }
    return result;
}

static vk::UniqueImageView createLayeredImageView(
    vk::Device device, const ImageWrapper &image) {
    return createImageView(
        device, image, vk::ImageViewType::e2DArray,
        ImageSubresourceRange{
            .layer_count = image.array_layers,
        });
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
                                     ImageMipLevelCount mip_levels = {},
                                     vk::SampleCountFlagBits samples =
                                         vk::SampleCountFlagBits::e1,
                                     std::uint32_t array_layers = 1,
                                     RenderTargetStorageMode storage_mode =
                                         RenderTargetStorageMode::materialized,
                                     bool aliasable = false,
                                     ImageResourceDimension dimension =
                                         ImageResourceDimension::two_d) {
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
        // Sampling support and filtering support are separate Vulkan
        // capabilities. Integer/data targets are commonly sampleable only
        // with nearest filtering, so the consuming pass validates its
        // authored sampler instead of imposing linear filtering here.
        required |= vk::FormatFeatureFlagBits::eSampledImage;
    }
    if (usage & vk::ImageUsageFlagBits::eStorage) {
        required |=
            vk::FormatFeatureFlagBits::eStorageImage;
    }
    if ((features & required) != required) {
        throw std::runtime_error("Render target format lacks required color capability: " + name);
    }
    const auto extent = resolveRenderTargetExtent(name, base_extent, extent_scale, fixed_extent);
    if (dimension ==
            ImageResourceDimension::cube &&
        (array_layers != 6 ||
         extent.width != extent.height)) {
        throw std::runtime_error(
            "Cube render target image creation requires a square "
            "extent and exactly six array layers: " +
            name);
    }
    const auto resolved_mip_levels =
        resolveImageMipLevels(
            mip_levels, extent.width,
            extent.height);
    const auto preferred_memory =
        storage_mode ==
                    RenderTargetStorageMode::
                        transient_attachment ||
                storage_mode ==
                    RenderTargetStorageMode::
                        tile_local_attachment
            ? vk::MemoryPropertyFlags{
                  vk::MemoryPropertyFlagBits::
                      eLazilyAllocated}
            : vk::MemoryPropertyFlags{};
    const auto allocation_flags =
        aliasable
            ? vma::AllocationCreateFlags{
                  vma::AllocationCreateFlagBits::eCanAlias}
            : vma::AllocationCreateFlags{};
    auto image_flags =
        aliasable
            ? vk::ImageCreateFlags{
                  vk::ImageCreateFlagBits::eAlias}
            : vk::ImageCreateFlags{};
    if (dimension ==
        ImageResourceDimension::cube) {
        image_flags |=
            vk::ImageCreateFlagBits::
                eCubeCompatible;
    }
    return vkcore.allocImage(vk::Extent3D{extent.width, extent.height, 1}, format, usage,
                             memory_usage, allocation_flags,
                             VulkanProcessType::graphics,
                             {}, resolved_mip_levels, samples,
                             array_layers,
                             preferred_memory, image_flags);
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
                    vk::ImageAspectFlagBits::eColor, 0,
                    image.mip_levels, 0,
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

struct PreparedRenderTargetExtent::Impl {
    struct Entry {
        GlobalRenderTargetId id;
        std::unique_ptr<RenderTargetResourceSet>
            resources;
    };

    const RenderTargetContainer *owner = nullptr;
    std::uint64_t base_revision = 0;
    vk::Extent2D base_extent{};
    std::vector<Entry> entries;
};

PreparedRenderTargetExtent::PreparedRenderTargetExtent() =
    default;
PreparedRenderTargetExtent::~PreparedRenderTargetExtent() =
    default;
PreparedRenderTargetExtent::PreparedRenderTargetExtent(
    std::unique_ptr<Impl> impl)
    : impl_{std::move(impl)} {}
PreparedRenderTargetExtent::PreparedRenderTargetExtent(
    PreparedRenderTargetExtent &&) noexcept = default;
PreparedRenderTargetExtent &
PreparedRenderTargetExtent::operator=(
    PreparedRenderTargetExtent &&) noexcept = default;

bool PreparedRenderTargetExtent::valid() const noexcept {
    return impl_ != nullptr;
}

vk::Extent2D PreparedRenderTargetExtent::extent() const noexcept {
    return impl_ != nullptr ? impl_->base_extent
                            : vk::Extent2D{};
}

std::size_t
PreparedRenderTargetExtent::targetCount() const noexcept {
    return impl_ != nullptr ? impl_->entries.size() : 0;
}

RenderTargetContainer::RenderTargetContainer()
    : device{GET_MODULE(VulkanManageCore).getDevice()},
      depth_resolve_mode{selectDepthResolveMode(
          GET_MODULE(VulkanManageCore).getPhysDevice())} {}

RenderTargetContainer::~RenderTargetContainer() {}

void RenderTargetContainer::bumpResourceRevision() {
    if (resource_revision ==
        std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error(
            "render target resource revision space exhausted");
    }
    ++resource_revision;
}

std::uint64_t RenderTargetContainer::createAliasGroupToken() {
    if (next_alias_group_token ==
        std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error(
            "render target alias group token space exhausted");
    }
    return next_alias_group_token++;
}

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
                                                                 ImageMipLevelCount mip_levels,
                                                                 std::uint32_t array_layers,
                                                                 RenderTargetStorageMode storage_mode,
                                                                 std::optional<std::string> alias_group,
                                                                 std::optional<std::uint64_t>
                                                                     alias_group_token,
                                                                 ImageResourceDimension
                                                                     dimension) {
    if (mip_levels.mode ==
        ImageMipLevelMode::full_chain) {
        mip_levels.count = 1;
    } else if (mip_levels.count == 0) {
        throw std::runtime_error(
            "Render target mip_levels must be greater than zero: " +
            name);
    }
    if (alias_group.has_value() !=
        alias_group_token.has_value()) {
        throw std::runtime_error(
            "render target alias group name and token must be provided together: " +
            name);
    }
    if (alias_group && alias_group->empty()) {
        throw std::runtime_error(
            "render target alias group must not be empty: " +
            name);
    }
    if (array_layers == 0) {
        throw std::runtime_error(
            "Render target array_layers must be greater than zero: " +
            name);
    }
    if (dimension ==
        ImageResourceDimension::cube) {
        if (array_layers != 6) {
            throw std::runtime_error(
                "Cube render target requires exactly six array "
                "layers: " +
                name);
        }
        const auto extent =
            resolveRenderTargetExtent(
                name, base_extent, extent_scale,
                fixed_extent);
        if (extent.width != extent.height) {
            throw std::runtime_error(
                "Cube render target requires a square extent: " +
                name);
        }
    }
    const auto attachment_usage =
        vk::ImageUsageFlagBits::eColorAttachment |
        vk::ImageUsageFlagBits::
            eDepthStencilAttachment;
    if (mip_levels != ImageMipLevelCount{} &&
        storage_mode !=
            RenderTargetStorageMode::materialized) {
        throw std::runtime_error(
            "multi-mip render target must be materialized: " +
            name);
    }
    if (storage_mode ==
        RenderTargetStorageMode::
            transient_attachment) {
        if (!(usage & attachment_usage) ||
            bool(usage & ~vk::ImageUsageFlags{
                              attachment_usage})) {
            throw std::runtime_error(
                "Transient render target may only use color/depth "
                "attachment usage: " +
                name);
        }
        if (history || samples != 1) {
            throw std::runtime_error(
                "Transient render target must be non-history and "
                "single-sample: " +
                name);
        }
        usage |= vk::ImageUsageFlagBits::
            eTransientAttachment;
    } else if (
        storage_mode ==
        RenderTargetStorageMode::
            tile_local_attachment) {
        const auto allowed_usage =
            attachment_usage |
            vk::ImageUsageFlagBits::eSampled |
            vk::ImageUsageFlagBits::
                eInputAttachment;
        if (!(usage & attachment_usage) ||
            bool(usage & ~vk::ImageUsageFlags{
                              allowed_usage})) {
            throw std::runtime_error(
                "Tile-local render target may only declare "
                "color/depth attachment, sampled, and input "
                "attachment usage: " +
                name);
        }
        if (history || samples != 1) {
            throw std::runtime_error(
                "Tile-local render target must be non-history and "
                "single-sample: " +
                name);
        }
        if (!GET_MODULE(VulkanManageCore)
                 .getRuntimeCapabilities()
                 .dynamic_rendering_local_read) {
            throw std::runtime_error(
                "Tile-local render target requires enabled Vulkan "
                "dynamic rendering local read: " +
                name);
        }
        usage =
            (usage & attachment_usage) |
            vk::ImageUsageFlagBits::eInputAttachment |
            vk::ImageUsageFlagBits::
                eTransientAttachment;
    }
    if (history && !(usage & vk::ImageUsageFlagBits::eSampled)) {
        throw std::runtime_error("History render target requires SAMPLED usage: " + name);
    }
    if (history && !(usage & vk::ImageUsageFlagBits::eColorAttachment)) {
        throw std::runtime_error("History render target requires COLOR_ATTACHMENT usage: " + name);
    }
    if (history) usage |= vk::ImageUsageFlagBits::eTransferDst;
    if (alias_group &&
        (history || samples != 1 ||
         storage_mode !=
             RenderTargetStorageMode::materialized)) {
        throw std::runtime_error(
            "aliased render target must be materialized, non-history, and single-sample: " +
            name);
    }

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
        if (existing.mip_levels != mip_levels)
            note("mip_levels");
        if (existing.dimension != dimension)
            note("dimension");
        if (existing.storage_mode != storage_mode)
            note("storage_mode");
        if (existing.alias_group != alias_group)
            note("alias_group");
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
    const InternalRenderTarget *alias_owner = nullptr;
    if (alias_group_token) {
        if (const auto owner =
                alias_group_owners.find(*alias_group_token);
            owner != alias_group_owners.end()) {
            alias_owner = &render_targets.get(owner->second);
            const auto requested_extent =
                resolveRenderTargetExtent(
                    name, base_extent, extent_scale,
                    fixed_extent);
            const auto &owner_image =
                alias_owner->resources->images[0];
            const auto owner_extent =
                vk::Extent2D{
                    owner_image.extent.width,
                    owner_image.extent.height};
            if (alias_owner->format != format ||
                alias_owner->usage != usage ||
                alias_owner->memory_usage != memUsage ||
                alias_owner->samples != samples ||
                alias_owner->mip_levels !=
                    mip_levels ||
                alias_owner->array_layers != array_layers ||
                alias_owner->dimension != dimension ||
                alias_owner->storage_mode != storage_mode ||
                owner_extent != requested_extent) {
                throw std::runtime_error(
                    "render target alias group members have incompatible physical contracts: " +
                    *alias_group + " (" +
                    alias_owner->name + ", " + name +
                    ")");
            }
        }
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
        images[i] =
            alias_owner != nullptr
                ? GET_MODULE(VulkanManageCore)
                          .allocAliasingImage(
                          alias_owner->resources->images[i])
                : createRenderTargetImage(
                      name, base_extent, extent_scale,
                      fixed_extent, format, usage, memUsage,
                      mip_levels,
                      vk::SampleCountFlagBits::e1,
                      array_layers, storage_mode,
                      alias_group_token.has_value(),
                      dimension);
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
                attachment_usage, memUsage,
                ImageMipLevelCount{}, sample_count,
                array_layers, storage_mode,
                false, dimension);
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
        .mip_levels = mip_levels,
        .array_layers = array_layers,
        .dimension = dimension,
        .storage_mode = storage_mode,
        .alias_group = alias_group,
        .alias_group_token = alias_group_token,
        .resources =
            std::make_unique<RenderTargetResourceSet>(
                RenderTargetResourceSet{
                    .images = std::move(images),
                    .image_layer_views =
                        std::move(image_layer_views),
                    .layered_image_views =
                        std::move(layered_image_views),
                    .attachment_images =
                        std::move(attachment_images),
                    .attachment_image_layer_views =
                        std::move(
                            attachment_image_layer_views),
                    .layered_attachment_image_views =
                        std::move(
                            layered_attachment_image_views),
                }),
    });

    try {
        if (!name_to_id.emplace(name, id).second) {
            throw std::runtime_error(
                "Render target name table changed during registration: " +
                name);
        }
        registration_order.push_back(id);
        if (alias_group_token &&
            alias_owner == nullptr &&
            !alias_group_owners
                 .emplace(*alias_group_token, id)
                 .second) {
            throw std::runtime_error(
                "render target alias group owner changed during registration: " +
                *alias_group);
        }
        bumpResourceRevision();
    } catch (...) {
        if (const auto found = name_to_id.find(name);
            found != name_to_id.end() &&
            found->second == id) {
            name_to_id.erase(found);
        }
        std::erase(registration_order, id);
        (void)render_targets.extract(id, false);
        rebuildAliasGroupOwners();
        throw;
    }
    return id;
}

PreparedRenderTargetExtent
RenderTargetContainer::prepareForExtent(
    vk::Extent2D base_extent) const {
    auto prepared =
        std::make_unique<
            PreparedRenderTargetExtent::Impl>();
    prepared->owner = this;
    prepared->base_revision = resource_revision;
    prepared->base_extent = base_extent;
    prepared->entries.reserve(
        registration_order.size());
    std::unordered_map<int, std::size_t>
        prepared_by_id;
    prepared_by_id.reserve(registration_order.size());

    for (const auto id : registration_order) {
        const auto &rt = render_targets.get(id);
        const RenderTargetResourceSet *
            alias_owner = nullptr;
        if (rt.alias_group_token) {
            const auto owner =
                alias_group_owners.find(
                    *rt.alias_group_token);
            if (owner ==
                alias_group_owners.end()) {
                throw std::runtime_error(
                    "render target alias group has no allocation owner: " +
                    rt.name);
            }
            if (owner->second != id) {
                const auto prepared_owner =
                    prepared_by_id.find(
                        owner->second.value);
                if (prepared_owner ==
                    prepared_by_id.end()) {
                    throw std::runtime_error(
                        "render target alias owner was not prepared before member: " +
                        rt.name);
                }
                alias_owner =
                    prepared->entries[
                        prepared_owner->second]
                        .resources.get();
            }
        }
        auto next =
            std::make_unique<
                RenderTargetResourceSet>();
        const uint32_t surface_count = rt.history ? 2u : 1u;
        for (uint32_t i = 0; i < surface_count; ++i) {
            next->images[i] =
                alias_owner != nullptr
                    ? GET_MODULE(VulkanManageCore)
                          .allocAliasingImage(
                              alias_owner->images[i])
                    : createRenderTargetImage(
                          rt.name, base_extent,
                          rt.extent_scale,
                          rt.fixed_extent, rt.format,
                          rt.usage, rt.memory_usage,
                          rt.mip_levels,
                          vk::SampleCountFlagBits::e1,
                          rt.array_layers,
                          rt.storage_mode,
                          rt.alias_group_token
                              .has_value(),
                          rt.dimension);
            next->image_layer_views[i] =
                createSequentialImageViews(
                    device, next->images[i]);
            next->layered_image_views[i] =
                createLayeredImageView(
                    device, next->images[i]);
            if (rt.samples > 1) {
                const auto attachment_usage =
                    rt.usage &
                    (vk::ImageUsageFlagBits::eColorAttachment |
                     vk::ImageUsageFlagBits::eDepthStencilAttachment);
                next->attachment_images[i] = createRenderTargetImage(
                    rt.name, base_extent, rt.extent_scale, rt.fixed_extent,
                    rt.format, attachment_usage, rt.memory_usage,
                    ImageMipLevelCount{},
                    toSampleCount(rt.samples), rt.array_layers,
                    rt.storage_mode, false,
                    rt.dimension);
                next->attachment_image_layer_views[i] =
                    createSequentialImageViews(
                        device, next->attachment_images[i]);
                next->layered_attachment_image_views[i] =
                    createLayeredImageView(
                        device, next->attachment_images[i]);
            }
        }
        nameRenderTargetSurfaces(
            rt.name, next->images,
            next->image_layer_views,
            next->layered_image_views, surface_count);
        if (rt.samples > 1) {
            nameAttachmentSurfaces(
                rt.name, next->attachment_images,
                next->attachment_image_layer_views,
                next->layered_attachment_image_views,
                surface_count);
        }
        if (rt.history) {
            clearHistoryImages(
                next->images,
                rt.history_clear_color);
        }
        prepared_by_id.emplace(
            id.value, prepared->entries.size());
        prepared->entries.push_back(
            PreparedRenderTargetExtent::Impl::Entry{
                id, std::move(next)});
    }
    return PreparedRenderTargetExtent{
        std::move(prepared)};
}

bool RenderTargetContainer::setRuntimeArrayLayers(
    GlobalRenderTargetId id,
    std::uint32_t array_layers) {
    if (array_layers == 0) {
        throw std::runtime_error(
            "runtime render-target array layer count must be positive");
    }
    auto &target = render_targets.get(id);
    if (target.array_layers == array_layers) {
        return false;
    }
    if (target.history || target.alias_group ||
        target.alias_group_token ||
        target.dimension !=
            ImageResourceDimension::two_d) {
        throw std::runtime_error(
            "runtime array-layer resizing requires a non-history, "
            "non-aliased 2D render target: " +
            target.name);
    }
    const auto maximum_layers =
        GET_MODULE(VulkanManageCore)
            .getPhysDevice()
            .getProperties()
            .limits.maxImageArrayLayers;
    if (array_layers > maximum_layers) {
        throw std::runtime_error(
            "runtime render-target array layer count exceeds the "
            "physical device limit for '" +
            target.name + "' (requested=" +
            std::to_string(array_layers) +
            ", max=" +
            std::to_string(maximum_layers) + ")");
    }
    if (resource_revision ==
        std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error(
            "render target resource revision space exhausted");
    }

    const auto current_extent = vk::Extent2D{
        target.resources->images[0].extent.width,
        target.resources->images[0].extent.height,
    };
    auto next =
        std::make_unique<RenderTargetResourceSet>();
    next->images[0] = createRenderTargetImage(
        target.name, current_extent, 1.0f,
        current_extent, target.format,
        target.usage, target.memory_usage,
        target.mip_levels,
        vk::SampleCountFlagBits::e1,
        array_layers, target.storage_mode,
        false, target.dimension);
    next->image_layer_views[0] =
        createSequentialImageViews(
            device, next->images[0]);
    next->layered_image_views[0] =
        createLayeredImageView(
            device, next->images[0]);
    if (target.samples > 1) {
        const auto attachment_usage =
            target.usage &
            (vk::ImageUsageFlagBits::eColorAttachment |
             vk::ImageUsageFlagBits::eDepthStencilAttachment);
        next->attachment_images[0] =
            createRenderTargetImage(
                target.name, current_extent, 1.0f,
                current_extent, target.format,
                attachment_usage,
                target.memory_usage,
                ImageMipLevelCount{},
                toSampleCount(target.samples),
                array_layers, target.storage_mode,
                false, target.dimension);
        next->attachment_image_layer_views[0] =
            createSequentialImageViews(
                device,
                next->attachment_images[0]);
        next->layered_attachment_image_views[0] =
            createLayeredImageView(
                device,
                next->attachment_images[0]);
    }
    nameRenderTargetSurfaces(
        target.name, next->images,
        next->image_layer_views,
        next->layered_image_views, 1);
    if (target.samples > 1) {
        nameAttachmentSurfaces(
            target.name,
            next->attachment_images,
            next->attachment_image_layer_views,
            next->layered_attachment_image_views,
            1);
    }

    auto retired = std::move(target.resources);
    if (auto *queue =
            FastModuleContainer::tryGet<DeletionQueue>();
        queue != nullptr &&
        queue->acceptingResources()) {
        queue->defer(std::move(retired));
    } else {
        GET_MODULE(VulkanManageCore).waitIdle();
    }
    target.resources = std::move(next);
    target.array_layers = array_layers;
    ++resource_revision;
    return true;
}

void RenderTargetContainer::publishPreparedExtent(
    PreparedRenderTargetExtent &&prepared) {
    if (!prepared.valid()) {
        throw std::runtime_error(
            "render target extent candidate is empty");
    }
    auto &candidate = *prepared.impl_;
    if (candidate.owner != this) {
        throw std::runtime_error(
            "render target extent candidate belongs to another container");
    }
    if (candidate.base_revision != resource_revision) {
        throw std::runtime_error(
            "render target extent candidate is stale");
    }
    if (candidate.entries.size() !=
        registration_order.size()) {
        throw std::runtime_error(
            "render target extent candidate does not cover the active registry");
    }
    if (resource_revision ==
        std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error(
            "render target resource revision space exhausted");
    }

    std::vector<InternalRenderTarget *> targets;
    targets.reserve(candidate.entries.size());
    for (std::size_t index = 0;
         index < candidate.entries.size(); ++index) {
        const auto expected_id =
            registration_order[index];
        const auto candidate_id =
            candidate.entries[index].id;
        if (candidate_id != expected_id ||
            !render_targets.contains(candidate_id)) {
            throw std::runtime_error(
                "render target extent candidate registry order changed");
        }
        auto &target =
            render_targets.get(candidate_id);
        if (target.resources == nullptr ||
            candidate.entries[index].resources ==
                nullptr) {
            throw std::runtime_error(
                "render target extent candidate contains an empty resource set");
        }
        targets.push_back(&target);
    }

    auto retired =
        std::make_shared<
            RetiredRenderTargetResourceBatch>();
    retired->targets.reserve(targets.size());
    // Enlist and reserve the complete retirement batch before the first
    // ownership move. Publication below consists only of noexcept unique_ptr
    // moves, so failure cannot expose a partially replaced target registry.
    GET_MODULE(DeletionQueue).defer(retired);

    for (std::size_t index = 0;
         index < targets.size(); ++index) {
        auto &rt = *targets[index];
        auto &next =
            candidate.entries[index].resources;
        retired->targets.push_back(
            std::move(rt.resources));
        rt.resources = std::move(next);
    }
    history_frame_index = 0;
    ++resource_revision;
    prepared.impl_.reset();
}

void RenderTargetContainer::resetHistory() {
    GET_MODULE(VulkanManageCore).waitIdle();
    for (const auto id : registration_order) {
        auto &rt = render_targets.get(id);
        if (rt.history) {
            clearHistoryImages(
                rt.resources->images,
                rt.history_clear_color);
        }
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
        .name = rt.name,
        .usage = rt.usage,
        .format = rt.format,
        .extent =
            vk::Extent2D{
                rt.resources->images[0].extent.width,
                rt.resources->images[0].extent.height},
        .history = rt.history,
        .samples = rt.samples,
        .mip_levels =
            rt.resources->images[0].mip_levels,
        .array_layers = rt.array_layers,
        .dimension = rt.dimension,
        .storage_mode = rt.storage_mode,
        .alias_group = rt.alias_group,
    };
}

const ImageWrapper &RenderTargetContainer::getImage(GlobalRenderTargetId id, bool history_read) const {
    return render_targets.get(id)
        .resources->images[
            surfaceIndex(id, history_read)];
}

const ImageWrapper &RenderTargetContainer::getImageForFrame(GlobalRenderTargetId id, bool history_read,
                                                            uint32_t frame_index) const {
    const auto &rt = render_targets.get(id);
    const uint32_t index = rt.history ? ((frame_index & 1u) ^ (history_read ? 1u : 0u)) : 0u;
    return rt.resources->images[index];
}

const ImageWrapper &RenderTargetContainer::getAttachmentImage(
    GlobalRenderTargetId id, bool history_read) const {
    const auto &rt = render_targets.get(id);
    const auto surface = surfaceIndex(id, history_read);
    return rt.samples > 1
               ? rt.resources->attachment_images[surface]
               : rt.resources->images[surface];
}

vk::ImageView RenderTargetContainer::getImageView(GlobalRenderTargetId id, bool history_read) const {
    return getImageLayerView(id, 0, history_read);
}

vk::ImageView RenderTargetContainer::getImageViewForFrame(GlobalRenderTargetId id, bool history_read,
                                                          uint32_t frame_index) const {
    return getImageLayerViewForFrame(
        id, 0, history_read, frame_index);
}

vk::ImageView
RenderTargetContainer::getImageSubresourceView(
    GlobalRenderTargetId id,
    ImageSubresourceRange subresource,
    bool array_view, bool history_read) const {
    return getImageSubresourceViewForFrame(
        id, subresource,
        array_view
            ? ImageSubresourceViewDimension::
                  two_d_array
            : ImageSubresourceViewDimension::
                  two_d,
        history_read, history_frame_index);
}

vk::ImageView
RenderTargetContainer::getImageSubresourceViewForFrame(
    GlobalRenderTargetId id,
    ImageSubresourceRange subresource,
    bool array_view, bool history_read,
    std::uint32_t frame_index) const {
    return getImageSubresourceViewForFrame(
        id, subresource,
        array_view
            ? ImageSubresourceViewDimension::
                  two_d_array
            : ImageSubresourceViewDimension::
                  two_d,
        history_read, frame_index);
}

vk::ImageView
RenderTargetContainer::getImageSubresourceView(
    GlobalRenderTargetId id,
    ImageSubresourceRange subresource,
    ImageSubresourceViewDimension dimension,
    bool history_read) const {
    return getImageSubresourceViewForFrame(
        id, subresource, dimension,
        history_read, history_frame_index);
}

vk::ImageView
RenderTargetContainer::getImageSubresourceViewForFrame(
    GlobalRenderTargetId id,
    ImageSubresourceRange subresource,
    ImageSubresourceViewDimension dimension,
    bool history_read,
    std::uint32_t frame_index) const {
    const auto &rt = render_targets.get(id);
    const auto surface =
        rt.history
            ? ((frame_index & 1u) ^
               (history_read ? 1u : 0u))
            : 0u;
    subresource =
        resolveImageSubresourceRange(
            subresource,
            rt.resources
                ->images[surface].mip_levels,
            rt.resources
                ->images[surface].array_layers);
    const ImageSubresourceViewKey key{
        .range = subresource,
        .dimension = dimension,
    };
    auto &views =
        rt.resources
            ->subresource_image_views[surface];
    if (const auto found = views.find(key);
        found != views.end()) {
        return found->second.get();
    }
    auto view = createImageView(
        device, rt.resources->images[surface],
        dimension ==
                ImageSubresourceViewDimension::
                    two_d_array
            ? vk::ImageViewType::e2DArray
        : dimension ==
                ImageSubresourceViewDimension::cube
            ? vk::ImageViewType::eCube
            : vk::ImageViewType::e2D,
        subresource);
    GET_MODULE(VulkanManageCore)
        .getDebugUtils()
        .nameImageView(
            view.get(),
            ("rt/" + rt.name + "/surface/" +
             std::to_string(surface) +
             "/mip/" +
             std::to_string(
                 subresource.base_mip_level) +
             "-" +
             std::to_string(
                 subresource.level_count) +
             "/layer/" +
             std::to_string(
                 subresource.base_array_layer) +
             "-" +
             std::to_string(
                 subresource.layer_count) +
             "/" +
             std::string{
                 imageSubresourceViewDimensionName(
                     dimension)} +
             "_view")
                .c_str());
    const auto [inserted, success] =
        views.emplace(key, std::move(view));
    if (!success) {
        throw std::runtime_error(
            "render target subresource view cache changed during creation");
    }
    return inserted->second.get();
}

vk::ImageView RenderTargetContainer::getImageLayerView(
    GlobalRenderTargetId id, std::uint32_t array_layer,
    bool history_read) const {
    const auto &rt = render_targets.get(id);
    const auto surface = surfaceIndex(id, history_read);
    if (array_layer >=
        rt.resources->image_layer_views[surface]
            .size()) {
        throw std::out_of_range(
            "render target array layer is out of range: " +
            rt.name);
    }
    return rt.resources
        ->image_layer_views[surface][array_layer]
        .get();
}

vk::ImageView
RenderTargetContainer::getImageLayerViewForFrame(
    GlobalRenderTargetId id, std::uint32_t array_layer,
    bool history_read, uint32_t frame_index) const {
    const auto &rt = render_targets.get(id);
    const uint32_t index = rt.history ? ((frame_index & 1u) ^ (history_read ? 1u : 0u)) : 0u;
    if (array_layer >=
        rt.resources->image_layer_views[index]
            .size()) {
        throw std::out_of_range(
            "render target array layer is out of range: " +
            rt.name);
    }
    return rt.resources
        ->image_layer_views[index][array_layer]
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
            ? rt.resources
                  ->attachment_image_layer_views[surface]
            : rt.resources
                  ->image_layer_views[surface];
    if (array_layer >= views.size()) {
        throw std::out_of_range(
            "render target attachment array layer is out of range: " +
            rt.name);
    }
    return views[array_layer].get();
}

vk::ImageView
RenderTargetContainer::getAttachmentImageSubresourceView(
    GlobalRenderTargetId id,
    ImageSubresourceRange subresource,
    bool array_view, bool history_read) const {
    const auto &rt = render_targets.get(id);
    if (rt.samples <= 1) {
        return getImageSubresourceView(
            id, subresource, array_view,
            history_read);
    }
    const auto surface =
        surfaceIndex(id, history_read);
    subresource =
        resolveImageSubresourceRange(
            subresource,
            rt.resources
                ->attachment_images[surface]
                .mip_levels,
            rt.resources
                ->attachment_images[surface]
                .array_layers);
    const ImageSubresourceViewKey key{
        .range = subresource,
        .dimension =
            array_view
                ? ImageSubresourceViewDimension::
                      two_d_array
                : ImageSubresourceViewDimension::
                      two_d,
    };
    auto &views =
        rt.resources
            ->attachment_subresource_image_views[
                surface];
    if (const auto found = views.find(key);
        found != views.end()) {
        return found->second.get();
    }
    auto view = createImageView(
        device,
        rt.resources
            ->attachment_images[surface],
        array_view
            ? vk::ImageViewType::e2DArray
            : vk::ImageViewType::e2D,
        subresource);
    GET_MODULE(VulkanManageCore)
        .getDebugUtils()
        .nameImageView(
            view.get(),
            ("rt/" + rt.name + "/surface/" +
             std::to_string(surface) +
             "/msaa/mip/" +
             std::to_string(
                 subresource.base_mip_level) +
             "/layer/" +
             std::to_string(
                 subresource.base_array_layer) +
             "-" +
             std::to_string(
                 subresource.layer_count) +
             (array_view ? "/array_view"
                         : "/view"))
                .c_str());
    const auto [inserted, success] =
        views.emplace(key, std::move(view));
    if (!success) {
        throw std::runtime_error(
            "render target attachment subresource view cache "
            "changed during creation");
    }
    return inserted->second.get();
}

vk::ImageView RenderTargetContainer::getLayeredImageView(
    GlobalRenderTargetId id, bool history_read) const {
    const auto &rt = render_targets.get(id);
    const auto surface = surfaceIndex(id, history_read);
    return rt.resources->layered_image_views[surface]
               ? rt.resources
                     ->layered_image_views[surface]
                     .get()
               : rt.resources
                     ->image_layer_views[surface]
                     .front()
                     .get();
}

vk::ImageView
RenderTargetContainer::getLayeredImageViewForFrame(
    GlobalRenderTargetId id, bool history_read,
    std::uint32_t frame_index) const {
    const auto &rt = render_targets.get(id);
    const std::uint32_t surface =
        rt.history
            ? ((frame_index & 1u) ^
               (history_read ? 1u : 0u))
            : 0u;
    return rt.resources->layered_image_views[surface]
               ? rt.resources
                     ->layered_image_views[surface]
                     .get()
               : rt.resources
                     ->image_layer_views[surface]
                     .front()
                     .get();
}

vk::ImageView
RenderTargetContainer::getLayeredAttachmentImageView(
    GlobalRenderTargetId id, bool history_read) const {
    const auto &rt = render_targets.get(id);
    const auto surface = surfaceIndex(id, history_read);
    if (rt.samples > 1) {
        return rt.resources
                       ->layered_attachment_image_views[surface]
                   ? rt.resources
                         ->layered_attachment_image_views[surface]
                         .get()
                   : rt.resources
                         ->attachment_image_layer_views[surface]
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

std::optional<std::uint64_t>
RenderTargetContainer::aliasGroup(
    GlobalRenderTargetId id) const {
    return render_targets.get(id).alias_group_token;
}

bool RenderTargetContainer::sharesAllocation(
    GlobalRenderTargetId left,
    GlobalRenderTargetId right) const {
    const auto &left_allocation =
        getImage(left).allocation;
    const auto &right_allocation =
        getImage(right).allocation;
    return left_allocation != nullptr &&
           right_allocation != nullptr &&
           left_allocation == right_allocation;
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
    const bool changed =
        registration_order.size() >
        checkpoint.registration_count;
    while (registration_order.size() >
           checkpoint.registration_count) {
        const auto id = registration_order.back();
        (void)render_targets.extract(id, false);
        registration_order.pop_back();
    }
    name_to_id = std::move(checkpoint.name_to_id);
    rebuildAliasGroupOwners();
    if (changed) bumpResourceRevision();
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
    bool changed = false;
    for (const auto id : ids) {
        if (!render_targets.contains(id)) continue;
        changed = true;
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
    rebuildAliasGroupOwners();
    if (changed) {
        if (resource_revision ==
            std::numeric_limits<std::uint64_t>::max()) {
            std::terminate();
        }
        ++resource_revision;
    }
}

void RenderTargetContainer::rebuildAliasGroupOwners() {
    alias_group_owners.clear();
    for (const auto id : registration_order) {
        if (!render_targets.contains(id)) continue;
        const auto &target = render_targets.get(id);
        if (target.alias_group_token) {
            alias_group_owners.emplace(
                *target.alias_group_token, id);
        }
    }
}

} // namespace Pelican
