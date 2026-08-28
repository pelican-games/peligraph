#include "../src/core/vkcore/rendertiming.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <algorithm>
#include <bit>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace Pelican {
namespace {

std::filesystem::path fixtureRoot() {
    return std::filesystem::path{PELICAN_TEST_SOURCE_DIR} / "test" / "fixtures";
}

nlohmann::json readJson(const std::filesystem::path &path) {
    std::ifstream stream{path};
    if (!stream) throw std::runtime_error("failed to open fixture: " + path.string());
    return nlohmann::json::parse(stream);
}

} // namespace

TEST_CASE("GPU timing identity is the command-label identity plus ordered subrange",
          "[gpu-timing][identity]") {
    const std::vector<std::pair<GpuTimingSubrange, std::string>> subranges{
        {GpuTimingSubrange::barriers, "barriers"},
        {GpuTimingSubrange::body, "body"},
    };
    for (const auto &[subrange, name] : subranges) {
        const GpuTimingSampleIdentity identity{
            42, "xr", 1, 7, "compute", "cull#xr", subrange};
        REQUIRE(makeGpuTimingSampleLabel(identity) ==
                "frame/42/graph/xr/view/1/node/7:compute:cull#xr/" + name);
    }
}

TEST_CASE("GPU timing attribution table is an exact fixture",
          "[gpu-timing][attribution]") {
    const auto expected = readJson(fixtureRoot() / "gpu_timing_attribution.json");
    REQUIRE(gpuTimingAttributionContractJson() == expected);

    const auto &rows = expected.at("rows");
    REQUIRE(rows.size() == 7);
    REQUIRE(rows.at(0).at("node_kind") == "render");
    REQUIRE(rows.at(1).at("node_kind") == "compute");
    REQUIRE(rows.at(2).at("node_kind") == "anchor");
    REQUIRE(rows.at(4).at("node_kind") == "output_transform");
    REQUIRE(rows.at(5).at("operation") == "xr_mirror_intermediate");
    REQUIRE(rows.at(6).at("operation") == "desktop_mirror");
}

TEST_CASE("disabled GPU timing status keeps the additive v2 schema",
          "[gpu-timing][status]") {
    const auto status = disabledGpuTimingStatusJson();
    REQUIRE(status.at("schema_version") == 2);
    REQUIRE_FALSE(status.at("enabled").get<bool>());
    REQUIRE_FALSE(status.at("supported").get<bool>());
    REQUIRE(status.at("reason") == "feature_not_enabled");
    REQUIRE(status.at("history_capacity") == 120);
    REQUIRE(status.at("history_count") == 0);
    REQUIRE(status.at("dropped_samples") == 0);
    REQUIRE(status.at(
                "logical_frame_averages")
                .empty());
    REQUIRE(status.at(
                "logical_frame_history")
                .empty());
    REQUIRE(status.at("views").empty());
    REQUIRE(status.at("nodes").empty());
}

TEST_CASE("GPU timing node averages use multiple frames and keep subranges independent",
          "[gpu-timing][average][wp351]") {
    const auto row = [](std::uint64_t frame, double barriers_ms,
                        double body_ms) {
        return GpuTimingNodeRow{
            frame, "flat", 0, 4, "render", "lit", barriers_ms,
            body_ms, true};
    };
    const std::vector<GpuTimingNodeFrame> body_varied_frames{
        {10, "flat", {row(10, 4.0, 2.0)}},
        {11, "flat", {row(11, 4.0, 8.0)}},
    };

    const auto body_varied = averageGpuTimingNodeFrames(body_varied_frames);
    REQUIRE(body_varied.frame_count == 2);
    REQUIRE(body_varied.nodes.size() == 1);
    const auto &body_node = body_varied.nodes.front();
    REQUIRE(body_node.sample_count == 2);
    REQUIRE(body_node.body_ms == Catch::Approx(5.0));
    REQUIRE(body_node.body_ms != Catch::Approx(2.0));
    REQUIRE(body_node.body_ms != Catch::Approx(8.0));
    REQUIRE(body_node.barriers_ms == Catch::Approx(4.0));

    const std::vector<GpuTimingNodeFrame> barriers_varied_frames{
        {20, "flat", {row(20, 2.0, 7.0)}},
        {21, "flat", {row(21, 8.0, 7.0)}},
    };
    const auto barriers_varied =
        averageGpuTimingNodeFrames(barriers_varied_frames);
    REQUIRE(barriers_varied.frame_count == 2);
    REQUIRE(barriers_varied.nodes.size() == 1);
    const auto &barriers_node = barriers_varied.nodes.front();
    REQUIRE(barriers_node.sample_count == 2);
    REQUIRE(barriers_node.barriers_ms == Catch::Approx(5.0));
    REQUIRE(barriers_node.barriers_ms != Catch::Approx(2.0));
    REQUIRE(barriers_node.barriers_ms != Catch::Approx(8.0));
    REQUIRE(barriers_node.body_ms == Catch::Approx(7.0));
}

