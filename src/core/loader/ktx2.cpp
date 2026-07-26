#include "ktx2.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>

namespace Pelican {
namespace {

constexpr std::array<std::uint8_t, 12> identifier{
    0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32, 0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A,
};
constexpr std::size_t headerSize = 80;
constexpr std::size_t levelEntrySize = 24;

[[noreturn]] void unsupported(std::string_view name, std::string_view reason) {
    throw std::runtime_error("Unsupported KTX2 texture '" + std::string{name} + "': " +
                             std::string{reason});
}

[[noreturn]] void invalid(std::string_view name, std::string_view reason) {
    throw std::runtime_error("Invalid KTX2 texture '" + std::string{name} + "': " +
                             std::string{reason});
}

template <class T> T readLe(std::span<const std::byte> data, std::size_t offset, std::string_view name) {
    if (offset > data.size() || sizeof(T) > data.size() - offset) invalid(name, "truncated header/index");
    T value{};
    std::memcpy(&value, data.data() + offset, sizeof(T));
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    if constexpr (sizeof(T) == 4) value = __builtin_bswap32(value);
    if constexpr (sizeof(T) == 8) value = __builtin_bswap64(value);
#endif
    return value;
}

std::uint32_t fullMipCount(std::uint32_t width,
                           std::uint32_t height,
                           std::uint32_t depth) {
    std::uint32_t count = 1;
    while (width > 1 || height > 1 || depth > 1) {
        width = std::max(1u, width / 2);
        height = std::max(1u, height / 2);
        depth = std::max(1u, depth / 2);
        ++count;
    }
    return count;
}

struct FormatInfo {
    Ktx2Format format;
    bool srgb;
    bool block_compressed;
};

FormatInfo formatInfo(std::uint32_t vk_format, std::string_view name) {
    // VkFormat numeric values are stable parts of the KTX2 on-disk contract.
    switch (vk_format) {
    case 37: return {Ktx2Format::Rgba8Unorm, false, false}; // VK_FORMAT_R8G8B8A8_UNORM
    case 43: return {Ktx2Format::Rgba8Srgb, true, false};   // VK_FORMAT_R8G8B8A8_SRGB
    case 141: return {Ktx2Format::Bc5Unorm, false, true};  // VK_FORMAT_BC5_UNORM_BLOCK
    case 145: return {Ktx2Format::Bc7Unorm, false, true};  // VK_FORMAT_BC7_UNORM_BLOCK
    case 146: return {Ktx2Format::Bc7Srgb, true, true};    // VK_FORMAT_BC7_SRGB_BLOCK
    default:
        unsupported(name, "VkFormat " + std::to_string(vk_format) +
                              " is outside the WP92 RGBA8/BC7/BC5 subset");
    }
}

std::uint64_t checkedMultiply(std::uint64_t left,
                              std::uint64_t right,
                              std::string_view name) {
    if (right != 0 &&
        left > std::numeric_limits<std::uint64_t>::max() /
                   right) {
        invalid(name, "mip level byte size overflows uint64");
    }
    return left * right;
}

std::uint64_t expectedLevelSize(
    const FormatInfo &format, std::uint32_t width,
    std::uint32_t height, std::uint32_t depth,
    std::uint32_t array_layers, std::string_view name) {
    auto size = format.block_compressed
                    ? checkedMultiply(
                          checkedMultiply(
                              std::max(1u, (width + 3) / 4),
                              std::max(1u, (height + 3) / 4),
                              name),
                          16, name)
                    : checkedMultiply(
                          checkedMultiply(width, height, name),
                          4, name);
    size = checkedMultiply(size, depth, name);
    return checkedMultiply(size, array_layers, name);
}

struct TextureShape {
    Ktx2Dimension dimension = Ktx2Dimension::TwoD;
    std::uint32_t depth = 1;
    std::uint32_t array_layers = 1;
};

TextureShape textureShape(
    std::uint32_t width, std::uint32_t height,
    std::uint32_t depth, std::uint32_t layer_count,
    std::uint32_t face_count, std::string_view name) {
    if (width == 0 || height == 0) {
        invalid(name, "width and height must be non-zero");
    }
    if (face_count != 1 && face_count != 6) {
        invalid(name, "faceCount must be 1 or 6");
    }
    if (depth != 0) {
        if (face_count != 1) {
            invalid(name,
                    "3D textures cannot contain cubemap faces");
        }
        if (layer_count != 0) {
            unsupported(name,
                        "3D array textures are outside the "
                        "WP209a public dimension set");
        }
        return {Ktx2Dimension::ThreeD, depth, 1};
    }
    if (face_count == 6) {
        if (layer_count != 0) {
            unsupported(name,
                        "cubemap arrays are outside the WP209a "
                        "public dimension set");
        }
        if (width != height) {
            invalid(name,
                    "cubemap width and height must match");
        }
        return {Ktx2Dimension::Cube, 1, 6};
    }
    if (layer_count != 0) {
        return {Ktx2Dimension::TwoDArray, 1,
                layer_count};
    }
    return {Ktx2Dimension::TwoD, 1, 1};
}

void validateDfd(std::span<const std::byte> data, std::uint32_t offset, std::uint32_t length,
                 const FormatInfo &format, std::string_view name) {
    if (length < 28 || offset > data.size() || length > data.size() - offset)
        invalid(name, "DFD is missing or truncated");
    const auto total_size = readLe<std::uint32_t>(data, offset, name);
    if (total_size < 28 || total_size != length) invalid(name, "DFD totalSize does not match dfdByteLength");
    if (readLe<std::uint32_t>(data, offset + 4, name) != 0)
        unsupported(name, "non-Khronos/basic DFD descriptor");
    if (readLe<std::uint16_t>(data, offset + 8, name) != 2)
        unsupported(name, "DFD descriptor version is not 2");
    const auto descriptor_block_size = readLe<std::uint16_t>(data, offset + 10, name);
    if (descriptor_block_size < 24 || std::uint32_t{descriptor_block_size} + 4 > total_size)
        invalid(name, "DFD descriptor block is invalid");
    const auto transfer = static_cast<std::uint8_t>(data[offset + 14]);
    const auto color_model = static_cast<std::uint8_t>(data[offset + 12]);
    const auto expected_model = format.format == Ktx2Format::Bc5Unorm ? std::uint8_t{132}
                              : (format.format == Ktx2Format::Bc7Unorm || format.format == Ktx2Format::Bc7Srgb)
                                  ? std::uint8_t{134} : std::uint8_t{1};
    if (color_model != expected_model)
        invalid(name, "DFD color model does not match VkFormat");
    constexpr std::uint8_t linearTransfer = 1;
    constexpr std::uint8_t srgbTransfer = 2;
    if (transfer != (format.srgb ? srgbTransfer : linearTransfer))
        invalid(name, format.srgb ? "DFD transfer function is not sRGB"
                                  : "DFD transfer function is not linear");
    const auto block_x = static_cast<std::uint8_t>(data[offset + 16]);
    const auto block_y = static_cast<std::uint8_t>(data[offset + 17]);
    const auto bytes_plane0 = static_cast<std::uint8_t>(data[offset + 20]);
    if (format.block_compressed) {
        if (block_x != 3 || block_y != 3 || bytes_plane0 != 16)
            invalid(name, "DFD does not describe 4x4 16-byte BC blocks");
    } else if (block_x != 0 || block_y != 0 || bytes_plane0 != 4) {
        invalid(name, "DFD does not describe 1x1 4-byte RGBA8 texels");
    }
}

} // namespace

Ktx2Image parseKtx2(std::span<const std::byte> data, std::string_view name) {
    if (data.size() < headerSize) invalid(name, "file is shorter than the KTX2 header");
    if (!std::equal(identifier.begin(), identifier.end(),
                    reinterpret_cast<const std::uint8_t *>(data.data())))
        invalid(name, "identifier does not match KTX2");

    const auto vk_format = readLe<std::uint32_t>(data, 12, name);
    const auto type_size = readLe<std::uint32_t>(data, 16, name);
    const auto width = readLe<std::uint32_t>(data, 20, name);
    const auto height = readLe<std::uint32_t>(data, 24, name);
    const auto depth = readLe<std::uint32_t>(data, 28, name);
    const auto layer_count = readLe<std::uint32_t>(data, 32, name);
    const auto face_count = readLe<std::uint32_t>(data, 36, name);
    const auto level_count = readLe<std::uint32_t>(data, 40, name);
    const auto supercompression = readLe<std::uint32_t>(data, 44, name);
    const auto dfd_offset = readLe<std::uint32_t>(data, 48, name);
    const auto dfd_length = readLe<std::uint32_t>(data, 52, name);
    const auto kvd_offset = readLe<std::uint32_t>(data, 56, name);
    const auto kvd_length = readLe<std::uint32_t>(data, 60, name);
    const auto sgd_offset = readLe<std::uint64_t>(data, 64, name);
    const auto sgd_length = readLe<std::uint64_t>(data, 72, name);

    const auto format = formatInfo(vk_format, name);
    if (type_size != 1) unsupported(name, "typeSize must be 1 for the supported formats");
    const auto shape = textureShape(
        width, height, depth, layer_count, face_count,
        name);
    if (supercompression != 0)
        unsupported(name, "supercompression scheme " + std::to_string(supercompression) +
                              " is not supported (Basis/zstd are future extensions)");
    if (sgd_offset != 0 || sgd_length != 0)
        unsupported(name, "supercompression global data is not supported");
    if ((kvd_offset == 0) != (kvd_length == 0) ||
        (kvd_length != 0 && (kvd_offset > data.size() || kvd_length > data.size() - kvd_offset)))
        invalid(name, "key/value data range is invalid");
    const auto expected_mips = fullMipCount(
        width, height, shape.depth);
    if (level_count != expected_mips)
        invalid(name, "incomplete mip chain: expected " + std::to_string(expected_mips) +
                          " levels for " + std::to_string(width) + "x" + std::to_string(height) +
                          ", found " + std::to_string(level_count));
    if (level_count > (data.size() - headerSize) / levelEntrySize)
        invalid(name, "level index is truncated");
    if (dfd_offset < headerSize + std::size_t{level_count} * levelEntrySize || dfd_offset % 4 != 0)
        invalid(name, "DFD overlaps the header/level index or is not 4-byte aligned");

    validateDfd(data, dfd_offset, dfd_length, format, name);

    Ktx2Image result;
    result.width = width;
    result.height = height;
    result.depth = shape.depth;
    result.array_layers = shape.array_layers;
    result.dimension = shape.dimension;
    result.format = format.format;
    result.levels.reserve(level_count);
    const auto protected_end = std::max<std::uint64_t>(
        headerSize + std::uint64_t{level_count} * levelEntrySize,
        std::max<std::uint64_t>(std::uint64_t{dfd_offset} + dfd_length,
                                std::uint64_t{kvd_offset} + kvd_length));
    std::vector<std::pair<std::uint64_t, std::uint64_t>> level_ranges;
    level_ranges.reserve(level_count);
    std::uint32_t level_width = width;
    std::uint32_t level_height = height;
    std::uint32_t level_depth = shape.depth;
    for (std::uint32_t level = 0; level < level_count; ++level) {
        const auto entry = headerSize + std::size_t{level} * levelEntrySize;
        const auto byte_offset = readLe<std::uint64_t>(data, entry, name);
        const auto byte_length = readLe<std::uint64_t>(data, entry + 8, name);
        const auto uncompressed_length = readLe<std::uint64_t>(data, entry + 16, name);
        const auto expected = expectedLevelSize(
            format, level_width, level_height, level_depth,
            shape.array_layers, name);
        if (byte_length != expected || uncompressed_length != expected)
            invalid(name, "level " + std::to_string(level) + " byte length is " +
                              std::to_string(byte_length) + ", expected " + std::to_string(expected));
        if (byte_offset > data.size() || byte_length > data.size() - byte_offset)
            invalid(name, "level " + std::to_string(level) + " data range is outside the file");
        const auto alignment = format.block_compressed ? std::uint64_t{16} : std::uint64_t{4};
        if (byte_offset < protected_end || byte_offset % alignment != 0)
            invalid(name, "level " + std::to_string(level) +
                              " overlaps metadata or violates texel-block alignment");
        for (const auto &[begin, end] : level_ranges) {
            if (byte_offset < end && begin < byte_offset + byte_length)
                invalid(name, "level " + std::to_string(level) + " overlaps another level");
        }
        level_ranges.emplace_back(byte_offset, byte_offset + byte_length);
        if (result.payload.size() > std::numeric_limits<std::size_t>::max() - byte_length)
            invalid(name, "level payload size overflows host size_t");
        const auto packed_offset = result.payload.size();
        result.payload.insert(result.payload.end(), data.begin() + static_cast<std::size_t>(byte_offset),
                              data.begin() + static_cast<std::size_t>(byte_offset + byte_length));
        result.levels.push_back(
            {packed_offset,
             static_cast<std::size_t>(byte_length), level_width,
             level_height, level_depth});
        level_width = std::max(1u, level_width / 2);
        level_height = std::max(1u, level_height / 2);
        level_depth = std::max(1u, level_depth / 2);
    }
    return result;
}

} // namespace Pelican
