#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string_view>
#include <vector>

namespace Pelican {

enum class ImagePixelFormat {
    Rgba8Unorm,
    Rgba8Srgb,
    Rgba16Sfloat,
    Rgba32Sfloat,
    Bc5Unorm,
    Bc7Unorm,
    Bc7Srgb,
    // IA-1: one- and two-channel float data from KTX2.
    R16Sfloat,
    R16G16Sfloat,
    R32Sfloat,
    R32G32Sfloat,
};

// IA-1: the Vulkan name of a pixel format without the VK_FORMAT_ prefix
// ("R32G32B32A32_SFLOAT"), the spelling of a render target's `format`.
std::string_view imagePixelFormatName(ImagePixelFormat format);

enum class LoadedImageDimension {
    TwoD,
    Cube,
    TwoDArray,
    ThreeD,
};

struct LoadedImageLevel {
    size_t offset = 0;
    size_t size = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t depth = 1;
};

struct LoadedImage {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t depth = 1;
    uint32_t array_layers = 1;
    LoadedImageDimension dimension =
        LoadedImageDimension::TwoD;
    ImagePixelFormat format = ImagePixelFormat::Rgba8Unorm;
    // IA-1: the file states its format, color space included (KTX2). A PNG
    // or JPG decodes to RGBA8 without saying whether it is sRGB.
    bool color_space_declared = false;
    std::vector<std::byte> pixels;
    std::vector<LoadedImageLevel> levels;

    size_t bytesPerPixel() const;
    bool isBlockCompressed() const noexcept;
    uint32_t mipLevels() const noexcept { return static_cast<uint32_t>(levels.empty() ? 1 : levels.size()); }
};

std::string_view loadedImageDimensionName(
    LoadedImageDimension dimension);

LoadedImage loadImageFile(const std::filesystem::path &path);
LoadedImage loadImageMemory(std::span<const std::byte> data, std::string_view name);

} // namespace Pelican
