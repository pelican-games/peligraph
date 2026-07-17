#include "vrm_xr_demo_fixture.hpp"

#include "../src/core/xractivation.hpp"
#include "../src/core/userpublic/animation/animgraph.hpp"
#include "../src/core/container.hpp"
#include "../src/core/launchconfig.hpp"
#include "../src/core/log.hpp"
#include "../src/core/material/standardmaterialresource.hpp"
#include "../src/core/model/gltf.hpp"
#include "../src/core/model/vertbufcontainer.hpp"
#include "../src/core/vkcore/core.hpp"

#include <catch2/catch_test_macros.hpp>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace {

std::vector<std::uint8_t> readBytes(const std::filesystem::path &path) {
    std::ifstream input{path, std::ios::binary};
    REQUIRE(input.good());
    return {std::istreambuf_iterator<char>{input},
            std::istreambuf_iterator<char>{}};
}

std::string readText(const std::filesystem::path &path) {
    std::ifstream input{path, std::ios::binary};
    REQUIRE(input.good());
    return {std::istreambuf_iterator<char>{input},
            std::istreambuf_iterator<char>{}};
}

std::uint32_t readU32(const std::vector<std::uint8_t> &bytes,
                      std::size_t offset) {
    REQUIRE(offset + 4 <= bytes.size());
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 8u) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16u) |
           (static_cast<std::uint32_t>(bytes[offset + 3]) << 24u);
}

nlohmann::json glbJson(const std::vector<std::uint8_t> &bytes) {
    REQUIRE(readU32(bytes, 0) == 0x46546c67u);
    REQUIRE(readU32(bytes, 4) == 2u);
    REQUIRE(readU32(bytes, 8) == bytes.size());
    const auto json_size = readU32(bytes, 12);
    REQUIRE(readU32(bytes, 16) == 0x4e4f534au);
    REQUIRE(20u + json_size <= bytes.size());
    return nlohmann::json::parse(
        std::string{reinterpret_cast<const char *>(bytes.data() + 20),
                    json_size});
}

struct DiscoveryFixture {
    int calls = 0;
};

Pelican::XrDiscoveryResult availableRuntime(void *context) {
    ++static_cast<DiscoveryFixture *>(context)->calls;
    return {Pelican::XrDiscoveryAvailability::available};
}

} // namespace

TEST_CASE("WP135 committed VRM is the deterministic full demo fixture",
          "[wp135][vrm][fixture]") {
    const auto root = std::filesystem::path{PELICAN_TEST_SOURCE_DIR};
    const auto committed = readBytes(
        root / "projects/vrm_xr_demo/assets/vrm_xr_character.vrm");
    REQUIRE(committed == Pelican::TestVrmXrDemoFixture::makeGlb());

    const auto document = glbJson(committed);
    REQUIRE(document.at("asset").at("version") == "2.0");
    const auto &vrm = document.at("extensions").at("VRMC_vrm");
    REQUIRE(vrm.at("specVersion") == "1.0");
    const auto &bones = vrm.at("humanoid").at("humanBones");
    constexpr std::array required_bones{
        "hips", "spine", "head", "leftUpperLeg", "leftLowerLeg",
        "leftFoot", "rightUpperLeg", "rightLowerLeg", "rightFoot",
        "leftUpperArm", "leftLowerArm", "leftHand", "rightUpperArm",
        "rightLowerArm", "rightHand",
    };
    for (const auto *name : required_bones) {
        CAPTURE(name);
        REQUIRE(bones.contains(name));
        REQUIRE(bones.at(name).at("node").get<std::size_t>() <
                document.at("nodes").size());
    }

    const auto &preset = vrm.at("expressions").at("preset");
    for (const auto *name : {"happy", "angry", "sad", "relaxed",
                             "lookLeft", "lookRight", "lookUp", "lookDown"}) {
        CAPTURE(name);
        REQUIRE(preset.contains(name));
    }
    REQUIRE(vrm.at("lookAt").at("type") == "expression");
    REQUIRE(vrm.at("firstPerson").at("meshAnnotations").at(0).at("type") ==
            "auto");

    std::vector<std::string> clips;
    for (const auto &animation : document.at("animations"))
        clips.push_back(animation.at("name").get<std::string>());
    REQUIRE(clips == std::vector<std::string>{"Idle", "Walk", "Run", "Jump"});
}

