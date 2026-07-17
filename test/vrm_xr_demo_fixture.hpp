#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <vector>

namespace Pelican::TestVrmXrDemoFixture {

namespace detail {

inline void appendU32(std::vector<std::uint8_t> &bytes, std::uint32_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value & 0xffu));
    bytes.push_back(static_cast<std::uint8_t>((value >> 8u) & 0xffu));
    bytes.push_back(static_cast<std::uint8_t>((value >> 16u) & 0xffu));
    bytes.push_back(static_cast<std::uint8_t>((value >> 24u) & 0xffu));
}

inline void align4(std::vector<std::uint8_t> &bytes) {
    while (bytes.size() % 4u != 0u) bytes.push_back(0);
}

template <class T>
inline void appendRaw(std::vector<std::uint8_t> &bytes, const T &value) {
    const auto *begin = reinterpret_cast<const std::uint8_t *>(&value);
    bytes.insert(bytes.end(), begin, begin + sizeof(T));
}

using Vec2 = std::array<float, 2>;
using Vec3 = std::array<float, 3>;
using Vec4 = std::array<float, 4>;
using U16Vec4 = std::array<std::uint16_t, 4>;
using Mat4 = std::array<float, 16>;

inline Mat4 identityMatrix() {
    return {1.0f, 0.0f, 0.0f, 0.0f,
            0.0f, 1.0f, 0.0f, 0.0f,
            0.0f, 0.0f, 1.0f, 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f};
}

} // namespace detail

