#include "../src/core/asset/model.hpp"
#include "../src/core/animation/animationservice.hpp"
#include "../src/core/container.hpp"
#include "../src/core/launchconfig.hpp"
#include "../src/core/loader/pathresolver.hpp"
#include "../src/core/loader/projectsrc.hpp"
#include "../src/core/material/materialcontainer.hpp"
#include "../src/core/material/standardmaterialresource.hpp"
#include "../src/core/model/vertbufcontainer.hpp"
#include "../src/core/renderer/polygoninstancecontainer.hpp"
#include "../src/core/vkcore/core.hpp"
#include "../src/core/vkcore/deletionqueue.hpp"
#include "../src/core/watch/assetkey.hpp"
#include "../src/core/watch/reloadservice.hpp"
#include "../src/core/userpublic/animation/animgraph.hpp"
#include "../src/core/userpublic/details/reload/registrationowner.hpp"
#include "gltf_fragment_fixture.hpp"
#include "skeletal_fixture.hpp"
#include "vulkan_test_support.hpp"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <glm/gtc/quaternion.hpp>
#include <nlohmann/json.hpp>
#include <string_view>

namespace Pelican {
namespace {

struct Sandbox {
    std::filesystem::path root;
    ~Sandbox() { std::filesystem::remove_all(root); }
};

Sandbox makeSandbox(std::string_view label) {
    const auto root = std::filesystem::temp_directory_path() /
                      ("pelican_hr2g_" + std::string{label} + "_" +
                       std::to_string(std::chrono::steady_clock::now()
                                          .time_since_epoch().count()));
    std::filesystem::create_directories(root);
    return {root};
}

void writeText(const std::filesystem::path &path, std::string_view value) {
    std::ofstream file{path, std::ios::binary | std::ios::trunc};
    file.write(value.data(), static_cast<std::streamsize>(value.size()));
}

void configureProject(const std::filesystem::path &root, const nlohmann::json &models) {
    writeText(root / "assets.json",
              nlohmann::json{{"schema", "pelican.asset_data"},
                             {"version", 1},
                             {"models", models}}
                  .dump());
    writeText(root / "scene.json",
              R"json({"schema":"pelican.scene","version":1,"scenes":{"default_scene":{"objects":[]}}})json");
    GET_MODULE(PathResolver).setup(root, false);
    GET_MODULE(ProjectSource).setSourceByData(nlohmann::json{{"basic_config", {
        {"scene_data_json", "scene.json"}, {"asset_data_json", "assets.json"}}}}.dump());
    auto &launch = GET_MODULE(EngineLaunchConfig);
    launch.headless = true;
    launch.headless_extent = vk::Extent2D{16, 16};
}

bool sameMatrix(const glm::mat4 &left, const glm::mat4 &right) {
    for (int column = 0; column < 4; ++column)
        for (int row = 0; row < 4; ++row)
            if (left[column][row] != right[column][row]) return false;
    return true;
}

std::size_t primitiveCount(const ModelTemplate &model) {
    std::size_t result = 0;
    for (const auto &material : model.material_primitives) result += material.primitives.size();
    return result;
}

watch::ReloadRequest modified(std::string_view path) {
    return {.key = watch::makeAssetKey(path), .kind = watch::ReloadKind::modified};
}

template <class T> T animationDescriptor() {
    T value{};
    value.struct_size = sizeof(T);
    value.version = Animation::descriptorVersionV1;
    return value;
}

} // namespace

TEST_CASE("HR2-G fragment and rig reload contract", "[wp110][model-reload][gpu]") {
    setupLogger();
    auto sandbox = makeSandbox("contract");
    const auto glb = sandbox.root / "fragment.glb";
    TestGltfFragmentFixture::writeGlb(glb);

    FastModuleContainer modules;
    configureProject(sandbox.root, nlohmann::json::array({
        {{"name", "mesh_a"}, {"path", "fragment.glb#mesh/MeshA"}},
        {{"name", "mesh_b"}, {"path", "fragment.glb#mesh/MeshB"}},
    }));
    TestSupport::requireVulkanDevice("Vulkan unavailable");
    (void)GET_MODULE(StandardMaterialResource);

    auto &models = GET_MODULE(ModelAssetContainer);
    auto &instances = GET_MODULE(PolygonInstanceContainer);
    auto &materials = GET_MODULE(MaterialContainer);
    auto &geometry = GET_MODULE(VertBufContainer);
    auto &reload = GET_MODULE(watch::ReloadService);
    const auto asset_a = models.assetIdForTesting("mesh_a");
    const auto asset_b = models.assetIdForTesting("mesh_b");
    const auto instance_a0 = instances.placeModelInstance(models.getModelTemplateByName("mesh_a"));
    const auto instance_a1 = instances.placeModelInstance(models.getModelTemplateByName("mesh_a"));
    const auto instance_b = instances.placeModelInstance(models.getModelTemplateByName("mesh_b"));
    instances.setTrs(instance_a0, {2.0f, 3.0f, 4.0f}, glm::quat{1.0f, 0.0f, 0.0f, 0.0f},
                     {1.0f, 2.0f, 1.0f});
    instances.setTrs(instance_a1, {-3.0f, 1.0f, 0.5f}, glm::quat{1.0f, 0.0f, 0.0f, 0.0f},
                     {0.5f, 0.5f, 0.5f});
    instances.setTrs(instance_b, {7.0f, -2.0f, 1.0f}, glm::quat{1.0f, 0.0f, 0.0f, 0.0f},
                     {1.0f, 1.0f, 1.0f});
    instances.advanceTemporalHistoryAfterRender();

    const auto matrix_a0 = instances.currentModelMatrixForTesting(instance_a0);
    const auto matrix_a1 = instances.currentModelMatrixForTesting(instance_a1);
    const auto matrix_b = instances.currentModelMatrixForTesting(instance_b);
    const auto generation_a0 = instances.animationGenerationForTesting(instance_a0);
    const auto texture_count = materials.textureCountForTesting();
    const auto material_count = materials.materialCountForTesting();
    const auto index_count = geometry.allocatedIndexCountForTesting();
    const auto vertex_count = geometry.allocatedVertexCountForTesting();

    REQUIRE(reload.applyRequestForTesting(modified("fragment.glb")));
    REQUIRE(models.assetIdForTesting("mesh_a") == asset_a);
    REQUIRE(models.assetIdForTesting("mesh_b") == asset_b);
    REQUIRE(models.contentRevisionForTesting("mesh_a") == 2);
    REQUIRE(models.contentRevisionForTesting("mesh_b") == 2);
    REQUIRE(models.compatibilityRevisionForTesting("mesh_a") == 0);
    REQUIRE(instances.instanceCountForAssetForTesting(asset_a) == 2);
    REQUIRE(instances.instanceCountForAssetForTesting(asset_b) == 1);
    REQUIRE(sameMatrix(instances.currentModelMatrixForTesting(instance_a0), matrix_a0));
    REQUIRE(sameMatrix(instances.currentModelMatrixForTesting(instance_a1), matrix_a1));
    REQUIRE(sameMatrix(instances.currentModelMatrixForTesting(instance_b), matrix_b));
    REQUIRE(sameMatrix(instances.previousModelMatrixForTesting(instance_a0), matrix_a0));
    REQUIRE(instances.animationGenerationForTesting(instance_a0) == generation_a0 + 1);
    REQUIRE(materials.textureCountForTesting() == texture_count);
    REQUIRE(materials.materialCountForTesting() == material_count);
    REQUIRE(geometry.allocatedIndexCountForTesting() == index_count);
    REQUIRE(geometry.allocatedVertexCountForTesting() == vertex_count);
    REQUIRE(reload.transactions().registry().snapshot()
                .reverseDependents(watch::makeAssetKey("fragment.glb")).size() == 2);

    writeText(glb, "not a glTF container");
    const auto failed_before = reload.transactions().status().failed;
    REQUIRE_FALSE(reload.applyRequestForTesting(modified("fragment.glb")));
    REQUIRE(reload.transactions().status().failed == failed_before + 1);
    REQUIRE(models.contentRevisionForTesting("mesh_a") == 2);
    REQUIRE(models.contentRevisionForTesting("mesh_b") == 2);
    REQUIRE(sameMatrix(instances.currentModelMatrixForTesting(instance_a0), matrix_a0));
    REQUIRE(materials.textureCountForTesting() == texture_count);
    REQUIRE(materials.materialCountForTesting() == material_count);
    REQUIRE(geometry.allocatedIndexCountForTesting() == index_count);
    REQUIRE(geometry.allocatedVertexCountForTesting() == vertex_count);

    // Force actor 2 to fail during GPU staging, after actor 1 has already
    // allocated texture/material/geometry candidates. The group must return
    // every partial allocation and keep both published revisions unchanged.
    TestGltfFragmentFixture::writeGlb(glb);
    const auto &standard = GET_MODULE(StandardMaterialResource);
    const MaterialInfo filler{
        .vert_shader = standard.standardVertShader(),
        .frag_shader = standard.standardFragShader(),
        .base_color_texture = standard.whiteTexture(),
        .metallic_roughness_texture = standard.metallicRoughnessDefaultTexture(),
        .normal_texture = standard.normalDefaultTexture(),
        .emissive_texture = standard.emissiveDefaultTexture(),
        .occlusion_texture = standard.occlusionDefaultTexture(),
    };
    while (materials.materialCountForTesting() + 1 < materials.materialCapacityForTesting())
        (void)materials.registerMaterial(filler);
    const auto staged_failure_materials = materials.materialCountForTesting();
    const auto staged_failure_textures = materials.textureCountForTesting();
    const auto staged_failure_indices = geometry.allocatedIndexCountForTesting();
    const auto staged_failure_vertices = geometry.allocatedVertexCountForTesting();
    REQUIRE_FALSE(reload.applyRequestForTesting(modified("fragment.glb")));
    REQUIRE(models.contentRevisionForTesting("mesh_a") == 2);
    REQUIRE(models.contentRevisionForTesting("mesh_b") == 2);
    REQUIRE(materials.materialCountForTesting() == staged_failure_materials);
    REQUIRE(materials.textureCountForTesting() == staged_failure_textures);
    REQUIRE(geometry.allocatedIndexCountForTesting() == staged_failure_indices);
    REQUIRE(geometry.allocatedVertexCountForTesting() == staged_failure_vertices);
    GET_MODULE(VulkanManageCore).waitIdle();
}

TEST_CASE("HR2-G rig layout change advances compatibility and resets temporal state",
          "[wp110][model-reload][gpu][skeletal]") {
    setupLogger();
    auto sandbox = makeSandbox("rig");
    const auto glb = sandbox.root / "model.glb";
    TestGltfFragmentFixture::writeGlb(glb);

    FastModuleContainer modules;
    configureProject(sandbox.root,
                     nlohmann::json::array({{{"name", "live"}, {"path", "model.glb"}}}));
    TestSupport::requireVulkanDevice("Vulkan unavailable");
    (void)GET_MODULE(StandardMaterialResource);
    auto &models = GET_MODULE(ModelAssetContainer);
    auto &instances = GET_MODULE(PolygonInstanceContainer);
    auto &reload = GET_MODULE(watch::ReloadService);
    const auto asset = models.assetIdForTesting("live");
    const auto instance = instances.placeModelInstance(models.getModelTemplateByName("live"));
    instances.setTrs(instance, {4.0f, 5.0f, 6.0f}, glm::quat{1.0f, 0.0f, 0.0f, 0.0f},
                     {2.0f, 2.0f, 2.0f});
    instances.advanceTemporalHistoryAfterRender();
    const auto matrix = instances.currentModelMatrixForTesting(instance);
    const auto generation = instances.animationGenerationForTesting(instance);
    REQUIRE_FALSE(models.getModelTemplateByName("live").skeletal);
    REQUIRE(primitiveCount(models.getModelTemplateByName("live")) == 2);

    TestSkeletalFixture::writeGlb(glb);
    REQUIRE(reload.applyRequestForTesting(modified("model.glb")));
    REQUIRE(models.assetIdForTesting("live") == asset);
    REQUIRE(models.contentRevisionForTesting("live") == 2);
    REQUIRE(models.compatibilityRevisionForTesting("live") == 1);
    REQUIRE(models.getModelTemplateByName("live").skeletal);
    REQUIRE(primitiveCount(models.getModelTemplateByName("live")) == 1);
    REQUIRE(sameMatrix(instances.currentModelMatrixForTesting(instance), matrix));
    REQUIRE(sameMatrix(instances.previousModelMatrixForTesting(instance), matrix));
    REQUIRE(instances.animationGenerationForTesting(instance) == generation + 1);
    REQUIRE(instances.currentAnimationRevisionForTesting(instance) == 0);
    REQUIRE(instances.previousAnimationRevisionForTesting(instance) == 0);
    GET_MODULE(VulkanManageCore).waitIdle();
}

TEST_CASE("WP147 repeated HR2-G reload keeps another model animation running",
          "[wp147][model-reload][animation][gpu]") {
    setupLogger();
    auto sandbox = makeSandbox("animation_generation");
    const auto reloaded_path = sandbox.root / "reloaded.glb";
    const auto stable_path = sandbox.root / "stable.glb";
    TestSkeletalFixture::writeGlb(reloaded_path);
    TestSkeletalFixture::writeGlb(stable_path);

    auto &animation_runtime = Animation::animationServiceRuntime();
    animation_runtime.reset();
    FastModuleContainer modules;
    configureProject(sandbox.root, nlohmann::json::array({
        {{"name", "reloaded"}, {"path", "reloaded.glb"}},
        {{"name", "stable"}, {"path", "stable.glb"}},
    }));
    TestSupport::requireVulkanDevice("Vulkan unavailable");
    (void)GET_MODULE(StandardMaterialResource);

    auto &models = GET_MODULE(ModelAssetContainer);
    auto &reload = GET_MODULE(watch::ReloadService);
    animation_runtime.registerObject(
        "Reloaded", *models.getModelTemplateByName("reloaded").skeletal);
    animation_runtime.registerObject(
        "Stable", *models.getModelTemplateByName("stable").skeletal);

    constexpr auto graph = R"json({
      "schema":"pelican.anim_graph","version":1,"initial_state":"turn",
      "states":[{"name":"turn","type":"clip","clip":"Turn"}]
    })json";
    AnimationGraph::EvaluatorV1 stable_evaluator{
        AnimationGraph::parseDocumentV1(graph), "Stable", 147};
    {
        internal::ScopedRegistrationOwner owner_scope{1470};
        REQUIRE(stable_evaluator.bind() == Animation::Status::ok);
    }

    auto api = animationDescriptor<Animation::ApiV1>();
    REQUIRE(Animation::getApiV1(Animation::abiVersionV1, &api) ==
            Animation::Status::ok);
    auto service = animationDescriptor<Animation::AnimationServiceV1>();
    REQUIRE(api.get_animation_service(
                api.context, Animation::animationServiceVersionV1, &service) ==
            Animation::Status::ok);
    const auto resolve_sink = [&](std::string_view name) {
        auto request =
            animationDescriptor<Animation::ResolveAnimationSinkDescV1>();
        request.object_name = name.data();
        request.object_name_size = static_cast<std::uint32_t>(name.size());
        request.sink_kind = Animation::AnimationSinkKind::skeletal_pose;
        REQUIRE(service.resolve_sink(service.context, &request) ==
                Animation::Status::ok);
        return request.sink;
    };
    const auto reloaded_sink = resolve_sink("Reloaded");
    const auto stable_sink = resolve_sink("Stable");
    auto current_instance =
        animationDescriptor<Animation::ResolveAnimationInstanceDescV1>();
    current_instance.sink = reloaded_sink;
    REQUIRE(service.resolve_instance(service.context, &current_instance) ==
            Animation::Status::ok);
    const auto registration_generation =
        animation_runtime.registrationGeneration();

    for (std::uint64_t reload_index = 0; reload_index < 3; ++reload_index) {
        auto old_rig = animationDescriptor<Animation::ResolveAnimationRigDescV1>();
        old_rig.instance = current_instance.instance;
        REQUIRE(service.resolve_rig(service.context, &old_rig) ==
                Animation::Status::ok);

        REQUIRE(reload.applyRequestForTesting(modified("reloaded.glb")));
        REQUIRE(models.contentRevisionForTesting("reloaded") ==
                reload_index + 2);
        REQUIRE(animation_runtime.registrationGeneration() ==
                registration_generation);

        auto stale_rig = animationDescriptor<Animation::ResolveAnimationRigDescV1>();
        stale_rig.instance = current_instance.instance;
        REQUIRE(service.resolve_rig(service.context, &stale_rig) ==
                Animation::Status::stale_generation);
        auto stale_layout =
            animationDescriptor<Animation::ResolvePoseLayoutDescV1>();
        stale_layout.rig = old_rig.rig;
        REQUIRE(service.resolve_layout(service.context, &stale_layout) ==
                Animation::Status::stale_generation);

        current_instance =
            animationDescriptor<Animation::ResolveAnimationInstanceDescV1>();
        current_instance.sink = reloaded_sink;
        REQUIRE(service.resolve_instance(service.context, &current_instance) ==
                Animation::Status::ok);

        const auto revision = 14700 + reload_index;
        REQUIRE(stable_evaluator.prepareTick(
                    static_cast<double>(revision) / 60.0, 0.1, revision) ==
                Animation::Status::ok);
        REQUIRE(animation_runtime.runPhases(stable_sink, revision) ==
                Animation::Status::ok);
        REQUIRE(!stable_evaluator.lastPoseBytes().empty());
    }

    animation_runtime.reset();
    GET_MODULE(VulkanManageCore).waitIdle();
}

