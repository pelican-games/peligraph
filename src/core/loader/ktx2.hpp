#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace Pelican {

enum class Ktx2Format {
    Rgba8Unorm,
    Rgba8Srgb,
    Bc5Unorm,
    Bc7Unorm,
    Bc7Srgb,
};

struct Ktx2Level {
    std::size_t offset = 0;
    std::size_t size = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

struct Ktx2Image {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    Ktx2Format format = Ktx2Format::Rgba8Unorm;
    std::vector<std::byte> payload;
    std::vector<Ktx2Level> levels;
};

// Parses the deliberately small WP92 KTX2 lane. The returned payload is packed
// level-by-level so it can be copied to a single Vulkan staging buffer.
Ktx2Image parseKtx2(std::span<const std::byte> data, std::string_view name);

} // namespace Pelican
