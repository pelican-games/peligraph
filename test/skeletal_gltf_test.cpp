#include "../src/core/appflow/enginetime.hpp"
#include "../src/core/animation/animationservice.hpp"
#include "../src/core/ecs/predefined/animationsystem.hpp"
#include "../src/core/userpublic/components/animation.hpp"
#include "../src/core/userpublic/components/modelview.hpp"
#include "../src/core/userpublic/gamecontext.hpp"
#include "../src/core/userpublic/animation/curve.hpp"
#include "../src/core/userpublic/valuegraph.hpp"
#include "../src/core/renderingpass/computetask.hpp"
#include "../src/core/asset/model.hpp"
#include "../src/core/container.hpp"
#include "../src/core/ecs/core.hpp"
#include "../src/core/ecs/predefined.hpp"
#include "../src/core/launchconfig.hpp"
#include "../src/core/log.hpp"
#include "../src/core/loader/pathresolver.hpp"
#include "../src/core/loader/projectsrc.hpp"
#include "../src/core/loader/basicconfig.hpp"
#include "../src/core/loader/scene.hpp"
#include "scene_load_test_support.hpp"
#include "../src/core/material/standardmaterialresource.hpp"
#include "../src/core/model/gltf.hpp"
#include "../src/core/renderer/polygoninstancecontainer.hpp"
#include "../src/core/vkcore/core.hpp"
#include "../src/core/vkcore/renderer.hpp"
#include "../src/core/vkcore/renderer_config.hpp"
#include "../src/core/renderingpass/renderingpasscontainer.hpp"
#include "../src/core/renderingpass/framegraphruntime.hpp"
#include "../src/core/vkcore/util.hpp"
#include "../src/core/renderer/frameresources.hpp"
#include "../src/core/shader/shaderlibrary.hpp"
#include "../src/core/renderingpass/rendertargetcontainer.hpp"
#include "../src/project/featurecompose.hpp"
#include "skeletal_fixture.hpp"
#include "graph_composition_fixture.hpp"
#include "vulkan_test_support.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <chrono>
#include <cmath>
#include <array>
#include <cstring>
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

TEST_CASE("prepared glTF attributes use shader array layout without GPU allocation", "[gltf][skeletal][graph-attribute]") {
    setupLogger();
    const auto root = std::filesystem::temp_directory_path() /
        ("pelican_graph_attribute_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    const TempDir guard{root};
    TestSkeletalFixture::writeGlb(root / "character.glb");
    FastModuleContainer modules;
    auto &loader = GET_MODULE(GltfLoader);
    const auto prepared = loader.prepareGltfBinary((root / "character.glb").string());
    using Element = ShaderResourceBufferElement;
    const auto positions = loader.readAttribute(prepared, 0, 0, "POSITION", Element::vec3);
    REQUIRE(positions.element == Element::vec3);
    REQUIRE(positions.count == 4);
    REQUIRE(positions.bytes.size() == 64);
    std::array<float, 16> values;
    std::memcpy(values.data(), positions.bytes.data(), positions.bytes.size());
    REQUIRE(values[0] == -0.3f);
    REQUIRE(values[4] == 0.3f);
    REQUIRE(values[3] == 0.0f);
    REQUIRE(values[7] == 0.0f);
    REQUIRE(values[11] == 0.0f);
    REQUIRE(values[15] == 0.0f);
    const auto joints = loader.readAttribute(prepared, 0, 0, "JOINTS_0", Element::uvec4);
    REQUIRE(joints.count == 4);
    REQUIRE(joints.bytes.size() == 64);
    std::array<std::uint32_t, 16> joint_values;
    std::memcpy(joint_values.data(), joints.bytes.data(), joints.bytes.size());
    REQUIRE(joint_values[0] == 0);
    REQUIRE(joint_values[8] == 1);
    REQUIRE_THROWS(loader.readAttribute(prepared, 0, 0, "POSITION", Element::vec2));
    REQUIRE_THROWS(loader.readAttribute(prepared, 0, 0, "POSITION", Element::ivec3));
    REQUIRE_THROWS(loader.readAttribute(prepared, 0, 0, "JOINTS_0", Element::ivec4));
    REQUIRE_THROWS(loader.readAttribute(prepared, 0, 0, "MISSING", Element::vec3));
    REQUIRE_THROWS(loader.readAttribute(prepared, 1, 0, "POSITION", Element::vec3));
    REQUIRE_THROWS(loader.readAttribute(PreparedGltf{}, 0, 0, "POSITION", Element::vec3));
    REQUIRE(FastModuleContainer::tryGet<VulkanManageCore>() == nullptr);
    REQUIRE(FastModuleContainer::tryGet<StandardMaterialResource>() == nullptr);
}

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
          {"name":"animation","graph":"turn.json","parameters":{
            "$list":{"v":1,"next_id":2},
            "rows":[{"$row":{"id":1,"v":1},"parameter":"go","value":1}]}}]}
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
    // Shared graph, different authored initial values: only the named actor turns.
    REQUIRE(std::abs(instances.currentSkinPaletteForTesting(unnamed_instance)[0][0][1]) < 0.0001f);
    REQUIRE(std::abs(instances.currentSkinPaletteForTesting(instance)[0][0][1]) > 0.1f);

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