TEST_CASE("HR2-G defers model material slot reuse across in-flight frames",
          "[wp110][model-reload][gpu][lifetime]") {
    setupLogger();
    auto sandbox = makeSandbox("material_lifetime");
    const auto glb = sandbox.root / "model.glb";
    TestGltfFragmentFixture::writeGlb(glb);

    FastModuleContainer modules;
    configureProject(sandbox.root, nlohmann::json::array({
        {{"name", "live"}, {"path", "model.glb#mesh/MeshA"}},
    }));
    TestSupport::requireVulkanDevice("Vulkan unavailable");
    (void)GET_MODULE(StandardMaterialResource);
    auto &models = GET_MODULE(ModelAssetContainer);
    auto &materials = GET_MODULE(MaterialContainer);
    auto &geometry = GET_MODULE(VertBufContainer);
    auto &reload = GET_MODULE(watch::ReloadService);
    auto &deletion_queue = GET_MODULE(DeletionQueue);
    const auto material_count = materials.materialCountForTesting();
    const auto index_count = geometry.allocatedIndexCountForTesting();
    const auto first = models.getModelTemplateByName("live").material_primitives.front().material;

    REQUIRE(reload.applyRequestForTesting(modified("model.glb")));
    const auto second = models.getModelTemplateByName("live").material_primitives.front().material;
    REQUIRE(reload.applyRequestForTesting(modified("model.glb")));
    const auto third = models.getModelTemplateByName("live").material_primitives.front().material;

    REQUIRE(first != second);
    REQUIRE(first != third);
    REQUIRE(second != third);
    REQUIRE(deletion_queue.pendingCountForTesting() > 0);
    REQUIRE(models.contentRevisionForTesting("live") == 3);
    REQUIRE(materials.materialCountForTesting() == material_count);
    GET_MODULE(VulkanManageCore).waitIdle();
    deletion_queue.flushAll();
    REQUIRE(geometry.allocatedIndexCountForTesting() == index_count);
}

