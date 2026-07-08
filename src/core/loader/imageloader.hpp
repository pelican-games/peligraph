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
    Rgba16Sfloat,
    Rgba32Sfloat,
};

struct LoadedImage {
    uint32_t width = 0;
    uint32_t height = 0;
    ImagePixelFormat format = ImagePixelFormat::Rgba8Unorm;
    std::vector<std::byte> pixels;

    size_t bytesPerPixel() const;
};

LoadedImage loadImageFile(const std::filesystem::path &path);
LoadedImage loadImageMemory(std::span<const std::byte> data, std::string_view name);

} // namespace Pelican
