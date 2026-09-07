#include "../src/core/appflow/enginetime.hpp"
#include "../src/core/animation/animationservice.hpp"
#include "../src/core/ecs/predefined/animationsystem.hpp"
#include "../src/core/userpublic/components/animation.hpp"
#include "../src/core/userpublic/components/modelview.hpp"
#include "../src/core/userpublic/gamecontext.hpp"
#include "../src/core/asset/model.hpp"
#include "../src/core/container.hpp"
#include "../src/core/ecs/core.hpp"
#include "../src/core/ecs/predefined.hpp"
#include "../src/core/launchconfig.hpp"
#include "../src/core/log.hpp"
#include "../src/core/loader/pathresolver.hpp"
#include "../src/core/loader/projectsrc.hpp"
#include "../src/core/loader/scene.hpp"
#include "scene_load_test_support.hpp"
#include "../src/core/material/standardmaterialresource.hpp"
#include "../src/core/model/gltf.hpp"
#include "../src/core/renderer/polygoninstancecontainer.hpp"
#include "../src/core/vkcore/core.hpp"
#include "skeletal_fixture.hpp"
#include "vulkan_test_support.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

namespace Pelican {
namespace {
void writeText(const std::filesystem::path &path, std::string_view value) {
    std::ofstream file{path, std::ios::binary}; file << value;
}
void writeRenderingConfig(const std::filesystem::path &root) {
    std::filesystem::create_directories(root / "passes");
    writeText(root / "passes/main_rendering_config.json",
              R"({"pipeline":{"preset":"engine://render_pipelines/hybrid_v1.json"}})");
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
    writeRenderingConfig(root);
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
    TestSupport::requireVulkanDevice("Vulkan unavailable");
    (void)GET_MODULE(StandardMaterialResource);

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
    test_support::loadScene(GET_MODULE(SceneLoader), "default_scene");
    time.advance();
    GET_MODULE(ECSCore).update();
    auto &instances = GET_MODULE(PolygonInstanceContainer);
    const auto instance = instances.modelInstanceIdForTesting(0);
    const auto first_revision = instances.currentAnimationRevisionForTesting(instance);
    REQUIRE(first_revision != 0);
    REQUIRE(instances.previousAnimationRevisionForTesting(instance) == first_revision);
    instances.advanceTemporalHistoryAfterRender();
    time.setTime(0.75);
    time.advance();
    GET_MODULE(ECSCore).update();
    REQUIRE(instances.currentAnimationRevisionForTesting(instance) > first_revision);
    REQUIRE(instances.previousAnimationRevisionForTesting(instance) == first_revision);
    instances.triggerUpdate();
    REQUIRE(instances.getDrawCalls().size() == 1);
    REQUIRE(instances.getDrawCalls().front().skinned);
    GET_MODULE(VulkanManageCore).waitIdle();
}

TEST_CASE("animation component rejects competing clip and graph sources", "[skeletal][scene]") {
    setupLogger();
    const auto root = std::filesystem::temp_directory_path() /
        ("pelican_wp38_graph_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    const TempDir guard{root};
    writeText(root / "assets.json",
              R"json({"schema":"pelican.asset_data","version":1,"models":[]})json");
    writeText(root / "scene.json", R"json({"schema":"pelican.scene","version":1,
      "scenes":{"default_scene":{"objects":[{"name":"Bad","components":[
        {"name":"animation","clip":"x.glb#animation/Walk","graph":"walk.anim.json"}
      ]}]}}})json");
    FastModuleContainer modules;
    GET_MODULE(PathResolver).setup(root, false);
    GET_MODULE(ProjectSource).setSourceByData(nlohmann::json{{"basic_config", {
        {"scene_data_json", "scene.json"}, {"asset_data_json", "assets.json"}}}}.dump());
    GET_MODULE(ECSPredefinedRegistration).reg();
    REQUIRE_THROWS_WITH(test_support::loadScene(GET_MODULE(SceneLoader), "default_scene"),
                        Catch::Matchers::ContainsSubstring("exactly one of clip or graph"));
}

TEST_CASE("authored animation graphs publish and release real scene model poses",
          "[gltf][skeletal][animation-graph-component]") {
    setupLogger();
    const auto root = std::filesystem::temp_directory_path() /
        ("pelican_graph_component_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    const TempDir guard{root};
    TestSkeletalFixture::writeGlb(root / "character.glb");
    writeText(root / "assets.json",
              R"({"schema":"pelican.asset_data","version":1,"models":[{"name":"character","path":"character.glb"}]})");
    writeRenderingConfig(root);
    writeText(root / "turn.json", R"({"schema":"pelican.anim_graph","version":1,
      "parameters":{"go":0},"initial_state":"turn",
      "states":[{"name":"turn","type":"clip","clip":"Turn","loop":false},
                {"name":"step","type":"clip","clip":"Step","loop":false}],
      "transitions":[{"from":"turn","to":"step","duration":0,
                      "conditions":[{"parameter":"go","op":">","value":0}]}]})");
    writeText(root / "scene.json", R"({"schema":"pelican.scene","version":1,
      "scenes":{"default_scene":{"objects":[
        {"name":"Character","components":[
          {"name":"transform","pos":[0,0,0],"rotation":[0,0,0,1],"scale":[1,1,1]},
          {"name":"simplemodelview","model":"character"},
          {"name":"animation","graph":"turn.json"}]},
        {"components":[
          {"name":"transform","pos":[0,0,0],"rotation":[0,0,0,1],"scale":[1,1,1]},
          {"name":"simplemodelview","model":"character"},
          {"name":"animation","graph":"turn.json"}]}
      ]}}})");
    auto &service = Animation::animationServiceRuntime();
    service.reset();
    FastModuleContainer modules;
    GET_MODULE(PathResolver).setup(root, false);
    GET_MODULE(ProjectSource).setSourceByData(nlohmann::json{{"basic_config", {
        {"scene_data_json", "scene.json"}, {"asset_data_json", "assets.json"}}}}.dump());
    auto &launch = GET_MODULE(EngineLaunchConfig);
    launch.headless = true;
    launch.headless_extent = vk::Extent2D{16, 16};
    TestSupport::requireVulkanDevice("Vulkan unavailable");
    (void)GET_MODULE(StandardMaterialResource);
    GET_MODULE(ECSPredefinedRegistration).reg();
    auto &scene = GET_MODULE(SceneLoader);
    test_support::loadScene(scene, "default_scene");
    auto &time = GET_MODULE(EngineTime);
    time.setup(EngineTime::Mode::fixed_step, 0.25);
    auto &ecs = GET_MODULE(ECSCore).getTemplatePublicModule();
    auto &instances = GET_MODULE(PolygonInstanceContainer);
    const auto frame = [&] {
        time.advance();
        GET_MODULE(ECSCore).update();
        service.synchronizeSceneObjects();
        GET_MODULE(AnimationSystem).updateGraphs();
        REQUIRE(service.runAllPhases(time.frameIndex()) == Animation::Status::ok);
        scene.applyPendingLoad();
    };
    const auto named = scene.objectId("Character");
    REQUIRE(named);
    frame();
    const auto instance = *ecs.tryComponent<SimpleModelViewComponent>(*named)->model_instance_id;
    const auto first_palette = instances.currentSkinPaletteForTesting(instance);
    REQUIRE(first_palette.size() == 2);
    REQUIRE(instances.currentAnimationRevisionForTesting(instance) == time.frameIndex());
    frame();
    REQUIRE(instances.currentSkinPaletteForTesting(instance) != first_palette);
    auto unnamed = invalidEntityId;
    for (const auto entity : ecs.liveEntities()) {
        if (entity != *named && ecs.tryComponent<AnimationComponent>(entity)) unnamed = entity;
    }
    REQUIRE(unnamed != invalidEntityId);
    REQUIRE_FALSE(scene.objectName(unnamed));
    const auto unnamed_instance = *ecs.tryComponent<SimpleModelViewComponent>(unnamed)->model_instance_id;
    REQUIRE(instances.currentAnimationRevisionForTesting(unnamed_instance) == time.frameIndex());

    GameContext context;
    const auto turn_palette = instances.currentSkinPaletteForTesting(instance);
    REQUIRE(std::abs(turn_palette[0][0][1]) > 0.1f);
    REQUIRE(context.setAnimationGraphParameter(*named, "go", 1) == Animation::Status::ok);
    frame();
    REQUIRE(instances.currentSkinPaletteForTesting(instance) != turn_palette);
    // Step translates the tip only; a continuing Turn would still rotate the root.
    REQUIRE(std::abs(instances.currentSkinPaletteForTesting(instance)[0][0][1]) < 0.0001f);
    REQUIRE(std::abs(instances.currentSkinPaletteForTesting(instance)[0][0][0] - 1.0f) < 0.0001f);
    REQUIRE(context.setAnimationGraphParameter(*named, "go", 0) == Animation::Status::ok);
    const auto step_palette = instances.currentSkinPaletteForTesting(instance);
    REQUIRE(context.forceAnimationGraphState(*named, "turn") == Animation::Status::ok);
    frame();
    REQUIRE(instances.currentSkinPaletteForTesting(instance) != step_palette);
    REQUIRE(std::abs(instances.currentSkinPaletteForTesting(instance)[0][0][1]) > 0.1f);

    // Switching to a clip removes the graph writer before phase evaluation.
    auto *animation = ecs.tryComponent<AnimationComponent>(*named);
    animation->graph.clear();
    animation->clip = "character.glb#animation/Turn";
    REQUIRE(context.setAnimationGraphParameter(*named, "go", 1) == Animation::Status::not_found);
    REQUIRE(context.forceAnimationGraphState(*named, "turn") == Animation::Status::not_found);
    frame();
    REQUIRE(instances.currentAnimationRevisionForTesting(instance) == time.frameIndex());
    REQUIRE(context.forceAnimationGraphState(*named, "turn") == Animation::Status::not_found);
    animation->clip.clear();
    animation->graph = "turn.json";
    frame();
    REQUIRE(instances.currentAnimationRevisionForTesting(instance) == time.frameIndex());
    // Removing the unnamed actor retires both its renderer slot and writer.
    REQUIRE(ecs.remove(unnamed));
    REQUIRE(context.forceAnimationGraphState(unnamed, "turn") == Animation::Status::not_found);
    frame();
    REQUIRE_THROWS(instances.currentAnimationRevisionForTesting(unnamed_instance));
    REQUIRE(context.forceAnimationGraphState(unnamed, "turn") == Animation::Status::not_found);

    test_support::loadScene(scene, "default_scene");
    frame();
    const auto reloaded = scene.objectId("Character");
    REQUIRE(reloaded);
    REQUIRE(*reloaded != *named);
    const auto new_instance = *ecs.tryComponent<SimpleModelViewComponent>(*reloaded)->model_instance_id;
    REQUIRE(new_instance != instance);
    REQUIRE(instances.currentAnimationRevisionForTesting(new_instance) == time.frameIndex());
    const auto reloaded_palette = instances.currentSkinPaletteForTesting(new_instance);
    frame();
    REQUIRE(instances.currentSkinPaletteForTesting(new_instance) != reloaded_palette);
    GET_MODULE(VulkanManageCore).waitIdle();
}

} // namespace Pelican
