#include "../src/core/container.hpp"
#include "../src/core/launchconfig.hpp"
#include "../src/core/log.hpp"
#include "../src/core/material/materialcontainer.hpp"
#include "../src/core/material/standardmaterialresource.hpp"
#include "../src/core/model/gltf.hpp"
#include "../src/core/model/vertbufcontainer.hpp"
#include "../src/core/vkcore/core.hpp"
#include "morph_fixture.hpp"
#include "scene_load_fault_test_support.hpp"
#include "vulkan_test_support.hpp"
#include "../src/core/vkcore/deletionqueue.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <array>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace Pelican {
namespace {

struct ReleaseSandbox {
    std::filesystem::path root;
    ~ReleaseSandbox() {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }
};

ReleaseSandbox makeReleaseSandbox() {
    auto root = std::filesystem::temp_directory_path() /
                ("pelican_wp380c_release_" +
                 std::to_string(std::chrono::steady_clock::now()
                                    .time_since_epoch()
                                    .count()));
    std::filesystem::create_directories(root);
    return {root};
}

CommonPolygonVertData skinnedTriangle() {
    CommonPolygonVertData data;
    data.pos = {{0.0F, 0.0F, 0.0F}, {1.0F, 0.0F, 0.0F},
                {0.0F, 1.0F, 0.0F}};
    data.indices = {0, 1, 2};
    data.joint.assign(3, glm::i16vec4{0, 0, 0, 0});
    data.weight.assign(3, glm::vec4{1.0F, 0.0F, 0.0F, 0.0F});
    return data;
}

CommonPolygonVertData morphSkinnedTriangle() {
    auto data = skinnedTriangle();
    MorphTargetVertexData target;
    target.position.assign(3, glm::vec3{0.0F});
    target.presence_mask = morphPositionPresent;
    data.morph_targets.push_back(std::move(target));
    return data;
}

ModelTemplate releaseCandidate(const std::filesystem::path &glb,
                               MaterialContainer &) {
    auto candidate = GET_MODULE(GltfLoader).loadGltfBinary(glb.string());
    REQUIRE(candidate.gpu_resources != nullptr);
    REQUIRE(candidate.gpu_resources->textures.size() == 1);
    return candidate;
}

} // namespace

TEST_CASE(
    "WP380c rejected model candidate reports morph release and returns every ownership column",
    "[WP380c][model-release][gpu]") {
    setupLogger();
    const auto sandbox = makeReleaseSandbox();
    const auto glb = sandbox.root / "morph.glb";
    TestMorphFixture::writeGlb(glb);

    FastModuleContainer modules;
    auto &launch = GET_MODULE(EngineLaunchConfig);
    launch.headless = true;
    launch.headless_extent = vk::Extent2D{16, 16};
    TestSupport::requireVulkanDevice("Vulkan unavailable");
    (void)GET_MODULE(StandardMaterialResource);

    auto &geometry = GET_MODULE(VertBufContainer);
    auto &materials = GET_MODULE(MaterialContainer);
    const auto baseline_indices = geometry.allocatedIndexCountForTesting();
    const auto baseline_static = geometry.allocatedVertexCountForTesting(false);
    const auto baseline_skinned = geometry.allocatedVertexCountForTesting(true);
    const auto baseline_morph = geometry.allocatedMorphDeltaCountForTesting();
    const auto baseline_materials = materials.materialCountForTesting();
    const auto baseline_textures = materials.textureCountForTesting();

    auto candidate = GET_MODULE(GltfLoader).loadGltfBinary(glb.string());
    REQUIRE(candidate.gpu_resources != nullptr);
    candidate.gpu_resources->geometry.push_back(
        geometry.addSkinnedPrimitiveAllocation(skinnedTriangle()));
    const std::array<std::uint8_t, 4> pixel{255, 255, 255, 255};
    candidate.gpu_resources->textures.push_back(materials.registerTexture(
        vk::Extent3D{1, 1, 1}, pixel.data(), vk::Format::eR8G8B8A8Unorm,
        pixel.size()));

    test_support::setMorphMetadataReleaseFailureForTesting(geometry, true);
    const auto report = releaseModelGpuResources(candidate, false);
    test_support::setMorphMetadataReleaseFailureForTesting(geometry, false);

    REQUIRE(report.error_count == 1);
    REQUIRE(report.overflow_count == 0);
    REQUIRE(report.errors[0].stage == ReleaseStage::vertbuf_morph_delta);
    REQUIRE(report.errors[0].code == ReleaseErrorCode::operation_failed);
    REQUIRE(report.errors[0].index == 0);
    REQUIRE(geometry.allocatedIndexCountForTesting() == baseline_indices);
    REQUIRE(geometry.allocatedVertexCountForTesting(false) == baseline_static);
    REQUIRE(geometry.allocatedVertexCountForTesting(true) == baseline_skinned);
    REQUIRE(geometry.allocatedMorphDeltaCountForTesting() == baseline_morph);
    REQUIRE(materials.materialCountForTesting() == baseline_materials);
    REQUIRE(materials.textureCountForTesting() == baseline_textures);
    GET_MODULE(VulkanManageCore).waitIdle();
}

