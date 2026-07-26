#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <vector>

namespace Pelican::TestKtx2 {

template <class T> inline void appendLe(std::vector<std::byte> &bytes, T value) {
    const auto old_size = bytes.size();
    bytes.resize(old_size + sizeof(T));
    std::memcpy(bytes.data() + old_size, &value, sizeof(T));
}

inline void writeLe64(std::vector<std::byte> &bytes, size_t offset, std::uint64_t value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

inline std::vector<std::byte> makeRgba8Srgb188() {
    static constexpr std::array<std::uint8_t, 12> id{
        0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32, 0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A,
    };
    std::vector<std::byte> bytes;
    for (auto value : id) bytes.push_back(static_cast<std::byte>(value));
    appendLe(bytes, std::uint32_t{43}); // VK_FORMAT_R8G8B8A8_SRGB
    appendLe(bytes, std::uint32_t{1});
    appendLe(bytes, std::uint32_t{2});
    appendLe(bytes, std::uint32_t{2});
    appendLe(bytes, std::uint32_t{0});
    appendLe(bytes, std::uint32_t{0});
    appendLe(bytes, std::uint32_t{1});
    appendLe(bytes, std::uint32_t{2});
    appendLe(bytes, std::uint32_t{0});
    appendLe(bytes, std::uint32_t{128}); // DFD follows two level entries
    appendLe(bytes, std::uint32_t{92});
    appendLe(bytes, std::uint32_t{0});
    appendLe(bytes, std::uint32_t{0});
    appendLe(bytes, std::uint64_t{0});
    appendLe(bytes, std::uint64_t{0});
    bytes.resize(128);
    writeLe64(bytes, 80, 224);
    writeLe64(bytes, 88, 16);
    writeLe64(bytes, 96, 16);
    writeLe64(bytes, 104, 220);
    writeLe64(bytes, 112, 4);
    writeLe64(bytes, 120, 4);
    appendLe(bytes, std::uint32_t{92});
    appendLe(bytes, std::uint32_t{0});
    appendLe(bytes, std::uint16_t{2});
    appendLe(bytes, std::uint16_t{88});
    bytes.insert(bytes.end(), {std::byte{1}, std::byte{1}, std::byte{2}, std::byte{0}});
    bytes.insert(bytes.end(), {std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}});
    appendLe(bytes, std::uint32_t{4});
    appendLe(bytes, std::uint32_t{0});
    for (std::uint16_t channel = 0; channel < 4; ++channel) {
        appendLe(bytes, static_cast<std::uint16_t>(channel * 8));
        bytes.push_back(std::byte{7});
        bytes.push_back(static_cast<std::byte>(channel == 3 ? 15 : channel));
        appendLe(bytes, std::uint32_t{0});
        appendLe(bytes, std::uint32_t{0});
        appendLe(bytes, std::uint32_t{255});
    }
    bytes.insert(bytes.end(), 4, std::byte{188});
    bytes.insert(bytes.end(), 16, std::byte{188});
    return bytes;
}

inline std::vector<std::byte> makeRgba8Unorm1x1(
    std::uint32_t depth, std::uint32_t layers,
    std::uint32_t faces,
    std::span<const std::array<std::uint8_t, 4>> texels) {
    const auto image_count =
        static_cast<std::size_t>(std::max(1u, depth)) *
        static_cast<std::size_t>(std::max(1u, layers)) *
        static_cast<std::size_t>(faces);
    if (texels.size() != image_count) {
        throw std::invalid_argument(
            "KTX2 test texel count does not match shape");
    }
    static constexpr std::array<std::uint8_t, 12> id{
        0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32, 0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A,
    };
    std::vector<std::byte> bytes;
    for (auto value : id) bytes.push_back(static_cast<std::byte>(value));
    appendLe(bytes, std::uint32_t{37}); // VK_FORMAT_R8G8B8A8_UNORM
    appendLe(bytes, std::uint32_t{1});
    appendLe(bytes, std::uint32_t{1});
    appendLe(bytes, std::uint32_t{1});
    appendLe(bytes, depth);
    appendLe(bytes, layers);
    appendLe(bytes, faces);
    appendLe(bytes, std::uint32_t{1});
    appendLe(bytes, std::uint32_t{0});
    appendLe(bytes, std::uint32_t{104}); // DFD follows one level entry
    appendLe(bytes, std::uint32_t{92});
    appendLe(bytes, std::uint32_t{0});
    appendLe(bytes, std::uint32_t{0});
    appendLe(bytes, std::uint64_t{0});
    appendLe(bytes, std::uint64_t{0});
    bytes.resize(104);
    writeLe64(bytes, 80, 196);
    const auto payload_size =
        static_cast<std::uint64_t>(texels.size() * 4);
    writeLe64(bytes, 88, payload_size);
    writeLe64(bytes, 96, payload_size);
    appendLe(bytes, std::uint32_t{92});
    appendLe(bytes, std::uint32_t{0});
    appendLe(bytes, std::uint16_t{2});
    appendLe(bytes, std::uint16_t{88});
    bytes.insert(bytes.end(), {std::byte{1}, std::byte{1}, std::byte{1}, std::byte{0}});
    bytes.insert(bytes.end(), {std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}});
    appendLe(bytes, std::uint32_t{4});
    appendLe(bytes, std::uint32_t{0});
    for (std::uint16_t channel = 0; channel < 4; ++channel) {
        appendLe(bytes, static_cast<std::uint16_t>(channel * 8));
        bytes.push_back(std::byte{7});
        bytes.push_back(static_cast<std::byte>(channel == 3 ? 15 : channel));
        appendLe(bytes, std::uint32_t{0});
        appendLe(bytes, std::uint32_t{0});
        appendLe(bytes, std::uint32_t{255});
    }
    for (const auto &texel : texels) {
        for (const auto channel : texel) {
            bytes.push_back(
                static_cast<std::byte>(channel));
        }
    }
    return bytes;
}

inline std::vector<std::byte>
makeRgba8UnormSingleMip() {
    constexpr std::array<std::array<std::uint8_t, 4>, 1>
        texels{
        std::array<std::uint8_t, 4>{64, 64, 64, 64},
    };
    return makeRgba8Unorm1x1(0, 0, 1, texels);
}

} // namespace Pelican::TestKtx2