TEST_CASE("HR2-G survives 1000 geometry reloads without allocation growth",
          "[wp110][model-reload][gpu][stress]") {
    setupLogger();
    auto sandbox = makeSandbox("stress");
    const auto glb = sandbox.root / "model.glb";
    TestGltfFragmentFixture::writeGlb(glb, false, true);

    FastModuleContainer modules;
    configureProject(sandbox.root, nlohmann::json::array({
        {{"name", "geometry"}, {"path", "model.glb#mesh/MeshA"}},
    }));
    TestSupport::requireVulkanDevice("Vulkan unavailable");
    (void)GET_MODULE(StandardMaterialResource);
    auto &models = GET_MODULE(ModelAssetContainer);
    auto &materials = GET_MODULE(MaterialContainer);
    auto &geometry = GET_MODULE(VertBufContainer);
    auto &reload = GET_MODULE(watch::ReloadService);
    auto &deletion_queue = GET_MODULE(DeletionQueue);
    const auto textures = materials.textureCountForTesting();
    const auto material_count = materials.materialCountForTesting();
    const auto indices = geometry.allocatedIndexCountForTesting();
    const auto vertices = geometry.allocatedVertexCountForTesting();
    for (int iteration = 0; iteration < 1000; ++iteration) {
        REQUIRE(reload.applyRequestForTesting(modified("model.glb")));
        GET_MODULE(VulkanManageCore).waitIdle();
        deletion_queue.flushAll();
    }
    REQUIRE(models.contentRevisionForTesting("geometry") == 1001);
    REQUIRE(materials.textureCountForTesting() == textures);
    REQUIRE(materials.materialCountForTesting() == material_count);
    REQUIRE(geometry.allocatedIndexCountForTesting() == indices);
    REQUIRE(geometry.allocatedVertexCountForTesting() == vertices);
}

} // namespace Pelican
