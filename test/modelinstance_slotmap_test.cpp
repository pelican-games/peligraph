#include "../src/core/container.hpp"
#include "../src/core/launchconfig.hpp"
#include "../src/core/log.hpp"
#include "../src/core/material/standardmaterialresource.hpp"
#include "../src/core/playback/seqplayer.hpp"
#include "../src/core/renderer/indirectdrawlimits.hpp"
#include "../src/core/renderer/polygoninstancecontainer.hpp"
#include "../src/core/userpublic/animation/abi_v1.hpp"
#include "../src/core/vkcore/core.hpp"
#include "vulkan_test_support.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace Pelican {
namespace {

void requireSlotMapVulkan() {
    auto &launch = GET_MODULE(EngineLaunchConfig);
    launch.headless = true;
    launch.headless_extent = vk::Extent2D{16, 16};
    TestSupport::requireVulkanDevice("Vulkan unavailable");
    (void)GET_MODULE(StandardMaterialResource);
}

ModelTemplate emptyModel() {
    ModelTemplate model;
    model.asset_id = ModelAssetId{146};
    return model;
}

struct TempSequence {
    std::filesystem::path path;

    TempSequence() {
        path = std::filesystem::temp_directory_path() /
               ("pelican_wp146_seq_" +
                std::to_string(std::chrono::steady_clock::now()
                                   .time_since_epoch()
                                   .count()) +
                ".jsonl");
        std::ofstream file{path, std::ios::binary};
        file << R"jsonl({"schema":"pelican.transform_seq","version":1,"fps":30,"objects":["owned"]}
{"t":0,"transforms":[{"pos":[0,0,0],"rot":[0,0,0,1],"scale":[1,1,1]}]}
)jsonl";
    }

    ~TempSequence() { std::filesystem::remove(path); }
};

} // namespace

TEST_CASE("indirect draw ranges respect the physical-device draw-count limit",
          "[renderer][indirect][limits]") {
    const auto segments =
        renderer_detail::splitIndirectDrawRange(7, 17, 4);
    REQUIRE(segments.size() == 3);
    CHECK(segments[0].first_command == 7);
    CHECK(segments[0].draw_count == 4);
    CHECK(segments[1].first_command == 11);
    CHECK(segments[1].draw_count == 4);
    CHECK(segments[2].first_command == 15);
    CHECK(segments[2].draw_count == 2);

    CHECK(renderer_detail::splitIndirectDrawRange(3, 3, 4).empty());
    CHECK_THROWS_AS(renderer_detail::splitIndirectDrawRange(0, 1, 0),
                    std::invalid_argument);
    CHECK_THROWS_AS(renderer_detail::splitIndirectDrawRange(2, 1, 4),
                    std::invalid_argument);
}

TEST_CASE("ModelInstanceId reuses one slot through ten thousand churn cycles",
          "[wp146][model-instance][slotmap][churn][gpu]") {
    setupLogger();
    FastModuleContainer modules;
    requireSlotMapVulkan();

    auto &instances = GET_MODULE(PolygonInstanceContainer);
    const auto model = emptyModel();
    const auto started = std::chrono::steady_clock::now();
    ModelInstanceId previous;
    for (std::uint32_t cycle = 0; cycle < 10'000; ++cycle) {
        const auto current = instances.placeModelInstance(model);
        REQUIRE(current.index == 0);
        REQUIRE(instances.isModelInstanceAlive(current));
        if (cycle != 0) {
            REQUIRE(current.generation > previous.generation);
        }
        REQUIRE(instances.removeModelInstance(current));
        REQUIRE_FALSE(instances.isModelInstanceAlive(current));
        previous = current;
    }
    const auto elapsed = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - started);
    std::cout << "WP146 churn_10000_ms=" << elapsed.count() << '\n';

    REQUIRE(instances.instanceCountForTesting() == 0);
    REQUIRE(instances.slotCountForTesting() == 1);
    GET_MODULE(VulkanManageCore).waitIdle();
}

TEST_CASE("ModelInstanceId rejects stale and double-remove handles without mutation",
          "[wp146][model-instance][slotmap][stale][gpu]") {
    setupLogger();
    FastModuleContainer modules;
    requireSlotMapVulkan();

    auto &instances = GET_MODULE(PolygonInstanceContainer);
    const auto model = emptyModel();
    const auto stale = instances.placeModelInstance(model);
    REQUIRE(instances.removeModelInstance(stale));
    REQUIRE_FALSE(instances.removeModelInstance(stale));

    const auto live = instances.placeModelInstance(model);
    REQUIRE(live.index == stale.index);
    REQUIRE(live.generation != stale.generation);
    instances.setTrs(live, {4.0f, 0.0f, 0.0f},
                     glm::quat{1.0f, 0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f});

    using Catch::Matchers::ContainsSubstring;
    REQUIRE_THROWS_WITH(
        instances.setTrs(stale, {9.0f, 0.0f, 0.0f},
                         glm::quat{1.0f, 0.0f, 0.0f, 0.0f},
                         {1.0f, 1.0f, 1.0f}),
        ContainsSubstring("setTrs") && ContainsSubstring(toString(stale)));
    REQUIRE_THROWS_WITH(instances.setSkinningPalette(stale, {}),
                        ContainsSubstring("setSkinningPalette") &&
                            ContainsSubstring(toString(stale)));
    REQUIRE_THROWS_WITH(instances.currentModelMatrixForTesting(stale),
                        ContainsSubstring("currentModelMatrixForTesting") &&
                            ContainsSubstring(toString(stale)));
    REQUIRE(instances.currentModelMatrixForTesting(live)[3].x == 4.0f);

    PublishMorphWeightFrameDescV1 morph;
    REQUIRE(instances.publishMorphWeightFrame(stale, morph) ==
            Animation::Status::invalid_handle);
    REQUIRE_FALSE(Animation::isValid(instances.animationInstance(stale)));
    REQUIRE(instances.instanceCountForTesting() == 1);
    GET_MODULE(VulkanManageCore).waitIdle();
}

