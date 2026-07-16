#pragma once

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <glm/glm.hpp>
#include <nlohmann/json.hpp>
#include <vector>

namespace Pelican::TestMaterialAbsoluteOverrideFixture {

namespace detail {

inline void appendU32(std::vector<std::uint8_t> &bytes, std::uint32_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value));
    bytes.push_back(static_cast<std::uint8_t>(value >> 8u));
    bytes.push_back(static_cast<std::uint8_t>(value >> 16u));
    bytes.push_back(static_cast<std::uint8_t>(value >> 24u));
}

template <class T>
inline void appendRaw(std::vector<std::uint8_t> &bytes, const T &value) {
    const auto *first = reinterpret_cast<const std::uint8_t *>(&value);
    bytes.insert(bytes.end(), first, first + sizeof(T));
}

inline void align4(std::vector<std::uint8_t> &bytes) {
    while ((bytes.size() & 3u) != 0u) bytes.push_back(0);
}

} // namespace detail

inline std::vector<std::uint8_t> makeGlb() {
    std::vector<std::uint8_t> bin;
    nlohmann::json views = nlohmann::json::array();
    nlohmann::json accessors = nlohmann::json::array();
    const auto add = [&](const auto &values, int component_type,
                         const char *type) {
        detail::align4(bin);
        const auto offset = bin.size();
        for (const auto &value : values) detail::appendRaw(bin, value);
        const auto view = views.size();
        views.push_back({{"buffer", 0},
                         {"byteOffset", offset},
                         {"byteLength", bin.size() - offset}});
        accessors.push_back({{"bufferView", view},
                             {"componentType", component_type},
                             {"count", values.size()},
                             {"type", type}});
        return static_cast<int>(accessors.size() - 1);
    };

    const std::vector<glm::vec3> positions{
        {-0.42f, -0.36f, 0.0f}, {0.42f, -0.36f, 0.0f},
        {-0.42f, 0.36f, 0.0f},  {0.42f, 0.36f, 0.0f},
    };
    const std::vector<glm::vec3> normals(4, {0.0f, 0.0f, 1.0f});
    const std::vector<glm::vec2> uvs{{0.0f, 0.0f}, {1.0f, 0.0f},
                                     {0.0f, 1.0f}, {1.0f, 1.0f}};
    const std::vector<std::uint16_t> indices{0, 1, 2, 2, 1, 3};
    const auto position = add(positions, 5126, "VEC3");
    const auto normal = add(normals, 5126, "VEC3");
    const auto uv = add(uvs, 5126, "VEC2");
    const auto index = add(indices, 5123, "SCALAR");
    const nlohmann::json attributes{{"POSITION", position},
                                    {"NORMAL", normal},
                                    {"TEXCOORD_0", uv}};
    const auto primitive = [&](int material) {
        return nlohmann::json{{"attributes", attributes},
                              {"indices", index},
                              {"material", material}};
    };

    const nlohmann::json json{
        {"asset", {{"version", "2.0"},
                   {"generator", "pelican WP122b fixture"}}},
        {"scene", 0},
        {"scenes", {{{"nodes", {0, 1}}}}},
        {"nodes", {{{"name", "ZeroBase"},
                    {"mesh", 0},
                    {"translation", {-0.48, 0.0, 0.0}}},
                   {{"name", "Unchanged"},
                    {"mesh", 1},
                    {"translation", {0.48, 0.0, 0.0}}}}},
        {"meshes", {{{"name", "ZeroBaseMesh"},
                     {"primitives", nlohmann::json::array({primitive(0)})}},
                    {{"name", "UnchangedMesh"},
                     {"primitives", nlohmann::json::array({primitive(1)})}}}},
        {"materials", {{{"name", "ZeroBaseMaterial"},
                        {"pbrMetallicRoughness",
                         {{"baseColorFactor", {0.0, 0.0, 0.0, 1.0}},
                          {"metallicFactor", 0.0},
                          {"roughnessFactor", 1.0}}},
                        {"emissiveFactor", {0.0, 0.0, 0.0}}},
                       {{"name", "UnchangedMaterial"},
                        {"pbrMetallicRoughness",
                         {{"baseColorFactor", {0.12, 0.85, 0.22, 1.0}},
                          {"metallicFactor", 0.0},
                          {"roughnessFactor", 1.0}}},
                        {"emissiveFactor", {0.0, 0.0, 0.0}}}}},
        {"bufferViews", views},
        {"accessors", accessors},
        {"buffers", {{{"byteLength", bin.size()}}}},
    };

    auto json_bytes = json.dump();
    while ((json_bytes.size() & 3u) != 0u) json_bytes.push_back(' ');
    detail::align4(bin);
    std::vector<std::uint8_t> glb;
    detail::appendU32(glb, 0x46546c67);
    detail::appendU32(glb, 2);
    detail::appendU32(
        glb, static_cast<std::uint32_t>(12 + 8 + json_bytes.size() + 8 +
                                        bin.size()));
    detail::appendU32(glb, static_cast<std::uint32_t>(json_bytes.size()));
    detail::appendU32(glb, 0x4e4f534a);
    glb.insert(glb.end(), json_bytes.begin(), json_bytes.end());
    detail::appendU32(glb, static_cast<std::uint32_t>(bin.size()));
    detail::appendU32(glb, 0x004e4942);
    glb.insert(glb.end(), bin.begin(), bin.end());
    return glb;
}

inline void writeGlb(const std::filesystem::path &path) {
    const auto bytes = makeGlb();
    std::ofstream file{path, std::ios::binary | std::ios::trunc};
    file.write(reinterpret_cast<const char *>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
}

} // namespace Pelican::TestMaterialAbsoluteOverrideFixture
