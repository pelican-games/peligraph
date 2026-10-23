#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string_view>

namespace Pelican {

struct TextureFileInfo {
    std::string_view dimension;
    bool supports_srgb = false;
};

// Authoring metadata only: ordinary image formats follow the supported
// extension policy; KTX2 reads the identifier and header through faceCount.
// No pixels, level index or compressed payload are read here.
inline TextureFileInfo textureFileInfo(const std::filesystem::path &file) {
    auto extension = file.extension().string();
    std::ranges::transform(extension, extension.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    if (extension == ".png" || extension == ".jpg" || extension == ".jpeg" ||
        extension == ".tga" || extension == ".bmp") return {"2d", true};
    if (extension == ".hdr" || extension == ".exr") return {"2d", false};
    if (extension != ".ktx2") return {};
    std::ifstream input{file, std::ios::binary};
    unsigned char header[40]{};
    if (!input.read(reinterpret_cast<char *>(header), sizeof header)) return {};
    static constexpr unsigned char identifier[12] = {0xAB, 'K', 'T', 'X', ' ', '2', '0', 0xBB, '\r', '\n', 0x1A, '\n'};
    if (!std::equal(std::begin(identifier), std::end(identifier), header)) return {};
    const auto word = [&](std::size_t offset) {
        return static_cast<std::uint32_t>(header[offset]) | (static_cast<std::uint32_t>(header[offset + 1]) << 8) |
               (static_cast<std::uint32_t>(header[offset + 2]) << 16) |
               (static_cast<std::uint32_t>(header[offset + 3]) << 24);
    };
    const auto format = word(12);
    // VK_FORMAT_R8G8B8A8_{UNORM,SRGB}, VK_FORMAT_BC7_{UNORM,SRGB}_BLOCK.
    const bool srgb = format == 37 || format == 43 || format == 145 || format == 146;
    const auto depth = word(28);
    const auto layers = word(32);
    const auto faces = word(36);
    if (depth > 0) return {"3d", srgb};
    if (faces == 6) return {layers > 0 ? "cube_array" : "cube", srgb};
    if (layers > 0) return {"2d_array", srgb};
    return {"2d", srgb};
}

} // namespace Pelican
