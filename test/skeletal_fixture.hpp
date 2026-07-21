#pragma once

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace Pelican::TestSkeletalFixture {
struct Options {
    bool cubic = false;
    bool invalid_animation_accessor = false;
    bool invalid_inverse_bind_buffer_view = false;
    bool truncated_inverse_bind_view = false;
    bool omit_scenes = false;
    bool invalid_default_scene = false;
};

namespace detail {
inline void u32(std::vector<std::uint8_t> &out, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) out.push_back(static_cast<std::uint8_t>(value >> shift));
}
template <class T> inline void raw(std::vector<std::uint8_t> &out, const T &value) {
    const auto *bytes = reinterpret_cast<const std::uint8_t *>(&value);
    out.insert(out.end(), bytes, bytes + sizeof(T));
}
inline void align4(std::vector<std::uint8_t> &out) { while (out.size() % 4) out.push_back(0); }
inline void write(const std::filesystem::path &path, const std::vector<std::uint8_t> &bytes) {
    std::ofstream file{path, std::ios::binary};
    file.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}
} // namespace detail

inline std::vector<std::uint8_t> makeGlb(Options options) {
    std::vector<std::uint8_t> bin;
    nlohmann::json views = nlohmann::json::array();
    nlohmann::json accessors = nlohmann::json::array();
    const auto add = [&](const auto &values, int component_type, const char *type) {
        detail::align4(bin);
        const auto offset = bin.size();
        for (const auto &value : values) detail::raw(bin, value);
        const auto view = views.size();
        views.push_back({{"buffer", 0}, {"byteOffset", offset}, {"byteLength", bin.size() - offset}});
        accessors.push_back({{"bufferView", view}, {"componentType", component_type},
                             {"count", values.size()}, {"type", type}});
        return static_cast<int>(accessors.size() - 1);
    };

    const std::vector<glm::vec3> positions{{-0.3f, 0.0f, 0.0f}, {0.3f, 0.0f, 0.0f},
                                           {-0.3f, 2.0f, 0.0f}, {0.3f, 2.0f, 0.0f}};
    const std::vector<glm::vec3> normals(4, glm::vec3{0.0f, 0.0f, 1.0f});
    const std::vector<glm::vec2> uvs{{0, 0}, {1, 0}, {0, 1}, {1, 1}};
    const std::vector<glm::u16vec4> joints{{0, 0, 0, 0}, {0, 0, 0, 0},
                                           {1, 0, 0, 0}, {1, 0, 0, 0}};
    const std::vector<glm::vec4> weights(4, glm::vec4{1.0f, 0.0f, 0.0f, 0.0f});
    const std::vector<std::uint16_t> indices{0, 1, 2, 2, 1, 3};
    const std::vector<glm::mat4> inverse_bind{
        glm::mat4{1.0f}, glm::translate(glm::mat4{1.0f}, glm::vec3{0.0f, -1.0f, 0.0f})};
    const std::vector<float> times{0.0f, 1.0f, 2.0f};
    const float s = 0.7071067811865475f;
    const std::vector<glm::vec4> rotations{{0, 0, 0, 1}, {0, 0, s, s}, {0, 0, 1, 0}};
    const std::vector<glm::vec3> translations{{0, 1, 0}, {1, 1, 0}, {2, 1, 0}};

    const int position_accessor = add(positions, 5126, "VEC3");
    const int normal_accessor = add(normals, 5126, "VEC3");
    const int uv_accessor = add(uvs, 5126, "VEC2");
    const int joint_accessor = add(joints, 5123, "VEC4");
    const int weight_accessor = add(weights, 5126, "VEC4");
    const int index_accessor = add(indices, 5123, "SCALAR");
    const int bind_accessor = add(inverse_bind, 5126, "MAT4");
    const int time_accessor = add(times, 5126, "SCALAR");
    const int rotation_accessor = add(rotations, 5126, "VEC4");
    const int translation_accessor = add(translations, 5126, "VEC3");

    nlohmann::json animations = nlohmann::json::array({
        {{"name", "Turn"},
         {"samplers", {{{"input", time_accessor}, {"output", rotation_accessor},
                         {"interpolation", options.cubic ? "CUBICSPLINE" : "LINEAR"}}}},
         {"channels", {{{"sampler", 0}, {"target", {{"node", 1}, {"path", "rotation"}}}}}}},
        {{"name", "Step"},
         {"samplers", {{{"input", time_accessor}, {"output", translation_accessor},
                         {"interpolation", "STEP"}}}},
         {"channels", {{{"sampler", 0}, {"target", {{"node", 2}, {"path", "translation"}}}}}}},
    });
    nlohmann::json json{
        {"asset", {{"version", "2.0"}, {"generator", "pelican WP38 fixture"}}},
        {"scene", 0}, {"scenes", {{{"nodes", {0}}}}},
        {"nodes", {{{"name", "Character"}, {"mesh", 0}, {"skin", 0}, {"children", {1}}},
                   {{"name", "RootJoint"}, {"children", {2}}},
                   {{"name", "TipJoint"}, {"translation", {0.0, 1.0, 0.0}}}}},
        {"skins", {{{"name", "TwoBone"}, {"joints", {1, 2}}, {"inverseBindMatrices", bind_accessor}}}},
        {"meshes", {{{"name", "Body"}, {"primitives", {{{"attributes", {
            {"POSITION", position_accessor}, {"NORMAL", normal_accessor}, {"TEXCOORD_0", uv_accessor},
            {"JOINTS_0", joint_accessor}, {"WEIGHTS_0", weight_accessor}}},
            {"indices", index_accessor}, {"material", 0}}}}}}},
        {"materials", {{{"name", "Skin"}, {"pbrMetallicRoughness", {
            {"baseColorFactor", {1.0, 0.45, 0.12, 1.0}}, {"metallicFactor", 0.0}, {"roughnessFactor", 1.0}}}}}},
        {"animations", animations}, {"bufferViews", views}, {"accessors", accessors},
        {"buffers", {{{"byteLength", bin.size()}}}},
    };
    if (options.invalid_animation_accessor) {
        json["animations"][0]["samplers"][0]["input"] =
            static_cast<int>(accessors.size()) + 7;
    }
    if (options.invalid_inverse_bind_buffer_view) {
        json["accessors"][bind_accessor]["bufferView"] =
            static_cast<int>(views.size()) + 3;
    }
    if (options.truncated_inverse_bind_view) {
        const auto view =
            json["accessors"][bind_accessor]["bufferView"].get<int>();
        json["bufferViews"][view]["byteLength"] = 4;
    }
    if (options.omit_scenes) {
        json.erase("scene");
        json.erase("scenes");
    } else if (options.invalid_default_scene) {
        json["scene"] = 99;
    }
    auto json_bytes = json.dump();
    while (json_bytes.size() % 4) json_bytes.push_back(' ');
    detail::align4(bin);
    std::vector<std::uint8_t> glb;
    detail::u32(glb, 0x46546c67); detail::u32(glb, 2);
    detail::u32(glb, static_cast<std::uint32_t>(12 + 8 + json_bytes.size() + 8 + bin.size()));
    detail::u32(glb, static_cast<std::uint32_t>(json_bytes.size())); detail::u32(glb, 0x4e4f534a);
    glb.insert(glb.end(), json_bytes.begin(), json_bytes.end());
    detail::u32(glb, static_cast<std::uint32_t>(bin.size())); detail::u32(glb, 0x004e4942);
    glb.insert(glb.end(), bin.begin(), bin.end());
    return glb;
}

inline std::vector<std::uint8_t> makeGlb(bool cubic = false) {
    return makeGlb(Options{.cubic = cubic});
}

inline void writeGlb(const std::filesystem::path &path, bool cubic = false) {
    detail::write(path, makeGlb(cubic));
}
inline void writeGlb(const std::filesystem::path &path, Options options) {
    detail::write(path, makeGlb(options));
}
} // namespace Pelican::TestSkeletalFixture
