#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <glm/glm.hpp>
#include <limits>
#include <nlohmann/json.hpp>
#include <optional>
#include <vector>

namespace Pelican::TestMorphFixture {

struct Options {
    bool skinned = false;
    double mesh_weight = 0.0;
    std::optional<double> node_weight;
    std::uint32_t target_count = 1;
    bool sparse_position = false;
    bool wrong_count = false;
    bool unknown_semantic = false;
    bool non_finite = false;
};

namespace detail {
inline void u32(std::vector<std::uint8_t> &out, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8)
        out.push_back(static_cast<std::uint8_t>(value >> shift));
}
template <class T>
inline void raw(std::vector<std::uint8_t> &out, const T &value) {
    const auto *bytes = reinterpret_cast<const std::uint8_t *>(&value);
    out.insert(out.end(), bytes, bytes + sizeof(T));
}
inline void align4(std::vector<std::uint8_t> &out) {
    while (out.size() % 4) out.push_back(0);
}
} // namespace detail

inline std::vector<std::uint8_t> makeGlb(const Options &options = {}) {
    std::vector<std::uint8_t> bin;
    nlohmann::json views = nlohmann::json::array();
    nlohmann::json accessors = nlohmann::json::array();
    const auto add = [&](const auto &values, int component_type, const char *type) {
        detail::align4(bin);
        const auto offset = bin.size();
        for (const auto &value : values) detail::raw(bin, value);
        const auto view = views.size();
        views.push_back({{"buffer", 0}, {"byteOffset", offset},
                         {"byteLength", bin.size() - offset}});
        accessors.push_back({{"bufferView", view}, {"componentType", component_type},
                             {"count", values.size()}, {"type", type}});
        return static_cast<int>(accessors.size() - 1);
    };

    const std::vector<glm::vec3> positions{
        {-0.6f, -0.6f, 0.0f}, {0.6f, -0.6f, 0.0f},
        {-0.6f, 0.6f, 0.0f}, {0.6f, 0.6f, 0.0f}};
    const std::vector<glm::vec3> normals(4, {0.0f, 0.0f, 1.0f});
    const std::vector<glm::vec4> tangents(4, {1.0f, 0.0f, 0.0f, 1.0f});
    const std::vector<glm::vec2> uvs{{0, 0}, {1, 0}, {0, 1}, {1, 1}};
    const std::vector<std::uint16_t> indices{0, 1, 2, 2, 1, 3};
    auto position_delta = std::vector<glm::vec3>{
        {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f},
        {0.55f, 0.35f, 0.0f}, {0.55f, 0.35f, 0.0f}};
    if (options.wrong_count) position_delta.pop_back();
    if (options.non_finite)
        position_delta[0].x = std::numeric_limits<float>::quiet_NaN();
    const std::vector<glm::vec3> normal_delta(
        options.wrong_count ? 3 : 4, glm::vec3{0.15f, 0.0f, -0.02f});
    const std::vector<glm::vec3> tangent_delta(
        options.wrong_count ? 3 : 4, glm::vec3{0.0f, 0.1f, 0.0f});

    const int position_accessor = add(positions, 5126, "VEC3");
    const int normal_accessor = add(normals, 5126, "VEC3");
    const int tangent_accessor = add(tangents, 5126, "VEC4");
    const int uv_accessor = add(uvs, 5126, "VEC2");
    const int index_accessor = add(indices, 5123, "SCALAR");
    const int delta_position_accessor = add(position_delta, 5126, "VEC3");
    const int delta_normal_accessor = add(normal_delta, 5126, "VEC3");
    const int delta_tangent_accessor = add(tangent_delta, 5126, "VEC3");
    if (options.sparse_position) {
        accessors[delta_position_accessor]["sparse"] = {
            {"count", 1},
            {"indices", {{"bufferView", accessors[delta_position_accessor]["bufferView"]},
                          {"componentType", 5123}}},
            {"values", {{"bufferView", accessors[delta_position_accessor]["bufferView"]}}},
        };
    }

    nlohmann::json attributes{{"POSITION", position_accessor},
                              {"NORMAL", normal_accessor},
                              {"TANGENT", tangent_accessor},
                              {"TEXCOORD_0", uv_accessor}};
    nlohmann::json nodes = nlohmann::json::array();
    nlohmann::json target{{"POSITION", delta_position_accessor},
                          {"NORMAL", delta_normal_accessor},
                          {"TANGENT", delta_tangent_accessor}};
    if (options.unknown_semantic) target["COLOR_0"] = delta_position_accessor;
    nlohmann::json targets = nlohmann::json::array();
    for (std::uint32_t index = 0; index < options.target_count; ++index)
        targets.push_back(target);
    nlohmann::json primitive{{"attributes", attributes}, {"indices", index_accessor},
                             {"material", 0}, {"targets", std::move(targets)}};

    nlohmann::json skins = nlohmann::json::array();
    if (options.skinned) {
        const std::vector<glm::u16vec4> joints(4, {0, 0, 0, 0});
        const std::vector<glm::vec4> weights(4, {1.0f, 0.0f, 0.0f, 0.0f});
        const std::vector<glm::mat4> inverse_bind{glm::mat4{1.0f}};
        attributes["JOINTS_0"] = add(joints, 5123, "VEC4");
        attributes["WEIGHTS_0"] = add(weights, 5126, "VEC4");
        primitive["attributes"] = attributes;
        const int inverse_bind_accessor = add(inverse_bind, 5126, "MAT4");
        nodes.push_back({{"name", "MorphMesh"}, {"mesh", 0}, {"skin", 0},
                         {"children", {1}}});
        nodes.push_back({{"name", "Joint"}});
        skins.push_back({{"name", "OneJoint"}, {"joints", {1}},
                         {"inverseBindMatrices", inverse_bind_accessor}});
    } else {
        nodes.push_back({{"name", "MorphMesh"}, {"mesh", 0}});
    }
    if (options.node_weight)
        nodes[0]["weights"] = std::vector<double>(options.target_count,
                                                   *options.node_weight);

    const std::vector<double> mesh_weights(options.target_count,
                                           options.mesh_weight);

    const nlohmann::json json{
        {"asset", {{"version", "2.0"}, {"generator", "pelican WP121 fixture"}}},
        {"scene", 0}, {"scenes", {{{"nodes", {0}}}}}, {"nodes", nodes},
        {"skins", skins},
        {"meshes", {{{"name", "MorphQuad"},
                     {"weights", mesh_weights},
                     {"primitives", nlohmann::json::array({primitive})}}}},
        {"materials", {{{"name", "MorphMaterial"},
                        {"pbrMetallicRoughness", {
                            {"baseColorFactor", {0.1, 0.65, 0.95, 1.0}},
                            {"metallicFactor", 0.0}, {"roughnessFactor", 1.0}}}}}},
        {"bufferViews", views}, {"accessors", accessors},
        {"buffers", {{{"byteLength", bin.size()}}}},
    };

    auto json_bytes = json.dump();
    while (json_bytes.size() % 4) json_bytes.push_back(' ');
    detail::align4(bin);
    std::vector<std::uint8_t> glb;
    detail::u32(glb, 0x46546c67); detail::u32(glb, 2);
    detail::u32(glb, static_cast<std::uint32_t>(12 + 8 + json_bytes.size() +
                                                8 + bin.size()));
    detail::u32(glb, static_cast<std::uint32_t>(json_bytes.size()));
    detail::u32(glb, 0x4e4f534a);
    glb.insert(glb.end(), json_bytes.begin(), json_bytes.end());
    detail::u32(glb, static_cast<std::uint32_t>(bin.size()));
    detail::u32(glb, 0x004e4942);
    glb.insert(glb.end(), bin.begin(), bin.end());
    return glb;
}

inline void writeGlb(const std::filesystem::path &path, const Options &options = {}) {
    const auto bytes = makeGlb(options);
    std::ofstream file{path, std::ios::binary | std::ios::trunc};
    file.write(reinterpret_cast<const char *>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
}

} // namespace Pelican::TestMorphFixture