TEST_CASE("curve samples publish to a runtime float render buffer", "[runtime-render-values][skeletal]") {
    setupLogger();
    FastModuleContainer modules;
    GET_MODULE(EngineLaunchConfig).headless = true;
    TestSupport::requireVulkanDevice("Vulkan unavailable");
    auto &resources = GET_MODULE(FrameGraphResourceContainer);
    auto &vkcore = GET_MODULE(VulkanManageCore);
    const auto declaration = nlohmann::json::parse(R"({"buffers":[
        {"name":"curve_values","size":8,"host_source":"runtime_f32"}]})");
    resources.registerBuffers(parseFrameGraphBufferDefinitionsFromJson(declaration));
    resources.uploadRuntimeValues();
    const auto read = [&] {
        const auto bytes = vkcore.readBuf(resources.buffer("curve_values"), 8);
        std::array<float, 2> values;
        std::memcpy(values.data(), bytes.data(), bytes.size());
        return values;
    };
    REQUIRE(read() == std::array<float, 2>{0.0f, 0.0f});
    const ValueGraph::Graph graph{R"({"schema":"pelican.value_graph","version":1,
      "nodes":[{"id":"t","op":"input","name":"time"},
        {"id":"shape","op":"curve","time":"t","keys":[{"time":0,"value":0},{"time":1,"value":1}]},
        {"id":"gain","op":"constant","value":2},
        {"id":"scaled","op":"multiply","a":"shape","b":"gain"},
        {"id":"one","op":"constant","value":1},
        {"id":"offset","op":"add","a":"scaled","b":"one"}],
      "outputs":{"values":["scaled","offset"]},
      "bindings":[{"output":"values","kind":"render_buffer","target":"curve_values"}]})"};
    const ValueGraph::Graph restored{graph.serialize()};
    GameContext context;
    ValueGraph::Inputs inputs{{"time", 0.25}};
    const auto published = restored.publish(context, inputs);
    REQUIRE(published.applied == 1);
    REQUIRE(published.unavailable.empty());
    inputs["time"] = 99; // Already published values are a snapshot.
    REQUIRE(read() == std::array<float, 2>{0.0f, 0.0f});
    resources.uploadRuntimeValues();
    REQUIRE(read() == std::array<float, 2>{0.5f, 1.5f});
    std::array<float, 2> values{2.0f, 0.0f};
    REQUIRE_FALSE(context.setRenderValues("missing", values));
    REQUIRE_THROWS(context.setRenderValues("curve_values", std::span{values}.first(1)));
    REQUIRE(context.setRenderValues("curve_values", values));
    resources.uploadRuntimeValues();
    REQUIRE(read() == values);
    auto invalid = declaration;
    invalid["buffers"][0]["size"] = 6;
    REQUIRE_THROWS(parseFrameGraphBufferDefinitionsFromJson(invalid));
    vkcore.waitIdle();
}