inline std::vector<std::uint8_t> makeGlb() {
    using namespace detail;

    std::vector<std::uint8_t> binary;
    nlohmann::json buffer_views = nlohmann::json::array();
    nlohmann::json accessors = nlohmann::json::array();
    const auto addAccessor = [&](const auto &values, int component_type,
                                 std::string type) {
        align4(binary);
        const auto byte_offset = binary.size();
        for (const auto &value : values) appendRaw(binary, value);
        const auto view_index = buffer_views.size();
        buffer_views.push_back({{"buffer", 0},
                                {"byteOffset", byte_offset},
                                {"byteLength", binary.size() - byte_offset}});
        accessors.push_back({{"bufferView", view_index},
                             {"componentType", component_type},
                             {"count", values.size()},
                             {"type", std::move(type)}});
        return static_cast<std::uint32_t>(accessors.size() - 1u);
    };

    const std::vector<Vec3> positions{
        {-0.46f, 0.0f, 0.0f}, {0.46f, 0.0f, 0.0f},
        {-0.42f, 1.28f, 0.0f}, {0.42f, 1.28f, 0.0f},
        {-0.36f, 1.30f, 0.0f}, {0.36f, 1.30f, 0.0f},
        {-0.36f, 2.08f, 0.0f}, {0.36f, 2.08f, 0.0f},
    };
    const std::vector<Vec3> normals(positions.size(), Vec3{0.0f, 0.0f, -1.0f});
    const std::vector<Vec4> tangents(positions.size(),
                                     Vec4{1.0f, 0.0f, 0.0f, -1.0f});
    const std::vector<Vec2> uvs{
        {0.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 0.6f}, {1.0f, 0.6f},
        {0.0f, 0.6f}, {1.0f, 0.6f}, {0.0f, 1.0f}, {1.0f, 1.0f},
    };
    const std::vector<std::uint16_t> indices{
        0, 2, 1, 2, 3, 1,
        4, 6, 5, 6, 7, 5,
    };

    // Values are indices into skin.joints, not glTF node indices.
    const std::vector<U16Vec4> joints{
        {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},
        {1, 0, 0, 0}, {1, 0, 0, 0}, {1, 0, 0, 0}, {1, 0, 0, 0},
    };
    const std::vector<Vec4> weights(positions.size(), Vec4{1.0f, 0.0f, 0.0f, 0.0f});
    const std::vector<Mat4> inverse_bind_matrices(2, identityMatrix());

    const auto position_accessor = addAccessor(positions, 5126, "VEC3");
    accessors[position_accessor]["min"] = {-0.46, 0.0, 0.0};
    accessors[position_accessor]["max"] = {0.46, 2.08, 0.0};
    const auto normal_accessor = addAccessor(normals, 5126, "VEC3");
    const auto tangent_accessor = addAccessor(tangents, 5126, "VEC4");
    const auto uv_accessor = addAccessor(uvs, 5126, "VEC2");
    const auto joint_accessor = addAccessor(joints, 5123, "VEC4");
    const auto weight_accessor = addAccessor(weights, 5126, "VEC4");
    const auto index_accessor = addAccessor(indices, 5123, "SCALAR");
    const auto inverse_bind_accessor =
        addAccessor(inverse_bind_matrices, 5126, "MAT4");

    const auto makeMorph = [](float head_x, float head_y, float top_x,
                              float top_y) {
        std::vector<Vec3> result(8, Vec3{0.0f, 0.0f, 0.0f});
        result[4] = {head_x, head_y, 0.0f};
        result[5] = {head_x, head_y, 0.0f};
        result[6] = {head_x - top_x, head_y + top_y, 0.0f};
        result[7] = {head_x + top_x, head_y + top_y, 0.0f};
        return result;
    };
    const std::array morphs{
        makeMorph(0.0f, 0.02f, 0.08f, 0.12f),   // happy
        makeMorph(0.0f, -0.03f, -0.06f, -0.08f),// angry
        makeMorph(0.0f, -0.08f, 0.02f, -0.04f), // sad
        makeMorph(0.0f, 0.0f, 0.04f, 0.02f),    // relaxed
        makeMorph(-0.14f, 0.0f, 0.0f, 0.0f),    // lookLeft
        makeMorph(0.14f, 0.0f, 0.0f, 0.0f),     // lookRight
        makeMorph(0.0f, 0.14f, 0.0f, 0.0f),     // lookUp
        makeMorph(0.0f, -0.14f, 0.0f, 0.0f),    // lookDown
    };
    nlohmann::json targets = nlohmann::json::array();
    for (const auto &morph : morphs) {
        targets.push_back({{"POSITION", addAccessor(morph, 5126, "VEC3")}});
    }

    const std::vector<float> animation_times{0.0f, 0.5f, 1.0f};
    const auto time_accessor = addAccessor(animation_times, 5126, "SCALAR");
    accessors[time_accessor]["min"] = {0.0};
    accessors[time_accessor]["max"] = {1.0};
    const auto idle_accessor = addAccessor(
        std::vector<Vec3>{{0.0f, 0.0f, 0.0f}, {0.0f, 0.025f, 0.0f},
                          {0.0f, 0.0f, 0.0f}},
        5126, "VEC3");
    const auto walk_accessor = addAccessor(
        std::vector<Vec3>{{0.0f, 0.0f, 0.0f}, {0.11f, 0.07f, 0.0f},
                          {0.0f, 0.0f, 0.0f}},
        5126, "VEC3");
    const auto run_accessor = addAccessor(
        std::vector<Vec3>{{0.0f, 0.0f, 0.0f}, {-0.22f, 0.13f, 0.0f},
                          {0.0f, 0.0f, 0.0f}},
        5126, "VEC3");
    const auto jump_accessor = addAccessor(
        std::vector<Vec3>{{0.0f, 0.0f, 0.0f}, {0.0f, 0.82f, 0.0f},
                          {0.0f, 0.0f, 0.0f}},
        5126, "VEC3");

    nlohmann::json nodes = nlohmann::json::array({
        {{"name", "VrmXrMesh"}, {"mesh", 0}, {"skin", 0}, {"children", {1}}},
        {{"name", "Hips"}, {"children", {2, 8, 11}}},
        {{"name", "Spine"}, {"children", {3}}},
        {{"name", "Chest"}, {"children", {4, 14, 18}}},
        {{"name", "Neck"}, {"children", {5}}},
        {{"name", "Head"}, {"children", {6, 7}}},
        {{"name", "LeftEye"}},
        {{"name", "RightEye"}},
        {{"name", "LeftUpperLeg"}, {"children", {9}}},
        {{"name", "LeftLowerLeg"}, {"children", {10}}},
        {{"name", "LeftFoot"}},
        {{"name", "RightUpperLeg"}, {"children", {12}}},
        {{"name", "RightLowerLeg"}, {"children", {13}}},
        {{"name", "RightFoot"}},
        {{"name", "LeftShoulder"}, {"children", {15}}},
        {{"name", "LeftUpperArm"}, {"children", {16}}},
        {{"name", "LeftLowerArm"}, {"children", {17}}},
        {{"name", "LeftHand"}},
        {{"name", "RightShoulder"}, {"children", {19}}},
        {{"name", "RightUpperArm"}, {"children", {20}}},
        {{"name", "RightLowerArm"}, {"children", {21}}},
        {{"name", "RightHand"}},
    });

    const auto clip = [&](std::string name, std::uint32_t output) {
        return nlohmann::json{
            {"name", std::move(name)},
            {"samplers", nlohmann::json::array({
                {{"input", time_accessor}, {"output", output},
                 {"interpolation", "LINEAR"}},
            })},
            {"channels", nlohmann::json::array({
                {{"sampler", 0}, {"target", {{"node", 1}, {"path", "translation"}}}},
            })},
        };
    };

    const auto expression = [](std::uint32_t target, nlohmann::json color) {
        return nlohmann::json{
            {"morphTargetBinds", nlohmann::json::array({
                {{"node", 0}, {"index", target}, {"weight", 1.0}},
            })},
            {"materialColorBinds", nlohmann::json::array({
                {{"material", 0}, {"type", "color"},
                 {"targetValue", std::move(color)}},
            })},
        };
    };
    const auto look_expression = [](std::uint32_t target) {
        return nlohmann::json{
            {"morphTargetBinds", nlohmann::json::array({
                {{"node", 0}, {"index", target}, {"weight", 1.0}},
            })},
        };
    };

    nlohmann::json human_bones{
        {"hips", {{"node", 1}}},
        {"spine", {{"node", 2}}},
        {"chest", {{"node", 3}}},
        {"neck", {{"node", 4}}},
        {"head", {{"node", 5}}},
        {"leftEye", {{"node", 6}}},
        {"rightEye", {{"node", 7}}},
        {"leftUpperLeg", {{"node", 8}}},
        {"leftLowerLeg", {{"node", 9}}},
        {"leftFoot", {{"node", 10}}},
        {"rightUpperLeg", {{"node", 11}}},
        {"rightLowerLeg", {{"node", 12}}},
        {"rightFoot", {{"node", 13}}},
        {"leftShoulder", {{"node", 14}}},
        {"leftUpperArm", {{"node", 15}}},
        {"leftLowerArm", {{"node", 16}}},
        {"leftHand", {{"node", 17}}},
        {"rightShoulder", {{"node", 18}}},
        {"rightUpperArm", {{"node", 19}}},
        {"rightLowerArm", {{"node", 20}}},
        {"rightHand", {{"node", 21}}},
    };

    nlohmann::json vrm{
        {"specVersion", "1.0"},
        {"meta", nlohmann::json::object()},
        {"humanoid", {{"humanBones", std::move(human_bones)}}},
        {"expressions",
         {{"preset",
           {{"happy", expression(0, {1.0, 0.22, 0.32, 1.0})},
            {"angry", expression(1, {0.72, 0.05, 0.02, 1.0})},
            {"sad", expression(2, {0.08, 0.18, 0.8, 1.0})},
            {"relaxed", expression(3, {0.18, 0.75, 0.38, 1.0})},
            {"lookLeft", look_expression(4)},
            {"lookRight", look_expression(5)},
            {"lookUp", look_expression(6)},
            {"lookDown", look_expression(7)}}},
          {"custom", nlohmann::json::object()}}},
        {"lookAt",
         {{"offsetFromHeadBone", {0.0, 0.08, 0.0}},
          {"type", "expression"},
          {"rangeMapHorizontalInner", {{"inputMaxValue", 35.0}, {"outputScale", 1.0}}},
          {"rangeMapHorizontalOuter", {{"inputMaxValue", 35.0}, {"outputScale", 1.0}}},
          {"rangeMapVerticalDown", {{"inputMaxValue", 25.0}, {"outputScale", 1.0}}},
          {"rangeMapVerticalUp", {{"inputMaxValue", 25.0}, {"outputScale", 1.0}}}}},
        {"firstPerson",
         {{"meshAnnotations", nlohmann::json::array({
              {{"node", 0}, {"type", "auto"}},
          })}}},
    };

    nlohmann::json document{
        {"asset", {{"version", "2.0"},
                   {"generator", "Pelican WP135 deterministic VRM XR demo fixture"}}},
        {"scene", 0},
        {"scenes", nlohmann::json::array({{{"nodes", {0}}}})},
        {"nodes", std::move(nodes)},
        {"skins", nlohmann::json::array({
            {{"name", "VrmXrHumanoid"},
             {"skeleton", 1},
             {"joints", {1, 5}},
             {"inverseBindMatrices", inverse_bind_accessor}},
        })},
        {"meshes", nlohmann::json::array({
            {{"name", "VrmXrCharacter"},
             {"weights", std::vector<float>(8, 0.0f)},
             {"primitives", nlohmann::json::array({
                 {{"attributes",
                   {{"POSITION", position_accessor},
                    {"NORMAL", normal_accessor},
                    {"TANGENT", tangent_accessor},
                    {"TEXCOORD_0", uv_accessor},
                    {"JOINTS_0", joint_accessor},
                    {"WEIGHTS_0", weight_accessor}}},
                  {"indices", index_accessor},
                  {"material", 0},
                  {"targets", std::move(targets)}}})}},
        })},
        {"materials", nlohmann::json::array({
            {{"name", "VrmXrMaterial"},
             {"pbrMetallicRoughness",
              {{"baseColorFactor", {0.12, 0.55, 0.92, 1.0}},
               {"metallicFactor", 0.0}, {"roughnessFactor", 0.85}}},
             {"emissiveFactor", {0.02, 0.03, 0.04}},
             {"doubleSided", true}},
        })},
        {"animations", nlohmann::json::array({
            clip("Idle", idle_accessor), clip("Walk", walk_accessor),
            clip("Run", run_accessor), clip("Jump", jump_accessor),
        })},
        {"bufferViews", std::move(buffer_views)},
        {"accessors", std::move(accessors)},
        {"buffers", nlohmann::json::array({{{"byteLength", binary.size()}}})},
        {"extensionsUsed", {"VRMC_vrm"}},
        {"extensions", {{"VRMC_vrm", std::move(vrm)}}},
    };

    auto json_bytes = document.dump();
    while (json_bytes.size() % 4u != 0u) json_bytes.push_back(' ');
    align4(binary);
    std::vector<std::uint8_t> glb;
    appendU32(glb, 0x46546c67u);
    appendU32(glb, 2u);
    appendU32(glb, static_cast<std::uint32_t>(12u + 8u + json_bytes.size() +
                                              8u + binary.size()));
    appendU32(glb, static_cast<std::uint32_t>(json_bytes.size()));
    appendU32(glb, 0x4e4f534au);
    glb.insert(glb.end(), json_bytes.begin(), json_bytes.end());
    appendU32(glb, static_cast<std::uint32_t>(binary.size()));
    appendU32(glb, 0x004e4942u);
    glb.insert(glb.end(), binary.begin(), binary.end());
    return glb;
}

inline void writeGlb(const std::filesystem::path &path) {
    const auto bytes = makeGlb();
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    if (!output) throw std::runtime_error("could not open VRM XR demo fixture output");
    output.write(reinterpret_cast<const char *>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
}

} // namespace Pelican::TestVrmXrDemoFixture
