#include "atlasassetresource.hpp"

#include "../loader/engineresources.hpp"
#include "../loader/imageloader.hpp"
#include "../vkcore/core.hpp"
#include "../vkcore/util.hpp"

#include <array>
#include <limits>
#include <span>
#include <stdexcept>

namespace Pelican {
namespace {

vk::UniqueDescriptorSetLayout createLayout(vk::Device device) {
    vk::DescriptorSetLayoutBinding binding{0, vk::DescriptorType::eCombinedImageSampler, 1,
                                            vk::ShaderStageFlagBits::eFragment};
    return device.createDescriptorSetLayoutUnique(vk::DescriptorSetLayoutCreateInfo{{}, binding});
}

vk::UniqueDescriptorPool createDescriptorPool(vk::Device device, std::uint32_t descriptor_count) {
    const vk::DescriptorPoolSize pool_size{vk::DescriptorType::eCombinedImageSampler,
                                           descriptor_count};
    vk::DescriptorPoolCreateInfo pool_info;
    pool_info.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
    pool_info.maxSets = descriptor_count;
    pool_info.setPoolSizes(pool_size);
    return device.createDescriptorPoolUnique(pool_info);
}

vk::UniqueSampler createSampler(vk::Device device, vk::Filter filter) {
    vk::SamplerCreateInfo info;
    info.magFilter = filter;
    info.minFilter = filter;
    info.mipmapMode = vk::SamplerMipmapMode::eNearest;
    info.addressModeU = vk::SamplerAddressMode::eClampToEdge;
    info.addressModeV = vk::SamplerAddressMode::eClampToEdge;
    info.addressModeW = vk::SamplerAddressMode::eClampToEdge;
    info.maxLod = VK_LOD_CLAMP_NONE;
    return device.createSamplerUnique(info);
}

vk::UniqueImageView createView(vk::Device device, const ImageWrapper &image) {
    vk::ImageViewCreateInfo info;
    info.image = image.image.get();
    info.viewType = vk::ImageViewType::e2D;
    info.format = image.format;
    info.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, image.mip_levels, 0, 1};
    return device.createImageViewUnique(info);
}

ImageWrapper uploadRgba8Srgb(vk::Extent3D extent, const void *pixels, std::size_t bytes) {
    auto &core = GET_MODULE(VulkanManageCore);
    auto image = core.allocImage(extent, vk::Format::eR8G8B8A8Srgb,
                                 vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
                                 vma::MemoryUsage::eAutoPreferDevice, {});
    GET_MODULE(VulkanUtils).safeTransferMemoryToImage(
        image, pixels, bytes,
        VulkanUtils::ImageTransferInfo{.old_layout = vk::ImageLayout::eUndefined,
                                       .new_layout = vk::ImageLayout::eShaderReadOnlyOptimal,
                                       .dst_stage = vk::PipelineStageFlagBits::eFragmentShader,
                                       .dst_access = vk::AccessFlagBits::eShaderRead});
    return image;
}

ImageWrapper uploadAtlasImage(const LoadedImage &loaded, std::string_view name) {
    if (loaded.dimension != LoadedImageDimension::TwoD) {
        throw std::runtime_error(
            "Atlas texture '" + std::string{name} +
            "' requires dimension 2d, found " +
            std::string{
                loadedImageDimensionName(loaded.dimension)});
    }
    vk::Format format;
    switch (loaded.format) {
    case ImagePixelFormat::Rgba8Unorm:
    case ImagePixelFormat::Rgba8Srgb: format = vk::Format::eR8G8B8A8Srgb; break;
    case ImagePixelFormat::Bc7Unorm:
    case ImagePixelFormat::Bc7Srgb: format = vk::Format::eBc7SrgbBlock; break;
    case ImagePixelFormat::Bc5Unorm:
        throw std::runtime_error("Atlas KTX2 texture '" + std::string{name} +
                                 "' uses BC5_UNORM; color pages require RGBA8 or BC7");
    default:
        throw std::runtime_error("Atlas texture '" + std::string{name} +
                                 "' is not RGBA8 or BC7 KTX2 color data");
    }
    auto &core = GET_MODULE(VulkanManageCore);
    const auto features = core.getPhysDevice().getFormatProperties(format).optimalTilingFeatures;
    const auto required = vk::FormatFeatureFlagBits::eSampledImage |
                          vk::FormatFeatureFlagBits::eSampledImageFilterLinear |
                          vk::FormatFeatureFlagBits::eTransferDst;
    if ((features & required) != required)
        throw std::runtime_error("Atlas texture '" + std::string{name} + "' format " +
                                 vk::to_string(format) +
                                 " lacks sampled/linear-filter/transfer-dst GPU support");
    auto image = core.allocImage({loaded.width, loaded.height, 1}, format,
                                 vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
                                 vma::MemoryUsage::eAutoPreferDevice, {}, VulkanProcessType::graphics,
                                 {}, loaded.mipLevels());
    std::vector<vk::BufferImageCopy> regions;
    regions.reserve(loaded.levels.size());
    for (std::uint32_t mip = 0; mip < loaded.levels.size(); ++mip) {
        const auto &level = loaded.levels[mip];
        vk::BufferImageCopy copy;
        copy.bufferOffset = level.offset;
        copy.imageSubresource = {vk::ImageAspectFlagBits::eColor, mip, 0, 1};
        copy.imageExtent = vk::Extent3D{level.width, level.height, 1};
        regions.push_back(copy);
    }
    GET_MODULE(VulkanUtils).safeTransferMemoryToImageLevels(
        image, loaded.pixels.data(), loaded.pixels.size(), regions,
        VulkanUtils::ImageTransferInfo{.old_layout = vk::ImageLayout::eUndefined,
                                       .new_layout = vk::ImageLayout::eShaderReadOnlyOptimal,
                                       .dst_stage = vk::PipelineStageFlagBits::eFragmentShader,
                                       .dst_access = vk::AccessFlagBits::eShaderRead});
    return image;
}

} // namespace