TEST_CASE("common graph public ports reach a render buffer", "[skeletal][graph-composition]") {
    setupLogger();
    FastModuleContainer modules;
    GET_MODULE(EngineLaunchConfig).headless = true;
    TestSupport::requireVulkanDevice("Vulkan unavailable");
    auto &resources = GET_MODULE(FrameGraphResourceContainer);
    auto &vkcore = GET_MODULE(VulkanManageCore);
    resources.registerBuffers(parseFrameGraphBufferDefinitionsFromJson(nlohmann::json::parse(
        R"({"buffers":[{"name":"scoped_value","size":4,"host_source":"runtime_f32"}]})")));
    const ValueGraph::Graph graph{graphCompositionFixture};
    const ValueGraph::Graph restored{graph.serialize()};
    REQUIRE(restored.publish(GameContext{}, {{"time", 0.25}}).applied == 1);
    resources.uploadRuntimeValues();
    const auto bytes = vkcore.readBuf(resources.buffer("scoped_value"), sizeof(float));
    float value{};
    std::memcpy(&value, bytes.data(), sizeof(value));
    REQUIRE(value == 0.5f);
    vkcore.waitIdle();
}

TEST_CASE("mixed graph publishes scalar values consumed by a compute shader", "[skeletal][graph-composition]") {
    setupLogger();
    const auto root = std::filesystem::temp_directory_path() /
        ("pelican_graph_compute_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root / "shaders");
    const TempDir guard{root};
    writeText(root / "shaders/graph_probe.comp", R"(
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "pelican_resource_ports.glsl"
layout(local_size_x=1,local_size_y=1,local_size_z=1) in;
void main() { pelican_store_destination(0, pelican_load_source(0) * 3.0 + 1.0); }
)");
    FastModuleContainer modules;
    GET_MODULE(PathResolver).setup(root, false);
    GET_MODULE(EngineLaunchConfig).headless = true;
    TestSupport::requireVulkanDevice("Vulkan unavailable");
    const ValueGraph::Graph authored{parameterGraphCompositionFixture().dump()};
    const ValueGraph::Graph graph{authored.serialize()};
    const auto config = composeRenderFeatureConfig(nlohmann::json::parse(graph.serialize())).config;
    auto &resources = GET_MODULE(FrameGraphResourceContainer);
    resources.registerBuffers(parseFrameGraphBufferDefinitionsFromJson(config));
    auto &tasks = GET_MODULE(ComputeTaskContainer);
    const auto definitions = parseComputeTaskDefinitionsFromConfigJson(config);
    REQUIRE(definitions.size() == 1);
    const auto task = tasks.registerComputeTask(definitions[0], ComputeTaskRuntimeDependencies{
        GET_MODULE(ShaderLibrary), GET_MODULE(PathResolver), GET_MODULE(RenderTargetContainer), resources});
    auto &frames = GET_MODULE(FrameResources);
    auto &vkcore = GET_MODULE(VulkanManageCore);
    const auto run = [&](double time, const ValueGraph::ParameterValues &parameters = ValueGraph::ParameterValues{}) {
        REQUIRE(graph.publish(GameContext{}, {{"time", time}}, {}, {}, parameters).applied == 1);
        resources.uploadRuntimeValues();
        GET_MODULE(VulkanUtils).executeOneTimeCmd([&](vk::CommandBuffer cmd) {
            tasks.dispatch(cmd, task, frames);
            const vk::MemoryBarrier barrier{vk::AccessFlagBits::eShaderWrite, vk::AccessFlagBits::eHostRead};
            cmd.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader, vk::PipelineStageFlagBits::eHost,
                                {}, barrier, {}, {});
        }, true);
        const auto bytes = vkcore.readBuf(resources.buffer("result"), sizeof(float));
        float value{};
        std::memcpy(&value, bytes.data(), sizeof(value));
        return value;
    };
    REQUIRE(run(0.25) == 2.5f);
    REQUIRE(run(0.75) == 5.5f);
    REQUIRE(run(0.25, {{"left/gain", 4}}) == 4.0f);
    REQUIRE(run(0.25) == 2.5f);
    vkcore.waitIdle();
}

