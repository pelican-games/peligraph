#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace Pelican::TestGltfSceneFixture {

inline void appendU32(std::vector<uint8_t> &bytes, uint32_t value) {
    bytes.push_back(static_cast<uint8_t>(value & 0xff));
    bytes.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
    bytes.push_back(static_cast<uint8_t>((value >> 16) & 0xff));
    bytes.push_back(static_cast<uint8_t>((value >> 24) & 0xff));
}

inline std::vector<uint8_t> makeGlb(bool include_render_components) {
    constexpr double sin_cos_45 = 0.7071067811865475244;
    nlohmann::json nodes = nlohmann::json::array({
        nlohmann::json{
            {"name", "Root"},
            {"translation", {10.0, 0.0, 0.0}},
            {"rotation", {0.0, 0.0, sin_cos_45, sin_cos_45}},
            {"children", {1, 2}},
        },
        nlohmann::json{
            {"name", "MeshNode"},
            {"translation", {2.0, 0.0, 0.0}},
        },
        nlohmann::json{
            {"name", "CameraRig"},
            {"translation", {0.0, 3.0, 0.0}},
            {"children", {3}},
        },
        nlohmann::json{
            {"name", "SpotNode"},
            {"translation", {0.0, 0.0, 4.0}},
        },
    });
    nlohmann::json gltf{
        {"asset", {{"version", "2.0"}, {"generator", "pelican WP79 fixture"}}},
        {"scene", 0},
        {"scenes", {{{"name", "default_scene"}, {"nodes", {0}}}}},
        {"nodes", std::move(nodes)},
    };
    if (include_render_components) {
        gltf["extensionsUsed"] = {"KHR_lights_punctual"};
        gltf["extensions"]["KHR_lights_punctual"]["lights"] = nlohmann::json::array({
            nlohmann::json{
                {"name", "WarmSpot"},
                {"type", "spot"},
                {"color", {0.25, 0.5, 1.0}},
                {"intensity", 12.0},
                {"range", 25.0},
                {"spot", {{"innerConeAngle", 0.2}, {"outerConeAngle", 0.6}}},
            },
        });
        gltf["meshes"] = {{{"name", "Triangle"}, {"primitives", nlohmann::json::array()}}};
        gltf["cameras"] = {{{"name", "MainCamera"},
                              {"type", "perspective"},
                              {"perspective",
                               {{"yfov", 0.7}, {"znear", 0.2}, {"zfar", 200.0}, {"aspectRatio", 1.5}}}}};
        gltf["nodes"][1]["mesh"] = 0;
        gltf["nodes"][1]["extras"] = {{"gameplay_tag", "mesh_extra"}, {"lod_bias", 2}};
        gltf["nodes"][2]["camera"] = 0;
        gltf["nodes"][3]["extensions"] = {{"KHR_lights_punctual", {{"light", 0}}}};
    }

    auto json_bytes = gltf.dump();
    while (json_bytes.size() % 4 != 0) {
        json_bytes.push_back(' ');
    }
    std::vector<uint8_t> glb;
    appendU32(glb, 0x46546c67);
    appendU32(glb, 2);
    appendU32(glb, static_cast<uint32_t>(12 + 8 + json_bytes.size()));
    appendU32(glb, static_cast<uint32_t>(json_bytes.size()));
    appendU32(glb, 0x4e4f534a);
    glb.insert(glb.end(), json_bytes.begin(), json_bytes.end());
    return glb;
}

inline void writeGlb(const std::filesystem::path &path, bool include_render_components) {
    const auto bytes = makeGlb(include_render_components);
    std::ofstream file{path, std::ios::binary};
    file.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

} // namespace Pelican::TestGltfSceneFixture
