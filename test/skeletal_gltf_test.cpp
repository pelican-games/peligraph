#include "../src/core/appflow/enginetime.hpp"
#include "../src/core/asset/model.hpp"
#include "../src/core/container.hpp"
#include "../src/core/ecs/core.hpp"
#include "../src/core/ecs/predefined.hpp"
#include "../src/core/launchconfig.hpp"
#include "../src/core/log.hpp"
#include "../src/core/loader/pathresolver.hpp"
#include "../src/core/loader/projectsrc.hpp"
#include "../src/core/loader/scene.hpp"
#include "../src/core/material/standardmaterialresource.hpp"
#include "../src/core/model/gltf.hpp"
#include "../src/core/renderer/polygoninstancecontainer.hpp"
#include "../src/core/vkcore/core.hpp"
#include "skeletal_fixture.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

namespace Pelican {
namespace {
void writeText(const std::filesystem::path &path, std::string_view value) {
    std::ofstream file{path, std::ios::binary}; file << value;
}
struct TempDir { std::filesystem::path path; ~TempDir() { std::filesystem::remove_all(path); } };
AssetFragmentRef animationFragment(std::string name) {
    return {.kind = "animation", .path = std::move(name),
            .address_kind = AssetFragmentAddressKind::name};
}
} // namespace

TEST_CASE("glTF skin and clips connect to animation component without ECS bones", "[gltf][skeletal]") {
    setupLogger();
    const auto root = std::filesystem::temp_directory_path() /
        ("pelican_wp38_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    const TempDir guard{root};
    TestSkeletalFixture::writeGlb(root / "character.glb");
    TestSkeletalFixture::writeGlb(root / "cubic.glb", true);
    TestSkeletalFixture::writeGlb(
        root / "no_scene.glb", TestSkeletalFixture::Options{.omit_scenes = true});
    TestSkeletalFixture::writeGlb(
        root / "bad_accessor.glb",
        TestSkeletalFixture::Options{.invalid_animation_accessor = true});
    TestSkeletalFixture::writeGlb(
        root / "bad_buffer_view.glb",
        TestSkeletalFixture::Options{.invalid_inverse_bind_buffer_view = true});
    TestSkeletalFixture::writeGlb(
        root / "truncated_accessor.glb",
        TestSkeletalFixture::Options{.truncated_inverse_bind_view = true});
    TestSkeletalFixture::writeGlb(
        root / "bad_default_scene.glb",
        TestSkeletalFixture::Options{.invalid_default_scene = true});
    writeText(root / "assets.json",
              R"json({"schema":"pelican.asset_data","version":1,"models":[{"name":"character","path":"character.glb"}]})json");
    writeText(root / "scene.json", R"json({
      "schema":"pelican.scene","version":1,"scenes":{"default_scene":{"objects":[{
        "name":"Character","components":[
          {"name":"transform","pos":[0,0,0],"rotation":[0,0,0,1],"scale":[1,1,1]},
          {"name":"simplemodelview","model":"character"},
          {"name":"animation","clip":"character.glb#animation/Turn","speed":1.0,"loop":true,"start_time":0.0}
        ]}]}}})json");

    FastModuleContainer modules;
    GET_MODULE(PathResolver).setup(root, false);
    GET_MODULE(ProjectSource).setSourceByData(nlohmann::json{{"basic_config", {
        {"scene_data_json", "scene.json"}, {"asset_data_json", "assets.json"}}}}.dump());
    auto &launch = GET_MODULE(EngineLaunchConfig);
    launch.headless = true; launch.headless_extent = vk::Extent2D{16, 16};
    try { (void)GET_MODULE(StandardMaterialResource); }
    catch (const std::exception &ex) { SKIP(std::string{"Vulkan unavailable: "} + ex.what()); }

    auto &loader = GET_MODULE(GltfLoader);
    const auto loaded = loader.loadGltfBinary((root / "character.glb").string());
    REQUIRE(loaded.skeletal);
    REQUIRE(loaded.skeletal->joint_nodes.size() == 2);
    REQUIRE(loaded.skeletal->nodes[1].name == "RootJoint");
    REQUIRE(loaded.skeletal->nodes[2].name == "TipJoint");
    REQUIRE(loaded.skeletal->skin_bindings.size() == 1);
    REQUIRE(loaded.skeletal->clips.size() == 2);
    REQUIRE(loaded.material_primitives.size() == 1);
    REQUIRE(loaded.material_primitives.front().primitives.front().skinned);
    const auto clip_only = loader.loadGltfBinary((root / "character.glb").string(),
                                                 animationFragment("Turn"));
    REQUIRE(clip_only.material_primitives.empty());
    REQUIRE(clip_only.skeletal);
    REQUIRE_THROWS_WITH(loader.loadGltfBinary((root / "cubic.glb").string()),
                        Catch::Matchers::ContainsSubstring("animation 'Turn'") &&
                        Catch::Matchers::ContainsSubstring("CUBICSPLINE"));
    REQUIRE(loader.loadGltfBinary((root / "no_scene.glb").string())
                .material_primitives.size() == 1);
    REQUIRE_THROWS_WITH(
        loader.loadGltfBinary((root / "bad_accessor.glb").string()),
        Catch::Matchers::ContainsSubstring("accessor 17") &&
            Catch::Matchers::ContainsSubstring("out of range"));
    REQUIRE_THROWS_WITH(
        loader.loadGltfBinary((root / "bad_buffer_view.glb").string()),
        Catch::Matchers::ContainsSubstring("no valid bufferView"));
    REQUIRE_THROWS_WITH(
        loader.loadGltfBinary((root / "truncated_accessor.glb").string()),
        Catch::Matchers::ContainsSubstring("data exceeds its bufferView"));
    REQUIRE_THROWS_WITH(
        loader.loadGltfBinary((root / "bad_default_scene.glb").string()),
        Catch::Matchers::ContainsSubstring("default scene index is out of range"));

    auto &time = GET_MODULE(EngineTime);
    time.setup(EngineTime::Mode::fixed_step, 1.0 / 60.0);
    time.setTime(0.5);
    GET_MODULE(ECSPredefinedRegistration).reg();
    GET_MODULE(SceneLoader).load("default_scene");
    GET_MODULE(ECSCore).update();
    auto &instances = GET_MODULE(PolygonInstanceContainer);
    const auto instance = instances.modelInstanceIdForTesting(0);
    const auto first_revision = instances.currentAnimationRevisionForTesting(instance);
    REQUIRE(first_revision != 0);
    REQUIRE(instances.previousAnimationRevisionForTesting(instance) == first_revision);
    instances.advanceTemporalHistoryAfterRender();
    time.setTime(0.75);
    GET_MODULE(ECSCore).update();
    REQUIRE(instances.currentAnimationRevisionForTesting(instance) > first_revision);
    REQUIRE(instances.previousAnimationRevisionForTesting(instance) == first_revision);
    instances.triggerUpdate();
    REQUIRE(instances.getDrawCalls().size() == 1);
    REQUIRE(instances.getDrawCalls().front().skinned);
    GET_MODULE(VulkanManageCore).waitIdle();
}

TEST_CASE("animation graph key is reserved in component v1", "[skeletal][scene]") {
    setupLogger();
    const auto root = std::filesystem::temp_directory_path() /
        ("pelican_wp38_graph_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    const TempDir guard{root};
    writeText(root / "assets.json",
              R"json({"schema":"pelican.asset_data","version":1,"models":[]})json");
    writeText(root / "scene.json", R"json({"schema":"pelican.scene","version":1,
      "scenes":{"default_scene":{"objects":[{"name":"Bad","components":[
        {"name":"animation","clip":"x.glb#animation/Walk","graph":{}}
      ]}]}}})json");
    FastModuleContainer modules;
    GET_MODULE(PathResolver).setup(root, false);
    GET_MODULE(ProjectSource).setSourceByData(nlohmann::json{{"basic_config", {
        {"scene_data_json", "scene.json"}, {"asset_data_json", "assets.json"}}}}.dump());
    GET_MODULE(ECSPredefinedRegistration).reg();
    REQUIRE_THROWS_WITH(GET_MODULE(SceneLoader).load("default_scene"),
                        Catch::Matchers::ContainsSubstring("reserved v1 key 'graph'"));
}

} // namespace Pelican