TEST_CASE("WP135 committed VRM decodes renderable geometry",
          "[wp135][vrm][gltf]") {
    Pelican::setupLogger();
    using Pelican::EngineLaunchConfig;
    using Pelican::FastModuleContainer;
    using Pelican::GltfLoader;
    using Pelican::StandardMaterialResource;
    using Pelican::VulkanManageCore;
    Pelican::FastModuleContainer modules;
    auto &launch = GET_MODULE(EngineLaunchConfig);
    launch.headless = true;
    launch.headless_extent = vk::Extent2D{16, 16};
    (void)GET_MODULE(StandardMaterialResource);
    const auto path = std::filesystem::path{PELICAN_TEST_SOURCE_DIR} /
                      "projects/vrm_xr_demo/assets/vrm_xr_character.vrm";
    const auto model = GET_MODULE(GltfLoader).loadGltfBinary(path.string());
    std::size_t primitive_count = 0;
    std::size_t index_count = 0;
    bool body_visible_in_both_views = false;
    bool head_third_person_only = false;
    for (const auto &material : model.material_primitives) {
        primitive_count += material.primitives.size();
        for (const auto &primitive : material.primitives) {
            index_count += primitive.index_count;
            REQUIRE(primitive.skinned);
            body_visible_in_both_views |=
                primitive.view_visibility == Pelican::PrimitiveViewVisibility::both;
            head_third_person_only |= primitive.view_visibility ==
                                      Pelican::PrimitiveViewVisibility::third_person_only;
        }
    }
    REQUIRE(primitive_count == 2);
    REQUIRE(index_count == 12);
    REQUIRE(body_visible_in_both_views);
    REQUIRE(head_third_person_only);
    REQUIRE(model.skeletal);
    REQUIRE(model.skeletal->joint_nodes.size() == 2);
    REQUIRE(model.vrm_semantic);
    GET_MODULE(VulkanManageCore).waitIdle();
}

TEST_CASE("WP135 project graph input and XR activation contracts are safe",
          "[wp135][project][openxr]") {
    const auto root = std::filesystem::path{PELICAN_TEST_SOURCE_DIR} /
                      "projects/vrm_xr_demo";
    const auto graph_text = readText(root / "movement.anim_graph.json");
    const auto graph = Pelican::AnimationGraph::parseDocumentV1(graph_text);
    REQUIRE(graph.initial_state == "Locomotion");
    REQUIRE(graph.states.size() == 2);
    REQUIRE(graph.transitions.size() == 2);

    const auto project = nlohmann::json::parse(readText(root / "project.json"));
    REQUIRE(project.at("basic_config").at("input_profile") == "keyboard");
    REQUIRE(project.at("basic_config").at("input_profiles").contains("touch"));
    const auto touch = nlohmann::json::parse(readText(root / "input/touch.json"));
    const auto touch_dump = touch.dump();
    for (const auto *binding : {
             "/user/hand/left/input/thumbstick",
             "/user/hand/right/input/thumbstick",
             "/user/hand/right/input/a/click",
             "/user/hand/right/input/b/click",
             "/user/hand/left/input/aim/pose",
             "/user/hand/right/input/aim/pose",
             "/user/hand/left/input/grip/pose",
             "/user/hand/right/input/grip/pose"}) {
        CAPTURE(binding);
        REQUIRE(touch_dump.find(binding) != std::string::npos);
    }
    REQUIRE(touch_dump.find("\"action\":\"head\"") == std::string::npos);

    const auto rendering =
        nlohmann::json::parse(readText(root / "passes/main.json"));
    REQUIRE(rendering.at("features").empty());
    const auto rendering_dump = rendering.dump();
    for (const auto *forbidden : {"taa", "velocity", "history", "pelican_ui",
                                  "projection_jitter"}) {
        CAPTURE(forbidden);
        REQUIRE(rendering_dump.find(forbidden) == std::string::npos);
    }

    Pelican::EngineLaunchConfig config;
    config.xr_mode = Pelican::XrMode::on;
    DiscoveryFixture discovery;
    const auto activation = Pelican::resolveXrActivation(
        config, true,
        Pelican::XrDiscoveryHook{.context = &discovery,
                                 .query = availableRuntime});
    REQUIRE(activation.active);
    REQUIRE(activation.resolved_mode == Pelican::XrMode::on);
    REQUIRE(discovery.calls == 1);
}
