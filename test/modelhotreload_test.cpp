#include "../src/core/asset/model.hpp"
#include "../src/core/phys/physworld.hpp"
#include "../src/core/ecs/predefined.hpp"
#include "../src/core/ecs/core.hpp"
#include "../src/core/ecs/predefined/transform.hpp"
#include "scene_load_test_support.hpp"
#include <catch2/catch_approx.hpp>
#include "../src/core/registrationownerhost.hpp"
#include "../src/core/animation/animationservice.hpp"
#include "../src/core/container.hpp"
#include "../src/core/launchconfig.hpp"
#include "../src/core/loader/pathresolver.hpp"
#include "../src/core/loader/projectsrc.hpp"
#include "../src/core/material/materialcontainer.hpp"
#include "materialcontainer_testaccess.hpp"
#include "../src/core/material/standardmaterialresource.hpp"
#include "../src/core/model/vertbufcontainer.hpp"
#include "../src/core/renderer/polygoninstancecontainer.hpp"
#include "polygoninstancecontainer_testaccess.hpp"
#include "../src/core/vkcore/core.hpp"
#include "../src/core/vkcore/deletionqueue.hpp"
#include "../src/core/watch/assetkey.hpp"
#include "../src/core/watch/reloadservice.hpp"
#include "../src/core/userpublic/animation/animgraph.hpp"
#include "../src/core/userpublic/details/reload/registrationowner.hpp"
#include "gltf_fragment_fixture.hpp"
#include "skeletal_fixture.hpp"
#include "raytracing_rt3_1_fixture.hpp"
#include "vulkan_test_support.hpp"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <glm/gtc/quaternion.hpp>
#include <limits>
#include <nlohmann/json.hpp>
#include <set>
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