AtlasAssetResource::AtlasAssetResource() : device{GET_MODULE(VulkanManageCore).getDevice()} {
    descriptor_set_layout = createLayout(device);
    nearest_sampler = createSampler(device, vk::Filter::eNearest);
    linear_sampler = createSampler(device, vk::Filter::eLinear);
    descriptor_pools.push_back(createDescriptorPool(device, descriptor_sets_per_pool));

    const std::array<std::uint8_t, 4> white{255, 255, 255, 255};
    pages.push_back(makePage(0, "white", uploadRgba8Srgb({1, 1, 1}, white.data(), white.size())));
}

AtlasAssetResource::~AtlasAssetResource() = default;

std::uint16_t AtlasAssetResource::registerPage(const AtlasPageSource &source) {
    for (const auto &page : pages)
        if (page.stable_name == source.stable_name) return page.id;
    if (pages.size() >= std::numeric_limits<std::uint16_t>::max())
        throw std::length_error("limit_exceeded: shared atlas GPU page count exceeds uint16");
    const auto loaded = [&] {
        if (source.embedded_resource.empty()) return loadImageFile(source.image_path);
        const auto bytes = engineResourceOrThrow(source.embedded_resource);
        return loadImageMemory(std::as_bytes(std::span{bytes.data(), bytes.size()}),
                               "engine://" + source.embedded_resource);
    }();
    const bool supported_color = loaded.format == ImagePixelFormat::Rgba8Unorm ||
                                 loaded.format == ImagePixelFormat::Rgba8Srgb ||
                                 loaded.format == ImagePixelFormat::Bc7Unorm ||
                                 loaded.format == ImagePixelFormat::Bc7Srgb;
    const auto source_name = source.embedded_resource.empty()
                                 ? source.image_path.string()
                                 : "engine://" + source.embedded_resource;
    if (!supported_color ||
        loaded.dimension != LoadedImageDimension::TwoD ||
        loaded.width != static_cast<std::uint32_t>(source.size.width) ||
        loaded.height != static_cast<std::uint32_t>(source.size.height))
        throw std::runtime_error("atlas page image does not match declared RGBA8/BC7 size: " + source_name);
    const auto id = static_cast<std::uint16_t>(pages.size());
    pages.push_back(makePage(id, source.stable_name, uploadAtlasImage(loaded, source_name)));
    return id;
}

AtlasGpuPage AtlasAssetResource::makePage(std::uint16_t id, std::string stable_name,
                                          ImageWrapper image) {
    AtlasGpuPage page{.id = id, .stable_name = std::move(stable_name), .image = std::move(image)};
    page.view = createView(device, page.image);
    auto sets = allocatePageDescriptorSets();
    page.nearest_set = std::move(sets[0]);
    page.linear_set = std::move(sets[1]);
    const auto write = [&](vk::DescriptorSet set, vk::Sampler sampler) {
        const vk::DescriptorImageInfo image_info{sampler, page.view.get(),
                                                 vk::ImageLayout::eShaderReadOnlyOptimal};
        vk::WriteDescriptorSet descriptor_write{set, 0, 0, 1,
                                                vk::DescriptorType::eCombinedImageSampler,
                                                &image_info};
        device.updateDescriptorSets(descriptor_write, {});
    };
    write(page.nearest_set.get(), nearest_sampler.get());
    write(page.linear_set.get(), linear_sampler.get());
    return page;
}

std::vector<vk::UniqueDescriptorSet> AtlasAssetResource::allocatePageDescriptorSets() {
    constexpr std::uint32_t descriptors_per_page = 2;
    if (active_pool_descriptor_count + descriptors_per_page > descriptor_sets_per_pool) {
        descriptor_pools.push_back(createDescriptorPool(device, descriptor_sets_per_pool));
        active_pool_descriptor_count = 0;
    }
    const std::array layouts{descriptor_set_layout.get(), descriptor_set_layout.get()};
    vk::DescriptorSetAllocateInfo allocate{descriptor_pools.back().get(),
                                           static_cast<std::uint32_t>(layouts.size()), layouts.data()};
    auto result = device.allocateDescriptorSetsUnique(allocate);
    active_pool_descriptor_count += descriptors_per_page;
    return result;
}

vk::DescriptorSet AtlasAssetResource::descriptor(std::uint16_t page,
                                                  asset::SamplerKey sampler) const {
    if (page >= pages.size() || pages[page].id != page)
        throw std::runtime_error("atlas texture page is not registered");
    return sampler == asset::SamplerKey::nearest ? pages[page].nearest_set.get()
                                                  : pages[page].linear_set.get();
}

} // namespace Pelican