TEST_CASE("ModelInstanceId clear advances scene epoch and invalidates every owner",
          "[wp146][model-instance][slotmap][clear][scene-owner][gpu]") {
    setupLogger();
    FastModuleContainer modules;
    requireSlotMapVulkan();

    auto &instances = GET_MODULE(PolygonInstanceContainer);
    const auto model = emptyModel();
    const auto scene_owned = instances.placeModelInstance(model);
    instances.clear();
    REQUIRE(instances.instanceCountForTesting() == 0);
    REQUIRE_FALSE(instances.isModelInstanceAlive(scene_owned));
    REQUIRE_FALSE(instances.removeModelInstance(scene_owned));

    const auto replacement = instances.placeModelInstance(model);
    REQUIRE(replacement.index == scene_owned.index);
    REQUIRE(replacement.scene_epoch != scene_owned.scene_epoch);
    REQUIRE(replacement.generation != scene_owned.generation);
    REQUIRE_THROWS_WITH(
        instances.setTrs(scene_owned, {}, {}, {1.0f, 1.0f, 1.0f}),
        Catch::Matchers::ContainsSubstring("setTrs"));
    REQUIRE(instances.isModelInstanceAlive(replacement));
    GET_MODULE(VulkanManageCore).waitIdle();
}

TEST_CASE("Model instance capacity remains an active-live limit and freed slots recover",
          "[wp146][model-instance][slotmap][capacity][gpu]") {
    setupLogger();
    FastModuleContainer modules;
    requireSlotMapVulkan();

    auto &instances = GET_MODULE(PolygonInstanceContainer);
    const auto model = emptyModel();
    std::vector<ModelInstanceId> handles;
    handles.reserve(1024);
    for (std::size_t index = 0; index < 1024; ++index) {
        handles.push_back(instances.placeModelInstance(model));
    }
    REQUIRE(instances.instanceCountForTesting() == 1024);
    REQUIRE_THROWS_WITH(instances.placeModelInstance(model),
                        "Model instance capacity exceeded");
    REQUIRE(instances.instanceCountForTesting() == 1024);

    constexpr std::size_t released = 517;
    REQUIRE(instances.removeModelInstance(handles[released]));
    const auto recycled = instances.placeModelInstance(model);
    REQUIRE(recycled.index == handles[released].index);
    REQUIRE(recycled.generation != handles[released].generation);
    REQUIRE(instances.instanceCountForTesting() == 1024);
    GET_MODULE(VulkanManageCore).waitIdle();
}

TEST_CASE("ModelInstanceId retires generation wrap and rejects scene epoch wrap",
          "[wp146][model-instance][slotmap][wrap][gpu]") {
    setupLogger();
    FastModuleContainer modules;
    requireSlotMapVulkan();

    auto &instances = GET_MODULE(PolygonInstanceContainer);
    const auto model = emptyModel();
    const auto first = instances.placeModelInstance(model);
    const auto terminal = instances.forceGenerationForTesting(
        first, std::numeric_limits<std::uint32_t>::max());
    REQUIRE(instances.removeModelInstance(terminal));
    const auto next = instances.placeModelInstance(model);
    REQUIRE(next.index != terminal.index);
    REQUIRE(instances.removeModelInstance(next));

    instances.forceSceneEpochForTesting(
        std::numeric_limits<std::uint64_t>::max());
    const auto at_terminal_epoch = instances.placeModelInstance(model);
    REQUIRE_THROWS_WITH(
        instances.clear(),
        "PolygonInstanceContainer::clear: ModelInstanceId scene epoch exhausted");
    REQUIRE(instances.isModelInstanceAlive(at_terminal_epoch));
    REQUIRE(instances.instanceCountForTesting() == 1);
    GET_MODULE(VulkanManageCore).waitIdle();
}

TEST_CASE("SeqPlayer releases playback-owned model instances at scene boundary",
          "[wp146][model-instance][ownership][seqplayer][gpu]") {
    setupLogger();
    FastModuleContainer modules;
    TempSequence sequence;
    auto &launch = GET_MODULE(EngineLaunchConfig);
    launch.headless = true;
    launch.headless_extent = vk::Extent2D{16, 16};
    launch.play_seq = sequence.path;
    launch.seq_mesh = "builtin:sphere";
    requireSlotMapVulkan();

    auto &player = GET_MODULE(SeqPlayer);
    REQUIRE(player.isEnabled());
    REQUIRE(player.instanceCountForTesting() == 1);
    const auto owned = player.instanceForTesting(0);
    auto &instances = GET_MODULE(PolygonInstanceContainer);
    REQUIRE(instances.isModelInstanceAlive(owned));

    player.releaseInstancesForSceneLoad();
    REQUIRE_FALSE(player.isEnabled());
    REQUIRE(player.instanceCountForTesting() == 0);
    REQUIRE_FALSE(instances.isModelInstanceAlive(owned));
    REQUIRE_FALSE(instances.removeModelInstance(owned));
    GET_MODULE(VulkanManageCore).waitIdle();
}

} // namespace Pelican