std::set<std::string> modelOccurrenceIds(const ModelTemplate &model) {
    std::set<std::string> result;
    for (const auto &material : model.material_primitives)
        for (const auto &primitive : material.primitives)
            if (primitive.occurrence)
                result.insert(primitive.occurrence->occurrence_id);
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

TEST_CASE("Direct model references do not invent logical occurrence asset keys",
          "[wp382c][model-reload][direct-asset-key][gpu]") {
    setupLogger();
    auto sandbox = makeSandbox("direct_asset_key");
    const auto glb = sandbox.root / "fragment.glb";
    TestGltfFragmentFixture::writeGlb(glb);
    FastModuleContainer modules;
    configureProject(sandbox.root, nlohmann::json::array({
        {{"name", "named_model"}, {"path", "fragment.glb#node/RootA/Cube"}},
    }));
    TestSupport::requireVulkanDevice("Vulkan unavailable");
    (void)GET_MODULE(StandardMaterialResource);
    auto &models = GET_MODULE(ModelAssetContainer);
    const std::string relative = "fragment.glb#node/RootA/Cube";
    const std::string absolute = glb.generic_string() + "#node/RootA/Cube";
    const auto check = [&](const std::string &reference, bool named) {
        const auto &model = models.getModelTemplateByName(reference);
        REQUIRE(primitiveCount(model) > 0);
        for (const auto &material : model.material_primitives) {
            for (const auto &primitive : material.primitives) {
                REQUIRE(primitive.occurrence);
                const auto &identity = *primitive.occurrence;
                if (named) {
                    REQUIRE(identity.asset);
                    REQUIRE(identity.asset->logical_name == "named_model");
                    REQUIRE_FALSE(identity.occurrence_id.empty());
                    REQUIRE_FALSE(identity.error);
                } else {
                    REQUIRE_FALSE(identity.asset);
                    REQUIRE(identity.occurrence_id.empty());
                    REQUIRE(identity.error);
                    REQUIRE(identity.error->reason == "asset_key_missing");
                }
            }
        }
    };
    check("named_model", true);
    check(relative, false);
    check(absolute, false);
    auto &reload = GET_MODULE(watch::ReloadService);
    REQUIRE(reload.applyRequestForTesting(modified("fragment.glb")));
    check("named_model", true);
    check(relative, false);
    check(absolute, false);
}

TEST_CASE("Scene collision follows model reload additions changes and removals",
          "[model-reload][mesh-scene-reload][gpu]") {
    setupLogger();
    auto sandbox = makeSandbox("mesh_scene");
    const auto glb = sandbox.root / "collision.glb";
    auto writeMesh = [&](std::optional<float> z) {
        TestGltfFragmentFixture::detail::writeBytes(glb,
            TestGltfFragmentFixture::makeGlb(false, true, false, true, z));
    };
    writeMesh(3);
    FastModuleContainer modules;
    configureProject(sandbox.root, nlohmann::json::array({
        {{"name", "target_model"}, {"path", "collision.glb"}}
    }));
    std::filesystem::create_directories(sandbox.root / "passes");
    writeText(sandbox.root / "passes/main_rendering_config.json",
        R"({"pipeline":{"preset":"engine://render_pipelines/hybrid_v1.json"},"features":[]})");
    writeText(sandbox.root / "scene.json", R"({"schema":"pelican.scene","version":1,
        "scenes":{"default_scene":{"objects":[{"name":"target","components":[
        {"name":"transform","pos":[0,0,0],"rotation":[0,0,0,1],"scale":[1,1,1]},
        {"name":"simplemodelview","model":"target_model"}]}]}}})");
    TestSupport::requireVulkanDevice("Vulkan unavailable");
    (void)GET_MODULE(StandardMaterialResource);
    GET_MODULE(ECSPredefinedRegistration).reg();
    auto &scene = GET_MODULE(SceneLoader);
    test_support::loadScene(scene, "default_scene");
    auto &world = GET_MODULE(PhysWorld);
    auto &reload = GET_MODULE(watch::ReloadService);
    const phys::Ray ray{{0, 0, 0}, {0, 0, 1}, 10};
    auto hits = world.raycastAll(ray);
    REQUIRE(hits.size() == 1);
    CHECK(hits[0].distance == Catch::Approx(3));
    const auto id = hits[0].identity.collider_id;
    writeMesh(7);
    REQUIRE(reload.applyRequestForTesting(modified("collision.glb")));
    hits = world.raycastAll(ray);
    REQUIRE(hits.size() == 1);
    CHECK(hits[0].distance == Catch::Approx(7));
    CHECK(hits[0].identity.collider_id == id);
    writeText(glb, "broken glTF");
    REQUIRE_FALSE(reload.applyRequestForTesting(modified("collision.glb")));
    REQUIRE(world.raycastClosest(ray, phys::every_collider_filter));
    CHECK(world.raycastClosest(ray, phys::every_collider_filter)->distance == Catch::Approx(7));
    writeMesh(std::nullopt);
    REQUIRE(reload.applyRequestForTesting(modified("collision.glb")));
    CHECK(world.raycastAll(ray).empty());
    writeMesh(4);
    REQUIRE(reload.applyRequestForTesting(modified("collision.glb")));
    hits = world.raycastAll(ray);
    REQUIRE(hits.size() == 1);
    CHECK(hits[0].distance == Catch::Approx(4));
    const auto entity = scene.objectId("target");
    REQUIRE(entity);
    GET_MODULE(ECSCore).getTemplatePublicModule().component<TransformComponent>(*entity).pos = {5, 0, 0};
    CHECK(world.raycastAll(ray).empty());
    scene.loadTransientGltf("collision.glb", std::string{"temporary"});
    hits = world.raycastAll(ray);
    REQUIRE(hits.size() == 1);
    CHECK(hits[0].id == "temporary/mesh");
    CHECK(hits[0].distance == Catch::Approx(4));
    CHECK_THROWS(scene.loadTransientGltf("collision.glb", std::string{"temporary"}));
    REQUIRE(world.raycastAll(ray).size() == 1);
    test_support::loadScene(scene, "default_scene");
    hits = world.raycastAll(ray);
    REQUIRE(hits.size() == 1);
    CHECK(hits[0].id != "temporary/mesh");
    // The engine drains the previous scene at the next frame boundary.
    scene.drainRetirementQueue();
    GET_MODULE(VulkanManageCore).waitIdle();
}

TEST_CASE("Model external buffer dependencies change with one batched publication",
          "[model-reload][external-buffer][gpu]") {
    setupLogger();
    auto sandbox = makeSandbox("external_buffer");
    const auto document = sandbox.root / "model.gltf";
    TestSkeletalFixture::writeGltf(document, "first.bin");
    FastModuleContainer modules;
    configureProject(sandbox.root, nlohmann::json::array({{{"name", "model"}, {"path", "model.gltf"}}}));
    TestSupport::requireVulkanDevice("Vulkan unavailable");
    (void)GET_MODULE(StandardMaterialResource);
    auto &models = GET_MODULE(ModelAssetContainer);
    auto &reload = GET_MODULE(watch::ReloadService);
    const auto identity = models.assetIdForTesting("model");
    REQUIRE(models.handlesReload(watch::makeAssetKey("first.bin")));
    TestSkeletalFixture::writeGltf(document, "second.bin", {.position_scale = 2});
    std::filesystem::remove(sandbox.root / "first.bin");
    const std::array changed{
        modified("model.gltf"), watch::ReloadRequest{.key = watch::makeAssetKey("first.bin"),
                                                   .kind = watch::ReloadKind::removed}};
    REQUIRE(reload.applyRequestsForTesting(changed) == std::vector<bool>{true, true});
    REQUIRE(models.contentRevisionForTesting("model") == 2);
    REQUIRE(models.assetIdForTesting("model") == identity);
    REQUIRE_FALSE(models.handlesReload(watch::makeAssetKey("first.bin")));
    REQUIRE(models.handlesReload(watch::makeAssetKey("second.bin")));
    const auto current = reload.transactions().registry().snapshot();
    REQUIRE(current.reverseDependents(watch::makeAssetKey("first.bin")).empty());
    REQUIRE(current.reverseDependents(watch::makeAssetKey("second.bin")).size() == 1);
    TestSkeletalFixture::writeGltf(document, "third.bin", {.invalid_inverse_bind_buffer_view = true});
    REQUIRE_FALSE(reload.applyRequestForTesting(modified("model.gltf")));
    REQUIRE(models.contentRevisionForTesting("model") == 2);
    REQUIRE(models.handlesReload(watch::makeAssetKey("second.bin")));
    REQUIRE_FALSE(models.handlesReload(watch::makeAssetKey("third.bin")));
    TestSkeletalFixture::writeGltf(document, "third.bin", {.position_scale = 3});
    REQUIRE(reload.applyRequestForTesting(modified("model.gltf")));
    REQUIRE(models.contentRevisionForTesting("model") == 3);
    REQUIRE_FALSE(models.handlesReload(watch::makeAssetKey("second.bin")));
    REQUIRE(models.handlesReload(watch::makeAssetKey("third.bin")));
    REQUIRE(current.reverseDependents(watch::makeAssetKey("second.bin")).size() == 1);
}

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

    const auto matrix_a0 = PolygonInstanceTestAccess::currentModelMatrix(instances, instance_a0);
    const auto matrix_a1 = PolygonInstanceTestAccess::currentModelMatrix(instances, instance_a1);
    const auto matrix_b = PolygonInstanceTestAccess::currentModelMatrix(instances, instance_b);
    const auto generation_a0 = PolygonInstanceTestAccess::animationGeneration(instances, instance_a0);
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
    REQUIRE(PolygonInstanceTestAccess::instanceCountForAsset(instances, asset_a) == 2);
    REQUIRE(PolygonInstanceTestAccess::instanceCountForAsset(instances, asset_b) == 1);
    REQUIRE(sameMatrix(PolygonInstanceTestAccess::currentModelMatrix(instances, instance_a0), matrix_a0));
    REQUIRE(sameMatrix(PolygonInstanceTestAccess::currentModelMatrix(instances, instance_a1), matrix_a1));
    REQUIRE(sameMatrix(PolygonInstanceTestAccess::currentModelMatrix(instances, instance_b), matrix_b));
    REQUIRE(sameMatrix(PolygonInstanceTestAccess::previousModelMatrix(instances, instance_a0), matrix_a0));
    REQUIRE(PolygonInstanceTestAccess::animationGeneration(instances, instance_a0) == generation_a0 + 1);
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
    REQUIRE(sameMatrix(PolygonInstanceTestAccess::currentModelMatrix(instances, instance_a0), matrix_a0));
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
    while (materials.materialCountForTesting() + 1 < MaterialContainerTestAccess::materialCapacity(materials))
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
    const auto matrix = PolygonInstanceTestAccess::currentModelMatrix(instances, instance);
    const auto generation = PolygonInstanceTestAccess::animationGeneration(instances, instance);
    const auto old_occurrences =
        modelOccurrenceIds(models.getModelTemplateByName("live"));
    const auto old_catalog_generation =
        models.occurrenceCatalogStatus().at("generation").get<std::uint64_t>();
    REQUIRE_FALSE(models.getModelTemplateByName("live").skeletal);
    REQUIRE(primitiveCount(models.getModelTemplateByName("live")) == 2);

    RayInstanceComponent declaration;
    declaration.overrides.emplace();
    declaration.overrides->next_id = 2;
    declaration.overrides->rows.push_back({
        .envelope = {.id = 1},
        .value = {.occurrence_id = *old_occurrences.begin(), .mask = 9},
    });
    instances.setRayInstanceDeclaration(instance, 382, declaration);
    auto ray_candidate = instances.snapshotPreparedScene();
    auto ray_project = std::make_shared<RayTracingProjectConfig>();
    ray_project->layout = RayTracingRt31Fixture::v3Layout();
    instances.prepareRayScenePlan(ray_candidate, ray_project);
    instances.publishPreparedScene(std::move(ray_candidate));
    const auto old_ray_plan = instances.rayAsPlan();
    const auto old_ray_inventory = instances.rayAsInventory();
    REQUIRE(old_ray_plan);
    REQUIRE_FALSE(old_ray_plan->rows.empty());
    REQUIRE(old_ray_plan->occurrence_overrides.size() == 1);
    REQUIRE(old_ray_plan->occurrence_overrides.front().q.has_value());
    CHECK(old_ray_plan->occurrence_overrides.front().mask == 9);

    TestSkeletalFixture::writeGlb(glb);
    REQUIRE(reload.applyRequestForTesting(modified("model.glb")));
    REQUIRE(models.assetIdForTesting("live") == asset);
    REQUIRE(models.contentRevisionForTesting("live") == 2);
    REQUIRE(models.compatibilityRevisionForTesting("live") == 1);
    REQUIRE(models.getModelTemplateByName("live").skeletal);
    REQUIRE(primitiveCount(models.getModelTemplateByName("live")) == 1);
    REQUIRE(sameMatrix(PolygonInstanceTestAccess::currentModelMatrix(instances, instance), matrix));
    REQUIRE(sameMatrix(PolygonInstanceTestAccess::previousModelMatrix(instances, instance), matrix));
    REQUIRE(PolygonInstanceTestAccess::animationGeneration(instances, instance) == generation + 1);
    REQUIRE(PolygonInstanceTestAccess::currentAnimationRevision(instances, instance) == 0);
    REQUIRE(PolygonInstanceTestAccess::previousAnimationRevision(instances, instance) == 0);
    const auto new_occurrences =
        modelOccurrenceIds(models.getModelTemplateByName("live"));
    const auto catalog = models.occurrenceCatalogStatus();
    REQUIRE(new_occurrences != old_occurrences);
    REQUIRE(catalog.at("generation") == old_catalog_generation + 1);
    REQUIRE(catalog.at("added").get<std::vector<std::string>>() ==
            std::vector<std::string>(new_occurrences.begin(),
                                     new_occurrences.end()));
    REQUIRE(catalog.at("removed").get<std::vector<std::string>>() ==
            std::vector<std::string>(old_occurrences.begin(),
                                     old_occurrences.end()));
    const auto inventory = instances.rayOccurrences();
    std::set<std::string> active_occurrences;
    for (const auto &entry : inventory)
        active_occurrences.insert(entry.occurrence_id);
    REQUIRE(active_occurrences == new_occurrences);
    REQUIRE(instances.rayAsPlan() != old_ray_plan);
    REQUIRE(instances.rayAsInventory() != old_ray_inventory);
    REQUIRE(instances.rayAsPlan()->generation > old_ray_plan->generation);
    // RT5 keeps skinned geometry eligible; only the replaced occurrence's
    // override becomes absent after this rig change.
    REQUIRE(instances.rayAsPlan()->rows.size() == 1);
    REQUIRE(inventory.front().q.has_value());
    REQUIRE(instances.rayAsInventory()->instances.size() == 1);
    REQUIRE(instances.rayAsInventory()->instances.front().skinned);
    REQUIRE(instances.rayAsPlan()->occurrence_overrides.size() == 1);
    const auto &unresolved = instances.rayAsPlan()->occurrence_overrides.front();
    CHECK(unresolved.authoring_object_id == 382);
    CHECK(unresolved.row_id == 1);
    CHECK(unresolved.occurrence_id == *old_occurrences.begin());
    CHECK(unresolved.error == "OccurrenceAbsent");
    CHECK_FALSE(unresolved.q.has_value());
    GET_MODULE(VulkanManageCore).waitIdle();
}

