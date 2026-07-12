#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
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

} // namespace Pelican::TestKtx2
