#pragma once

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <optional>
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
                                    bool geometry_only = false,
                                    bool project_name_collision = false) {
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
    if (project_name_collision && !geometry_only) {
        json["materials"][0]["name"] = "ProjectLeaf";
    }
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

// WP368's production-path fixture deliberately distinguishes authored absence
// from authored zeroes, and gives the two embedded materials different image
// bytes. It is loaded through ModelAssetContainer/GltfLoader; no geometry or
// material registration is synthesized by the test.
inline std::vector<uint8_t> makeWp368Glb() {
    std::vector<uint8_t> bin;
    const auto append = [&](std::initializer_list<float> values) {
        for (const auto value : values) detail::appendF32(bin, value);
    };
    append({-0.75f, -0.75f, 0.0f,
             0.75f, -0.75f, 0.0f,
             0.0f,   0.75f, 0.0f});
    const auto normal_offset = bin.size();
    append({0.0f, 0.0f, 0.0f,
            0.0f, 0.0f, 0.0f,
            0.0f, 0.0f, 0.0f});
    const auto uv_offset = bin.size();
    append({0.0f, 0.0f,
            0.0f, 0.0f,
            0.0f, 0.0f});
    const auto color_offset = bin.size();
    append({0.0f, 0.0f, 0.0f, 0.0f,
            0.0f, 0.0f, 0.0f, 0.0f,
            0.0f, 0.0f, 0.0f, 0.0f});

    const std::string red_png =
        "data:image/png;base64,"
        "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGP4z8DwHwAF"
        "AAH/iZk9HQAAAABJRU5ErkJggg==";
    const std::string green_png =
        "data:image/png;base64,"
        "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGNg+M/wHwAE"
        "AQH/cetH5QAAAABJRU5ErkJggg==";
    const nlohmann::json full_attributes{
        {"POSITION", 0}, {"NORMAL", 1}, {"TEXCOORD_0", 2}, {"COLOR_0", 3}};
    nlohmann::json json{
        {"asset", {{"version", "2.0"},
                   {"generator", "pelican WP368 production fixture"}}},
        {"scene", 0},
        {"scenes", {{{"nodes", {0, 1, 2, 3}}}}},
        {"nodes",
         {
             {{"name", "Lane0Missing"}, {"mesh", 0},
              {"translation", {-3.0, 0.0, 8.0}}},
             {{"name", "Lane1ZeroPresent"}, {"mesh", 1},
              {"translation", {-1.0, 0.0, 8.0}}},
             {{"name", "Lane2ProjectOak"}, {"mesh", 2},
              {"translation", {1.0, 0.0, 8.0}}},
             {{"name", "Lane3ProjectPine"}, {"mesh", 3},
              {"translation", {3.0, 0.0, 8.0}}},
         }},
        {"materials",
         {
             {{"name", "EmbeddedRed"},
              {"pbrMetallicRoughness",
               {{"baseColorFactor", {1.0, 1.0, 1.0, 1.0}},
                {"baseColorTexture", {{"index", 0}}}}}},
             {{"name", "EmbeddedGreen"},
              {"pbrMetallicRoughness",
               {{"baseColorFactor", {1.0, 1.0, 1.0, 1.0}},
                {"baseColorTexture", {{"index", 1}}}}}},
             {{"name", "EmbeddedProjectOakSlot"}},
             {{"name", "EmbeddedProjectPineSlot"}},
         }},
        {"images", {{{"name", "Red1x1"}, {"uri", red_png}},
                    {{"name", "Green1x1"}, {"uri", green_png}}}},
        {"textures", {{{"source", 0}}, {{"source", 1}}}},
        {"meshes",
         {
             {{"name", "MissingAttributes"},
              {"primitives", {{{"attributes", {{"POSITION", 0}}},
                                {"material", 0}}}}},
             {{"name", "AllZeroAttributes"},
              {"primitives", {{{"attributes", full_attributes},
                                {"material", 1}}}}},
             {{"name", "ProjectOakMesh"},
              {"primitives", {{{"attributes", full_attributes},
                                {"material", 2}}}}},
             {{"name", "ProjectPineMesh"},
              {"primitives", {{{"attributes", full_attributes},
                                {"material", 3}}}}},
         }},
        {"buffers", {{{"byteLength", bin.size()}}}},
        {"bufferViews",
         {
             {{"buffer", 0}, {"byteOffset", 0}, {"byteLength", normal_offset}},
             {{"buffer", 0}, {"byteOffset", normal_offset},
              {"byteLength", uv_offset - normal_offset}},
             {{"buffer", 0}, {"byteOffset", uv_offset},
              {"byteLength", color_offset - uv_offset}},
             {{"buffer", 0}, {"byteOffset", color_offset},
              {"byteLength", bin.size() - color_offset}},
         }},
        {"accessors",
         {
             {{"bufferView", 0}, {"componentType", 5126}, {"count", 3},
              {"type", "VEC3"}, {"min", {-0.75, -0.75, 0.0}},
              {"max", {0.75, 0.75, 0.0}}},
             {{"bufferView", 1}, {"componentType", 5126}, {"count", 3},
              {"type", "VEC3"}},
             {{"bufferView", 2}, {"componentType", 5126}, {"count", 3},
              {"type", "VEC2"}},
             {{"bufferView", 3}, {"componentType", 5126}, {"count", 3},
              {"type", "VEC4"}},
         }},
    };

    auto json_bytes = json.dump();
    while (json_bytes.size() % 4 != 0) json_bytes.push_back(' ');
    while (bin.size() % 4 != 0) bin.push_back(0);
    std::vector<uint8_t> glb;
    const auto total_length = static_cast<uint32_t>(
        12 + 8 + json_bytes.size() + 8 + bin.size());
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

// WP369's checker fixture puts every barycentrically interpolated UV at a
// texel center. The two 2x2 mip-0 samples are therefore a fixed normalized
// margin 0.25 from every texel boundary. The same authored scene also carries
// real COLOR_0 low/high lanes, both clamp counterexamples, and non-MASK lanes.
inline std::vector<uint8_t> makeWp369Glb(
    const std::optional<nlohmann::json> &texture_transform = std::nullopt) {
    std::vector<uint8_t> bin;
    const auto append = [&](std::initializer_list<float> values) {
        for (const auto value : values) detail::appendF32(bin, value);
    };
    append({-0.30f, -0.65f, 0.0f,
             0.0f,   0.65f, 0.0f,
             0.30f, -0.65f, 0.0f});
    const auto low_uv_offset = bin.size();
    append({0.25f, 0.25f,
            0.25f, 0.25f,
            0.25f, 0.25f});
    const auto high_uv_offset = bin.size();
    append({0.75f, 0.25f,
            0.75f, 0.25f,
            0.75f, 0.25f});
    const auto low_color_offset = bin.size();
    append({1.0f, 1.0f, 1.0f, 0.25f,
            1.0f, 1.0f, 1.0f, 0.25f,
            1.0f, 1.0f, 1.0f, 0.25f});
    const auto high_color_offset = bin.size();
    append({1.0f, 1.0f, 1.0f, 0.75f,
            1.0f, 1.0f, 1.0f, 0.75f,
            1.0f, 1.0f, 1.0f, 0.75f});

    const std::string checker_png =
        "data:image/png;base64,"
        "iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAYAAABytg0kAAAAFElEQVR42mP4//+/"
        "AxAfYAARIA4AgKAN9Ubdi9sAAAAASUVORK5CYII=";
    nlohmann::json json{
        {"asset", {{"version", "2.0"},
                   {"generator", "pelican WP369 margin checker"}}},
        {"scene", 0},
        {"scenes", {{{"nodes", {0, 1, 2, 3, 4, 5, 6, 7}}}}},
        {"nodes",
         {
              {{"name", "CheckerLow"}, {"mesh", 0},
               {"translation", {-2.625, 0.0, 8.0}}},
              {{"name", "CheckerHigh"}, {"mesh", 1},
               {"translation", {-1.875, 0.0, 8.0}}},
              {{"name", "NegativeClamp"}, {"mesh", 2},
               {"translation", {-1.125, 0.0, 8.0}}},
              {{"name", "ColorLow"}, {"mesh", 3},
               {"translation", {-0.375, 0.0, 8.0}}},
              {{"name", "ColorHigh"}, {"mesh", 4},
               {"translation", {0.375, 0.0, 8.0}}},
              {{"name", "CutoffClamp"}, {"mesh", 5},
               {"translation", {1.125, 0.0, 8.0}}},
              {{"name", "ProjectOpaque"}, {"mesh", 6},
               {"translation", {1.875, 0.0, 8.0}}},
              {{"name", "Blend"}, {"mesh", 7},
               {"translation", {2.625, 0.0, 8.0}}},
          }},
        {"materials",
         {
              {{"name", "CheckerLowMask"},
               {"alphaMode", "MASK"},
               {"alphaCutoff", 0.5},
               {"pbrMetallicRoughness",
                {{"baseColorFactor", {1.0, 0.1, 0.1, 1.0}},
                 {"baseColorTexture", {{"index", 0}}}}}},
              {{"name", "CheckerHighMask"},
               {"alphaMode", "MASK"},
               {"alphaCutoff", 0.5},
               {"pbrMetallicRoughness",
                {{"baseColorFactor", {0.1, 1.0, 0.1, 1.0}},
                 {"baseColorTexture", {{"index", 0}}}}}},
              {{"name", "NegativeClampMask"},
               {"alphaMode", "MASK"},
               {"alphaCutoff", 0.0},
               {"pbrMetallicRoughness",
                {{"baseColorFactor", {0.1, 0.1, 1.0, 1.0}}}}},
              {{"name", "ColorLowMask"},
               {"alphaMode", "MASK"},
               {"alphaCutoff", 0.5},
               {"pbrMetallicRoughness",
                {{"baseColorFactor", {1.0, 1.0, 0.1, 1.0}}}}},
              {{"name", "ColorHighMask"},
               {"alphaMode", "MASK"},
               {"alphaCutoff", 0.5},
               {"pbrMetallicRoughness",
                {{"baseColorFactor", {0.1, 1.0, 1.0, 1.0}}}}},
              {{"name", "CutoffClampMask"},
               {"alphaMode", "MASK"},
               {"alphaCutoff", 2.0},
               {"pbrMetallicRoughness",
                {{"baseColorFactor", {1.0, 0.1, 1.0, 1.0}}}}},
              {{"name", "EmbeddedOpaquePlaceholder"},
               {"pbrMetallicRoughness",
                {{"baseColorFactor", {0.8, 0.4, 0.1, 1.0}}}}},
              {{"name", "BlendMaterial"},
               {"alphaMode", "BLEND"},
               {"pbrMetallicRoughness",
                {{"baseColorFactor", {0.5, 0.2, 0.8, 0.1}}}}},
          }},
        {"images", {{{"name", "CheckerMip0"}, {"uri", checker_png}}}},
        {"textures", {{{"source", 0}}}},
        {"meshes",
         {
              {{"name", "CheckerLowMesh"},
               {"primitives", {{{"attributes", {{"POSITION", 0},
                                                  {"TEXCOORD_0", 1}}},
                                 {"material", 0}}}}},
              {{"name", "CheckerHighMesh"},
               {"primitives", {{{"attributes", {{"POSITION", 0},
                                                  {"TEXCOORD_0", 2}}},
                                 {"material", 1}}}}},
              {{"name", "NegativeClampMesh"},
               {"primitives", {{{"attributes", {{"POSITION", 0}}},
                                 {"material", 2}}}}},
              {{"name", "ColorLowMesh"},
               {"primitives", {{{"attributes", {{"POSITION", 0},
                                                  {"COLOR_0", 3}}},
                                 {"material", 3}}}}},
              {{"name", "ColorHighMesh"},
               {"primitives", {{{"attributes", {{"POSITION", 0},
                                                  {"COLOR_0", 4}}},
                                 {"material", 4}}}}},
              {{"name", "CutoffClampMesh"},
               {"primitives", {{{"attributes", {{"POSITION", 0}}},
                                 {"material", 5}}}}},
              {{"name", "ProjectOpaqueMesh"},
               {"primitives", {{{"attributes", {{"POSITION", 0}}},
                                 {"material", 6}}}}},
              {{"name", "BlendMesh"},
               {"primitives", {{{"attributes", {{"POSITION", 0}}},
                                 {"material", 7}}}}},
          }},
        {"buffers", {{{"byteLength", bin.size()}}}},
        {"bufferViews",
         {
             {{"buffer", 0}, {"byteOffset", 0},
              {"byteLength", low_uv_offset}},
             {{"buffer", 0}, {"byteOffset", low_uv_offset},
              {"byteLength", high_uv_offset - low_uv_offset}},
              {{"buffer", 0}, {"byteOffset", high_uv_offset},
               {"byteLength", low_color_offset - high_uv_offset}},
              {{"buffer", 0}, {"byteOffset", low_color_offset},
               {"byteLength", high_color_offset - low_color_offset}},
              {{"buffer", 0}, {"byteOffset", high_color_offset},
               {"byteLength", bin.size() - high_color_offset}},
         }},
        {"accessors",
         {
             {{"bufferView", 0}, {"componentType", 5126}, {"count", 3},
              {"type", "VEC3"}, {"min", {-0.75, -0.75, 0.0}},
              {"max", {0.75, 0.75, 0.0}}},
             {{"bufferView", 1}, {"componentType", 5126}, {"count", 3},
              {"type", "VEC2"}},
              {{"bufferView", 2}, {"componentType", 5126}, {"count", 3},
               {"type", "VEC2"}},
              {{"bufferView", 3}, {"componentType", 5126}, {"count", 3},
               {"type", "VEC4"}},
              {{"bufferView", 4}, {"componentType", 5126}, {"count", 3},
               {"type", "VEC4"}},
          }},
    };

    if (texture_transform) {
        json["extensionsUsed"] = {"KHR_texture_transform"};
        json["materials"][0]["pbrMetallicRoughness"]
            ["baseColorTexture"]["extensions"]
            ["KHR_texture_transform"] = *texture_transform;
    }

    auto json_bytes = json.dump();
    while (json_bytes.size() % 4 != 0) json_bytes.push_back(' ');
    while (bin.size() % 4 != 0) bin.push_back(0);
    std::vector<uint8_t> glb;
    const auto total_length = static_cast<uint32_t>(
        12 + 8 + json_bytes.size() + 8 + bin.size());
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
                     bool geometry_only = false,
                     bool project_name_collision = false) {
    detail::writeBytes(
        path, makeGlb(duplicate_root_names, geometry_only,
                      project_name_collision));
}

inline void writeNormalizedPositionGlb(
    const std::filesystem::path &path) {
    detail::writeBytes(path, makeNormalizedPositionGlb());
}

inline void writeWp368Glb(const std::filesystem::path &path) {
    detail::writeBytes(path, makeWp368Glb());
}

inline void writeWp369Glb(
    const std::filesystem::path &path,
    const std::optional<nlohmann::json> &texture_transform = std::nullopt) {
    detail::writeBytes(path, makeWp369Glb(texture_transform));
}

inline void writeMaterialRoutingGlb(
    const std::filesystem::path &path) {
    detail::writeBytes(path, makeMaterialRoutingGlb());
}

} // namespace Pelican::TestGltfFragmentFixture