TEST_CASE("WP382c model reload rollback keeps the published occurrence catalog and instances",
          "[wp382c][model-reload][rollback][gpu]") {
    setupLogger();
    auto sandbox = makeSandbox("catalog_rollback");
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
    const auto instance =
        instances.placeModelInstance(models.getModelTemplateByName("live"));
    (void)instance;
    REQUIRE_FALSE(models.getModelTemplateByName("live").skeletal);
    const auto old_occurrences =
        modelOccurrenceIds(models.getModelTemplateByName("live"));
    REQUIRE_FALSE(old_occurrences.empty());
    const auto old_catalog = models.occurrenceCatalogStatus();
    const auto old_primitives =
        primitiveCount(models.getModelTemplateByName("live"));
    std::set<std::string> old_active;
    for (const auto &entry : instances.rayOccurrences())
        old_active.insert(entry.occurrence_id);
    REQUIRE(old_active == old_occurrences);

    auto ray_candidate = instances.snapshotPreparedScene();
    auto ray_project = std::make_shared<RayTracingProjectConfig>();
    ray_project->layout = RayTracingRt31Fixture::v3Layout();
    instances.prepareRayScenePlan(ray_candidate, ray_project);
    instances.publishPreparedScene(std::move(ray_candidate));
    const auto old_ray_plan = instances.rayAsPlan();
    const auto old_ray_inventory = instances.rayAsInventory();
    REQUIRE(old_ray_plan);
    REQUIRE_FALSE(old_ray_plan->rows.empty());

    // Decision 30: the model participant's prepare_publish builds the whole
    // publication (instance rebuild list, candidate catalog diff, staged
    // payloads) and only then asks the renderer for the prepared scene. Arming
    // the production exhaustion check in prepareModelInstanceRebuild is the
    // one failure that lands in the coordinator's rollback branch with a
    // fully prepared publication in hand, so it is the only way to observe
    // that rollback drops the candidate catalog instead of committing it.
    test_support::setRayQueryGeometryGenerationForTesting(
        instances, std::numeric_limits<std::uint64_t>::max());

    TestSkeletalFixture::writeGlb(glb);
    REQUIRE_FALSE(reload.applyRequestForTesting(modified("model.glb")));

    // The live model is still the pre-reload one.
    REQUIRE(models.assetIdForTesting("live") == asset);
    REQUIRE(models.contentRevisionForTesting("live") == 1);
    REQUIRE(models.compatibilityRevisionForTesting("live") == 0);
    REQUIRE_FALSE(models.getModelTemplateByName("live").skeletal);
    REQUIRE(primitiveCount(models.getModelTemplateByName("live")) ==
            old_primitives);
    REQUIRE(modelOccurrenceIds(models.getModelTemplateByName("live")) ==
            old_occurrences);

    // ... and so is the published occurrence catalog: no generation step and
    // no candidate diff.
    const auto catalog = models.occurrenceCatalogStatus();
    REQUIRE(catalog.at("generation") == old_catalog.at("generation"));
    REQUIRE(catalog.at("added") == old_catalog.at("added"));
    REQUIRE(catalog.at("removed") == old_catalog.at("removed"));
    REQUIRE(catalog.at("ambiguous") == old_catalog.at("ambiguous"));

    // The active mapping is still the old one, so the renderer candidate was
    // dropped with the catalog rather than published on its own.
    std::set<std::string> active;
    for (const auto &entry : instances.rayOccurrences())
        active.insert(entry.occurrence_id);
    REQUIRE(active == old_occurrences);
    REQUIRE(instances.rayAsPlan() == old_ray_plan);
    REQUIRE(instances.rayAsInventory() == old_ray_inventory);
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
