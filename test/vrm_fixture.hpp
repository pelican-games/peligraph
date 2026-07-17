#pragma once

#include "vrm_xr_demo_fixture.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace Pelican::TestVrmFixture {

enum class Kind {
    full,
    optional_bones_missing,
    unknown_bone,
    duplicate_bone,
    missing_required_bone,
    invalid_node,
    unsupported_version,
    unsupported_constraint_version,
    plain_glb,
    vrm0,
    xr_demo,
};

inline Kind kindFromString(std::string_view value) {
    if (value == "full") return Kind::full;
    if (value == "optional") return Kind::optional_bones_missing;
    if (value == "unknown") return Kind::unknown_bone;
    if (value == "duplicate") return Kind::duplicate_bone;
    if (value == "missing-required") return Kind::missing_required_bone;
    if (value == "invalid-node") return Kind::invalid_node;
    if (value == "unsupported-version") return Kind::unsupported_version;
    if (value == "unsupported-constraint") return Kind::unsupported_constraint_version;
    if (value == "plain") return Kind::plain_glb;
    if (value == "vrm0") return Kind::vrm0;
    if (value == "xr-demo") return Kind::xr_demo;
    throw std::runtime_error("unknown VRM fixture kind");
}

inline void appendU32(std::vector<std::uint8_t> &bytes, std::uint32_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value & 0xff));
    bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
    bytes.push_back(static_cast<std::uint8_t>((value >> 16) & 0xff));
    bytes.push_back(static_cast<std::uint8_t>((value >> 24) & 0xff));
}

inline nlohmann::json makeDocument(Kind kind) {
    static constexpr std::array required_bones{
        "hips",          "spine",         "head",          "leftUpperLeg",
        "leftLowerLeg",  "leftFoot",      "rightUpperLeg", "rightLowerLeg",
        "rightFoot",     "leftUpperArm",  "leftLowerArm",  "leftHand",
        "rightUpperArm", "rightLowerArm", "rightHand",
    };
    nlohmann::json nodes = nlohmann::json::array();
    nlohmann::json human_bones = nlohmann::json::object();
    for (std::size_t i = 0; i < required_bones.size(); ++i) {
        nodes.push_back({{"name", std::string{required_bones[i]} + "Node"}});
        human_bones[required_bones[i]] = {{"node", i}};
    }

    nlohmann::json gltf{
        {"asset", {{"version", "2.0"}, {"generator", "pelican WP111 fixture"}}},
        {"scene", 0},
        {"scenes", nlohmann::json::array({nlohmann::json{{"nodes", {0}}}})},
        {"nodes", std::move(nodes)},
    };
    if (kind == Kind::plain_glb) return gltf;
    if (kind == Kind::vrm0) {
        gltf["extensionsUsed"] = {"VRM"};
        gltf["extensions"]["VRM"] = {{"exporterVersion", "UniVRM-0.99.0"}};
        return gltf;
    }

    const bool include_optional = kind != Kind::optional_bones_missing &&
                                  kind != Kind::unknown_bone &&
                                  kind != Kind::missing_required_bone &&
                                  kind != Kind::invalid_node &&
                                  kind != Kind::unsupported_version;
    if (include_optional) {
        const auto chest_node = gltf["nodes"].size();
        gltf["nodes"].push_back({{"name", "ChestNode"}});
        human_bones["chest"] = {{"node", chest_node}};
    }
    if (kind == Kind::unknown_bone) {
        const auto tail_node = gltf["nodes"].size();
        gltf["nodes"].push_back({{"name", "TailNode"}});
        human_bones["tail"] = {{"node", tail_node}};
    }
    if (kind == Kind::duplicate_bone) human_bones["chest"]["node"] = 0;
    if (kind == Kind::missing_required_bone) human_bones.erase("rightHand");
    if (kind == Kind::invalid_node) human_bones["head"]["node"] = 999;

    nlohmann::json vrm{
        {"specVersion", kind == Kind::unsupported_version ? "1.1" : "1.0"},
        {"meta", nlohmann::json::object()},
        {"humanoid", {{"humanBones", std::move(human_bones)}}},
    };
    if (kind == Kind::full) {
        gltf["materials"] = nlohmann::json::array({nlohmann::json::object()});
        vrm["expressions"] = {
            {"preset",
             {{"happy",
               {{"isBinary", true},
                {"overrideBlink", "blend"},
                {"overrideLookAt", "block"},
                {"overrideMouth", "none"},
                {"morphTargetBinds", {{{"node", 2}, {"index", 1}, {"weight", 0.75}}}},
                {"materialColorBinds",
                 {{{"material", 0},
                   {"type", "color"},
                   {"targetValue", {0.1, 0.2, 0.3, 0.4}}}}},
                {"textureTransformBinds",
                 {{{"material", 0}, {"scale", {2.0, 3.0}}, {"offset", {0.25, 0.5}}}}}}}}},
            {"custom", {{"smileWide", {{"isBinary", false}}}}},
        };
        vrm["lookAt"] = {
            {"offsetFromHeadBone", {0.0, 0.06, 0.0}},
            {"type", "expression"},
            {"rangeMapHorizontalOuter", {{"inputMaxValue", 90.0}, {"outputScale", 1.0}}},
            {"rangeMapVerticalDown", {{"inputMaxValue", 45.0}, {"outputScale", 0.8}}},
        };
        vrm["firstPerson"] = {
            {"meshAnnotations", {{{"node", 2}, {"type", "thirdPersonOnly"}}}},
        };
    }

    gltf["extensionsUsed"] = {"VRMC_vrm", "VRMC_node_constraint"};
    gltf["extensions"]["VRMC_vrm"] = std::move(vrm);
    if (kind == Kind::full || kind == Kind::unsupported_constraint_version) {
        const auto version = kind == Kind::unsupported_constraint_version ? "2.0" : "1.0";
        gltf["nodes"][1]["extensions"]["VRMC_node_constraint"] = {
            {"specVersion", version},
            {"constraint", {{"rotation", {{"source", 0}, {"weight", 0.5}}}}},
        };
    }
    return gltf;
}

inline std::vector<std::uint8_t> makeGlb(Kind kind) {
    if (kind == Kind::xr_demo) return TestVrmXrDemoFixture::makeGlb();
    auto json_bytes = makeDocument(kind).dump();
    while (json_bytes.size() % 4 != 0) json_bytes.push_back(' ');
    std::vector<std::uint8_t> glb;
    appendU32(glb, 0x46546c67);
    appendU32(glb, 2);
    appendU32(glb, static_cast<std::uint32_t>(12 + 8 + json_bytes.size()));
    appendU32(glb, static_cast<std::uint32_t>(json_bytes.size()));
    appendU32(glb, 0x4e4f534a);
    glb.insert(glb.end(), json_bytes.begin(), json_bytes.end());
    return glb;
}

inline void writeGlb(const std::filesystem::path &path, Kind kind) {
    const auto bytes = makeGlb(kind);
    std::ofstream file{path, std::ios::binary};
    if (!file) throw std::runtime_error("could not open VRM fixture output");
    file.write(reinterpret_cast<const char *>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
}

} // namespace Pelican::TestVrmFixture