TEST_CASE(
    "WP380d material and texture release failures are isolated and return every ownership column",
    "[WP380d][model-release][gpu]") {
    setupLogger();
    const auto sandbox = makeReleaseSandbox();
    const auto glb = sandbox.root / "morph.glb";
    TestMorphFixture::writeGlb(glb);

    FastModuleContainer modules;
    auto &launch = GET_MODULE(EngineLaunchConfig);
    launch.headless = true;
    launch.headless_extent = vk::Extent2D{16, 16};
    TestSupport::requireVulkanDevice("Vulkan unavailable");
    (void)GET_MODULE(StandardMaterialResource);
    auto &geometry = GET_MODULE(VertBufContainer);
    auto &materials = GET_MODULE(MaterialContainer);
    const auto baseline_indices = geometry.allocatedIndexCountForTesting();
    const auto baseline_static = geometry.allocatedVertexCountForTesting(false);
    const auto baseline_skinned = geometry.allocatedVertexCountForTesting(true);
    const auto baseline_morph = geometry.allocatedMorphDeltaCountForTesting();
    const auto baseline_materials = materials.materialCountForTesting();
    const auto baseline_textures = materials.textureCountForTesting();

    const auto require_baseline = [&] {
        REQUIRE(geometry.allocatedIndexCountForTesting() == baseline_indices);
        REQUIRE(geometry.allocatedVertexCountForTesting(false) == baseline_static);
        REQUIRE(geometry.allocatedVertexCountForTesting(true) == baseline_skinned);
        REQUIRE(geometry.allocatedMorphDeltaCountForTesting() == baseline_morph);
        REQUIRE(materials.materialCountForTesting() == baseline_materials);
        REQUIRE(materials.textureCountForTesting() == baseline_textures);
    };

    SECTION("texture") {
        auto control = releaseCandidate(glb, materials);
        const auto control_report = releaseModelGpuResources(control, false);
        REQUIRE(control_report.empty());
        require_baseline();

        auto candidate = releaseCandidate(glb, materials);
        const auto texture = candidate.gpu_resources->textures.back();
        test_support::setTextureReleaseFailureForTesting(materials, true);
        const auto report = releaseModelGpuResources(candidate, false);
        test_support::setTextureReleaseFailureForTesting(materials, false);
        REQUIRE(report.error_count == 1);
        REQUIRE(report.overflow_count == 0);
        REQUIRE(report.errors[0] == ReleaseError{
                                       ReleaseStage::texture,
                                       ReleaseErrorCode::operation_failed,
                                       static_cast<std::uint32_t>(texture.value)});
        require_baseline();
    }

    SECTION("material") {
        auto control = releaseCandidate(glb, materials);
        const auto control_report = releaseModelGpuResources(control, false);
        REQUIRE(control_report.empty());
        require_baseline();

        auto candidate = releaseCandidate(glb, materials);
        REQUIRE_FALSE(candidate.gpu_resources->materials.empty());
        const auto material = candidate.gpu_resources->materials.front();
        test_support::setMaterialReleaseFailureForTesting(materials, true);
        const auto report = releaseModelGpuResources(candidate, false);
        test_support::setMaterialReleaseFailureForTesting(materials, false);
        REQUIRE(report.error_count == 1);
        REQUIRE(report.overflow_count == 0);
        REQUIRE(report.errors[0] == ReleaseError{
                                       ReleaseStage::material,
                                       ReleaseErrorCode::operation_failed,
                                       static_cast<std::uint32_t>(material.value)});
        require_baseline();
    }
    GET_MODULE(VulkanManageCore).waitIdle();
}

TEST_CASE(
    "WP380d deferred morph releases expose report overflow through the structured sink",
    "[WP380d][model-release][gpu]") {
    setupLogger();
    FastModuleContainer modules;
    auto &launch = GET_MODULE(EngineLaunchConfig);
    launch.headless = true;
    launch.headless_extent = vk::Extent2D{16, 16};
    TestSupport::requireVulkanDevice("Vulkan unavailable");
    auto &geometry = GET_MODULE(VertBufContainer);
    auto &deletion_queue = GET_MODULE(DeletionQueue);
    const auto baseline = geometry.allocatedMorphDeltaCountForTesting();
    const auto before = ReleaseErrorSink::status();

    ModelTemplate candidate;
    candidate.gpu_resources = std::make_shared<ModelGpuResources>();
    for (std::uint32_t index = 0; index < 9; ++index) {
        candidate.gpu_resources->geometry.push_back(
            geometry.addSkinnedPrimitiveAllocation(morphSkinnedTriangle()));
    }
    test_support::setMorphMetadataReleaseFailureForTesting(geometry, true);
    const auto report = releaseModelGpuResources(candidate, true);
    REQUIRE(report.empty());
    deletion_queue.flushAll();
    test_support::setMorphMetadataReleaseFailureForTesting(geometry, false);

    const auto after = ReleaseErrorSink::status();
    REQUIRE(after.release_errors == before.release_errors +
                                        ReleaseReport::capacity);
    REQUIRE(after.overflow_count == before.overflow_count + 1);
    REQUIRE(geometry.allocatedMorphDeltaCountForTesting() == baseline);
    GET_MODULE(VulkanManageCore).waitIdle();
}

} // namespace Pelican