TEST_CASE("GPU timing node averages divide by samples when a node is absent from a frame",
          "[gpu-timing][average][sample-count][wp351a]") {
    const std::vector<GpuTimingNodeFrame> frames{
        {30, "flat", {}},
        {31,
         "flat",
         {GpuTimingNodeRow{31, "flat", 0, 4, "render", "lit",
                           9.0, 12.0, true}}},
    };

    const auto average = averageGpuTimingNodeFrames(frames);
    REQUIRE(average.frame_count == 2);
    REQUIRE(average.nodes.size() == 1);
    const auto &node = average.nodes.front();
    REQUIRE(node.sample_count == 1);
    REQUIRE(node.barriers_ms == Catch::Approx(9.0));
    REQUIRE(node.body_ms == Catch::Approx(12.0));
}

TEST_CASE("GPU timing node averages discard frames older than the fixed window",
          "[gpu-timing][average][window][wp351]") {
    std::vector<GpuTimingNodeFrame> frames;
    frames.reserve(gpu_timing_node_average_window + 1);
    for (std::size_t index = 0;
         index < gpu_timing_node_average_window + 1; ++index) {
        const bool discarded = index == 0;
        frames.push_back(GpuTimingNodeFrame{
            index + 1,
            "flat",
            {GpuTimingNodeRow{
                index + 1, "flat", 0, 1, "compute", "cull",
                discarded ? 6000.0 : 6.0,
                discarded ? 3000.0 : 3.0, true}},
        });
    }

    const auto average = averageGpuTimingNodeFrames(frames);
    REQUIRE(average.frame_count == gpu_timing_node_average_window);
    REQUIRE(average.nodes.front().sample_count ==
            gpu_timing_node_average_window);
    REQUIRE(average.nodes.front().barriers_ms == Catch::Approx(6.0));
    REQUIRE(average.nodes.front().body_ms == Catch::Approx(3.0));
}

TEST_CASE("GPU timing average RPC projection is light and reports disabled state",
          "[gpu-timing][rpc][wp351]") {
    GpuTimingNodeAverageSnapshot snapshot;
    snapshot.frame_count = 3;
    snapshot.nodes.push_back(GpuTimingNodeAverageRow{
        9, "flat", 0, 2, "render", "present", 0.25, 1.5,
        true, 3});
    const auto enabled = gpuTimingNodeAverageStatusJson(
        snapshot, true, true, "enabled");
    REQUIRE(enabled.at("schema") ==
            "pelican.gpu_timing_node_averages");
    REQUIRE(enabled.at("version") == 1);
    REQUIRE(enabled.at("window_size") ==
            gpu_timing_node_average_window);
    REQUIRE(enabled.at("frame_count") == 3);
    REQUIRE(enabled.at("nodes").at(0).at("sample_count") == 3);
    REQUIRE_FALSE(enabled.contains("logical_frame_history"));

    const auto disabled = disabledGpuTimingNodeAverageStatusJson();
    REQUIRE_FALSE(disabled.at("enabled").get<bool>());
    REQUIRE_FALSE(disabled.at("supported").get<bool>());
    REQUIRE(disabled.at("reason") == "feature_not_enabled");
    REQUIRE(disabled.at("frame_count") == 0);
    REQUIRE(disabled.at("nodes").empty());
    REQUIRE_FALSE(disabled.contains("logical_frame_history"));
}

