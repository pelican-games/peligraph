#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
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
    // IA-1: uncompressed float data (a LUT, a baked analysis), linear.
    R16Sfloat,
    R16G16Sfloat,
    R16G16B16A16Sfloat,
    R32Sfloat,
    R32G32Sfloat,
    R32G32B32A32Sfloat,
};

enum class Ktx2Dimension {
    TwoD,
    Cube,
    TwoDArray,
    ThreeD,
};

struct Ktx2Level {
    std::size_t offset = 0;
    std::size_t size = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t depth = 1;
};

struct Ktx2Image {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t depth = 1;
    std::uint32_t array_layers = 1;
    Ktx2Dimension dimension = Ktx2Dimension::TwoD;
    Ktx2Format format = Ktx2Format::Rgba8Unorm;
    std::vector<std::byte> payload;
    std::vector<Ktx2Level> levels;
};

// Parses the deliberately small WP92 KTX2 lane. The returned payload is packed
// level-by-level so it can be copied to a single Vulkan staging buffer.
// A file holds either one level or the complete mip chain (IA-1: one level is
// a texture without mips; a partial chain is still refused).
Ktx2Image parseKtx2(std::span<const std::byte> data, std::string_view name);

// IA-1: the VkFormat number a format is stored with, its Vulkan name without
// the VK_FORMAT_ prefix ("R32G32B32A32_SFLOAT", the spelling of a render
// target's `format`), the format of such a name, and the bytes of one texel
// (0 for a block-compressed format).
std::uint32_t ktx2VkFormat(Ktx2Format format);
std::string_view ktx2FormatName(Ktx2Format format);
std::optional<Ktx2Format> ktx2FormatFromName(std::string_view name);
std::size_t ktx2TexelBytes(Ktx2Format format);

// IA-1: a single-level, uncompressed 2D KTX2 file (no supercompression, no
// key/value data) that parseKtx2 reads back. `texels` are rows of `width`
// texels packed without padding, little endian. Throws std::invalid_argument
// for a block-compressed format, a zero extent or a byte count that does not
// match width * height * texel bytes.
std::vector<std::byte> writeKtx2(Ktx2Format format, std::uint32_t width, std::uint32_t height,
                                 std::span<const std::byte> texels);

} // namespace Pelican
