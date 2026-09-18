#include "materialtexturestore.hpp"
#include "../vkcore/core.hpp"
#include "../vkcore/util.hpp"
#include <algorithm>
#include <array>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>

namespace Pelican {

namespace {

vk::UniqueSampler createSampler(vk::Device device, vk::Filter filter) {
    vk::SamplerCreateInfo create_info;
    create_info.magFilter = filter;
    create_info.minFilter = filter;
    create_info.mipmapMode = vk::SamplerMipmapMode::eLinear;
    create_info.addressModeU = vk::SamplerAddressMode::eRepeat;
    create_info.addressModeV = vk::SamplerAddressMode::eRepeat;
    create_info.addressModeW = vk::SamplerAddressMode::eRepeat;
    create_info.mipLodBias = 0.0f;
    create_info.anisotropyEnable = false;
    create_info.maxAnisotropy = 1.0f;
    create_info.compareEnable = false;
    create_info.compareOp = vk::CompareOp::eAlways;
    create_info.minLod = 0.0f;
    create_info.maxLod = VK_LOD_CLAMP_NONE;
    create_info.borderColor = vk::BorderColor::eIntOpaqueBlack;
    create_info.unnormalizedCoordinates = false;
    return device.createSamplerUnique(create_info);
}

vk::Filter materialTextureFilter(
    SurfaceTextureFilter filter) {
    return filter == SurfaceTextureFilter::nearest
               ? vk::Filter::eNearest
               : vk::Filter::eLinear;
}

vk::SamplerMipmapMode materialTextureMipmapMode(
    SurfaceTextureFilter filter) {
    return filter == SurfaceTextureFilter::nearest
               ? vk::SamplerMipmapMode::eNearest
               : vk::SamplerMipmapMode::eLinear;
}

vk::SamplerAddressMode materialTextureAddressMode(
    SurfaceTextureAddressMode address) {
    switch (address) {
    case SurfaceTextureAddressMode::repeat:
        return vk::SamplerAddressMode::eRepeat;
    case SurfaceTextureAddressMode::mirrored_repeat:
        return vk::SamplerAddressMode::eMirroredRepeat;
    case SurfaceTextureAddressMode::clamp_to_edge:
        return vk::SamplerAddressMode::eClampToEdge;
    }
    throw std::runtime_error(
        "unknown material texture address mode");
}

vk::CompareOp materialTextureCompareOp(
    SurfaceTextureCompare compare) {
    switch (compare) {
    case SurfaceTextureCompare::none:
    case SurfaceTextureCompare::always:
        return vk::CompareOp::eAlways;
    case SurfaceTextureCompare::never:
        return vk::CompareOp::eNever;
    case SurfaceTextureCompare::less:
        return vk::CompareOp::eLess;
    case SurfaceTextureCompare::equal:
        return vk::CompareOp::eEqual;
    case SurfaceTextureCompare::less_equal:
        return vk::CompareOp::eLessOrEqual;
    case SurfaceTextureCompare::greater:
        return vk::CompareOp::eGreater;
    case SurfaceTextureCompare::not_equal:
        return vk::CompareOp::eNotEqual;
    case SurfaceTextureCompare::greater_equal:
        return vk::CompareOp::eGreaterOrEqual;
    }
    throw std::runtime_error(
        "unknown material texture comparison");
}

vk::ImageViewType materialTextureViewType(
    SurfaceTextureDimension dimension) {
    switch (dimension) {
    case SurfaceTextureDimension::two_d:
        return vk::ImageViewType::e2D;
    case SurfaceTextureDimension::cube:
        return vk::ImageViewType::eCube;
    case SurfaceTextureDimension::two_d_array:
        return vk::ImageViewType::e2DArray;
    case SurfaceTextureDimension::three_d:
        return vk::ImageViewType::e3D;
    }
    throw std::runtime_error(
        "unknown material texture dimension");
}

vk::UniqueImageView createImageView(
    vk::Device device, const ImageWrapper &image,
    vk::Format format,
    SurfaceTextureDimension dimension =
        SurfaceTextureDimension::two_d) {
    vk::ImageViewCreateInfo create_info;
    create_info.image = image.image.get();
    create_info.viewType =
        materialTextureViewType(dimension);
    create_info.format = format;
    create_info.components.r = vk::ComponentSwizzle::eR;
    create_info.components.g = vk::ComponentSwizzle::eG;
    create_info.components.b = vk::ComponentSwizzle::eB;
    create_info.components.a = vk::ComponentSwizzle::eA;
    create_info.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
    create_info.subresourceRange.baseMipLevel = 0;
    create_info.subresourceRange.levelCount = image.mip_levels;
    create_info.subresourceRange.baseArrayLayer = 0;
    create_info.subresourceRange.layerCount =
        dimension ==
                SurfaceTextureDimension::three_d
            ? 1
            : image.array_layers;

    return device.createImageViewUnique(create_info);
}

std::uint32_t fullMipLevelCount(vk::Extent3D extent) {
    auto longest_side = std::max(extent.width, extent.height);
    std::uint32_t levels = 1;
    while (longest_side > 1) {
        longest_side /= 2;
        ++levels;
    }
    return levels;
}

std::uint32_t validateRgba8MipInput(
    vk::Extent3D extent, const void *data,
    vk::DeviceSize bytes_num) {
    if (extent.width == 0 || extent.height == 0 || extent.depth != 1) {
        throw std::runtime_error(
            "RGBA8 material textures require a non-zero 2D extent");
    }
    if (data == nullptr) {
        throw std::runtime_error("RGBA8 material texture data is null");
    }
    constexpr vk::DeviceSize bytes_per_pixel = 4;
    if (extent.width >
        std::numeric_limits<vk::DeviceSize>::max() /
            extent.height / bytes_per_pixel) {
        throw std::runtime_error(
            "RGBA8 material texture byte size overflows");
    }
    const auto base_bytes = static_cast<vk::DeviceSize>(extent.width) *
                            extent.height * bytes_per_pixel;
    if (bytes_num != base_bytes) {
        throw std::runtime_error(
            "RGBA8 material texture byte size mismatch: expected " +
            std::to_string(base_bytes) + ", got " +
            std::to_string(bytes_num));
    }
    if (base_bytes > std::numeric_limits<std::size_t>::max()) {
        throw std::runtime_error(
            "RGBA8 material texture exceeds host address space");
    }
    return fullMipLevelCount(extent);
}

vk::ImageType materialTextureImageType(
    SurfaceTextureDimension dimension) {
    return dimension ==
                   SurfaceTextureDimension::three_d
               ? vk::ImageType::e3D
               : vk::ImageType::e2D;
}

vk::ImageCreateFlags materialTextureImageFlags(
    SurfaceTextureDimension dimension) {
    return dimension ==
                   SurfaceTextureDimension::cube
               ? vk::ImageCreateFlagBits::eCubeCompatible
               : vk::ImageCreateFlags{};
}

void requireCompressedTextureFeatures(vk::PhysicalDevice physical_device, vk::Format format,
                                      std::string_view name) {
    if (format != vk::Format::eBc5UnormBlock && format != vk::Format::eBc7UnormBlock &&
        format != vk::Format::eBc7SrgbBlock) return;
    const auto features = physical_device.getFormatProperties(format).optimalTilingFeatures;
    const auto required = vk::FormatFeatureFlagBits::eSampledImage |
                          vk::FormatFeatureFlagBits::eSampledImageFilterLinear |
                          vk::FormatFeatureFlagBits::eTransferDst;
    if ((features & required) != required)
        throw std::runtime_error("KTX2 texture '" + std::string{name} + "' format " +
                                 vk::to_string(format) +
                                 " lacks sampled/linear-filter/transfer-dst GPU support");
}

} // namespace

bool supportsDepthComparisonSampling(
    vk::Format format) {
    switch (format) {
    case vk::Format::eD16Unorm:
    case vk::Format::eX8D24UnormPack32:
    case vk::Format::eD32Sfloat:
    case vk::Format::eD16UnormS8Uint:
    case vk::Format::eD24UnormS8Uint:
    case vk::Format::eD32SfloatS8Uint:
        return true;
    default:
        return false;
    }
}

vk::Format vkFormatForLoadedImage(ImagePixelFormat format) {
    switch (format) {
    case ImagePixelFormat::Rgba8Unorm: return vk::Format::eR8G8B8A8Unorm;
    case ImagePixelFormat::Rgba8Srgb: return vk::Format::eR8G8B8A8Srgb;
    case ImagePixelFormat::Rgba16Sfloat: return vk::Format::eR16G16B16A16Sfloat;
    case ImagePixelFormat::Rgba32Sfloat: return vk::Format::eR32G32B32A32Sfloat;
    case ImagePixelFormat::Bc5Unorm: return vk::Format::eBc5UnormBlock;
    case ImagePixelFormat::Bc7Unorm: return vk::Format::eBc7UnormBlock;
    case ImagePixelFormat::Bc7Srgb: return vk::Format::eBc7SrgbBlock;
    }
    throw std::runtime_error("Unknown loaded image format");
}

SurfaceTextureDimension surfaceTextureDimensionOf(
    LoadedImageDimension dimension) {
    switch (dimension) {
    case LoadedImageDimension::TwoD:
        return SurfaceTextureDimension::two_d;
    case LoadedImageDimension::Cube:
        return SurfaceTextureDimension::cube;
    case LoadedImageDimension::TwoDArray:
        return SurfaceTextureDimension::two_d_array;
    case LoadedImageDimension::ThreeD:
        return SurfaceTextureDimension::three_d;
    }
    throw std::runtime_error(
        "Unknown loaded image dimension");
}

void validateLoadedTextureShape(
    const LoadedImage &loaded, std::string_view name,
    const vk::PhysicalDeviceLimits &limits) {
    if (loaded.width == 0 || loaded.height == 0 ||
        loaded.depth == 0 || loaded.array_layers == 0) {
        throw std::runtime_error(
            "Texture '" + std::string{name} +
            "' has a zero-sized dimension or array layer count");
    }
    switch (loaded.dimension) {
    case LoadedImageDimension::TwoD:
        if (loaded.depth != 1 || loaded.array_layers != 1) {
            throw std::runtime_error(
                "Texture '" + std::string{name} +
                "' dimension 2d requires depth=1 and "
                "array_layers=1");
        }
        if (loaded.width > limits.maxImageDimension2D ||
            loaded.height > limits.maxImageDimension2D) {
            throw std::runtime_error(
                "Texture '" + std::string{name} +
                "' dimension 2d exceeds device "
                "maxImageDimension2D " +
                std::to_string(limits.maxImageDimension2D));
        }
        break;
    case LoadedImageDimension::Cube:
        if (loaded.width != loaded.height ||
            loaded.depth != 1 ||
            loaded.array_layers != 6) {
            throw std::runtime_error(
                "Texture '" + std::string{name} +
                "' dimension cube requires square faces, "
                "depth=1, and exactly 6 array layers");
        }
        if (loaded.width > limits.maxImageDimensionCube) {
            throw std::runtime_error(
                "Texture '" + std::string{name} +
                "' dimension cube exceeds device "
                "maxImageDimensionCube " +
                std::to_string(limits.maxImageDimensionCube));
        }
        break;
    case LoadedImageDimension::TwoDArray:
        if (loaded.depth != 1) {
            throw std::runtime_error(
                "Texture '" + std::string{name} +
                "' dimension 2d_array requires depth=1");
        }
        if (loaded.width > limits.maxImageDimension2D ||
            loaded.height > limits.maxImageDimension2D ||
            loaded.array_layers > limits.maxImageArrayLayers) {
            throw std::runtime_error(
                "Texture '" + std::string{name} +
                "' dimension 2d_array exceeds device 2D "
                "dimension or array-layer limits");
        }
        break;
    case LoadedImageDimension::ThreeD:
        if (loaded.array_layers != 1) {
            throw std::runtime_error(
                "Texture '" + std::string{name} +
                "' dimension 3d requires array_layers=1");
        }
        if (loaded.width > limits.maxImageDimension3D ||
            loaded.height > limits.maxImageDimension3D ||
            loaded.depth > limits.maxImageDimension3D) {
            throw std::runtime_error(
                "Texture '" + std::string{name} +
                "' dimension 3d exceeds device "
                "maxImageDimension3D " +
                std::to_string(limits.maxImageDimension3D));
        }
        break;
    }
}

MaterialTextureStore::MaterialTextureStore(vk::Device device)
    : device{device},
      nearest_sampler{createSampler(device, vk::Filter::eNearest)},
      linear_sampler{createSampler(device, vk::Filter::eLinear)} {}

vk::Sampler MaterialTextureStore::materialTextureSampler(
    const ResolvedMaterialSampler &sampler) {
    const MaterialSamplerKey key{
        sampler.filter, sampler.mip_filter, sampler.address,
        sampler.compare, sampler.anisotropy_enabled,
        sampler.max_anisotropy};
    const auto existing =
        custom_texture_samplers.find(key);
    if (existing != custom_texture_samplers.end()) {
        return existing->second.get();
    }

    vk::SamplerCreateInfo create_info;
    create_info.magFilter =
        materialTextureFilter(sampler.filter);
    create_info.minFilter = create_info.magFilter;
    create_info.mipmapMode =
        materialTextureMipmapMode(sampler.mip_filter);
    create_info.addressModeU =
        materialTextureAddressMode(sampler.address);
    create_info.addressModeV = create_info.addressModeU;
    create_info.addressModeW = create_info.addressModeU;
    create_info.mipLodBias = 0.0f;
    create_info.anisotropyEnable =
        sampler.anisotropy_enabled;
    create_info.maxAnisotropy =
        sampler.anisotropy_enabled
            ? sampler.max_anisotropy
            : 1.0f;
    create_info.compareEnable =
        sampler.compare != SurfaceTextureCompare::none;
    create_info.compareOp =
        materialTextureCompareOp(sampler.compare);
    create_info.minLod = 0.0f;
    create_info.maxLod = VK_LOD_CLAMP_NONE;
    create_info.borderColor =
        vk::BorderColor::eIntOpaqueBlack;
    create_info.unnormalizedCoordinates = false;
    auto created =
        device.createSamplerUnique(create_info);
    const auto handle = created.get();
    custom_texture_samplers.emplace(
        key, std::move(created));
    return handle;
}

GlobalTextureId MaterialTextureStore::registerTexture(
    vk::Extent3D extent, const void *data) {
    return registerTexture(extent, data, vk::Format::eR8G8B8A8Unorm,
                           static_cast<vk::DeviceSize>(extent.width) * extent.height * extent.depth * 4);
}

GlobalTextureId MaterialTextureStore::registerTexture(
    vk::Extent3D extent, const void *data, vk::Format format,
    vk::DeviceSize bytes_num) {
    const auto &vkcore = GET_MODULE(VulkanManageCore);
    const bool rgba8 = format == vk::Format::eR8G8B8A8Unorm;
    // A raw glTF image can feed both data (UNORM) and color (SRGB) material
    // bindings. Vulkan decodes an SRGB image before a linear blit, but this
    // backing image is created as UNORM; SRGB is only a mutable sampling view.
    // The GPU therefore filters stored values, matching WP246's encoded-value
    // averaging rather than applying a color-only transfer function.
    const auto mip_levels = rgba8
                                ? validateRgba8MipInput(
                                      extent, data, bytes_num)
                                : 1u;
    const std::array mutable_formats{vk::Format::eR8G8B8A8Unorm, vk::Format::eR8G8B8A8Srgb};
    auto image = vkcore.allocImage(extent, format,
                                   vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst |
                                       vk::ImageUsageFlagBits::eTransferSrc,
                                   vma::MemoryUsage::eAutoPreferDevice, {}, VulkanProcessType::graphics,
                                   rgba8 ? std::span<const vk::Format>{mutable_formats}
                                         : std::span<const vk::Format>{},
                                   mip_levels);

    auto &vkutil = GET_MODULE(VulkanUtils);
    const VulkanUtils::ImageTransferInfo transfer_info{
        .old_layout = vk::ImageLayout::eUndefined,
        .new_layout = vk::ImageLayout::eShaderReadOnlyOptimal,
        .dst_stage = vk::PipelineStageFlagBits::eVertexShader |
                     vk::PipelineStageFlagBits::eFragmentShader |
                     vk::PipelineStageFlagBits::eComputeShader,
        .dst_access = vk::AccessFlagBits::eShaderRead,
    };
    if (rgba8) {
        vkutil.safeTransferMemoryToImageAndGenerateMipmaps(
            image, data, bytes_num, transfer_info);
    } else {
        vkutil.safeTransferMemoryToImage(image, data, bytes_num,
                                         transfer_info);
    }

    auto linear_view = createImageView(device, image, format);
    vk::UniqueImageView srgb_view;
    if (rgba8) {
        const auto features = vkcore.getPhysDevice()
                                  .getFormatProperties(vk::Format::eR8G8B8A8Srgb)
                                  .optimalTilingFeatures;
        const auto required = vk::FormatFeatureFlagBits::eSampledImage |
                              vk::FormatFeatureFlagBits::eSampledImageFilterLinear;
        if ((features & required) != required) {
            throw std::runtime_error("SRGB material texture lacks sampled/linear-filter support");
        }
        srgb_view = createImageView(device, image, vk::Format::eR8G8B8A8Srgb);
    }

    return textures.reg(MaterialTextureResource{
        .image = std::move(image),
        .linear_view = std::move(linear_view),
        .srgb_view = std::move(srgb_view),
        .dimension = SurfaceTextureDimension::two_d,
        .view_type = vk::ImageViewType::e2D,
    });
}

MaterialTextureResource
MaterialTextureStore::createTextureResource(
    const LoadedImage &loaded, std::string_view name) const {
    if (loaded.pixels.empty() || loaded.levels.empty())
        throw std::runtime_error("Texture '" + std::string{name} + "' has no image levels");
    const auto &vkcore = GET_MODULE(VulkanManageCore);
    const auto format = vkFormatForLoadedImage(loaded.format);
    requireCompressedTextureFeatures(vkcore.getPhysDevice(), format, name);
    validateLoadedTextureShape(
        loaded, name,
        vkcore.getPhysDevice().getProperties().limits);
    const auto dimension =
        surfaceTextureDimensionOf(loaded.dimension);
    const auto image_type =
        materialTextureImageType(dimension);
    const auto image_flags =
        materialTextureImageFlags(dimension);
    const vk::Extent3D extent{
        loaded.width, loaded.height, loaded.depth};

    const bool rgba8 = loaded.format == ImagePixelFormat::Rgba8Unorm ||
                       loaded.format == ImagePixelFormat::Rgba8Srgb;
    const bool bc7 = loaded.format == ImagePixelFormat::Bc7Unorm ||
                     loaded.format == ImagePixelFormat::Bc7Srgb;
    const std::array rgba_formats{vk::Format::eR8G8B8A8Unorm, vk::Format::eR8G8B8A8Srgb};
    const std::array bc7_formats{vk::Format::eBc7UnormBlock, vk::Format::eBc7SrgbBlock};
    const auto compatible = rgba8 ? std::span<const vk::Format>{rgba_formats}
                                  : bc7 ? std::span<const vk::Format>{bc7_formats}
                                        : std::span<const vk::Format>{};
    auto image = vkcore.allocImage(
        extent, format,
        vk::ImageUsageFlagBits::eSampled |
            vk::ImageUsageFlagBits::eTransferDst |
            vk::ImageUsageFlagBits::eTransferSrc,
        vma::MemoryUsage::eAutoPreferDevice, {},
        VulkanProcessType::graphics, compatible,
        loaded.mipLevels(), vk::SampleCountFlagBits::e1,
        loaded.array_layers, {}, image_flags, image_type);
    std::vector<vk::BufferImageCopy> regions;
    regions.reserve(loaded.levels.size());
    for (std::uint32_t mip = 0; mip < loaded.levels.size(); ++mip) {
        const auto &level = loaded.levels[mip];
        vk::BufferImageCopy copy;
        copy.bufferOffset = level.offset;
        copy.imageSubresource = {
            vk::ImageAspectFlagBits::eColor, mip, 0,
            loaded.dimension ==
                    LoadedImageDimension::ThreeD
                ? 1u
                : loaded.array_layers};
        copy.imageExtent = vk::Extent3D{
            level.width, level.height, level.depth};
        regions.push_back(copy);
    }
    GET_MODULE(VulkanUtils).safeTransferMemoryToImageLevels(
        image, loaded.pixels.data(), loaded.pixels.size(), regions,
        VulkanUtils::ImageTransferInfo{.old_layout = vk::ImageLayout::eUndefined,
                                       .new_layout = vk::ImageLayout::eShaderReadOnlyOptimal,
                                       .dst_stage = vk::PipelineStageFlagBits::eVertexShader |
                                                    vk::PipelineStageFlagBits::eFragmentShader |
                                                    vk::PipelineStageFlagBits::eComputeShader,
                                       .dst_access = vk::AccessFlagBits::eShaderRead});

    const auto linear_format = rgba8 ? vk::Format::eR8G8B8A8Unorm
                                     : bc7 ? vk::Format::eBc7UnormBlock : format;
    requireCompressedTextureFeatures(vkcore.getPhysDevice(), linear_format, name);
    auto linear_view =
        createImageView(device, image, linear_format,
                        dimension);
    vk::UniqueImageView srgb_view;
    if (rgba8 || bc7) {
        const auto srgb_format = rgba8 ? vk::Format::eR8G8B8A8Srgb : vk::Format::eBc7SrgbBlock;
        const auto features = vkcore.getPhysDevice().getFormatProperties(srgb_format).optimalTilingFeatures;
        const auto required = vk::FormatFeatureFlagBits::eSampledImage |
                              vk::FormatFeatureFlagBits::eSampledImageFilterLinear;
        if ((features & required) != required)
            throw std::runtime_error("KTX2 texture '" + std::string{name} + "' SRGB view " +
                                     vk::to_string(srgb_format) + " lacks sampled/linear-filter GPU support");
        srgb_view = createImageView(
            device, image, srgb_format, dimension);
    }
    return {
        std::move(image), std::move(linear_view),
        std::move(srgb_view), dimension,
        materialTextureViewType(dimension)};
}

GlobalTextureId MaterialTextureStore::registerTexture(
    const LoadedImage &loaded, std::string_view name) {
    return textures.reg(createTextureResource(loaded, name));
}

GlobalTextureId MaterialTextureStore::registerTextureFile(
    const std::filesystem::path &path) {
    return registerTexture(loadImageFile(path), path.string());
}

bool MaterialTextureStore::textureShapeMatches(
    GlobalTextureId texture,
    const LoadedImage &image) const {
    const auto &resource = textures.get(texture);
    const auto &live = resource.image;
    return resource.dimension ==
               surfaceTextureDimensionOf(image.dimension) &&
           live.extent ==
               vk::Extent3D{image.width, image.height,
                            image.depth} &&
           live.array_layers == image.array_layers &&
           live.format == vkFormatForLoadedImage(image.format) &&
           live.mip_levels == image.mipLevels();
}

void MaterialTextureStore::uploadTextureInPlace(
    GlobalTextureId texture,
    const LoadedImage &image) const {
    if (!textureShapeMatches(texture, image)) {
        throw std::runtime_error(
            "in-place texture upload requires identical "
            "dimension/extent/layers/format/mips");
    }
    // The logical image handle must stay unchanged, so there is no old image
    // to defer. Drain prior readers before writing the live allocation.
    GET_MODULE(VulkanManageCore).waitIdle();
    std::vector<vk::BufferImageCopy> regions;
    regions.reserve(image.levels.size());
    for (std::uint32_t mip = 0; mip < image.levels.size(); ++mip) {
        const auto &level = image.levels[mip];
        vk::BufferImageCopy copy;
        copy.bufferOffset = level.offset;
        copy.imageSubresource = {
            vk::ImageAspectFlagBits::eColor, mip, 0,
            image.dimension ==
                    LoadedImageDimension::ThreeD
                ? 1u
                : image.array_layers};
        copy.imageExtent = vk::Extent3D{
            level.width, level.height, level.depth};
        regions.push_back(copy);
    }
    GET_MODULE(VulkanUtils).safeTransferMemoryToImageLevels(
        textures.get(texture).image, image.pixels.data(), image.pixels.size(), regions,
        VulkanUtils::ImageTransferInfo{
            .old_layout = vk::ImageLayout::eShaderReadOnlyOptimal,
            .new_layout = vk::ImageLayout::eShaderReadOnlyOptimal,
            .dst_stage = vk::PipelineStageFlagBits::eVertexShader |
                         vk::PipelineStageFlagBits::eFragmentShader |
                         vk::PipelineStageFlagBits::eComputeShader,
            .dst_access = vk::AccessFlagBits::eShaderRead});
}

std::pair<vk::ImageView, vk::ImageView>
MaterialTextureStore::textureViewsForTesting(
    GlobalTextureId texture) const {
    const auto &resource = textures.get(texture);
    return {resource.linear_view.get(), resource.srgb_view.get()};
}

std::vector<std::uint8_t>
MaterialTextureStore::texturePixelsForTesting(
    GlobalTextureId texture,
    std::uint32_t mip_level) const {
    const auto &resource = textures.get(texture);
    if (resource.dimension !=
        SurfaceTextureDimension::two_d) {
        throw std::runtime_error(
            "texture test readback only supports dimension 2d");
    }
    if (resource.image.format != vk::Format::eR8G8B8A8Unorm &&
        resource.image.format != vk::Format::eR8G8B8A8Srgb) {
        throw std::runtime_error("texture test readback only supports RGBA8");
    }
    if (mip_level >= resource.image.mip_levels) {
        throw std::runtime_error(
            "texture test readback mip level is out of range");
    }
    const auto width =
        std::max<std::uint32_t>(1, resource.image.extent.width >> mip_level);
    const auto height =
        std::max<std::uint32_t>(1, resource.image.extent.height >> mip_level);
    const auto bytes = static_cast<vk::DeviceSize>(width) * height * 4;
    auto &vkcore = GET_MODULE(VulkanManageCore);
    auto staging = vkcore.allocBuf(bytes, vk::BufferUsageFlagBits::eTransferDst,
                                   vma::MemoryUsage::eAutoPreferHost,
                                   vma::AllocationCreateFlagBits::eHostAccessRandom);
    auto &utils = GET_MODULE(VulkanUtils);
    utils.executeOneTimeCmd(
        [&](vk::CommandBuffer command) {
            utils.changeImageLayoutCmd(
                command, resource.image, vk::ImageLayout::eShaderReadOnlyOptimal,
                vk::ImageLayout::eTransferSrcOptimal,
                {.src_stage = vk::PipelineStageFlagBits::eVertexShader |
                              vk::PipelineStageFlagBits::eFragmentShader |
                              vk::PipelineStageFlagBits::eComputeShader,
                 .dst_stage = vk::PipelineStageFlagBits::eTransfer,
                 .src_access = vk::AccessFlagBits::eShaderRead,
                 .dst_access = vk::AccessFlagBits::eTransferRead});
            vk::BufferImageCopy copy;
            copy.imageSubresource = {
                vk::ImageAspectFlagBits::eColor, mip_level, 0, 1};
            copy.imageExtent = vk::Extent3D{width, height, 1};
            command.copyImageToBuffer(resource.image.image.get(),
                                      vk::ImageLayout::eTransferSrcOptimal,
                                      staging.buffer.get(), copy);
            utils.changeImageLayoutCmd(
                command, resource.image, vk::ImageLayout::eTransferSrcOptimal,
                vk::ImageLayout::eShaderReadOnlyOptimal,
                {.src_stage = vk::PipelineStageFlagBits::eTransfer,
                 .dst_stage = vk::PipelineStageFlagBits::eVertexShader |
                              vk::PipelineStageFlagBits::eFragmentShader |
                              vk::PipelineStageFlagBits::eComputeShader,
                 .src_access = vk::AccessFlagBits::eTransferRead,
                 .dst_access = vk::AccessFlagBits::eShaderRead});
        },
        true);
    return vkcore.readBuf(staging, bytes);
}

} // namespace Pelican
