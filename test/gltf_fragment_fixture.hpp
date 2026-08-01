#pragma once

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace Pelican::TestGltfFragmentFixture {

namespace detail {

inline void appendU32(std::vector<uint8_t> &bytes, uint32_t value) {
    bytes.push_back(static_cast<uint8_t>(value & 0xff));
    bytes.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
    bytes.push_back(static_cast<uint8_t>((value >> 16) & 0xff));
    bytes.push_back(static_cast<uint8_t>((value >> 24) & 0xff));
}

inline void appendF32(std::vector<uint8_t> &bytes, float value) {
    uint32_t raw = 0;
    std::memcpy(&raw, &value, sizeof(value));
    appendU32(bytes, raw);
}

inline void writeBytes(const std::filesystem::path &path, const std::vector<uint8_t> &bytes) {
    std::ofstream file{path, std::ios::binary};
    file.write(reinterpret_cast<const char *>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
}

} // namespace detail

inline std::vector<uint8_t> makeGlb(bool duplicate_root_names = false,
                                    bool geometry_only = false) {
    std::vector<uint8_t> bin;
    for (const float value : {
             -0.5f, -0.5f, 0.0f,
              0.5f, -0.5f, 0.0f,
              0.0f,  0.5f, 0.0f,
         }) {
        detail::appendF32(bin, value);
    }

    const std::string second_root = duplicate_root_names ? "RootA" : "RootB";
    const std::string pixel_png =
        "data:image/png;base64,"
        "iVBORw0KGgoAAAANSUhEUgAAAAQAAAAECAYAAACp8Z5+AAAAAXNSR0IArs4c6QAAAARn"
        "QU1BAACxjwv8YQUAAAAJcEhZcwAADsMAAA7DAcdvqGQAAAAbSURBVBhXY2BgYPj/////"
        "/3AahcPA8J+BoAoAP0sn2dw5JWEAAAAASUVORK5CYII=";
    nlohmann::json json{
        {"asset", {{"version", "2.0"}, {"generator", "pelican WP77 fixture"}}},
        {"scene", 0},
        {"scenes", {{{"nodes", {0, 2}}}}},
        {"nodes",
         {
             {{"name", "RootA"}, {"children", {1}}},
             {{"name", "Cube"}, {"mesh", 0}},
             {{"name", second_root}, {"children", {3}}},
             {{"name", "Cube"}, {"mesh", 1}},
         }},
        {"materials",
         {
             {{"name", "MatA"},
              {"pbrMetallicRoughness",
               {{"baseColorFactor", {1.0, 0.0, 0.0, 1.0}},
                {"baseColorTexture",
                 {{"index", 0},
                  {"extensions",
                   {{"KHR_texture_transform",
                     {{"offset", {0.125, -0.25}},
                      {"scale", {2.0, 0.5}},
                      {"rotation", 0.25}}}}}}},
                {"metallicRoughnessTexture", {{"index", 2}}}}},
              {"occlusionTexture",
               {{"index", 2}, {"strength", 0.65}}}},
             {{"name", "MatB"},
              {"pbrMetallicRoughness",
               {{"baseColorFactor", {0.0, 1.0, 0.0, 1.0}},
                {"baseColorTexture", {{"index", 1}}}}}},
         }},
        {"images", {{{"name", "Red"}, {"uri", pixel_png}},
                    {{"name", "Green"}, {"uri", pixel_png}},
                    {{"name", "ORM"}, {"uri", pixel_png}}}},
        {"textures", {{{"source", 0}}, {{"source", 1}}, {{"source", 2}}}},
        {"meshes",
         {
             {{"name", "MeshA"},
              {"primitives", {{{"attributes", {{"POSITION", 0}}}, {"material", 0}}}}},
             {{"name", "MeshB"},
              {"primitives", {{{"attributes", {{"POSITION", 0}}}, {"material", 1}}}}},
         }},
        {"animations", {{{"name", "Walk"}}}},
        {"buffers", {{{"byteLength", bin.size()}}}},
        {"bufferViews", {{{"buffer", 0}, {"byteOffset", 0}, {"byteLength", bin.size()}}}},
        {"accessors",
         {{{"bufferView", 0},
           {"componentType", 5126},
           {"count", 3},
           {"type", "VEC3"},
           {"min", {-0.5, -0.5, 0.0}},
           {"max", {0.5, 0.5, 0.0}}}}},
    };
    if (geometry_only) {
        json.erase("materials");
        json.erase("images");
        json.erase("textures");
        for (auto &mesh : json["meshes"]) {
            for (auto &primitive : mesh["primitives"]) primitive.erase("material");
        }
    }

    auto json_bytes = json.dump();
    while (json_bytes.size() % 4 != 0) {
        json_bytes.push_back(' ');
    }
    while (bin.size() % 4 != 0) {
        bin.push_back(0);
    }

    std::vector<uint8_t> glb;
    const auto total_length = static_cast<uint32_t>(12 + 8 + json_bytes.size() + 8 + bin.size());
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

inline std::vector<uint8_t> makeNormalizedPositionGlb() {
    std::vector<uint8_t> bin{
        0x80, 0xc0, 0x00,
        0x7f, 0xc0, 0x00,
        0x00, 0x7f, 0x00,
    };
    nlohmann::json json{
        {"asset",
         {{"version", "2.0"},
          {"generator",
           "pelican normalized accessor fixture"}}},
        {"extensionsUsed", {"KHR_mesh_quantization"}},
        {"extensionsRequired", {"KHR_mesh_quantization"}},
        {"scene", 0},
        {"scenes", {{{"nodes", {0}}}}},
        {"nodes", {{{"name", "Quantized"}, {"mesh", 0}}}},
        {"meshes",
         {{{"name", "QuantizedMesh"},
           {"primitives",
            {{{"attributes", {{"POSITION", 0}}}}}}}}},
        {"buffers", {{{"byteLength", bin.size()}}}},
        {"bufferViews",
         {{{"buffer", 0},
           {"byteOffset", 0},
           {"byteLength", bin.size()}}}},
        {"accessors",
         {{{"bufferView", 0},
           {"componentType", 5120},
           {"normalized", true},
           {"count", 3},
           {"type", "VEC3"},
           {"min", {-128, -64, 0}},
           {"max", {127, 127, 0}}}}},
    };

    auto json_bytes = json.dump();
    while (json_bytes.size() % 4 != 0) {
        json_bytes.push_back(' ');
    }
    while (bin.size() % 4 != 0) {
        bin.push_back(0);
    }

    std::vector<uint8_t> glb;
    const auto total_length = static_cast<uint32_t>(
        12 + 8 + json_bytes.size() + 8 + bin.size());
    detail::appendU32(glb, 0x46546c67);
    detail::appendU32(glb, 2);
    detail::appendU32(glb, total_length);
    detail::appendU32(
        glb, static_cast<uint32_t>(json_bytes.size()));
    detail::appendU32(glb, 0x4e4f534a);
    glb.insert(
        glb.end(), json_bytes.begin(), json_bytes.end());
    detail::appendU32(
        glb, static_cast<uint32_t>(bin.size()));
    detail::appendU32(glb, 0x004e4942);
    glb.insert(glb.end(), bin.begin(), bin.end());
    return glb;
}

inline std::vector<uint8_t> makeMaterialRoutingGlb() {
    std::vector<uint8_t> bin;
    for (const float value : {
             -0.5f, -0.5f, 0.0f,
              0.5f, -0.5f, 0.0f,
              0.0f,  0.5f, 0.0f,
         }) {
        detail::appendF32(bin, value);
    }
    nlohmann::json json{
        {"asset",
         {{"version", "2.0"},
          {"generator",
           "pelican WP240c routing fixture"}}},
        {"scene", 0},
        {"scenes", {{{"nodes", {0, 1, 2}}}}},
        {"nodes",
         {
             {{"name", "Opaque"}, {"mesh", 0}},
             {{"name", "Mask"}, {"mesh", 1}},
             {{"name", "Blend"}, {"mesh", 2}},
         }},
        {"materials",
         {
             {{"name", "OpaqueMaterial"},
              {"alphaMode", "OPAQUE"}},
             {{"name", "MaskMaterial"},
              {"alphaMode", "MASK"},
              {"alphaCutoff", 0.625}},
             {{"name", "BlendMaterial"},
              {"alphaMode", "BLEND"},
              {"doubleSided", true},
              {"pbrMetallicRoughness",
               {{"baseColorFactor",
                 {0.2, 0.4, 0.8, 0.35}}}}},
         }},
        {"meshes",
         {
             {{"name", "OpaqueMesh"},
              {"primitives",
               {{{"attributes", {{"POSITION", 0}}},
                 {"material", 0}}}}},
             {{"name", "MaskMesh"},
              {"primitives",
               {{{"attributes", {{"POSITION", 0}}},
                 {"material", 1}}}}},
             {{"name", "BlendMesh"},
              {"primitives",
               {{{"attributes", {{"POSITION", 0}}},
                 {"material", 2}}}}},
         }},
        {"buffers", {{{"byteLength", bin.size()}}}},
        {"bufferViews",
         {{{"buffer", 0},
           {"byteOffset", 0},
           {"byteLength", bin.size()}}}},
        {"accessors",
         {{{"bufferView", 0},
           {"componentType", 5126},
           {"count", 3},
           {"type", "VEC3"},
           {"min", {-0.5, -0.5, 0.0}},
           {"max", {0.5, 0.5, 0.0}}}}},
    };

    auto json_bytes = json.dump();
    while (json_bytes.size() % 4 != 0) {
        json_bytes.push_back(' ');
    }
    while (bin.size() % 4 != 0) {
        bin.push_back(0);
    }

    std::vector<uint8_t> glb;
    const auto total_length = static_cast<uint32_t>(
        12 + 8 + json_bytes.size() + 8 +
        bin.size());
    detail::appendU32(glb, 0x46546c67);
    detail::appendU32(glb, 2);
    detail::appendU32(glb, total_length);
    detail::appendU32(
        glb,
        static_cast<uint32_t>(json_bytes.size()));
    detail::appendU32(glb, 0x4e4f534a);
    glb.insert(
        glb.end(), json_bytes.begin(),
        json_bytes.end());
    detail::appendU32(
        glb, static_cast<uint32_t>(bin.size()));
    detail::appendU32(glb, 0x004e4942);
    glb.insert(glb.end(), bin.begin(), bin.end());
    return glb;
}

inline void writeGlb(const std::filesystem::path &path, bool duplicate_root_names = false,
                     bool geometry_only = false) {
    detail::writeBytes(path, makeGlb(duplicate_root_names, geometry_only));
}

inline void writeNormalizedPositionGlb(
    const std::filesystem::path &path) {
    detail::writeBytes(path, makeNormalizedPositionGlb());
}

inline void writeMaterialRoutingGlb(
    const std::filesystem::path &path) {
    detail::writeBytes(path, makeMaterialRoutingGlb());
}

} // namespace Pelican::TestGltfFragmentFixture
