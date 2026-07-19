#pragma once

#include <array>
#include <bit>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace Pelican::TestVrmaFixture {

enum class Kind {
    full,
    body_only,
    expression_only,
    gaze_only,
    empty,
    unsupported_version,
    invalid_channel_type,
    missing_humanoid_map,
    unknown_channel,
};

inline Kind kindFromString(std::string_view value) {
    if (value == "full") return Kind::full;
    if (value == "body-only") return Kind::body_only;
    if (value == "expression-only") return Kind::expression_only;
    if (value == "gaze-only") return Kind::gaze_only;
    if (value == "empty") return Kind::empty;
    if (value == "unsupported-version") return Kind::unsupported_version;
    if (value == "invalid-channel") return Kind::invalid_channel_type;
    if (value == "missing-humanoid") return Kind::missing_humanoid_map;
    if (value == "unknown-channel") return Kind::unknown_channel;
    throw std::runtime_error("unknown VRMA fixture kind");
}

inline void appendU32(std::vector<std::uint8_t> &bytes, std::uint32_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value & 0xff));
    bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
    bytes.push_back(static_cast<std::uint8_t>((value >> 16) & 0xff));
    bytes.push_back(static_cast<std::uint8_t>((value >> 24) & 0xff));
}

struct DocumentBuilder {
    nlohmann::json document;
    std::vector<std::uint8_t> binary;

    int addFloatAccessor(std::span<const float> values, std::string_view type,
                         std::size_t count) {
        while (binary.size() % 4 != 0) binary.push_back(0);
        const auto byte_offset = binary.size();
        for (const auto value : values)
            appendU32(binary, std::bit_cast<std::uint32_t>(value));
        const auto view = document["bufferViews"].size();
        document["bufferViews"].push_back({
            {"buffer", 0},
            {"byteOffset", byte_offset},
            {"byteLength", values.size() * sizeof(float)},
        });
        const auto accessor = document["accessors"].size();
        document["accessors"].push_back({
            {"bufferView", view},
            {"componentType", 5126},
            {"count", count},
            {"type", type},
        });
        return static_cast<int>(accessor);
    }
};