TEST_CASE("renderer evaluates the common graph using committed scene time", "[skeletal][graph-composition]") {
    setupLogger();
    const auto root = std::filesystem::temp_directory_path() /
        ("pelican_graph_frame_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root / "shaders");
    std::filesystem::create_directories(root / "passes");
    const TempDir guard{root};
    writeText(root / "shaders/graph_probe.comp", R"(
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "pelican_resource_ports.glsl"
layout(local_size_x=1,local_size_y=1,local_size_z=1) in;
void main() { pelican_store_destination(0, pelican_load_source(0) * 3.0 + 1.0); }
)");
    auto document = parameterGraphCompositionFixture();
    auto right_buffer = document["nodes"][5];
    right_buffer["id"] = "right_render";
    right_buffer["inputs"]["value"]["node"] = "right";
    document["nodes"].push_back(right_buffer);
    auto right_result = document["nodes"][6];
    right_result["id"] = "right_result";
    document["nodes"].push_back(right_result);
    auto right_probe = document["nodes"][7];
    right_probe["id"] = "right_probe";
    right_probe["inputs"]["source"]["node"] = "right_render";
    right_probe["inputs"]["destination"]["node"] = "right_result";
    document["nodes"].push_back(right_probe);
    bool graphics_path = false;
    SECTION("buffer compute path") {}
    SECTION("image compute path") {
        addGraphImageStage(document);
        std::string image_load = "imageLoad(pelican_resource_source, ivec2(0)).x";
        SECTION("storage image read") {}
        SECTION("sampled image read") {
            document["nodes"].back()["parameters"]["ports"]["source"]["binding"] = "sampled";
            image_load = "texelFetch(pelican_resource_source, ivec2(0), 0).x";
        }
        SECTION("graphics attachment write") {
            graphics_path = true;
            document["nodes"][5]["op"] = "render.attribute";
            document["nodes"][5]["parameters"]["domain"] = "instance";
            document["nodes"][5]["parameters"]["count"] = 1;
            auto &writer = document["nodes"][7];
            writer["parameters"]["ports"]["source"]["domain"] = "instance";
            writer["op"] = "render.fullscreen";
            writer["parameters"].erase("dispatch");
            writer["parameters"]["pass_set"] = "main_render";
            writer["parameters"]["shader"] = {{"vertex", "engine://fullscreen"},
                                                 {"fragment", "shaders/graph_image_write"}};
            writer["parameters"]["ports"]["destination"]["attachment"] = "color";
            writer["parameters"]["ports"]["destination"]["location"] = 0;
            writer["parameters"]["color_load_op"] = "clear";
            SECTION("fullscreen") {}
            SECTION("raster") {
                writer["op"] = "render.raster";
                writer["parameters"]["draw"] = {{"vertex_count", 3}};
                writer["parameters"]["draw"]["instance_source"] = "source";
            }
            for (auto &node : document["nodes"])
                if (node["id"] == "intermediate") {
                    node["parameters"]["usage"].push_back("COLOR_ATTACHMENT");
                }
            writeText(root / "shaders/graph_image_write.frag", R"(
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "pelican_resource_ports.glsl"
layout(location=0) out vec4 color;
void main() { color = vec4(pelican_count_source() == 1u ? pelican_load_source(0) : -100.0); }
)");
        }
        writeText(root / "shaders/graph_image_write.comp", R"(
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "pelican_resource_ports.glsl"
layout(local_size_x=1,local_size_y=1,local_size_z=1) in;
void main() { imageStore(pelican_resource_destination, ivec2(0), vec4(pelican_load_source(0))); }
)");
        writeText(root / "shaders/graph_image_read.comp", R"(
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "pelican_resource_ports.glsl"
layout(local_size_x=1,local_size_y=1,local_size_z=1) in;
void main() { pelican_store_destination(0, )" + image_load + R"( * 3.0 + 1.0); }
)");
    }
    document["nodes"][0]["parameters"]["name"] = "scene_time";
    document["rendering"] = {{"pipeline", {{"preset", "engine://render_pipelines/hybrid_v1.json"}}}};
    writeText(root / "passes/main_rendering_config.json", document.dump());
    writeText(root / "assets.json", R"({"schema":"pelican.asset_data","version":1,"models":[]})");
    writeText(root / "scene.json", R"({"schema":"pelican.scene","version":1,"scenes":{"default_scene":{"objects":[
      {"name":"Left","components":[{"name":"transform","pos":[0,0,0],"rotation":[0,0,0,1],"scale":[1,1,1]}]},
      {"name":"Right","components":[{"name":"transform","pos":[0,0,0],"rotation":[0,0,0,1],"scale":[1,1,1]}]}
    ]}}})");
    FastModuleContainer modules;
    GET_MODULE(PathResolver).setup(root, false);
    GET_MODULE(ProjectSource).setSourceByData(nlohmann::json{{"basic_config", {
        {"scene_data_json", "scene.json"}, {"asset_data_json", "assets.json"}}}}.dump());
    auto &launch = GET_MODULE(EngineLaunchConfig);
    launch.headless = true;
    launch.headless_extent = vk::Extent2D{16, 16};
    TestSupport::requireVulkanDevice("Vulkan unavailable");
    auto &time = GET_MODULE(EngineTime);
    time.setTime(10);
    GET_MODULE(ECSPredefinedRegistration).reg();
    auto &scene = GET_MODULE(SceneLoader);
    test_support::loadScene(scene, "default_scene");
    REQUIRE(scene.sceneStartTime() == 10);
    const auto left = *scene.objectId("Left");
    const auto right = *scene.objectId("Right");
    for (const auto &binding : scene.runtimeObjectBindings()) {
        if (binding.object_id == left || binding.object_id == right)
            document["nodes"][binding.object_id == left ? 3 : 4]["owner"] = {
                {"scene", "default_scene"}, {"object", binding.authoring_object_id.value}};
    }
    GET_MODULE(ProjectBasicConfig).publishRenderingConfigJson(document.dump());
    auto &renderer = GET_MODULE(Renderer);
    renderer.setExecutionTracingForTesting(true);
    auto &vkcore = GET_MODULE(VulkanManageCore);
    const auto read = [&](const char *buffer) {
        const auto bytes = vkcore.readBuf(GET_MODULE(FrameGraphResourceContainer).buffer(buffer), sizeof(float));
        float value{};
        std::memcpy(&value, bytes.data(), sizeof(value));
        return value;
    };
    const auto render = [&](double now) {
        time.setTime(now);
        renderer.render();
        vkcore.waitIdle();
        return read("result");
    };
    REQUIRE(render(10.25) == 2.5f);
    REQUIRE(render(10.75) == 5.5f);
    GameContext context;
    REQUIRE_FALSE(context.setGraphParameter(right, "left/gain", 3));
    REQUIRE(context.setGraphParameter(left, "left/gain", 3));
    REQUIRE(render(10.25) == 3.25f);
    const auto other_graph = std::make_shared<const ValueGraph::Graph>(document.dump());
    REQUIRE(scene.setGraphParameter(other_graph, left, "left/gain", 9));
    REQUIRE(render(10.25) == 3.25f);
    REQUIRE(scene.graphParameterValues(other_graph).at("left/gain") == 9);
    REQUIRE(context.setGraphParameter(right, "right/gain", 6));
    REQUIRE(render(10.25) == 3.25f);
    REQUIRE(context.setGraphParameter(left, "left/gain", std::nullopt));
    REQUIRE(render(10.25) == 2.5f);
    REQUIRE(context.setGraphParameter(left, "left/gain", 4));
    GameObjects{}.remove(left);
    REQUIRE_FALSE(context.setGraphParameter(left, "left/gain", 5));
    const std::array<float, 1> sentinel{-17};
    const std::array<float, 1> input_sentinel{9};
    REQUIRE(context.setRenderValues("result", sentinel));
    REQUIRE(context.setRenderValues("render", input_sentinel));
    REQUIRE(render(10.25) == -17);
    REQUIRE(read("render") == 9);
    REQUIRE(read("right_result") == 5.5f);
    if (graphics_path) {
        const auto &trace = renderer.lastExecutionTraceForTesting();
        const auto &view = trace.contains("view_family") ? trace.at("view_family") : trace;
        const auto &nodes = view.at("nodes");
        const auto probe = std::find_if(nodes.begin(), nodes.end(), [](const auto &node) {
            return node.at("name") == "probe";
        });
        REQUIRE(probe != nodes.end());
        REQUIRE_FALSE(probe->at("recorded").template get<bool>());
        const auto &layouts = view.at("final_layouts");
        const auto intermediate = std::find_if(layouts.begin(), layouts.end(), [](const auto &entry) {
            return entry.at("resource") == "intermediate";
        });
        REQUIRE(intermediate != layouts.end());
        REQUIRE(intermediate->at("layout") == "undefined");
    }
    REQUIRE(scene.graphParameterValues(other_graph).empty());
    REQUIRE(context.setGraphParameter(right, "right/gain", 8));
    REQUIRE(render(10.5) == -17);
    REQUIRE(read("right_result") == 13);
    time.setTime(10.75);
    test_support::loadScene(scene, "default_scene");
    REQUIRE(scene.sceneStartTime() == 10.75);
    REQUIRE(render(11) == 2.5f);
    const auto reloaded_left = *scene.objectId("Left");
    REQUIRE(reloaded_left != left);
    REQUIRE_FALSE(context.setGraphParameter(left, "left/gain", 3));
    REQUIRE(context.setGraphParameter(reloaded_left, "left/gain", 3));
    REQUIRE(render(11) == 3.25f);
    const auto generation = GET_MODULE(RenderingPassContainer).activeGeneration();
    auto invalid = document;
    invalid["nodes"][0]["parameters"]["name"] = "unbound_input";
    REQUIRE_THROWS(loadRenderGraphVariantsFromConfigData(invalid.dump()));
    REQUIRE(GET_MODULE(RenderingPassContainer).activeGeneration() == generation);
    REQUIRE(render(11) == 3.25f);
    document["nodes"][3]["parameters"]["gain"] = 4;
    loadRenderGraphVariantsFromConfigData(document.dump());
    REQUIRE(GET_MODULE(RenderingPassContainer).activeGeneration() > generation);
    REQUIRE(render(11) == 4.0f);
    auto event_document = document;
    addGraphEventClock(event_document);
    addGraphTriggerClock(document);
    loadRenderGraphVariantsFromConfigData(document.dump());
    REQUIRE(render(12) == 1);
    REQUIRE(context.setGraphParameter(reloaded_left, "left/fire", 1));
    REQUIRE(render(12) == 1);
    REQUIRE(render(12.25) == 4);
    REQUIRE(context.setGraphParameter(reloaded_left, "left/paused", 1));
    REQUIRE(render(12.25) == 4);
    REQUIRE(render(12.75) == 4);
    REQUIRE(context.setGraphParameter(reloaded_left, "left/paused", 0));
    REQUIRE(render(12.75) == 4);
    REQUIRE(render(13) == 7);
    REQUIRE_THROWS(loadRenderGraphVariantsFromConfigData(invalid.dump()));
    REQUIRE(render(13) == 7);
    REQUIRE(context.setGraphParameter(reloaded_left, "left/reset", 1));
    REQUIRE(render(13) == 1);
    REQUIRE(context.setGraphParameter(reloaded_left, "left/reset", 0));
    REQUIRE(render(13.25) == 1); // A held trigger does not restart after reset.
    REQUIRE(context.setGraphParameter(reloaded_left, "left/fire", 0));
    REQUIRE(render(13.25) == 1);
    REQUIRE(context.setGraphParameter(reloaded_left, "left/fire", 1));
    REQUIRE(render(13.25) == 1);
    REQUIRE(render(13.5) == 4);
    test_support::loadScene(scene, "default_scene");
    REQUIRE(render(14) == 1);
    loadRenderGraphVariantsFromConfigData(event_document.dump());
    const auto event_left = *scene.objectId("Left");
    const auto event_right = *scene.objectId("Right");
    using Command = Animation::CurvePlaybackCommand;
    REQUIRE_FALSE(context.emitGraphEvent(event_right, "left/commands", Command::start));
    REQUIRE_FALSE(context.emitGraphEvent(event_left, "missing", Command::start));
    REQUIRE(context.emitGraphEvent(event_left, "left/commands", Command::start));
    REQUIRE(render(14) == 1);
    time.setTime(14.125);
    REQUIRE(context.emitGraphEvent(event_left, "left/commands", Command::pause));
    time.setTime(14.5);
    REQUIRE(context.emitGraphEvent(event_left, "left/commands", Command::resume));
    REQUIRE(render(14.75) == 5.5);
    REQUIRE(render(14.75) == 5.5);
    REQUIRE(render(15) == 8.5);
    REQUIRE(context.emitGraphEvent(event_left, "left/commands", Command::reset));
    REQUIRE(render(15.25) == 1);
    REQUIRE(context.emitGraphEvent(event_left, "left/commands", Command::start));
    test_support::loadScene(scene, "default_scene");
    REQUIRE_FALSE(context.emitGraphEvent(event_left, "left/commands", Command::start));
    REQUIRE(render(17) == 1);
    REQUIRE(render(18.25) == 1); // A retired scene's queued start must not fire later.
