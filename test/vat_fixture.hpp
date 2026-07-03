#pragma once

#include <array>
#include <cstring>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <vector>

namespace Pelican::TestVatFixture {

namespace detail {

inline void align4(std::vector<uint8_t> &bytes) {
    while (bytes.size() % 4 != 0) {
        bytes.push_back(0);
    }
}

inline void appendU16(std::vector<uint8_t> &bytes, uint16_t value) {
    bytes.push_back(static_cast<uint8_t>(value & 0xff));
    bytes.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
}

inline void appendU32(std::vector<uint8_t> &bytes, uint32_t value) {
    bytes.push_back(static_cast<uint8_t>(value & 0xff));
    bytes.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
    bytes.push_back(static_cast<uint8_t>((value >> 16) & 0xff));
    bytes.push_back(static_cast<uint8_t>((value >> 24) & 0xff));
}

inline void appendF32(std::vector<uint8_t> &bytes, float value) {
    static_assert(sizeof(float) == sizeof(uint32_t));
    uint32_t raw = 0;
    std::memcpy(&raw, &value, sizeof(float));
    appendU32(bytes, raw);
}

inline size_t appendFloats(std::vector<uint8_t> &bytes, std::initializer_list<float> values) {
    align4(bytes);
    const auto offset = bytes.size();
    for (const auto value : values) {
        appendF32(bytes, value);
    }
    return offset;
}

inline size_t appendU16s(std::vector<uint8_t> &bytes, std::initializer_list<uint16_t> values) {
    align4(bytes);
    const auto offset = bytes.size();
    for (const auto value : values) {
        appendU16(bytes, value);
    }
    align4(bytes);
    return offset;
}

inline uint16_t half01(float value) {
    if (value == 0.0f) {
        return 0x0000;
    }
    if (value == 0.5f) {
        return 0x3800;
    }
    if (value == 1.0f) {
        return 0x3c00;
    }
    throw std::runtime_error("test VAT fixture only encodes 0, 0.5, and 1 half values");
}

inline size_t appendVatPositions(std::vector<uint8_t> &bytes) {
    align4(bytes);
    const auto offset = bytes.size();

    const std::array<std::array<float, 4>, 6> pixels{{
        {0.0f, 0.0f, 0.5f, 1.0f},
        {0.5f, 0.0f, 0.5f, 1.0f},
        {0.0f, 1.0f, 0.5f, 1.0f},
        {0.5f, 0.0f, 0.5f, 1.0f},
        {1.0f, 0.0f, 0.5f, 1.0f},
        {0.5f, 1.0f, 0.5f, 1.0f},
    }};
    for (const auto &pixel : pixels) {
        for (const auto component : pixel) {
            appendU16(bytes, half01(component));
        }
    }
    return offset;
}

inline void writeBytes(const std::filesystem::path &path, const std::vector<uint8_t> &bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file{path, std::ios::binary};
    file.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

} // namespace detail

inline std::vector<uint8_t> makeTinyVatGlb() {
    std::vector<uint8_t> bin;
    const auto position_offset = detail::appendFloats(bin, {
        -0.8f, -0.4f, 0.0f,
         0.0f, -0.4f, 0.0f,
        -0.8f,  0.4f, 0.0f,
    });
    const auto normal_offset = detail::appendFloats(bin, {
        0.0f, 0.0f, 1.0f,
        0.0f, 0.0f, 1.0f,
        0.0f, 0.0f, 1.0f,
    });
    const auto uv_offset = detail::appendFloats(bin, {
        0.0f, 0.0f,
        1.0f, 0.0f,
        0.0f, 1.0f,
    });
    const auto index_offset = detail::appendU16s(bin, {0, 1, 2});
    const auto vat_offset = detail::appendVatPositions(bin);
    const auto bin_length = bin.size();

    const nlohmann::json json{
        {"asset", {{"version", "2.0"}, {"generator", "pelican vat test fixture"}}},
        {"scene", 0},
        {"scenes", {{{"nodes", {0}}}}},
        {"nodes", {{{"mesh", 0}}}},
        {"materials",
         {{
             {"pbrMetallicRoughness",
              {{"baseColorFactor", {1.0, 0.9, 0.2, 1.0}},
               {"metallicFactor", 0.0},
               {"roughnessFactor", 0.8}}},
         }}},
        {"meshes",
         {{
             {"primitives",
              {{
                  {"attributes", {{"POSITION", 0}, {"NORMAL", 1}, {"TEXCOORD_0", 2}}},
                  {"indices", 3},
                  {"material", 0},
                  {"extras",
                   {{"pelican.vat",
                     {{"schema", "pelican.vat"},
                      {"version", 1},
                      {"generator", "pelican vat test fixture"},
                      {"fps", 1.0},
                      {"frame_count", 2},
                      {"vertex_count", 3},
                      {"bounds_min", {-0.8, -0.4, -0.1}},
                      {"bounds_max", {0.8, 0.4, 0.1}},
                      {"loop", false},
                      {"position_view", 4}}}}},
              }}},
         }}},
        {"buffers", {{{"byteLength", bin_length}}}},
        {"bufferViews",
         {
             {{"buffer", 0}, {"byteOffset", position_offset}, {"byteLength", 36}},
             {{"buffer", 0}, {"byteOffset", normal_offset}, {"byteLength", 36}},
             {{"buffer", 0}, {"byteOffset", uv_offset}, {"byteLength", 24}},
             {{"buffer", 0}, {"byteOffset", index_offset}, {"byteLength", 6}},
             {{"buffer", 0}, {"byteOffset", vat_offset}, {"byteLength", 48}},
         }},
        {"accessors",
         {
             {{"bufferView", 0}, {"componentType", 5126}, {"count", 3}, {"type", "VEC3"},
              {"min", {-0.8, -0.4, 0.0}}, {"max", {0.0, 0.4, 0.0}}},
             {{"bufferView", 1}, {"componentType", 5126}, {"count", 3}, {"type", "VEC3"}},
             {{"bufferView", 2}, {"componentType", 5126}, {"count", 3}, {"type", "VEC2"}},
             {{"bufferView", 3}, {"componentType", 5123}, {"count", 3}, {"type", "SCALAR"}},
         }},
    };

    auto json_bytes = json.dump();
    while (json_bytes.size() % 4 != 0) {
        json_bytes.push_back(' ');
    }
    detail::align4(bin);

    std::vector<uint8_t> glb;
    const uint32_t total_length =
        12u + 8u + static_cast<uint32_t>(json_bytes.size()) + 8u + static_cast<uint32_t>(bin.size());
    detail::appendU32(glb, 0x46546c67);
    detail::appendU32(glb, 2);
    detail::appendU32(glb, total_length);
    detail::appendU32(glb, static_cast<uint32_t>(json_bytes.size()));
    detail::appendU32(glb, 0x4e4f534a);
    glb.insert(glb.end(), json_bytes.begin(), json_bytes.end());
    detail::appendU32(glb, static_cast<uint32_t>(bin.size()));
    detail::appendU32(glb, 0x004e4942);
    glb.insert(glb.end(), bin.begin(), bin.end());
    return glb;
}

inline void writeTinyVatGlb(const std::filesystem::path &path) {
    detail::writeBytes(path, makeTinyVatGlb());
}

} // namespace Pelican::TestVatFixture
