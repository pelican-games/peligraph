#include "rendertargetcontainer.hpp"
#include "rendertargetimagecontract.hpp"
#include "renderingsamplecount.hpp"
#include "vulkanimagevocabulary.hpp"
#include "../../project/targetrenderplanning.hpp"
#include "../../project/targetrendermemory.hpp"
#include "rendertargetmemory.hpp"
#include "../vkcore/deletionqueue.hpp"
#include "../vkcore/core.hpp"
#include "../vkcore/util.hpp"
#include <array>
#include <cmath>
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
    std::vector<std::shared_ptr<RenderTargetResourceSet>>
        targets;
};

vk::ImageViewType toImageViewType(
    ImageSubresourceViewDimension dimension) {
    switch (dimension) {
    case ImageSubresourceViewDimension::two_d:
        return vk::ImageViewType::e2D;
    case ImageSubresourceViewDimension::two_d_array:
        return vk::ImageViewType::e2DArray;
    case ImageSubresourceViewDimension::cube:
        return vk::ImageViewType::eCube;
    }
    throw std::runtime_error(
        "unknown image subresource view dimension");
}

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

vk::Extent2D resolveRenderTargetExtent(const std::string &name, vk::Extent2D base_extent,
    float extent_scale, std::optional<ResolvedResourceExtent> fixed_extent) {
    RenderTargetDefinition definition;
    definition.name = name;
    definition.extent_scale = extent_scale;
    definition.fixed_extent = fixed_extent;
    const auto info = renderTargetImageContract(definition, base_extent, 0).image;
    return {info.extent.width, info.extent.height};
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

RenderTargetImageContract actualImageContract(
    const ImageWrapper &image, std::optional<std::uint32_t> queue_family = std::nullopt) {
    RenderTargetImageContract result;
    result.usage = imageUsageFromVulkan(image.usage);
    result.queue_family = queue_family;
    auto &info = result.image;
    info.flags = image.create_flags;
    info.imageType = image.image_type;
    info.format = image.format;
    info.extent = image.extent;
    info.mipLevels = image.mip_levels;
    info.arrayLayers = image.array_layers;
    info.samples = image.samples;
    info.tiling = vk::ImageTiling::eOptimal;
    info.usage = image.usage;
    info.sharingMode = vk::SharingMode::eExclusive;
    info.initialLayout = vk::ImageLayout::eUndefined;
    return result;
}

ImageWrapper createRenderTargetImage(const std::string &name, vk::Extent2D base_extent, float extent_scale,
                                     std::optional<ResolvedResourceExtent> fixed_extent,
                                     ImageFormat image_format, ImageUsage image_usage,
                                     vma::MemoryUsage memory_usage,
                                     ImageMipLevelCount mip_levels = {},
                                     vk::SampleCountFlagBits samples =
                                         vk::SampleCountFlagBits::e1,
                                     std::uint32_t array_layers = 1,
                                     RenderTargetStorageMode storage_mode =
                                         RenderTargetStorageMode::materialized,
                                     ImageResourceDimension dimension =
                                         ImageResourceDimension::two_d) {
    const auto &vkcore = GET_MODULE(VulkanManageCore);
    RenderTargetDefinition definition;
    definition.name = name;
    definition.extent_scale = extent_scale;
    definition.fixed_extent = fixed_extent;
    definition.format = image_format;
    definition.usage = image_usage;
    definition.mip_levels = mip_levels;
    definition.samples = static_cast<std::uint32_t>(samples);
    definition.array_layers = array_layers;
    definition.storage_mode = storage_mode;
    definition.dimension = dimension;
    const auto contract = renderTargetImageContract(definition, base_extent,
        vkcore.getGraphicsQueueFamilyIndex(), samples != vk::SampleCountFlagBits::e1);
    const auto create_info = contract.createInfo();
    const auto format = create_info.format;
    const auto usage = create_info.usage;
    const auto features = vkcore.getPhysDevice().getFormatProperties(format).optimalTilingFeatures;
    vk::FormatFeatureFlags required;
    if (usage & vk::ImageUsageFlagBits::eColorAttachment) {
        required |= vk::FormatFeatureFlagBits::eColorAttachment;
        if (image_format == ImageFormat::r8g8b8a8_srgb ||
            image_format == ImageFormat::b8g8r8a8_srgb) {
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
    return vkcore.allocImage(create_info, memory_usage, {}, preferred_memory);
}

// Every image contributes its own real requirements before any heap is bound.
std::map<std::string, ImageWrapper, std::less<>> prepareRegionImages(
    const std::vector<RenderTargetDefinition> &definitions, vk::Extent2D extent,
    const RuntimeTargetMemoryContract &contract) {
    auto &core=GET_MODULE(VulkanManageCore);
    std::map<std::string, ImageWrapper, std::less<>> images;
    MemoryPackingRequirements requirements;
    for(const auto &resource:contract.inputs.resources) {
        const auto found=std::find_if(definitions.begin(),definitions.end(),[&](const auto &d){return d.name==resource.resource;});
        if(found==definitions.end()) throw std::runtime_error("render_target.memory_resource_missing@1: "+resource.resource);
        auto d=*found;
        d.planned_memory_region=0; // ALIAS_BIT, independently of a same-offset group.
        const auto ci=renderTargetImageContract(d,extent,core.getGraphicsQueueFamilyIndex());
        auto image=core.createUnboundImage(ci.createInfo());
        auto req=core.imageRequirements(image);
        if(contract.requirements_for_testing) req=contract.requirements_for_testing(d.name,extent,req);
        requirements.emplace(d.name,req);
        images.emplace(d.name,std::move(image));
    }
    std::map<std::string,std::string,std::less<>> rejections;
    const auto packed=repackMemoryRegions(contract.inputs,requirements,contract.required_sets,rejections);
    for(const auto &region:packed.regions) {
        const auto allocation=core.allocateImageRegion(vk::MemoryRequirements{region.size,region.alignment,region.memory_type_bits});
        for(const auto &r:region.residents) core.bindImageRegion(images.at(r.resource),allocation,r.offset,*requirements.at(r.resource),r.resource);
    }
    // Required dedicated allocations use vmaCreateImage's dedicated-aware path.
    for(const auto &name:packed.excluded) {
        auto &image=images.at(name);
        const auto d=std::find_if(definitions.begin(),definitions.end(),[&](const auto &r){return r.name==name;});
        const auto ci=renderTargetImageContract(*d,extent,core.getGraphicsQueueFamilyIndex());
        image=core.allocImage(ci.createInfo(),vma::MemoryUsage::eAutoPreferDevice,{});
    }
    for(const auto &[name,reason]:rejections) images.at(name).memory_rejection=reason;
    return images;
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

// EN-2: writes a file image's pixels into a newly created image (every mip
// when it has a chain) and leaves it SHADER_READ_ONLY_OPTIMAL, the layout the
// layout tracker assumes for it.
void uploadSourcePixels(const ImageWrapper &image,
                        const RenderTargetSourcePixels &pixels,
                        const std::string &name) {
    if (image.extent.width != pixels.width ||
        image.extent.height != pixels.height ||
        pixels.rgba8.size() !=
            static_cast<std::size_t>(pixels.width) *
                pixels.height * 4u) {
        throw std::runtime_error(
            "render target source pixels do not match its image: " +
            name);
    }
    const VulkanUtils::ImageTransferInfo info{
        .old_layout = vk::ImageLayout::eUndefined,
        .new_layout = vk::ImageLayout::eShaderReadOnlyOptimal,
        .dst_stage = vk::PipelineStageFlagBits::eAllCommands,
        .dst_access = vk::AccessFlagBits::eShaderRead,
    };
    auto &utils = GET_MODULE(VulkanUtils);
    if (image.mip_levels > 1) {
        utils.safeTransferMemoryToImageAndGenerateMipmaps(
            image, pixels.rgba8.data(), pixels.rgba8.size(), info);
    } else {
        utils.safeTransferMemoryToImage(
            image, pixels.rgba8.data(), pixels.rgba8.size(), info);
    }
}

} // namespace

struct PreparedRenderTargetExtent::Impl {
    struct Entry {
        GlobalRenderTargetId id;
        std::shared_ptr<RenderTargetResourceSet>
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


vk::ClearColorValue physicalRenderTargetHistoryClearColor(
    const RenderTargetDefinition &definition) {
    const auto numeric =
        imageFormatShaderNumeric(definition.format);
    const auto signed_integer =
        numeric == ImageShaderNumeric::signed_integer;
    const auto unsigned_integer =
        numeric == ImageShaderNumeric::unsigned_integer;
    std::array<float, 4> floating{};
    std::array<std::int32_t, 4> signed_values{};
    std::array<std::uint32_t, 4> unsigned_values{};
    for (std::size_t component = 0;
         component <
         definition.history_clear_color.size();
         ++component) {
        const auto value =
            definition.history_clear_color[component];
        if (!std::isfinite(value)) {
            throw std::runtime_error(
                "render target history clear value must be finite: " +
                definition.name);
        }
        if (signed_integer) {
            if (std::trunc(value) != value ||
                value <
                    static_cast<double>(
                        std::numeric_limits<
                            std::int32_t>::min()) ||
                value >
                    static_cast<double>(
                        std::numeric_limits<
                            std::int32_t>::max())) {
                throw std::runtime_error(
                    "signed integer render target history clear "
                    "value is fractional or out of range: " +
                    definition.name);
            }
            signed_values[component] =
                static_cast<std::int32_t>(value);
            continue;
        }
        if (unsigned_integer) {
            if (std::trunc(value) != value ||
                value < 0.0 ||
                value >
                    static_cast<double>(
                        std::numeric_limits<
                            std::uint32_t>::max())) {
                throw std::runtime_error(
                    "unsigned integer render target history clear "
                    "value is fractional or out of range: " +
                    definition.name);
            }
            unsigned_values[component] =
                static_cast<std::uint32_t>(value);
            continue;
        }
        if (value <
                -static_cast<double>(
                    std::numeric_limits<float>::max()) ||
            value >
                static_cast<double>(
                    std::numeric_limits<float>::max())) {
            throw std::runtime_error(
                "floating render target history clear value is "
                "out of range: " +
                definition.name);
        }
        floating[component] =
            static_cast<float>(value);
    }
    if (signed_integer) {
        return vk::ClearColorValue{
            signed_values};
    }
    if (unsigned_integer) {
        return vk::ClearColorValue{
            unsigned_values};
    }
    return vk::ClearColorValue{floating};
}

GlobalRenderTargetId RenderTargetContainer::registerRenderTarget(
    const RenderTargetDefinition &definition,
    vk::Extent2D base_extent,
    vma::MemoryUsage memUsage,
    ImageWrapper prepared_image,
    std::shared_ptr<const RuntimeTargetMemoryContract> memory_contract) {
    // The physical clear value is resolved first so an unrepresentable
    // authored value is reported before any other contract check, as it was
    // when the caller resolved it ahead of the call.
    const auto history_clear_color =
        physicalRenderTargetHistoryClearColor(definition);
    const auto &name = definition.name;
    const auto &format_class = definition.format_class;
    const auto &role = definition.role;
    const auto extent_scale = definition.extent_scale;
    const auto &fixed_extent = definition.fixed_extent;
    const auto format = definition.format;
    // RC-3c2a: a definition of the generation's resource description carries
    // the final usage (engineImageUsage applied); one made directly (the XR
    // mirror, a test) may carry the declared bits only. The checks below take
    // either; the registered usage is the engine's rule over it (the same
    // definition, idempotent on a final usage).
    auto usage = definition.usage;
    const auto history = definition.history;
    const auto samples = definition.samples;
    auto mip_levels = definition.mip_levels;
    const auto array_layers = definition.array_layers;
    const auto storage_mode = definition.storage_mode;
    const auto &alias_group = definition.alias_group;
    const auto dimension = definition.dimension;
    if (mip_levels.mode ==
        ImageMipLevelMode::full_chain) {
        mip_levels.count = 1;
    } else if (mip_levels.count == 0) {
        throw std::runtime_error(
            "Render target mip_levels must be greater than zero: " +
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
        ImageUsageBit::color_attachment |
        ImageUsageBit::
            depth_stencil_attachment;
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
            bool(usage & ~(attachment_usage |
                           ImageUsageBit::transient_attachment))) {
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
    } else if (
        storage_mode ==
        RenderTargetStorageMode::
            tile_local_attachment) {
        const auto allowed_usage =
            attachment_usage |
            ImageUsageBit::sampled |
            ImageUsageBit::
                input_attachment |
            ImageUsageBit::transient_attachment;
        if (!(usage & attachment_usage) ||
            bool(usage & ~ImageUsage{
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
    }
    if (history && !(usage & ImageUsageBit::sampled)) {
        throw std::runtime_error("History render target requires SAMPLED usage: " + name);
    }
    if (history && !(usage & ImageUsageBit::color_attachment)) {
        throw std::runtime_error("History render target requires COLOR_ATTACHMENT usage: " + name);
    }
    usage = engineImageUsage({.declared = definition.usage,
        .history = history, .storage = storage_mode});
    if (alias_group &&
        (history || samples != 1 ||
         storage_mode !=
             RenderTargetStorageMode::materialized)) {
        throw std::runtime_error(
            "aliased render target must be materialized, non-history, and single-sample: " +
            name);
    }
    const auto &source_pixels = definition.source_pixels;
    if (definition.source.has_value() != (source_pixels != nullptr)) {
        throw std::runtime_error(
            "render target source and its pixels must be provided together: " +
            name);
    }
    if (source_pixels) {
        const auto attachment_or_storage =
            ImageUsageBit::color_attachment |
            ImageUsageBit::depth_stencil_attachment |
            ImageUsageBit::storage;
        if (history || samples != 1 || alias_group ||
            storage_mode != RenderTargetStorageMode::materialized ||
            dimension != ImageResourceDimension::two_d ||
            array_layers != 1 ||
            !(usage & ImageUsageBit::sampled) ||
            !(usage & ImageUsageBit::transfer_dst) ||
            bool(usage & attachment_or_storage) ||
            fixed_extent !=
                ResolvedResourceExtent{source_pixels->width,
                                       source_pixels->height}) {
            throw std::runtime_error(
                "render target read from a file must be a single 2D, "
                "sampled, transfer-destination image of the file's "
                "size, never written, aliased or kept as history: " +
                name);
        }
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
        if (existing.source_pixels != source_pixels)
            note("source");
        // A previously allocated array image is a safe physical superset for
        // a sequential/flat registration. Expanding a live image during a
        // candidate transaction is not failure-atomic, so it remains an
        // explicit restart boundary.

        if (!changed.empty()) {
            throw std::runtime_error(
                "Render target re-registration changed its physical contract: " +
                name + " (" + changed + ")");
        }
        if(memory_contract && (!existing.memory_contract ||
            existing.memory_contract->inputs!=memory_contract->inputs ||
            existing.memory_contract->required_sets!=memory_contract->required_sets))
            throw std::runtime_error("render_target.memory_contract_changed@1: "+name);
        const auto &live_image = existing.resources->images[0];
        const auto live_contract = actualImageContract(live_image,
            GET_MODULE(VulkanManageCore).getGraphicsQueueFamilyIndex());
        const auto reused_contract = renderTargetImageContract(definition, base_extent,
            GET_MODULE(VulkanManageCore).getGraphicsQueueFamilyIndex(), false, &live_contract);
        (void)reused_contract; // The existing image already has exactly this create info.
        // PS-1: a lifetime is not part of the image. Once some registration
        // keeps the contents, the layout tracker keeps tracking them: that
        // is harmless for a transient user and survives a rolled-back
        // candidate, so the flag only ever turns on.
        if (definition.persistent)
            render_targets.get(it->second).persistent = true;
        return it->second;
    }

    const auto sample_count = toSampleCount(samples);
    if (samples > 1 && depth_resolve_mode == vk::ResolveModeFlagBits::eNone &&
        (usage & ImageUsageBit::depth_stencil_attachment)) {
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
        images[i] = (i == 0 && prepared_image.image) ? std::move(prepared_image) : createRenderTargetImage(
            name, base_extent, extent_scale, fixed_extent, format, usage, memUsage,
            mip_levels, vk::SampleCountFlagBits::e1, array_layers, storage_mode,
            dimension);
        image_layer_views[i] =
            createSequentialImageViews(device, images[i]);
        layered_image_views[i] =
            createLayeredImageView(device, images[i]);
        if (samples > 1) {
            const auto attachment_usage =
                usage & (ImageUsageBit::color_attachment |
                         ImageUsageBit::depth_stencil_attachment);
            attachment_images[i] = createRenderTargetImage(
                name, base_extent, extent_scale, fixed_extent, format,
                attachment_usage, memUsage,
                ImageMipLevelCount{}, sample_count,
                array_layers, storage_mode,
                dimension);
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
    if (source_pixels) uploadSourcePixels(images[0], *source_pixels, name);

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
        .memory_contract = std::move(memory_contract),
        .persistent = definition.persistent,
        .resources =
            std::make_shared<RenderTargetResourceSet>(
                RenderTargetResourceSet{
                    .base_extent=base_extent, .revision=resource_revision+1,
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
        .source_pixels = source_pixels,
    });

    try {
        if (!name_to_id.emplace(name, id).second) {
            throw std::runtime_error(
                "Render target name table changed during registration: " +
                name);
        }
        registration_order.push_back(id);
        bumpResourceRevision();
    } catch (...) {
        if (const auto found = name_to_id.find(name);
            found != name_to_id.end() &&
            found->second == id) {
            name_to_id.erase(found);
        }
        std::erase(registration_order, id);
        (void)render_targets.extract(id, false);
        throw;
    }
    return id;
}

void RenderTargetContainer::registerMemoryTargets(
    const std::vector<RenderTargetDefinition> &definitions, vk::Extent2D extent,
    std::shared_ptr<const RuntimeTargetMemoryContract> contract) {
    const auto checkpoint=checkpointRegistrations();
    try {
        // Resolve live contracts before preparing any new images. In particular,
        // a live relative image keeps its original extent (and mip chain).
        std::vector<RenderTargetDefinition> fresh;
        for(const auto &d:definitions) {
            const bool member=contract && std::any_of(contract->inputs.resources.begin(),contract->inputs.resources.end(),[&](const auto &r){return r.resource==d.name;});
            if(name_to_id.contains(d.name))
                registerRenderTarget(d,extent,vma::MemoryUsage::eAutoPreferDevice,{},member ? contract : nullptr);
            else fresh.push_back(d);
        }
        auto pending=contract ? *contract : RuntimeTargetMemoryContract{};
        const auto reused=[&](const auto &name){return name_to_id.contains(name);};
        std::erase_if(pending.inputs.resources,[&](const auto &r){return reused(r.resource);});
        std::erase_if(pending.inputs.order,reused);
        std::erase_if(pending.inputs.forbidden_pairs,[&](const auto &p){return reused(p.first)||reused(p.second);});
        for(auto *sets:{&pending.inputs.same_offset_sets,&pending.required_sets}) {
            for(auto &set:*sets) std::erase_if(set,reused);
            std::erase_if(*sets,[](const auto &set){return set.size()<2;});
        }
        auto images=contract ? prepareRegionImages(fresh,extent,pending)
                             : std::map<std::string,ImageWrapper,std::less<>>{};
        for(const auto &d:fresh) {
            auto found=images.find(d.name);
            const bool member=contract && std::any_of(contract->inputs.resources.begin(),contract->inputs.resources.end(),[&](const auto &r){return r.resource==d.name;});
            registerRenderTarget(d,extent,vma::MemoryUsage::eAutoPreferDevice,
                found==images.end() ? ImageWrapper{} : std::move(found->second), member ? contract : nullptr);
        }
    } catch(...) { rollbackRegistrations(checkpoint); throw; }
}

std::vector<std::shared_ptr<const void>> RenderTargetContainer::resourceLeases() const {
    std::vector<std::shared_ptr<const void>> result;
    for(const auto id:registration_order) result.push_back(render_targets.get(id).resources);
    return result;
}

std::vector<std::pair<std::uint64_t,std::uint64_t>> RenderTargetContainer::allocationMeasurements(GlobalRenderTargetId id) const {
    std::vector<std::pair<std::uint64_t,std::uint64_t>> result;
    const auto &rt=render_targets.get(id);
    const auto add=[&](const ImageWrapper &image) {
        if(image.image && image.allocation) result.emplace_back(
            reinterpret_cast<std::uintptr_t>(image.allocation.get()),GET_MODULE(VulkanManageCore).imageAllocationBytes(image));
    };
    for(std::uint32_t i=0;i<(rt.history?2u:1u);++i) {add(rt.resources->images[i]); add(rt.resources->attachment_images[i]);}
    return result;
}

nlohmann::json RenderTargetContainer::actualPlacement(std::span<const GlobalRenderTargetId> ids) const {
    using Json=nlohmann::json;
    Json result={{"extent",nullptr},{"revision",resource_revision},{"regions",Json::array()},{"overlaps",Json::array()}};
    std::map<const void*,std::size_t> regions;
    std::map<VkDeviceMemory,std::size_t> memory_blocks;
    result["revision"]=0;
    auto &core=GET_MODULE(VulkanManageCore);
    std::uint64_t bytes=0;
    const auto add=[&](const ImageWrapper &image,const std::string &name) {
        if(!image.image || !image.allocation) return;
        const auto info=core.imageAllocationInfo(image);
        auto [it,inserted]=regions.emplace(image.allocation.get(),regions.size());
        if(inserted) {
            bytes+=info.size;
            const auto block=memory_blocks.try_emplace(static_cast<VkDeviceMemory>(info.deviceMemory),memory_blocks.size()).first->second;
            result["regions"].push_back({{"region",it->second},{"memoryTypeIndex",info.memoryType},{"device_memory_block",block},
                {"size",info.size},{"residents",Json::array()}});
        }
        const auto req=core.imageRequirements(image);
        result["regions"][it->second]["residents"].push_back({{"resource",name},{"offset",image.memory_offset},
            {"size",req.size},{"alignment",req.alignment},{"memory_type_bits",req.memory_type_bits},
            {"region_bound",image.region_bound},{"rejection",image.memory_rejection.empty()?Json(nullptr):Json(image.memory_rejection)}});
    };
    for(const auto id:ids) {
        const auto &rt=render_targets.get(id);
        result["extent"]={{"width",rt.resources->base_extent.width},{"height",rt.resources->base_extent.height}};
        result["revision"]=std::max(result["revision"].get<std::uint64_t>(),rt.resources->revision);
        for(std::uint32_t surface=0;surface<(rt.history?2u:1u);++surface) {
            const auto suffix=surface ? "@history" : "";
            add(rt.resources->images[surface],rt.name+suffix);
            add(rt.resources->attachment_images[surface],rt.name+suffix+"@attachment");
        }
    }
    for(const auto &region:result["regions"]) {
        const auto &r=region["residents"];
        for(std::size_t i=0;i<r.size();++i) for(std::size_t j=i+1;j<r.size();++j) {
            const auto a=r[i]["offset"].get<std::uint64_t>(), b=r[j]["offset"].get<std::uint64_t>();
            if(a<b+r[j]["size"].get<std::uint64_t>() && b<a+r[i]["size"].get<std::uint64_t>())
                result["overlaps"].push_back({{"region",region["region"]},{"left",r[i]["resource"]},{"right",r[j]["resource"]}});
        }
    }
    result["allocation_count"]=regions.size(); result["allocation_bytes"]=bytes;
    result["device_memory_block_count"]=memory_blocks.size();
    return result;
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

    std::map<int, ImageWrapper> region_images;
    std::map<const RuntimeTargetMemoryContract *, std::vector<GlobalRenderTargetId>> groups;
    for (const auto id : registration_order) {
        const auto &rt = render_targets.get(id);
        if (rt.memory_contract) groups[rt.memory_contract.get()].push_back(id);
    }
    for (const auto &[contract, ids] : groups) {
        std::vector<RenderTargetDefinition> definitions;
        for (const auto id : ids) {
            const auto &rt = render_targets.get(id);
            RenderTargetDefinition d;
            d.name=rt.name; d.extent_scale=rt.extent_scale; d.fixed_extent=rt.fixed_extent;
            d.format=rt.format; d.usage=rt.usage; d.array_layers=rt.array_layers;
            d.mip_levels=rt.mip_levels; d.dimension=rt.dimension; d.alias_group=rt.alias_group;
            definitions.push_back(std::move(d));
        }
        auto images = prepareRegionImages(definitions, base_extent, *contract);
        for (const auto id : ids) {
            const auto found=images.find(render_targets.get(id).name);
            if(found!=images.end()) region_images.emplace(id.value,std::move(found->second));
        }
    }
    for (const auto id : registration_order) {
        const auto &rt = render_targets.get(id);
        auto region_image = region_images.find(id.value);
        auto next =
            std::make_shared<
                RenderTargetResourceSet>();
        next->base_extent=base_extent;
        next->revision=resource_revision+1;
        const uint32_t surface_count = rt.history ? 2u : 1u;
        for (uint32_t i = 0; i < surface_count; ++i) {
            next->images[i] = (i == 0 && region_image != region_images.end()) ? std::move(region_image->second) : createRenderTargetImage(
                rt.name, base_extent, rt.extent_scale, rt.fixed_extent, rt.format,
                rt.usage, rt.memory_usage, rt.mip_levels, vk::SampleCountFlagBits::e1,
                rt.array_layers, rt.storage_mode, rt.dimension);
            next->image_layer_views[i] =
                createSequentialImageViews(
                    device, next->images[i]);
            next->layered_image_views[i] =
                createLayeredImageView(
                    device, next->images[i]);
            if (rt.samples > 1) {
                const auto attachment_usage =
                    rt.usage &
                    (ImageUsageBit::color_attachment |
                     ImageUsageBit::depth_stencil_attachment);
                next->attachment_images[i] = createRenderTargetImage(
                    rt.name, base_extent, rt.extent_scale, rt.fixed_extent,
                    rt.format, attachment_usage, rt.memory_usage,
                    ImageMipLevelCount{},
                    toSampleCount(rt.samples), rt.array_layers,
                    rt.storage_mode, rt.dimension);
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
        if (rt.source_pixels) {
            uploadSourcePixels(next->images[0],
                               *rt.source_pixels, rt.name);
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
        target.memory_contract || target.source_pixels ||
        target.dimension !=
            ImageResourceDimension::two_d) {
        throw std::runtime_error(
            "runtime array-layer resizing requires a non-history, "
            "non-aliased 2D render target not read from a file: " +
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
        std::make_shared<RenderTargetResourceSet>();
    next->base_extent=target.resources->base_extent;
    next->revision=resource_revision+1;
    next->images[0] = createRenderTargetImage(
        target.name, current_extent, 1.0f,
        extentFromVulkan(current_extent), target.format,
        target.usage, target.memory_usage,
        target.mip_levels,
        vk::SampleCountFlagBits::e1,
        array_layers, target.storage_mode, target.dimension);
    next->image_layer_views[0] =
        createSequentialImageViews(
            device, next->images[0]);
    next->layered_image_views[0] =
        createLayeredImageView(
            device, next->images[0]);
    if (target.samples > 1) {
        const auto attachment_usage =
            target.usage &
            (ImageUsageBit::color_attachment |
             ImageUsageBit::depth_stencil_attachment);
        next->attachment_images[0] =
            createRenderTargetImage(
                target.name, current_extent, 1.0f,
                extentFromVulkan(current_extent), target.format,
                attachment_usage,
                target.memory_usage,
                ImageMipLevelCount{},
                toSampleCount(target.samples),
                array_layers, target.storage_mode, target.dimension);
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
    const bool has_history=std::any_of(registration_order.begin(),registration_order.end(),
        [&](const auto id){return render_targets.get(id).history;});
    if(has_history) GET_MODULE(VulkanManageCore).waitIdle();
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

uint32_t RenderTargetContainer::surfaceIndex(GlobalRenderTargetId id, RenderTargetRead read) const {
    const auto &rt = render_targets.get(id);
    if (!rt.history) return 0;
    if (read.frame) {
        return (*read.frame & 1u) ^ (read.previous ? 1u : 0u);
    }
    return read.previous ? (history_frame_index ^ 1u) : history_frame_index;
}

GlobalRenderTargetId RenderTargetContainer::getRenderTargetIdByName(const std::string &name) const {
    if (auto it = name_to_id.find(name); it != name_to_id.end()) {
        return it->second;
    }
    return noRenderTargetId();
}

std::string_view
RenderTargetContainer::getRenderTargetName(
    GlobalRenderTargetId id) const {
    return render_targets.get(id).name;
}

RenderTargetMetadata RenderTargetContainer::getMetadata(GlobalRenderTargetId id) const {
    const auto &rt = render_targets.get(id);
    return RenderTargetMetadata{
        .name = rt.name,
        .usage = rt.usage,
        .format = rt.format,
        .extent =
            ResolvedResourceExtent{
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

const ImageWrapper &RenderTargetContainer::getImage(GlobalRenderTargetId id, RenderTargetRead read) const {
    return render_targets.get(id)
        .resources->images[
            surfaceIndex(id, read)];
}

const ImageWrapper &RenderTargetContainer::getAttachmentImage(
    GlobalRenderTargetId id, RenderTargetRead read) const {
    const auto &rt = render_targets.get(id);
    const auto surface = surfaceIndex(id, read);
    return rt.samples > 1
               ? rt.resources->attachment_images[surface]
               : rt.resources->images[surface];
}

vk::ImageView RenderTargetContainer::getImageView(GlobalRenderTargetId id, RenderTargetRead read) const {
    return getImageLayerView(id, 0, read);
}

vk::ImageView
RenderTargetContainer::getImageSubresourceView(
    GlobalRenderTargetId id,
    ImageSubresourceRange subresource,
    ImageSubresourceViewDimension dimension,
    RenderTargetRead read) const {
    const auto &rt = render_targets.get(id);
    const auto surface = surfaceIndex(id, read);
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
        toImageViewType(dimension), subresource);
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
    RenderTargetRead read) const {
    const auto &rt = render_targets.get(id);
    const auto surface = surfaceIndex(id, read);
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

vk::ImageView RenderTargetContainer::getAttachmentImageView(
    GlobalRenderTargetId id, RenderTargetRead read) const {
    return getAttachmentImageLayerView(
        id, 0, read);
}

vk::ImageView
RenderTargetContainer::getAttachmentImageLayerView(
    GlobalRenderTargetId id, std::uint32_t array_layer,
    RenderTargetRead read) const {
    const auto &rt = render_targets.get(id);
    const auto surface = surfaceIndex(id, read);
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
    ImageSubresourceViewDimension dimension,
    RenderTargetRead read) const {
    const auto &rt = render_targets.get(id);
    if (rt.samples <= 1) {
        return getImageSubresourceView(
            id, subresource, dimension, read);
    }
    const auto surface =
        surfaceIndex(id, read);
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
        .dimension = dimension,
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
        toImageViewType(dimension), subresource);
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
             (dimension ==
                      ImageSubresourceViewDimension::
                          two_d_array
                  ? "/array_view"
              : dimension ==
                      ImageSubresourceViewDimension::
                          cube
                  ? "/cube_view"
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
    GlobalRenderTargetId id, RenderTargetRead read) const {
    const auto &rt = render_targets.get(id);
    const auto surface = surfaceIndex(id, read);
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
    GlobalRenderTargetId id, RenderTargetRead read) const {
    const auto &rt = render_targets.get(id);
    const auto surface = surfaceIndex(id, read);
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
    return getLayeredImageView(id, read);
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
    if (rt.usage & ImageUsageBit::depth_stencil_attachment) {
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
    // EN-2: a file image was left readable by its upload and is only read.
    return rt.history || rt.source_pixels
               ? vk::ImageLayout::eShaderReadOnlyOptimal
               : vk::ImageLayout::eUndefined;
}

bool RenderTargetContainer::persistent(
    GlobalRenderTargetId id) const {
    return render_targets.get(id).persistent;
}

std::optional<std::uint64_t>
RenderTargetContainer::aliasGroup(
    GlobalRenderTargetId id) const {
    const auto &image = getImage(id);
    if (!image.region_bound) return std::nullopt;
    return reinterpret_cast<std::uintptr_t>(image.allocation.get());
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
    if (changed) {
        if (resource_revision ==
            std::numeric_limits<std::uint64_t>::max()) {
            std::terminate();
        }
        ++resource_revision;
    }
}

std::uint64_t renderTargetBytes(const RenderTargetMetadata &metadata) {
    return imageResourceBytes(ImageResourceByteShape{
        .format = metadata.format,
        .extent = {metadata.extent.width, metadata.extent.height},
        .mip_levels = metadata.mip_levels,
        .array_layers = metadata.array_layers,
        .samples = metadata.samples,
        .surfaces = metadata.history ? 2u : 1u,
    });
}

RegisteredTargetMemory registeredRenderTargetMemory(
    const RenderTargetMetadata &metadata,
    std::optional<std::uint64_t> alias_group, bool used) {
    // Metadata-only estimate. Runtime callers attach measured allocations.
    auto single_sample = metadata;
    single_sample.samples = 1;
    return RegisteredTargetMemory{
        .name = metadata.name,
        .bytes = renderTargetBytes(metadata),
        .alias_shared_bytes = renderTargetBytes(single_sample),
        .alias_group = alias_group,
        .used = used,
    };
}

} // namespace Pelican
