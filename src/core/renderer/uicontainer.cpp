#include "uicontainer.hpp"

#include "../loader/imageloader.hpp"
#include "../ui/module.hpp"
#include "../vkcore/core.hpp"
#include "../vkcore/util.hpp"

#include <array>
#include <stdexcept>

namespace Pelican {
namespace {

vk::UniqueDescriptorSetLayout createLayout(vk::Device device) {
    vk::DescriptorSetLayoutBinding binding{0, vk::DescriptorType::eCombinedImageSampler, 1,
                                            vk::ShaderStageFlagBits::eFragment};
    return device.createDescriptorSetLayoutUnique(vk::DescriptorSetLayoutCreateInfo{{}, binding});
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

ImageWrapper uploadUiImage(const LoadedImage &loaded, std::string_view name) {
    vk::Format format;
    switch (loaded.format) {
    case ImagePixelFormat::Rgba8Unorm:
    case ImagePixelFormat::Rgba8Srgb: format = vk::Format::eR8G8B8A8Srgb; break;
    case ImagePixelFormat::Bc7Unorm:
    case ImagePixelFormat::Bc7Srgb: format = vk::Format::eBc7SrgbBlock; break;
    case ImagePixelFormat::Bc5Unorm:
        throw std::runtime_error("UI atlas KTX2 texture '" + std::string{name} +
                                 "' uses BC5_UNORM data format; UI pages require a color format");
    default:
        throw std::runtime_error("UI atlas texture '" + std::string{name} +
                                 "' is not RGBA8 or BC7 KTX2 color data");
    }
    auto &core = GET_MODULE(VulkanManageCore);
    const auto features = core.getPhysDevice().getFormatProperties(format).optimalTilingFeatures;
    const auto required = vk::FormatFeatureFlagBits::eSampledImage |
                          vk::FormatFeatureFlagBits::eSampledImageFilterLinear |
                          vk::FormatFeatureFlagBits::eTransferDst;
    if ((features & required) != required)
        throw std::runtime_error("UI atlas KTX2 texture '" + std::string{name} + "' format " +
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

UIContainer::UIContainer() : device{GET_MODULE(VulkanManageCore).getDevice()} {
    const auto &source_pages = GET_MODULE(ui::UiModule).pages();
    descriptor_set_layout = createLayout(device);
    nearest_sampler = createSampler(device, vk::Filter::eNearest);
    linear_sampler = createSampler(device, vk::Filter::eLinear);
    const auto descriptor_count = static_cast<std::uint32_t>((source_pages.size() + 1) * 2);
    const vk::DescriptorPoolSize pool_size{vk::DescriptorType::eCombinedImageSampler, descriptor_count};
    vk::DescriptorPoolCreateInfo pool_info;
    pool_info.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
    pool_info.maxSets = descriptor_count;
    pool_info.setPoolSizes(pool_size);
    descriptor_pool = device.createDescriptorPoolUnique(pool_info);

    const std::array<std::uint8_t, 4> white{255, 255, 255, 255};
    pages.push_back(makePage(0, "white", uploadRgba8Srgb({1, 1, 1}, white.data(), white.size())));
    for (const auto &source : source_pages) {
        const auto loaded = loadImageFile(source.image_path);
        const bool supported_color = loaded.format == ImagePixelFormat::Rgba8Unorm ||
                                     loaded.format == ImagePixelFormat::Rgba8Srgb ||
                                     loaded.format == ImagePixelFormat::Bc7Unorm ||
                                     loaded.format == ImagePixelFormat::Bc7Srgb;
        if (!supported_color || loaded.width != static_cast<std::uint32_t>(source.size.x) ||
            loaded.height != static_cast<std::uint32_t>(source.size.y))
            throw std::runtime_error("pelican.atlas v1 page image does not match declared RGBA8/BC7 size: " + source.image_path.string());
        pages.push_back(makePage(source.id, source.stable_name,
                                 uploadUiImage(loaded, source.image_path.string())));
    }
}

UIContainer::~UIContainer() = default;

UiGpuPage UIContainer::makePage(std::uint16_t id, std::string stable_name, ImageWrapper image) {
    UiGpuPage page{.id = id, .stable_name = std::move(stable_name), .image = std::move(image)};
    page.view = createView(device, page.image);
    const vk::DescriptorSetLayout layout = descriptor_set_layout.get();
    vk::DescriptorSetAllocateInfo allocate{descriptor_pool.get(), 1, &layout};
    auto nearest = device.allocateDescriptorSetsUnique(allocate);
    auto linear = device.allocateDescriptorSetsUnique(allocate);
    page.nearest_set = std::move(nearest.front());
    page.linear_set = std::move(linear.front());
    const auto write = [&](vk::DescriptorSet set, vk::Sampler sampler) {
        const vk::DescriptorImageInfo image_info{sampler, page.view.get(), vk::ImageLayout::eShaderReadOnlyOptimal};
        vk::WriteDescriptorSet descriptor_write{set, 0, 0, 1, vk::DescriptorType::eCombinedImageSampler,
                                                &image_info};
        device.updateDescriptorSets(descriptor_write, {});
    };
    write(page.nearest_set.get(), nearest_sampler.get());
    write(page.linear_set.get(), linear_sampler.get());
    return page;
}

vk::DescriptorSet UIContainer::descriptor(std::uint16_t page, ui::Sampler sampler) const {
    if (page >= pages.size() || pages[page].id != page) throw std::runtime_error("UI texture page is not registered");
    return sampler == ui::Sampler::Nearest ? pages[page].nearest_set.get() : pages[page].linear_set.get();
}

} // namespace Pelican