TEST_CASE(
    "WP355 status totals match the pre-WP351b destructive canonical sort bit for bit",
    "[gpu-timing][status][wp355][canonical-order][bit-exact]") {
    const auto sample = [](std::size_t ordinal, std::string name,
                           GpuTimingSubrange subrange, double ms) {
        return GpuTimingSample{
            .identity =
                GpuTimingSampleIdentity{
                    42, "flat", 0, ordinal, "render",
                    std::move(name), subrange},
            .supported = true,
            .ms = ms,
        };
    };

    // Declaration order is red_after_blue (ordinal 0), blue_first
    // (ordinal 1), while execution order is blue then red, matching the
    // explicit-order golden fixture.  All values are non-negative GPU times.
    GpuTimingHistoryFrame frame{
        .logical_frame = 42,
        .graph_variant = "flat",
        .samples = {
            sample(1, "blue_first", GpuTimingSubrange::barriers, 1.0e16),
            sample(1, "blue_first", GpuTimingSubrange::body, 1.0),
            sample(0, "red_after_blue", GpuTimingSubrange::barriers, 1.0),
            sample(0, "red_after_blue", GpuTimingSubrange::body, 1.0),
        },
    };
    const auto bits = [](double value) {
        return std::bit_cast<std::uint64_t>(value);
    };
    constexpr std::uint64_t execution_order_bits =
        0x4341c37937e08000ULL;
    constexpr std::uint64_t parent_canonical_bits =
        0x4341c37937e08002ULL;

    double execution_total = 0.0;
    for (const auto &entry : frame.samples) {
        execution_total += entry.ms;
    }
    REQUIRE(bits(execution_total) == execution_order_bits);

    // This is the exact destructive comparator and subsequent addition order
    // from git show 29a0615^:src/core/vkcore/rendertiming.cpp.
    auto parent_samples = frame.samples;
    std::sort(
        parent_samples.begin(), parent_samples.end(),
        [](const auto &left, const auto &right) {
            return std::tie(left.identity.view_index,
                            left.identity.node_ordinal,
                            left.identity.node_kind,
                            left.identity.node_name,
                            left.identity.subrange) <
                   std::tie(right.identity.view_index,
                            right.identity.node_ordinal,
                            right.identity.node_kind,
                            right.identity.node_name,
                            right.identity.subrange);
        });
    double parent_total = 0.0;
    for (const auto &entry : parent_samples) {
        parent_total += entry.ms;
    }
    REQUIRE(bits(parent_total) == parent_canonical_bits);
    REQUIRE(bits(parent_total) != bits(execution_total));

    const std::deque<GpuTimingHistoryFrame> history{frame};
    const auto latest = publishLatestGpuTimingSnapshot(history);
    const auto status = projectGpuTimingStatus(
        history,
        std::span<const GpuTimingViewRow>{latest.latest_views});
    CAPTURE(bits(status.logical_frame_history.at(0).at("total_ms")
                     .get<double>()),
            bits(status.logical_frame_averages.at(0)
                     .at("average_total_ms")
                     .get<double>()),
            bits(latest.latest_views.at(0).total_ms),
            bits(status.logical_frame_total_sum_views_ms));
    CHECK(bits(status.logical_frame_history.at(0).at("total_ms")
                   .get<double>()) == parent_canonical_bits);
    CHECK(bits(status.logical_frame_averages.at(0)
                   .at("average_total_ms")
                   .get<double>()) == parent_canonical_bits);
    CHECK(bits(latest.latest_views.at(0).total_ms) ==
          parent_canonical_bits);
    CHECK(bits(status.views.at(0).at("total_ms").get<double>()) ==
          parent_canonical_bits);
    CHECK(bits(status.logical_frame_total_sum_views_ms) ==
          parent_canonical_bits);

    REQUIRE(status.nodes.size() == 4);
    REQUIRE(status.nodes.at(0).at("node_name") == "red_after_blue");
    REQUIRE(status.nodes.at(0).at("subrange") == "barriers");
    REQUIRE(status.nodes.at(1).at("node_name") == "red_after_blue");
    REQUIRE(status.nodes.at(1).at("subrange") == "body");
    REQUIRE(status.nodes.at(2).at("node_name") == "blue_first");
    REQUIRE(status.nodes.at(2).at("subrange") == "barriers");
    REQUIRE(status.nodes.at(3).at("node_name") == "blue_first");
    REQUIRE(status.nodes.at(3).at("subrange") == "body");
}

TEST_CASE(
    "WP355 latest GPU timing publisher reports its real one-frame traversal",
    "[gpu-timing][status][wp355][history-visits][negative-contrast]") {
    std::deque<GpuTimingHistoryFrame> history;
    for (std::uint64_t logical_frame = 1;
         logical_frame <= gpu_timing_history_capacity; ++logical_frame) {
        history.push_back(GpuTimingHistoryFrame{
            .logical_frame = logical_frame,
            .graph_variant = "flat",
            .samples = {
                GpuTimingSample{
                    .identity = GpuTimingSampleIdentity{
                        logical_frame, "flat", 0, 0, "render", "present",
                        GpuTimingSubrange::body},
                    .supported = true,
                    .ms = static_cast<double>(logical_frame),
                },
            },
        });
    }

    const auto snapshot = publishLatestGpuTimingSnapshot(history);
    REQUIRE(snapshot.history_frame_visits == 1);
    REQUIRE(snapshot.latest_nodes.size() == 1);
    REQUIRE(snapshot.latest_nodes.front().logical_frame ==
            gpu_timing_history_capacity);
    REQUIRE(snapshot.latest_views.size() == 1);
    REQUIRE(snapshot.latest_views.front().logical_frame ==
            gpu_timing_history_capacity);
}

TEST_CASE(
    "WP355a GPU timing rejects a duplicate sample identity within a frame",
    "[gpu-timing][identity][wp355a][duplicate][fail-fast]") {
    const GpuTimingSample duplicate{
        .identity = GpuTimingSampleIdentity{
            42, "flat", 0, 7, "render", "lighting",
            GpuTimingSubrange::body},
        .supported = true,
        .ms = 0.25,
    };
    const std::deque<GpuTimingHistoryFrame> history{
        GpuTimingHistoryFrame{
            .logical_frame = 42,
            .graph_variant = "flat",
            .samples = {duplicate, duplicate},
        }};
    const std::string expected =
        "Duplicate GpuTimingSampleIdentity in frame: "
        "frame/42/graph/flat/view/0/node/7:render:lighting/body";

    REQUIRE_THROWS_WITH(
        publishLatestGpuTimingSnapshot(history), expected);
    REQUIRE_THROWS_WITH(
        projectGpuTimingStatus(history, {}), expected);
}

} // namespace Pelican
