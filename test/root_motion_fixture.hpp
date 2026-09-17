#pragma once

// AG-2. A skinned asset whose clip actually travels: the root node moves two
// metres along +x over one second and the last key is the loop point's next
// step, so one whole loop advances by exactly that span.
//
// This is a separate builder from test/skeletal_fixture.hpp on purpose. That
// one is shared by four test files, two of which assert the exact clip count
// and the exact node names it produces; adding a travelling clip to it would
// change what those files measure. Nothing here is shared, so it can say
// exactly what root motion needs and nothing else.
//
// Node layout, which is also the pose layout's topological order:
//   0 Character (mesh + skin, the root the clips translate)
//   1 RootJoint
//   2 TipJoint
//
// Clips:
//   Advance -- node 0 translation (0,0,0) -> (2,0,0) over [0,1]. Loop-closed by
//             the engine convention: the last key is where the clip would be one
//             step past the loop point, not a copy of the first key, so
//             root(end) - root(start) is the displacement of one whole loop. A
//             clip authored the other way (last key == first key) has a net
//             displacement of zero and carries no root motion at all.
//   Turn    -- node 1 rotation only, so a document can ask for root motion on a
//             clip that never translates the node it called the root.

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace Pelican::TestRootMotionFixture {

struct Options {
    // Authors Advance the other way round: out to x=2 and back, with the last
    // key equal to the first. The clip is then seamless across the loop point
    // and its net displacement is zero, so the individual travels out and back
    // inside each loop and never leaves the origin. That is a property of the
    // clip, not a failure of the extraction, and the test says so.
    bool seamless_loop = false;
    // One keyframe, so the clip has no duration at all: start == end. There is
    // no "one loop" to divide by, which is the shape root motion has to name
    // rather than divide by zero.
    bool single_key = false;
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

inline std::vector<std::uint8_t> makeGlb(Options options = {}) {
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

    // Two keys one second apart: the whole clip is one linear metre-per-half-
    // second slide, which makes every acceptance number in the ledger an exact
    // decimal instead of an interpolation artefact.
    std::vector<float> times{0.0f, 1.0f};
    std::vector<glm::vec3> advance{{0.0f, 0.0f, 0.0f}, {2.0f, 0.0f, 0.0f}};
    if (options.seamless_loop) {
        times = {0.0f, 0.5f, 1.0f};
        advance = {{0.0f, 0.0f, 0.0f}, {2.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}};
    }
    if (options.single_key) {
        times = {0.0f};
        advance = {{0.0f, 0.0f, 0.0f}};
    }
    const float s = 0.7071067811865475f;
    std::vector<glm::vec4> rotations{{0, 0, 0, 1}, {0, 0, s, s}};
    if (times.size() != rotations.size()) rotations.resize(times.size(), glm::vec4{0, 0, 0, 1});

    const int position_accessor = add(positions, 5126, "VEC3");
    const int normal_accessor = add(normals, 5126, "VEC3");
    const int uv_accessor = add(uvs, 5126, "VEC2");
    const int joint_accessor = add(joints, 5123, "VEC4");
    const int weight_accessor = add(weights, 5126, "VEC4");
    const int index_accessor = add(indices, 5123, "SCALAR");
    const int bind_accessor = add(inverse_bind, 5126, "MAT4");
    const int time_accessor = add(times, 5126, "SCALAR");
    const int advance_accessor = add(advance, 5126, "VEC3");
    const int rotation_accessor = add(rotations, 5126, "VEC4");

    nlohmann::json animations = nlohmann::json::array({
        {{"name", "Advance"},
         {"samplers", {{{"input", time_accessor}, {"output", advance_accessor},
                        {"interpolation", "LINEAR"}}}},
         {"channels", {{{"sampler", 0}, {"target", {{"node", 0}, {"path", "translation"}}}}}}},
        {{"name", "Turn"},
         {"samplers", {{{"input", time_accessor}, {"output", rotation_accessor},
                        {"interpolation", "LINEAR"}}}},
         {"channels", {{{"sampler", 0}, {"target", {{"node", 1}, {"path", "rotation"}}}}}}},
    });
    nlohmann::json json{
        {"asset", {{"version", "2.0"}, {"generator", "pelican AG-2 root motion fixture"}}},
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
            {"baseColorFactor", {0.2, 0.6, 0.9, 1.0}}, {"metallicFactor", 0.0}, {"roughnessFactor", 1.0}}}}}},
        {"animations", animations}, {"bufferViews", views}, {"accessors", accessors},
        {"buffers", {{{"byteLength", bin.size()}}}},
    };
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

inline void writeGltf(const std::filesystem::path &path, const std::string &buffer_file,
                      Options options = {}) {
    const auto glb = makeGlb(options);
    std::uint32_t json_size{};
    std::memcpy(&json_size, glb.data() + 12, sizeof(json_size));
    auto document = nlohmann::json::parse(glb.begin() + 20, glb.begin() + 20 + json_size);
    document["buffers"][0]["uri"] = buffer_file;
    detail::write(path.parent_path() / buffer_file,
                  {glb.begin() + 28 + json_size, glb.end()});
    std::ofstream file{path, std::ios::binary};
    file << document.dump();
}

} // namespace Pelican::TestRootMotionFixture
