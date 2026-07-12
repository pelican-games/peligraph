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
};

struct LoadedImageLevel {
    size_t offset = 0;
    size_t size = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

struct LoadedImage {
    uint32_t width = 0;
    uint32_t height = 0;
    ImagePixelFormat format = ImagePixelFormat::Rgba8Unorm;
    std::vector<std::byte> pixels;
    std::vector<LoadedImageLevel> levels;

    size_t bytesPerPixel() const;
    bool isBlockCompressed() const noexcept;
    uint32_t mipLevels() const noexcept { return static_cast<uint32_t>(levels.empty() ? 1 : levels.size()); }
};

LoadedImage loadImageFile(const std::filesystem::path &path);
LoadedImage loadImageMemory(std::span<const std::byte> data, std::string_view name);

} // namespace Pelican