#if PELICAN_WITH_AUDIO
    const unsigned char wav[]{'R','I','F','F',44,0,0,0,'W','A','V','E','f','m','t',' ',16,0,0,0,
        1,0,1,0,64,31,0,0,128,62,0,0,2,0,16,0,'d','a','t','a',8,0,0,0,0,0,0,0,0,0,0,0};
    writeText(root / "tone.wav", {reinterpret_cast<const char *>(wav), sizeof(wav)});
    event_document["definitions"]["scaled_curve"]["nodes"].push_back({
        {"id", "sound"}, {"op", "audio.playback"}, {"parameters", {{"asset", "project://tone.wav"}}},
        {"inputs", {{"time", {{"node", "time"}, {"port", "value"}}},
                    {"events", {{"node", "commands"}, {"port", "events"}}}}}, {"outputs", {"voice"}}});
    for (const auto *op : {"audio.volume", "audio.pitch"})
        event_document["definitions"]["scaled_curve"]["nodes"].push_back({
            {"id", op}, {"op", op}, {"parameters", nlohmann::json::object()},
            {"inputs", {{"value", {{"node", "gain"}, {"port", "value"}}},
                        {"voice", {{"node", "sound"}, {"port", "voice"}}}}},
            {"outputs", nlohmann::json::array()}});
    loadRenderGraphVariantsFromConfigData(event_document.dump());
    auto sound_graph = GET_MODULE(RenderingPassContainer).snapshot()->programs.begin()->second.frame_graph.value_graph;
    const auto requireSound = [&](const auto &graph, const char *node) {
        const auto sound = scene.graphSound(graph, node);
        REQUIRE(sound);
        return *sound;
    };
    const auto sound_left = *scene.objectId("Left");
    const auto sound_right = *scene.objectId("Right");
    REQUIRE(context.setGraphParameter(sound_left, "left/gain", 3));
    (void)render(18.25);
    REQUIRE_FALSE(scene.graphSound(sound_graph, "left/sound"));
    auto wrong_voice = event_document;
    wrong_voice["definitions"]["scaled_curve"]["nodes"].back()["inputs"]["voice"] =
        {{"node", "gain"}, {"port", "value"}};
    REQUIRE_THROWS(ValueGraph::Graph{wrong_voice.dump()});
    auto double_target = event_document;
    double_target["definitions"]["scaled_curve"]["nodes"].back()["parameters"]["target"] = "external";
    REQUIRE_THROWS(ValueGraph::Graph{double_target.dump()});
    REQUIRE(context.emitGraphEvent(sound_left, "left/commands", Command::start));
    REQUIRE(context.emitGraphEvent(sound_right, "right/commands", Command::start));
    (void)render(18.25);
    const auto left_voice = requireSound(sound_graph, "left/sound");
    const auto right_voice = requireSound(sound_graph, "right/sound");
    REQUIRE(left_voice != right_voice);
    REQUIRE(context.isPlaying(left_voice));
    REQUIRE(context.isPlaying(right_voice));
    REQUIRE(context.soundState(left_voice));
    REQUIRE(context.soundState(right_voice));
    REQUIRE(context.soundState(left_voice)->volume == 3.0f);
    REQUIRE(context.soundState(left_voice)->pitch == 3.0f);
    REQUIRE(context.soundState(right_voice)->volume == 4.0f);
    REQUIRE(context.soundState(right_voice)->pitch == 4.0f);
    REQUIRE(context.emitGraphEvent(sound_left, "left/commands", Command::pause));
    (void)render(18.5);
    REQUIRE(context.soundState(left_voice));
    REQUIRE(context.soundState(left_voice)->paused);
    REQUIRE(context.isPlaying(right_voice));
    REQUIRE(context.emitGraphEvent(sound_left, "left/commands", Command::resume));
    (void)render(18.75);
    REQUIRE(context.isPlaying(left_voice));
    REQUIRE(context.emitGraphEvent(sound_left, "left/commands", Command::reset));
    (void)render(19);
    REQUIRE_FALSE(context.soundState(left_voice));
    REQUIRE(context.setGraphParameter(sound_left, "left/gain", 6));
    REQUIRE(context.emitGraphEvent(sound_left, "left/commands", Command::start));
    (void)render(19.25);
    const auto restarted_voice = requireSound(sound_graph, "left/sound");
    REQUIRE(restarted_voice != left_voice);
    REQUIRE(context.soundState(restarted_voice));
    REQUIRE(context.soundState(restarted_voice)->volume == 6.0f);
    REQUIRE(context.soundState(restarted_voice)->pitch == 6.0f);
    auto bad_audio = event_document;
    for (auto &node : bad_audio["definitions"]["scaled_curve"]["nodes"])
        if (node["id"] == "sound") node["parameters"]["asset"] = "project://missing.wav";
    REQUIRE_THROWS(loadRenderGraphVariantsFromConfigData(bad_audio.dump()));
    REQUIRE(context.isPlaying(restarted_voice));
    GameObjects{}.remove(sound_left);
    scene.updateGraphRuntime();
    REQUIRE_FALSE(context.soundState(restarted_voice));
    REQUIRE(context.isPlaying(right_voice));
    test_support::loadScene(scene, "default_scene");
    REQUIRE_FALSE(context.soundState(right_voice));
    auto temporary_graph = std::make_shared<const ValueGraph::Graph>(event_document.dump());
    REQUIRE(scene.applyGraphAudioEvent(temporary_graph, "right/sound", Command::start));
    const auto temporary_voice = requireSound(temporary_graph, "right/sound");
    temporary_graph.reset();
    scene.updateGraphRuntime();
    REQUIRE_FALSE(context.soundState(temporary_voice));
#endif
}

} // namespace Pelican
