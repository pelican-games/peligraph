#include "../src/core/container.hpp"
#include "../src/core/launchconfig.hpp"
#include "../src/core/log.hpp"
#include "../src/core/material/standardmaterialresource.hpp"
#include "../src/core/model/gltf.hpp"
#include "../src/core/model/vertbufcontainer.hpp"
#include "../src/core/renderer/polygoninstancecontainer.hpp"
#include "../src/core/vkcore/core.hpp"
#include "vulkan_test_support.hpp"
#include "morph_fixture.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <limits>
#include <vector>

namespace Pelican {
namespace {

struct MorphSandbox {
    std::filesystem::path root;
    ~MorphSandbox() { std::filesystem::remove_all(root); }
};

MorphSandbox makeMorphSandbox(std::string_view name) {
    auto root = std::filesystem::temp_directory_path() /
                ("pelican_wp121_" + std::string{name} + "_" +
                 std::to_string(std::chrono::steady_clock::now()
                                    .time_since_epoch().count()));
    std::filesystem::create_directories(root);
    return {root};
}

void requireMorphVulkan() {
    auto &launch = GET_MODULE(EngineLaunchConfig);
    launch.headless = true;
    launch.headless_extent = vk::Extent2D{16, 16};
    TestSupport::requireVulkanDevice("Vulkan unavailable");
    (void)GET_MODULE(StandardMaterialResource);
}

PublishMorphWeightFrameDescV1 publishDesc(const ModelTemplate &model,
                                          std::uint64_t revision,
                                          const float *weights,
                                          std::uint32_t count,
                                          std::uint32_t flags = 0) {
    return {
        .layout_generation = model.morph_targets->generation,
        .frame_revision = revision,
        .weights = weights,
        .weight_count = count,
        .flags = flags,
    };
}

} // namespace

TEST_CASE("glTF morph targets decode into deterministic shared ranges",
          "[wp121][gltf][morph]") {
    setupLogger();
    auto sandbox = makeMorphSandbox("decode");
    const auto valid = sandbox.root / "valid.glb";
    TestMorphFixture::writeGlb(valid, {.mesh_weight = 0.25,
                                       .node_weight = 0.5});

    FastModuleContainer modules;
    requireMorphVulkan();
    auto &loader = GET_MODULE(GltfLoader);
    const auto model = loader.loadGltfBinary(valid.string());
    REQUIRE(model.morph_targets);
    REQUIRE(model.morph_targets->generation != 0);
    REQUIRE(model.morph_targets->default_weights == std::vector<float>{0.5f});
    REQUIRE(model.morph_targets->primitives.size() == 1);
    const auto &primitive = model.morph_targets->primitives.front();
    REQUIRE(primitive.node_index == 0);
    REQUIRE(primitive.mesh_index == 0);
    REQUIRE(primitive.primitive_index == 0);
    REQUIRE_FALSE(primitive.skinned);
    REQUIRE(primitive.weight_offset == 0);
    REQUIRE(primitive.delta_ranges.size() == 1);
    REQUIRE(primitive.delta_ranges[0].vertex_count == 4);
    REQUIRE(primitive.delta_ranges[0].presence_mask ==
            (morphPositionPresent | morphNormalPresent | morphTangentPresent));
    REQUIRE(GET_MODULE(VertBufContainer).allocatedMorphDeltaCountForTesting() == 4);

    const auto vertex_abi = VertBufContainer::getDescription();
    REQUIRE(vertex_abi.binding_descs.size() == 1);
    REQUIRE(vertex_abi.binding_descs[0].stride == sizeof(CommonVertStruct));
    REQUIRE(vertex_abi.attr_descs.size() == 5);
    GET_MODULE(VulkanManageCore).waitIdle();
}

TEST_CASE("glTF morph target validation names unsupported input",
          "[wp121][gltf][morph][validation]") {
    setupLogger();
    auto sandbox = makeMorphSandbox("invalid");
    const auto sparse = sandbox.root / "sparse.glb";
    const auto count = sandbox.root / "count.glb";
    const auto semantic = sandbox.root / "semantic.glb";
    const auto finite = sandbox.root / "finite.glb";
    const auto target_limit = sandbox.root / "target_limit.glb";
    TestMorphFixture::writeGlb(sparse, {.sparse_position = true});
    TestMorphFixture::writeGlb(count, {.wrong_count = true});
    TestMorphFixture::writeGlb(semantic, {.unknown_semantic = true});
    TestMorphFixture::writeGlb(finite, {.non_finite = true});
    TestMorphFixture::writeGlb(target_limit, {.target_count = 65});

    FastModuleContainer modules;
    requireMorphVulkan();
    auto &loader = GET_MODULE(GltfLoader);
    REQUIRE_THROWS_WITH(loader.loadGltfBinary(sparse.string()),
                        Catch::Matchers::ContainsSubstring("target 0 POSITION") &&
                            Catch::Matchers::ContainsSubstring("sparse accessor"));
    REQUIRE_THROWS_WITH(loader.loadGltfBinary(count.string()),
                        Catch::Matchers::ContainsSubstring("target 0") &&
                            Catch::Matchers::ContainsSubstring("count"));
    REQUIRE_THROWS_WITH(loader.loadGltfBinary(semantic.string()),
                        Catch::Matchers::ContainsSubstring("target 0") &&
                            Catch::Matchers::ContainsSubstring("COLOR_0"));
    REQUIRE_THROWS_WITH(loader.loadGltfBinary(finite.string()),
                        Catch::Matchers::ContainsSubstring("target 0 POSITION") &&
                            Catch::Matchers::ContainsSubstring("non-finite"));
    REQUIRE_THROWS_WITH(loader.loadGltfBinary(target_limit.string()),
                        Catch::Matchers::ContainsSubstring("morph target count") &&
                            Catch::Matchers::ContainsSubstring("limit of 64"));
    REQUIRE(GET_MODULE(VertBufContainer).allocatedMorphDeltaCountForTesting() == 0);
}

TEST_CASE("morph weight frames are per-instance N and N-1 state",
          "[wp121][morph][instance][temporal]") {
    setupLogger();
    auto sandbox = makeMorphSandbox("instances");
    const auto path = sandbox.root / "morph.glb";
    TestMorphFixture::writeGlb(path);

    FastModuleContainer modules;
    requireMorphVulkan();
    const auto model = GET_MODULE(GltfLoader).loadGltfBinary(path.string());
    auto &instances = GET_MODULE(PolygonInstanceContainer);
    const auto first = instances.placeModelInstance(model);
    const auto second = instances.placeModelInstance(model);
    const float off = 0.0f;
    const float on = 1.0f;
    auto first_frame = publishDesc(model, 7, &off, 1);
    auto second_frame = publishDesc(model, 7, &on, 1);
    REQUIRE(instances.publishMorphWeightFrame(first, first_frame) ==
            Animation::Status::ok);
    REQUIRE(instances.publishMorphWeightFrame(second, second_frame) ==
            Animation::Status::ok);
    REQUIRE(instances.morphWeightFrameForTesting(first).current ==
            std::vector<float>{0.0f});
    REQUIRE(instances.morphWeightFrameForTesting(second).current ==
            std::vector<float>{1.0f});
    REQUIRE(instances.morphWeightFrameForTesting(first).current_revision == 7);
    REQUIRE(instances.morphWeightFrameForTesting(second).current_revision == 7);
    REQUIRE(instances.morphWeightFrameForTesting(first).previous ==
            std::vector<float>{0.0f});
    REQUIRE(instances.morphWeightFrameForTesting(second).previous ==
            std::vector<float>{1.0f});

    instances.advanceMorphHistoryAfterRender();
    const float moved = 0.75f;
    auto moved_frame = publishDesc(model, 8, &moved, 1);
    REQUIRE(instances.publishMorphWeightFrame(first, moved_frame) ==
            Animation::Status::ok);
    REQUIRE(instances.morphWeightFrameForTesting(first).previous ==
            std::vector<float>{0.0f});
    REQUIRE(instances.morphWeightFrameForTesting(first).previous_revision == 7);
    REQUIRE(instances.morphWeightFrameForTesting(second).previous ==
            std::vector<float>{1.0f});

    instances.resetTemporalHistory();
    REQUIRE(instances.morphWeightFrameForTesting(first).previous ==
            std::vector<float>{0.75f});
    auto duplicate = publishDesc(model, 8, &on, 1);
    REQUIRE(instances.publishMorphWeightFrame(first, duplicate) ==
            Animation::Status::duplicate_revision);
    duplicate.layout_generation++;
    duplicate.frame_revision = 9;
    REQUIRE(instances.publishMorphWeightFrame(first, duplicate) ==
            Animation::Status::stale_generation);
    auto discontinuity = publishDesc(model, 9, &on, 1,
                                     morphCommitDiscontinuity);
    REQUIRE(instances.publishMorphWeightFrame(first, discontinuity) ==
            Animation::Status::ok);
    REQUIRE(instances.morphWeightFrameForTesting(first).previous ==
            std::vector<float>{1.0f});
    const auto nan = std::numeric_limits<float>::quiet_NaN();
    auto non_finite = publishDesc(model, 10, &nan, 1);
    REQUIRE(instances.publishMorphWeightFrame(first, non_finite) ==
            Animation::Status::invalid_argument);
    REQUIRE(instances.morphWeightFrameForTesting(first).current ==
            std::vector<float>{1.0f});
}

TEST_CASE("skinned morph reload keeps instance identity and resets weights",
          "[wp121][morph][skinned][reload]") {
    setupLogger();
    auto sandbox = makeMorphSandbox("reload");
    const auto first_path = sandbox.root / "first.glb";
    const auto second_path = sandbox.root / "second.glb";
    TestMorphFixture::writeGlb(first_path, {.skinned = true});
    TestMorphFixture::writeGlb(second_path, {.skinned = true,
                                             .mesh_weight = 0.6});

    FastModuleContainer modules;
    requireMorphVulkan();
    auto &loader = GET_MODULE(GltfLoader);
    auto first_model = loader.loadGltfBinary(first_path.string());
    first_model.asset_id = ModelAssetId{42};
    auto second_model = loader.loadGltfBinary(second_path.string());
    second_model.asset_id = first_model.asset_id;
    REQUIRE(first_model.material_primitives.front().primitives.front().skinned);
    REQUIRE(first_model.morph_targets->primitives.front().skinned);

    auto &instances = GET_MODULE(PolygonInstanceContainer);
    const auto instance = instances.placeModelInstance(first_model);
    const auto identity =
        instances.morphWeightFrameForTesting(instance).instance_identity;
    const auto old_layout = instances.morphLayoutGenerationForTesting(instance);
    const float on = 1.0f;
    auto frame = publishDesc(first_model, 5, &on, 1);
    REQUIRE(instances.publishMorphWeightFrame(instance, frame) ==
            Animation::Status::ok);
    instances.advanceTemporalHistoryAfterRender();

    instances.rebuildModelInstances(first_model.asset_id, second_model);
    const auto &reset = instances.morphWeightFrameForTesting(instance);
    REQUIRE(reset.instance_identity == identity);
    REQUIRE(reset.layout_generation != old_layout);
    REQUIRE(reset.current == std::vector<float>{0.6f});
    REQUIRE(reset.previous == reset.current);
    REQUIRE(reset.current_revision == 0);
    REQUIRE(reset.previous_revision == 0);
}

TEST_CASE("VRM firstPerson auto becomes exact node-aware per-view draw ranges",
          "[wp134][vrm][firstperson][gltf][gpu]") {
    setupLogger();
    auto sandbox = makeMorphSandbox("first_person_auto");
    const auto path = sandbox.root / "first_person.vrm";
    TestMorphFixture::writeGlb(path, {.skinned = true,
                                      .vrm_first_person = true});

    FastModuleContainer modules;
    requireMorphVulkan();
    const auto model = GET_MODULE(GltfLoader).loadGltfBinary(path.string());
    REQUIRE(model.vrm_semantic);
    REQUIRE(model.material_primitives.size() == 1);
    const auto &primitives = model.material_primitives.front().primitives;
    REQUIRE(primitives.size() == 2);
    REQUIRE(primitives[0].node_index == 0);
    REQUIRE(primitives[1].node_index == 0);
    REQUIRE(primitives[0].index_count == 3);
    REQUIRE(primitives[1].index_count == 3);
    REQUIRE(primitives[0].skinned);
    REQUIRE(primitives[1].skinned);
    const auto count_visibility = [&](PrimitiveViewVisibility visibility) {
        return std::count_if(primitives.begin(), primitives.end(),
                             [&](const auto &primitive) {
                                 return primitive.view_visibility == visibility;
                             });
    };
    REQUIRE(count_visibility(PrimitiveViewVisibility::both) == 1);
    REQUIRE(count_visibility(PrimitiveViewVisibility::third_person_only) == 1);
    REQUIRE(model.morph_targets);
    REQUIRE(model.morph_targets->primitives.size() == 2);

    auto &instances = GET_MODULE(PolygonInstanceContainer);
    (void)instances.placeModelInstance(model);
    instances.triggerUpdate();
    const auto draw_count = [&](bool first_person) {
        std::uint32_t total = 0;
        for (const auto &draw : instances.getDrawCalls(first_person))
            total += draw.draw_count;
        return total;
    };
    REQUIRE(draw_count(false) == 2);
    REQUIRE(draw_count(true) == 1);
    REQUIRE(instances.drawItemsForTesting().size() == 2);
    REQUIRE(instances.drawItemsForTesting()[0].stable_identity.node_index == 0);
    REQUIRE(instances.drawItemsForTesting()[1].stable_identity.node_index == 0);
    REQUIRE(instances.drawItemsForTesting()[0].declaration_ordinal !=
            instances.drawItemsForTesting()[1].declaration_ordinal);
    REQUIRE(instances.drawItemsForTesting()[0].phase == MaterialPhase::opaque);

    const auto first_bytes = [&] {
        const auto bytes =
            instances.compiledDrawQueueForTesting().indirectBytes();
        return std::vector<std::byte>{bytes.begin(), bytes.end()};
    }();
    const auto first_third_person = instances.getDrawCalls(false);
    const auto first_first_person = instances.getDrawCalls(true);
    instances.triggerUpdate();
    const auto second_bytes =
        instances.compiledDrawQueueForTesting().indirectBytes();
    REQUIRE(std::equal(first_bytes.begin(), first_bytes.end(),
                       second_bytes.begin(), second_bytes.end()));
    REQUIRE(instances.getDrawCalls(false) == first_third_person);
    REQUIRE(instances.getDrawCalls(true) == first_first_person);
    GET_MODULE(VulkanManageCore).waitIdle();
}

} // namespace Pelican