inline DocumentBuilder makeDocument(Kind kind) {
    static constexpr std::array required_bones{
        "hips",          "spine",         "head",          "leftUpperLeg",
        "leftLowerLeg",  "leftFoot",      "rightUpperLeg", "rightLowerLeg",
        "rightFoot",     "leftUpperArm",  "leftLowerArm",  "leftHand",
        "rightUpperArm", "rightLowerArm", "rightHand",
    };

    DocumentBuilder builder;
    auto &gltf = builder.document;
    gltf = {
        {"asset", {{"version", "2.0"}, {"generator", "pelican WP176 fixture"}}},
        {"nodes", nlohmann::json::array()},
        {"bufferViews", nlohmann::json::array()},
        {"accessors", nlohmann::json::array()},
        {"animations", nlohmann::json::array()},
        {"extensionsUsed", {"VRMC_vrm_animation"}},
    };
    nlohmann::json human_bones = nlohmann::json::object();
    for (std::size_t i = 0; i < required_bones.size(); ++i) {
        gltf["nodes"].push_back({{"name", std::string{required_bones[i]} + "Node"}});
        human_bones[required_bones[i]] = {{"node", i}};
    }
    constexpr int expression_node = static_cast<int>(required_bones.size());
    constexpr int gaze_node = expression_node + 1;
    constexpr int unknown_node = gaze_node + 1;
    gltf["nodes"].push_back({{"name", "HappyWeight"}});
    gltf["nodes"].push_back({{"name", "LookAtDirection"}});
    gltf["nodes"].push_back({{"name", "Unmapped"}});

    const bool body = kind == Kind::full || kind == Kind::body_only ||
                      kind == Kind::unsupported_version ||
                      kind == Kind::missing_humanoid_map ||
                      kind == Kind::unknown_channel;
    const bool expression = kind == Kind::full || kind == Kind::expression_only ||
                            kind == Kind::invalid_channel_type ||
                            kind == Kind::unsupported_version ||
                            kind == Kind::unknown_channel;
    const bool gaze = kind == Kind::full || kind == Kind::gaze_only ||
                      kind == Kind::unsupported_version ||
                      kind == Kind::unknown_channel;
    if (kind == Kind::missing_humanoid_map) human_bones.erase("hips");

    nlohmann::json extension{
        {"specVersion", kind == Kind::unsupported_version ? "1.1" : "1.0"},
    };
    if (body) extension["humanoid"] = {{"humanBones", std::move(human_bones)}};
    if (expression) {
        extension["expressions"] = {
            {"preset", {{"happy", {{"node", expression_node}}}}},
        };
    }
    if (gaze) {
        extension["lookAt"] = {
            {"node", gaze_node},
            {"offsetFromHeadBone", {0.0, 0.06, 0.0}},
        };
    }
    gltf["extensions"]["VRMC_vrm_animation"] = std::move(extension);

    nlohmann::json animation{
        {"samplers", nlohmann::json::array()},
        {"channels", nlohmann::json::array()},
    };
    auto addChannel = [&](int node, std::string_view path, std::span<const float> output,
                          std::string_view output_type, std::size_t output_count) {
        static constexpr std::array times{0.0f, 1.0f};
        const auto input_accessor = builder.addFloatAccessor(times, "SCALAR", times.size());
        const auto output_accessor =
            builder.addFloatAccessor(output, output_type, output_count);
        const auto sampler = animation["samplers"].size();
        animation["samplers"].push_back({
            {"input", input_accessor},
            {"output", output_accessor},
            {"interpolation", "LINEAR"},
        });
        animation["channels"].push_back({
            {"sampler", sampler},
            {"target", {{"node", node}, {"path", path}}},
        });
    };

    static constexpr std::array hips_translation{
        0.0f, 0.0f, 0.0f,
        0.25f, 0.0f, 0.0f,
    };
    static constexpr std::array spine_rotation{
        0.0f, 0.0f, 0.0f, 1.0f,
        0.0f, 0.38268343f, 0.0f, 0.92387953f,
    };
    static constexpr std::array expression_weight{
        0.0f, 0.0f, 0.0f,
        1.25f, 0.0f, 0.0f,
    };
    static constexpr std::array gaze_rotation{
        0.0f, 0.0f, 0.0f, 1.0f,
        0.0f, 0.25881904f, 0.0f, 0.96592582f,
    };

    if (body) {
        addChannel(0, "translation", hips_translation, "VEC3", 2);
        addChannel(1, "rotation", spine_rotation, "VEC4", 2);
    }
    if (expression) {
        if (kind == Kind::invalid_channel_type)
            addChannel(expression_node, "rotation", gaze_rotation, "VEC4", 2);
        else
            addChannel(expression_node, "translation", expression_weight, "VEC3", 2);
    }
    if (gaze) addChannel(gaze_node, "rotation", gaze_rotation, "VEC4", 2);
    if (kind == Kind::unknown_channel)
        addChannel(unknown_node, "translation", hips_translation, "VEC3", 2);

    gltf["animations"].push_back(std::move(animation));
    // The decoder's normative default is the first animation. Keeping a second
    // entry in the combined fixture makes accidental "decode all" behavior visible.
    if (kind == Kind::full)
        gltf["animations"].push_back({
            {"name", "IgnoredSecondAnimation"},
            {"samplers", nlohmann::json::array()},
            {"channels", nlohmann::json::array()},
        });
    if (!builder.binary.empty())
        gltf["buffers"] = nlohmann::json::array({
            nlohmann::json{{"byteLength", builder.binary.size()}},
        });
    return builder;
}

inline std::vector<std::uint8_t> makeGlb(Kind kind) {
    auto builder = makeDocument(kind);
    auto json_bytes = builder.document.dump();
    while (json_bytes.size() % 4 != 0) json_bytes.push_back(' ');
    while (builder.binary.size() % 4 != 0) builder.binary.push_back(0);
    const auto binary_chunk_size = builder.binary.empty() ? 0u : 8u + builder.binary.size();
    std::vector<std::uint8_t> glb;
    appendU32(glb, 0x46546c67);
    appendU32(glb, 2);
    appendU32(glb, static_cast<std::uint32_t>(12 + 8 + json_bytes.size() +
                                              binary_chunk_size));
    appendU32(glb, static_cast<std::uint32_t>(json_bytes.size()));
    appendU32(glb, 0x4e4f534a);
    glb.insert(glb.end(), json_bytes.begin(), json_bytes.end());
    if (!builder.binary.empty()) {
        appendU32(glb, static_cast<std::uint32_t>(builder.binary.size()));
        appendU32(glb, 0x004e4942);
        glb.insert(glb.end(), builder.binary.begin(), builder.binary.end());
    }
    return glb;
}

inline std::vector<std::uint8_t> makeNonGlbAlias() {
    static constexpr std::string_view json = R"({"asset":{"version":"2.0"}})";
    return {json.begin(), json.end()};
}

inline void writeGlb(const std::filesystem::path &path, Kind kind) {
    const auto bytes = makeGlb(kind);
    std::ofstream file{path, std::ios::binary};
    if (!file) throw std::runtime_error("could not open VRMA fixture output");
    file.write(reinterpret_cast<const char *>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
}

} // namespace Pelican::TestVrmaFixture
